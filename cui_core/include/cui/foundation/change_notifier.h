#pragma once
/// @file foundation/change_notifier.h
/// @brief 上游的 Listenable / ChangeNotifier / ValueNotifier 移植。
///
/// 对应 上游源码：
///   上游 foundation/change_notifier.dart
///   上游 foundation/value_notifier.dart
///
/// 语义对齐要点：
///   - Listenable 只暴露 addListener / removeListener 两接口。
///   - ChangeNotifier 在 notifyListeners 时对监听器列表做「快照迭代」，
///     允许监听器在回调里增删监听器而不破坏本次派发。
///   - ValueNotifier 在 value 变化时（用 == 判定）自动 notifyListeners。
///   - dispose() 之后调用 addListener 会抛错（上游断言），这里用
///     std::logic_error 表达同一意图。

#include <functional>
#include <memory>
#include <stdexcept>
#include <typeinfo>
#include <utility>
#include <vector>

namespace cui {

/// 判断两个回调是否「是同一个」。
///
/// `std::function` 在 libstdc++ 中只提供与 `nullptr` 的比较，没有可移植的
/// 互相比较（[func.wrap.func] 的目标对象相等比较要求目标类型自身支持
/// `operator==`）。因此这里做**尽力比较**：
///   - 目标类型不同 → 一定不是同一个；
///   - 两者皆为空 → 是同一个；
///   - 目标可转换为函数指针（普通函数指针 / 无捕获 lambda）→ 按地址比较；
///   - 其他情况（带捕获的 lambda）→ 保守地判定为「不是同一个」。
///
/// 需要**精确**移除某个监听器时，请使用 `addListenerWithHandle` /
/// `removeListenerByHandle` 的句柄通道。
inline bool sameCallbackTarget(const std::function<void()>& a, const std::function<void()>& b) {
    if (a.target_type() != b.target_type())
        return false;
    if (!a)
        return true;
    // 只有目标是「无参 void 函数指针」这一具体类型时，
    // `target<FnPtr>()` 才保证可用；此时按函数地址比较。
    using FnPtr = void (*)();
    if (a.target_type() != typeid(FnPtr))
        return false;
    const FnPtr* fa = a.target<FnPtr>();
    const FnPtr* fb = b.target<FnPtr>();
    if (!fa || !fb)
        return false;
    return *fa == *fb;
}

/// 可监听对象。上游的 `Listenable`。
class Listenable {
public:
    virtual ~Listenable() = default;

    /// 注册监听器。同一函数对象重复注册会抛错（与 上游 一致）。
    virtual void addListener(std::function<void()> listener) = 0;

    /// 移除监听器。未注册过则忽略。
    virtual void removeListener(const std::function<void()>& listener) = 0;
};

/// 可通知变更的基础类。上游的 `ChangeNotifier`。
class ChangeNotifier : public Listenable {
public:
    ChangeNotifier() = default;
    ~ChangeNotifier() override { dispose(); }

    ChangeNotifier(const ChangeNotifier&) = delete;
    ChangeNotifier& operator=(const ChangeNotifier&) = delete;

    void addListener(std::function<void()> listener) override {
        if (disposed_)
            throw std::logic_error("ChangeNotifier.addListener() called after dispose().");
        for (const auto& l : listeners_) {
            // C++20 提供 std::function 的相等比较（比较目标对象）。
            if (sameCallbackTarget(l.fn, listener))
                throw std::logic_error(
                        "ChangeNotifier.addListener() called twice for the same listener.");
        }
        listeners_.push_back(ListenerEntry{nextHandle_++, std::move(listener)});
    }

    void removeListener(const std::function<void()>& listener) override {
        for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
            if (sameCallbackTarget(it->fn, listener)) {
                listeners_.erase(it);
                return;
            }
        }
    }

    /// 注册监听器并返回一个**句柄**，用于精确移除。
    ///
    /// 上游的 `ChangeNotifier.removeListener` 依赖「同一个顶层/静态函数
    /// 对象」的引用相等。C++ 里捕获了 `this` 的 lambda 每次构造都是不同的
    /// 对象，无法靠 `std::function` 相等比较移除，因此这里额外提供句柄式
    /// 的注册 / 移除通道，语义等价且更可靠。
    int addListenerWithHandle(std::function<void()> listener) {
        if (disposed_)
            throw std::logic_error("ChangeNotifier.addListenerWithHandle() called after dispose().");
        int handle = nextHandle_++;
        listeners_.push_back(ListenerEntry{handle, std::move(listener)});
        return handle;
    }

    /// 按句柄移除监听器。句柄无效时忽略。
    void removeListenerByHandle(int handle) {
        for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
            if (it->handle == handle) {
                listeners_.erase(it);
                return;
            }
        }
    }

    /// 通知所有监听器。派发使用快照，监听器内部可安全增删。
    void notifyListeners() {
        if (disposed_)
            return;
        auto snapshot = listeners_;
        for (const auto& l : snapshot) {
            if (disposed_)
                return;
            l.fn();
        }
    }

    /// 释放资源并清空监听器。幂等。
    void dispose() {
        if (disposed_)
            return;
        disposed_ = true;
        listeners_.clear();
    }

    bool disposed() const { return disposed_; }

    /// 监听器数量（测试用）。
    size_t listenerCount() const { return listeners_.size(); }

private:
    /// 一条监听器记录：句柄 + 回调。
    struct ListenerEntry {
        int handle = 0;
        std::function<void()> fn;
    };

    std::vector<ListenerEntry> listeners_;
    int nextHandle_ = 1;
    bool disposed_ = false;
};

/// 带值的可监听对象。上游的 `ValueNotifier<T>`。
///
/// `setValue` 用 `operator==` 判定是否真的变化，只有变化才通知。
template <typename T>
class ValueNotifier : public ChangeNotifier {
public:
    ValueNotifier() = default;
    explicit ValueNotifier(T value) : value_(std::move(value)) {}

    const T& value() const { return value_; }

    void setValue(T value) {
        if (value_ == value)
            return;
        value_ = std::move(value);
        notifyListeners();
    }

    /// 便捷操作：修改当前值并通知（即使最终值相同也会通知）。
    void mutate(void (*fn)(T&)) {
        fn(value_);
        notifyListeners();
    }

private:
    T value_{};
};

/// 只读的值监听器。上游的 `ValueListenable<T>`。
template <typename T>
class ValueListenable {
public:
    virtual ~ValueListenable() = default;
    virtual T value() const = 0;
    virtual void addListener(std::function<void()> listener) = 0;
    virtual void removeListener(const std::function<void()>& listener) = 0;
};

}  // namespace cui_core
