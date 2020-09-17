/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specic language governing permissions and
 * limitations under the License.
 */

#ifndef MEDIA_PROVIDER_JNI_NODE_INL_H_
#define MEDIA_PROVIDER_JNI_NODE_INL_H_

#include <android-base/logging.h>

#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "libfuse_jni/ReaddirHelper.h"
#include "libfuse_jni/RedactionInfo.h"

class NodeTest;

namespace mediaprovider {
namespace fuse {

struct handle {
    explicit handle(int fd, const RedactionInfo* ri, bool cached) : fd(fd), ri(ri), cached(cached) {
        CHECK(ri != nullptr);
    }

    const int fd;
    const std::unique_ptr<const RedactionInfo> ri;
    const bool cached;

    ~handle() { close(fd); }
};

struct dirhandle {
    explicit dirhandle(DIR* dir) : d(dir), next_off(0) { CHECK(dir != nullptr); }

    DIR* const d;
    off_t next_off;
    // Fuse readdir() is called multiple times based on the size of the buffer and
    // number of directory entries in the given directory. 'de' holds the list
    // of directory entries for the directory handle and this list is available
    // across subsequent readdir() calls for the same directory handle.
    std::vector<std::shared_ptr<DirectoryEntry>> de;

    ~dirhandle() { closedir(d); }
};

// Whether inode tracking is enabled or not. When enabled, we maintain a
// separate mapping from inode numbers to "live" nodes so we can detect when
// we receive a request to a node that has been deleted.
static constexpr bool kEnableInodeTracking = true;

class node;

// Tracks the set of active nodes associated with a FUSE instance so that we
// can assert that we only ever return an active node in response to a lookup.
class NodeTracker {
  public:
    explicit NodeTracker(std::recursive_mutex* lock) : lock_(lock) {}

    void CheckTracked(__u64 ino) const {
        if (kEnableInodeTracking) {
            const node* node = reinterpret_cast<const class node*>(ino);
            std::lock_guard<std::recursive_mutex> guard(*lock_);
            CHECK(active_nodes_.find(node) != active_nodes_.end());
        }
    }

    void NodeDeleted(const node* node) {
        if (kEnableInodeTracking) {
            std::lock_guard<std::recursive_mutex> guard(*lock_);
            LOG(DEBUG) << "Node: " << reinterpret_cast<uintptr_t>(node) << " deleted.";

            CHECK(active_nodes_.find(node) != active_nodes_.end());
            active_nodes_.erase(node);
        }
    }

    void NodeCreated(const node* node) {
        if (kEnableInodeTracking) {
            std::lock_guard<std::recursive_mutex> guard(*lock_);
            LOG(DEBUG) << "Node: " << reinterpret_cast<uintptr_t>(node) << " created.";

            CHECK(active_nodes_.find(node) == active_nodes_.end());
            active_nodes_.insert(node);
        }
    }

  private:
    std::recursive_mutex* lock_;
    std::unordered_set<const node*> active_nodes_;
};

class node {
  public:
    // Creates a new node with the specified parent, name and lock.
    static node* Create(node* parent, const std::string& name, std::recursive_mutex* lock,
                        NodeTracker* tracker) {
        // Place the entire constructor under a critical section to make sure
        // node creation, tracking (if enabled) and the addition to a parent are
        // atomic.
        std::lock_guard<std::recursive_mutex> guard(*lock);
        return new node(parent, name, lock, tracker);
    }

    // Creates a new root node. Root nodes have no parents by definition
    // and their "name" must signify an absolute path.
    static node* CreateRoot(const std::string& path, std::recursive_mutex* lock,
                            NodeTracker* tracker) {
        std::lock_guard<std::recursive_mutex> guard(*lock);
        node* root = new node(nullptr, path, lock, tracker);

        // The root always has one extra reference to avoid it being
        // accidentally collected.
        root->Acquire();
        return root;
    }

    // Maps an inode to its associated node.
    static inline node* FromInode(__u64 ino, const NodeTracker* tracker) {
        tracker->CheckTracked(ino);
        return reinterpret_cast<node*>(static_cast<uintptr_t>(ino));
    }

    // Maps a node to its associated inode.
    static __u64 ToInode(node* node) {
        return static_cast<__u64>(reinterpret_cast<uintptr_t>(node));
    }

    // Releases a reference to a node. Returns true iff the refcount dropped to
    // zero as a result of this call to Release, meaning that it's no longer
    // safe to perform any operations on references to this node.
    bool Release(uint32_t count) {
        std::lock_guard<std::recursive_mutex> guard(*lock_);
        if (refcount_ >= count) {
            refcount_ -= count;
            if (refcount_ == 0) {
                delete this;
                return true;
            }
        } else {
            LOG(ERROR) << "Mismatched reference count: refcount_ = " << this->refcount_
                       << " ,count = " << count;
        }

        return false;
    }

    // Builds the full path associated with this node, including all path segments
    // associated with its descendants.
    std::string BuildPath() const;

    // Builds the full PII safe path associated with this node, including all path segments
    // associated with its descendants.
    std::string BuildSafePath() const;

    // Looks up a direct descendant of this node by name. If |acquire| is true,
    // also Acquire the node before returning a reference to it.
    node* LookupChildByName(const std::string& name, bool acquire) const {
        std::lock_guard<std::recursive_mutex> guard(*lock_);

        const char* name_char = name.c_str();
        for (node* child : children_) {
            const std::string& child_name = child->GetName();
            if (!strcasecmp(name_char, child_name.c_str()) && !child->deleted_) {
                if (acquire) {
                    child->Acquire();
                }

                return child;
            }
        }
        return nullptr;
    }

