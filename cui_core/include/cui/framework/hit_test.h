#pragma once
/// @file framework/hit_test.h
/// @brief HitTestResult — the result of a hit test traversal.
///
/// In 上游, hit testing produces a list of HitTestEntry objects, each
/// containing the RenderObject that was hit and the transform from global
/// to that object's local coordinates.  The list is ordered from deepest
/// (most specific) to shallowest (root).
///
/// This file defines:
///   - HitTestEntry: a single hit (RenderObject + transform)
///   - HitTestResult: the ordered list of hits

#include <vector>

#include "cui/foundation/geometry.h"

namespace cui {

class RenderObject;

/// A single hit test entry: the RenderObject that was hit, plus the
/// transform from global coordinates to that object's local coordinates.
struct HitTestEntry {
    RenderObject* target = nullptr;
    Offset offset;  // global-to-local offset

    HitTestEntry() = default;
    HitTestEntry(RenderObject* t, const Offset& o) : target(t), offset(o) {}
};

/// The result of a hit test traversal.  Contains an ordered list of
/// HitTestEntry objects, from deepest (most specific) to shallowest.
class HitTestResult {
public:
    HitTestResult() = default;

    /// Add a hit entry.  Entries are added in order from deepest to shallowest.
    void add(const HitTestEntry& entry) { entries_.push_back(entry); }

    /// The list of hit entries.
    const std::vector<HitTestEntry>& entries() const { return entries_; }

    /// Clear all entries.
    void clear() { entries_.clear(); }

    /// True if there are no hits.
    bool empty() const { return entries_.empty(); }

    /// The deepest (most specific) hit, or nullptr if no hits.
    RenderObject* deepestTarget() const {
        return entries_.empty() ? nullptr : entries_.back().target;
    }

private:
    std::vector<HitTestEntry> entries_;
};

}  // namespace cui_core
