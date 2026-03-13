#pragma once
/// @file foundation/key.h
/// @brief Widget identity keys, mirroring the upstream `Key` family.
///
/// A Key gives a Widget a stable identity so `Element::updateChild` can tell
/// whether an existing Element/RenderObject can be reused across rebuilds
/// instead of being torn down and recreated.  Three flavours are modelled:
///   - `ValueKey<T>`   : a leaf identity compared by value.
///   - `GlobalKey`     : a process-unique identity that also exposes the
///                       mounted `Element` / `State` (used by overlays, focus
///                       rebparenting, and tests that reach into a subtree).
///   - `LocalKey`      : base for library-private identity (e.g. a built-in
///                       icon slot) that must never collide across subtrees.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include "cui/foundation/shared_ptr.h"

namespace cui {

/// Base of the identity hierarchy.  Two keys are equal only when they share
/// a concrete type AND compare equal; the `shortToString()` is diagnostic
/// only (kept so failing assertions name the widget, as in the upstream).
class Key {
public:
    virtual ~Key() = default;
    virtual bool operator==(const Key& other) const = 0;
    virtual std::string toString() const = 0;
};

using KeyPtr = SharedPtr<const Key>;

/// A key identified by an arbitrary hashable value.  `==` requires both the
/// same runtime type and equal stored value, mirroring the upstream ValueKey.
template <typename T>
class ValueKey : public Key {
public:
    explicit ValueKey(T value) : value_(std::move(value)) {}
    const T& value() const { return value_; }

    bool operator==(const Key& other) const override {
        auto* o = dynamic_cast<const ValueKey<T>*>(&other);
        return o && o->value_ == value_;
    }
    std::string toString() const override { return "ValueKey"; }

private:
    T value_;
};

/// Marker for keys that must never match across different parent subtrees —
/// a `LocalKey` compares by object identity only.
class LocalKey : public Key {
public:
    bool operator==(const Key& other) const override { return this == &other; }
    std::string toString() const override { return "LocalKey"; }
};

/// A globally-unique key that tracks its mounted Element.  At most one
/// Element may hold a given GlobalKey in the tree at a time (上游 asserts
/// this); `currentElement()` is how code reaches a specific subtree, and the
/// owner registry enforces the single-mount invariant.
///
/// Element 槽位说明：本工程存在两套 Element 体系（旧三树桥的 cui_core::Element
/// 与标准三树的 cui_core::Element），GlobalKey 作为 foundation 层类型
/// 不能依赖任何一个，因此槽位以 void* 存储、由调用方通过模板访问器取回
/// 具体类型（与 上游的 GlobalKey<T extends State> 泛型思想一致）。
class GlobalKey : public Key {
public:
    GlobalKey() = default;
    explicit GlobalKey(std::string debugLabel) : debugLabel_(std::move(debugLabel)) {}

    bool operator==(const Key& other) const override { return this == &other; }
    std::string toString() const override {
        return debugLabel_.empty() ? "GlobalKey" : ("GlobalKey(" + debugLabel_ + ")");
    }

    /// 当前挂载在该 key 上的 Element（具体类型），或 null。
    template <typename E>
    E* currentElement() const {
        return static_cast<E*>(currentElement_);
    }
    /// 由 Element 生命周期在挂载 / 卸载时调用（上游的 _registerGlobalKey）。
    void setCurrentElement(void* e) { currentElement_ = e; }

    const std::string& debugLabel() const { return debugLabel_; }

    /// 进程内唯一 id（上游的 GlobalKey 稳定标识）。
    static uint64_t nextId();

private:
    std::string debugLabel_;
    void* currentElement_ = nullptr;  // mutable-by-key, not owned
};

}  // namespace cui_core
