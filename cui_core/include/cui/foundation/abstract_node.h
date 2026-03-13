#pragma once
/// @file foundation/abstract_node.h
/// @brief Base of the render tree, mirroring the upstream `AbstractNode`.
///
/// Owns the parent link, the cached subtree `depth`, and the attach/detach
/// lifecycle that propagates an `Owner` down the tree.  `RenderObject` extends
/// this and layers on layout/paint dirty bookkeeping.  Tree shape (who the
/// children are) lives in the subclasses: they expose it through
/// `visitChildren`, which is what makes attach/detach/depth recomputation
/// uniform across node kinds.

#include <functional>

namespace cui {

class Owner;  // change-notifier base; PipelineOwner derives from it

/// A node in an owner-managed tree.  Non-copyable, address-stable (identity
/// is the pointer, like 上游).
class AbstractNode {
public:
    virtual ~AbstractNode() = default;

    AbstractNode(const AbstractNode&) = delete;
    AbstractNode& operator=(const AbstractNode&) = delete;

    AbstractNode* parent() const { return parent_; }
    void setParent(AbstractNode* newParent);

    /// Distance from the root (root == 0).  Cached; refreshed on reparent.
    int depth() const { return depth_; }

    /// The owner this node is currently attached to, or null when detached.
    Owner* owner() const { return owner_; }

    /// Insert `child` under `this` and, if this node is attached, propagate
    /// the owner down `child`'s subtree.  Containers call this when wiring a
    /// new RenderObject in; the child ordering itself is the container's job.
    /// Virtual so subclasses (e.g. RenderShiftedBox) can allocate ParentData.
    virtual void adoptChild(AbstractNode* child);
    /// Reverse of `adoptChild`: detach `child`'s subtree from this owner and
    /// clear its parent link.  Does NOT delete the child.
    virtual void dropChild(AbstractNode* child);

    /// Walk this node's direct children (render objects, not widgets).  Base
    /// has none; leaf render objects inherit the no-op, containers override.
    virtual void visitChildren(const std::function<void(AbstractNode*)>& visitor) {
        (void)visitor;
    }

    bool attached() const { return isAttached_; }

    /// Called when the node enters/leaves an attached tree.  RenderObject
    /// overrides these to push itself onto / pull off the owner's dirty lists.
    virtual void attachedToRoot() { isAttached_ = true; }
    virtual void detachedFromRoot() { isAttached_ = false; }

protected:
    AbstractNode() = default;

    void recalculateDepth();

    AbstractNode* parent_ = nullptr;
    int depth_ = 0;
    Owner* owner_ = nullptr;
    bool isAttached_ = false;

private:
    void attachOwnerRecursive(Owner* o);
    void detachOwnerRecursive();

    friend class PipelineOwner;  // setRootNode/clearRootNode need attach access
};

/// Minimal change-notifier stand-in for the upstream's `ChangeNotifier`.  The
/// PipelineOwner derives from Owner; nodes only need to know which owner they
/// belong to, not observe it, so this stays a tagged base.
class Owner {
public:
    virtual ~Owner() = default;
};

}  // namespace cui_core
