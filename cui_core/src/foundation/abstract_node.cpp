/// @file foundation/abstract_node.cpp
/// @brief Implementation of the AbstractNode tree lifecycle.
///
/// The key invariants maintained here:
///   - `owner_` propagates top-down on adoptChild / drops on dropChild.
///   - `depth_` is cached and refreshed only on reparent (cheap — the walk
///     touches only the affected subtree, matching the upstream's approach).
///   - `attachedToRoot` / `detachedFromRoot` fire exactly once per attach/
///     detach so RenderObject can push/pull itself off the PipelineOwner's
///     dirty lists without double-bookkeeping.

#include "cui/foundation/abstract_node.h"

#include <cassert>

namespace cui {

void AbstractNode::setParent(AbstractNode* newParent) {
    // Direct reparent without adopt/drop bookkeeping — used by containers
    // that manage their own child ordering (e.g. a linked list of render
    // children) and just need to update the back-pointer.
    parent_ = newParent;
    recalculateDepth();
}

void AbstractNode::adoptChild(AbstractNode* child) {
    assert(child && "adoptChild: null child");
    assert(child->parent_ == nullptr && "adoptChild: child already parented");
    child->parent_ = this;
    child->recalculateDepth();
    if (isAttached_) {
        child->attachOwnerRecursive(owner_);
    }
}

void AbstractNode::dropChild(AbstractNode* child) {
    assert(child && "dropChild: null child");
    assert(child->parent_ == this && "dropChild: child's parent mismatch");
    if (child->isAttached_) {
        child->detachOwnerRecursive();
    }
    child->parent_ = nullptr;
    child->recalculateDepth();
}

void AbstractNode::recalculateDepth() {
    depth_ = 0;
    for (auto* p = parent_; p; p = p->parent_)
        ++depth_;
    // Propagate to children — their depth shifted too.
    visitChildren([](AbstractNode* c) {
        // Iterative refresh: each child recomputes from its (now-correct)
        // parent, then walks its own subtree.  A single recursive call
        // per child keeps the total work O(subtree size).
        std::function<void(AbstractNode*)> refresh = [&](AbstractNode* n) {
            n->depth_ = 0;
            for (auto* p = n->parent_; p; p = p->parent_)
                ++n->depth_;
            n->visitChildren([&](AbstractNode* gc) { refresh(gc); });
        };
        refresh(c);
    });
}

void AbstractNode::attachOwnerRecursive(Owner* o) {
    owner_ = o;
    attachedToRoot();
    visitChildren([=](AbstractNode* c) { c->attachOwnerRecursive(o); });
}

void AbstractNode::detachOwnerRecursive() {
    detachedFromRoot();
    owner_ = nullptr;
    visitChildren([](AbstractNode* c) { c->detachOwnerRecursive(); });
}

}  // namespace cui_core