    // Marks this node as deleted. It is still associated with its parent, and
    // all open handles etc. to this node are preserved until its refcount goes
    // to zero.
    void SetDeleted() {
        std::lock_guard<std::recursive_mutex> guard(*lock_);

        deleted_ = true;
    }

    void Rename(const std::string& name, node* new_parent) {
        std::lock_guard<std::recursive_mutex> guard(*lock_);

        name_ = name;
        if (new_parent != parent_) {
            RemoveFromParent();
            AddToParent(new_parent);
        }
    }

    const std::string& GetName() const {
        std::lock_guard<std::recursive_mutex> guard(*lock_);
        return name_;
    }

    node* GetParent() const {
        std::lock_guard<std::recursive_mutex> guard(*lock_);
        return parent_;
    }

    inline void AddHandle(handle* h) {
        std::lock_guard<std::recursive_mutex> guard(*lock_);
        handles_.emplace_back(std::unique_ptr<handle>(h));
    }

    void DestroyHandle(handle* h) {
        std::lock_guard<std::recursive_mutex> guard(*lock_);

        auto comp = [h](const std::unique_ptr<handle>& ptr) { return ptr.get() == h; };
        auto it = std::find_if(handles_.begin(), handles_.end(), comp);
        CHECK(it != handles_.end());
        handles_.erase(it);
    }

    bool HasCachedHandle() const {
        std::lock_guard<std::recursive_mutex> guard(*lock_);

        for (const auto& handle : handles_) {
            if (handle->cached) {
                return true;
            }
        }
        return false;
    }

    inline void AddDirHandle(dirhandle* d) {
        std::lock_guard<std::recursive_mutex> guard(*lock_);

        dirhandles_.emplace_back(std::unique_ptr<dirhandle>(d));
    }

    void DestroyDirHandle(dirhandle* d) {
        std::lock_guard<std::recursive_mutex> guard(*lock_);

        auto comp = [d](const std::unique_ptr<dirhandle>& ptr) { return ptr.get() == d; };
        auto it = std::find_if(dirhandles_.begin(), dirhandles_.end(), comp);
        CHECK(it != dirhandles_.end());
        dirhandles_.erase(it);
    }

    // Deletes the tree of nodes rooted at |tree|.
    static void DeleteTree(node* tree);

    // Looks up an absolute path rooted at |root|, or nullptr if no such path
    // through the hierarchy exists.
    static const node* LookupAbsolutePath(const node* root, const std::string& absolute_path);

  private:
    node(node* parent, const std::string& name, std::recursive_mutex* lock, NodeTracker* tracker)
        : name_(name),
          refcount_(0),
          parent_(nullptr),
          deleted_(false),
          lock_(lock),
          tracker_(tracker) {
        tracker_->NodeCreated(this);
        Acquire();
        // This is a special case for the root node. All other nodes will have a
        // non-null parent.
        if (parent != nullptr) {
            AddToParent(parent);
        }
    }

    // Acquires a reference to a node. This maps to the "lookup count" specified
    // by the FUSE documentation and must only happen under the circumstances
    // documented in libfuse/include/fuse_lowlevel.h.
    inline void Acquire() {
        std::lock_guard<std::recursive_mutex> guard(*lock_);
        refcount_++;
    }

    // Adds this node to a specified parent.
    void AddToParent(node* parent) {
        std::lock_guard<std::recursive_mutex> guard(*lock_);
        // This method assumes this node is currently unparented.
        CHECK(parent_ == nullptr);
        // Check that the new parent isn't nullptr either.
        CHECK(parent != nullptr);

        parent_ = parent;
        parent_->children_.push_back(this);

        // TODO(narayan, zezeozue): It's unclear why we need to call Acquire on the
        // parent node when we're adding a child to it.
        parent_->Acquire();
    }

    // Removes this node from its current parent, and set its parent to nullptr.
    void RemoveFromParent() {
        std::lock_guard<std::recursive_mutex> guard(*lock_);

        if (parent_ != nullptr) {
            std::list<node*>& children = parent_->children_;
            std::list<node*>::iterator it = std::find(children.begin(), children.end(), this);

            CHECK(it != children.end());
            children.erase(it);
            parent_->Release(1);
            parent_ = nullptr;
        }
    }

    // A helper function to recursively construct the absolute path of a given node.
    // If |safe| is true, builds a PII safe path instead
    void BuildPathForNodeRecursive(bool safe, const node* node, std::stringstream* path) const;

    // The name of this node. Non-const because it can change during renames.
    std::string name_;
    // The reference count for this node. Guarded by |lock_|.
    uint32_t refcount_;
    // List of children of this node. All of them contain a back reference
    // to their parent. Guarded by |lock_|.
    std::list<node*> children_;
    // Containing directory for this node. Guarded by |lock_|.
    node* parent_;
    // List of file handles associated with this node. Guarded by |lock_|.
    std::vector<std::unique_ptr<handle>> handles_;
    // List of directory handles associated with this node. Guarded by |lock_|.
    std::vector<std::unique_ptr<dirhandle>> dirhandles_;
    bool deleted_;
    std::recursive_mutex* lock_;

    NodeTracker* const tracker_;

    ~node() {
        RemoveFromParent();

        handles_.clear();
        dirhandles_.clear();

        tracker_->NodeDeleted(this);
    }

    friend class ::NodeTest;
};

}  // namespace fuse
}  // namespace mediaprovider

#endif  // MEDIA_PROVIDER_JNI_MODE_INL_H_
