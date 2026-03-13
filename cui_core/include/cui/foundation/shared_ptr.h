#pragma once
/// @file foundation/shared_ptr.h
/// @brief `cui_core::SharedPtr<T>` —— boost::local_shared_ptr 的本仓移植。
///
/// 与 `std::shared_ptr` 的唯一区别：**引用计数是非原子的**。控件树 / 元素树 /
/// 渲染树里的共享所有权每帧成生成百次地拷贝与析构，原子加减在 ARM/x86 上是
/// 这条热路径上最贵的指令之一；boost 的结论（也是本文件的依据）是：只要所有
/// 权始终在同一个线程里流动，非原子计数就是正确且更快的选择。
///
/// ── 成立前提（本仓的线程模型）────────────────────────────────────────────
/// GUI 是单线程的：整棵 Widget/Element/RenderObject 树只在 GUI 线程被读写。
///
/// ── 铁律：与线程交互的地方一律用 std::shared_ptr ──────────────────────────
/// 所有权真的跨线程流动的几处，**保持 `std::shared_ptr`（原子计数）**，不在迁
/// 移范围内：
///   - `utils/future.h`   —— Future<T>::State 由线程池与 GUI 线程共享；
///   - `utils/task.h`     —— 协程帧的恢复可能发生在任意线程；
///   - `utils/thread_pool.h` / `utils/lock_free_queue.h`；
///   - `widgets/async_builder.h` —— scope_/task_ 跟着协程跨线程；
///   - `foundation/application.h` 的 `runAsync()` 与 `postTask()` 投递的闭包。
/// 由此推出两条写代码时的硬约束：
///   1. 投递给线程池 / 事件循环的闭包**不得捕获 `SharedPtr`**（拷贝构造会在非
///      GUI 线程动同一个计数）。要跨线程传递的东西用值类型、裸指针，或
///      `std::shared_ptr`；
///   2. 后台线程的 work 回调里不得碰任何 GUI 对象（含 `SharedPtr` 成员）。
///      结果只能通过 `runAsync` 的 onDone（GUI 线程）落地。
/// `SharedPtr` 与 `std::shared_ptr` **故意不提供互相转换**：两种计数不能共用
/// 一个控制块，一旦允许隐式转换，上面那条边界就会在某个调用点悄悄失效。
///
/// ── 覆盖面 ────────────────────────────────────────────────────────────────
/// 按本仓实际用法实现（无 weak_ptr、无 `shared_ptr<T[]>`、无自定义 deleter、
/// 无 `dynamic_pointer_cast`）：默认/空构造、裸指针构造（含派生→基类、含
/// `SharedPtr<void>`）、拷贝/移动及其跨类型版本、`reset()`、`get()`、`*`/`->`
///（void 时 SFINAE 掉）、`explicit operator bool`、`==`/`!=`、`swap`、
/// `make_shared`（对象与控制块一次分配）、`static_pointer_cast`、
/// `EnableSharedFromThis`。

#include <cassert>
#include <cstddef>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

namespace cui {

namespace detail {

/// 控制块。只有强计数：本仓 0 处 weak_ptr，于是 boost 里那份「weak 计数仍用
/// 原子」的复杂度可以整段省掉——这也是本类型不跨线程的必然结果。
struct SharedPtrCtrl {
    explicit SharedPtrCtrl(long c) noexcept : count(c) {}
    virtual ~SharedPtrCtrl() = default;
    /// 析构所指对象（不释放控制块）。
    virtual void destroyObject() noexcept = 0;
    /// 释放控制块自身。
    virtual void deallocate() noexcept = 0;

    long count;
};

/// 分别分配（`SharedPtr<T>(new T(...))`）：对象在控制块之外。
/// 模板参数 U 是**实际**对象类型，于是删除走正确类型（基类无虚析构也不 UB）。
template <class U>
struct SeparateCtrl final : SharedPtrCtrl {
    explicit SeparateCtrl(U* p) noexcept : SharedPtrCtrl(1), object(p) {}
    void destroyObject() noexcept override { delete object; }
    void deallocate() noexcept override { delete this; }

    U* object;
};

/// 合并分配（`make_shared`）：对象嵌在控制块里，一次 new 搞定 —— 这是本次迁移
/// 的主要收益点（少一次分配，缓存也更近）。
///
/// 参数转发必须用 `std::forward<Args>` 而不是 `std::move(as)`：Args 里存的是
/// 左值引用时（`make_shared<X>(someLog)` 配 `X(Log&)`），`std::move` 会把它变成
/// 右值，非 const 左值引用的构造函数就绑不上（MSVC C2665）。
///
/// **计数从 0 起**：`make_shared` 用 `withControlBlock` 造返回值，那里 +1 就是
/// 唯一的所有者引用。若这里也从 1 起，每个 make_shared 出来的对象都会多一个
/// 永不归零的引用 —— 析构不跑、内存泄漏（`test_drag_target` §13 靠
/// `~DragAvatar` 摘 OverlayEntry，最先暴露了这条）。
/// `SeparateCtrl` 不同：`SharedPtr(U*)` 自己不再 +1，所以那里起始是 1。
template <class T, class... Args>
struct CombinedCtrl final : SharedPtrCtrl {
    template <class... U>
    explicit CombinedCtrl(U&&... a) : SharedPtrCtrl(0), args(std::forward<U>(a)...) {
        std::apply([this](Args&... as) { new (storage) T(std::forward<Args>(as)...); }, args);
    }

    void destroyObject() noexcept override { reinterpret_cast<T*>(storage)->~T(); }
    void deallocate() noexcept override { delete this; }
    T* object() noexcept { return reinterpret_cast<T*>(storage); }

    std::tuple<Args...> args;
    alignas(alignof(T)) std::byte storage[sizeof(T)];
};

/// `EnableSharedFromThis` 的擦除类型基类：控制块创建时把句柄写进这里，
/// `shared_from_this()` 再从它造出共享同一个计数的新引用。
struct EnableSharedFromThisBase {
    mutable SharedPtrCtrl* sharedFromThisCtrl = nullptr;
};

/// 仅当 T 确实派生自 EnableSharedFromThis 时才登记（否则空操作）。
template <class T>
inline void registerEnableSharedFromThis(T* p, SharedPtrCtrl* ctrl) {
    if constexpr (std::is_base_of_v<EnableSharedFromThisBase, std::remove_const_t<T>>) {
        if (p)
            p->sharedFromThisCtrl = ctrl;
    }
}

}  // namespace detail

/// 非原子引用计数的共享所有权指针（单线程版 shared_ptr）。见文件头。
template <class T>
class SharedPtr {
public:
    using element_type = T;

    SharedPtr() noexcept = default;
    SharedPtr(std::nullptr_t) noexcept {}

    /// 接管一块裸指针（计数 1）。U 可以是 T 的派生类，也可以是 `T = void`。
    template <class U, class = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    explicit SharedPtr(U* p) : ptr_(p), ctrl_(p ? new detail::SeparateCtrl<U>(p) : nullptr) {
        detail::registerEnableSharedFromThis(ptr_, ctrl_);
    }

    SharedPtr(const SharedPtr& o) noexcept : ptr_(o.ptr_), ctrl_(o.ctrl_) { retain(); }

    SharedPtr(SharedPtr&& o) noexcept : ptr_(o.ptr_), ctrl_(o.ctrl_) {
        o.ptr_ = nullptr;
        o.ctrl_ = nullptr;
    }

    /// 跨类型拷贝（派生→基类、任意→void、T→const T）。
    template <class U, class = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    SharedPtr(const SharedPtr<U>& o) noexcept : ptr_(o.ptr_), ctrl_(o.ctrl_) {
        retain();
    }

    template <class U, class = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    SharedPtr(SharedPtr<U>&& o) noexcept : ptr_(o.ptr_), ctrl_(o.ctrl_) {
        o.ptr_ = nullptr;
        o.ctrl_ = nullptr;
    }

    ~SharedPtr() { release(); }

    SharedPtr& operator=(const SharedPtr& o) noexcept {
        if (ctrl_ != o.ctrl_ || ptr_ != o.ptr_) {
            release();
            ptr_ = o.ptr_;
            ctrl_ = o.ctrl_;
            retain();
        }
        return *this;
    }

    SharedPtr& operator=(SharedPtr&& o) noexcept {
        if (this != &o) {
            release();
            ptr_ = o.ptr_;
            ctrl_ = o.ctrl_;
            o.ptr_ = nullptr;
            o.ctrl_ = nullptr;
        }
        return *this;
    }

    template <class U, class = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    SharedPtr& operator=(const SharedPtr<U>& o) noexcept {
        if (ctrl_ != o.ctrl_ || ptr_ != o.ptr_) {
            release();
            ptr_ = o.ptr_;
            ctrl_ = o.ctrl_;
            retain();
        }
        return *this;
    }

    template <class U, class = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    SharedPtr& operator=(SharedPtr<U>&& o) noexcept {
        release();
        ptr_ = o.ptr_;
        ctrl_ = o.ctrl_;
        o.ptr_ = nullptr;
        o.ctrl_ = nullptr;
        return *this;
    }

    SharedPtr& operator=(std::nullptr_t) noexcept {
        release();
        return *this;
    }

    void reset() noexcept { release(); }

    template <class U, class = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    void reset(U* p) {
        SharedPtr(p).swap(*this);
    }

    void swap(SharedPtr& o) noexcept {
        std::swap(ptr_, o.ptr_);
        std::swap(ctrl_, o.ctrl_);
    }

    T* get() const noexcept { return ptr_; }

    template <class X = T, class = std::enable_if_t<!std::is_void_v<X>>>
    X& operator*() const noexcept {
        return *ptr_;
    }

    template <class X = T, class = std::enable_if_t<!std::is_void_v<X>>>
    X* operator->() const noexcept {
        return ptr_;
    }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    /// 与既有控制块共享所有权（计数 +1）。给 `static_pointer_cast` 与
    /// `EnableSharedFromThis::shared_from_this()` 用，业务代码请走那两个入口。
    static SharedPtr withControlBlock(T* p, detail::SharedPtrCtrl* ctrl) noexcept {
        SharedPtr r;
        r.ptr_ = p;
        r.ctrl_ = ctrl;
        r.retain();
        return r;
    }

    /// 控制块句柄（同上，只服务于那两个入口）。
    detail::SharedPtrCtrl* sharedPtrCtrl() const noexcept { return ctrl_; }

private:
    /// 跨类型构造/赋值（派生→基类、任意→void、T→const T）要直接读写
    /// 另一个特例化（`SharedPtr<U>`）的 ptr_/ctrl_，因此必须互相开放 ——
    /// 与 std::shared_ptr 的 `template<class U> friend class shared_ptr;` 同源。
    template <class U>
    friend class SharedPtr;

    void retain() noexcept {
        if (ctrl_)
            ++ctrl_->count;
    }

    void release() noexcept {
        if (ctrl_ && --ctrl_->count == 0) {
            ctrl_->destroyObject();
            ctrl_->deallocate();
        }
        ptr_ = nullptr;
        ctrl_ = nullptr;
    }

    T* ptr_ = nullptr;
    detail::SharedPtrCtrl* ctrl_ = nullptr;
};

/// 一次分配造出对象 + 控制块（= boost::make_local_shared）。
template <class T, class... Args>
SharedPtr<T> make_shared(Args&&... args) {
    auto* ctrl = new detail::CombinedCtrl<T, Args...>(std::forward<Args>(args)...);
    T* obj = ctrl->object();
    detail::registerEnableSharedFromThis(obj, ctrl);
    return SharedPtr<T>::withControlBlock(obj, ctrl);
}

/// 指针型别转换（不另起计数）。上游 `std::static_pointer_cast` 的对应物。
template <class T, class U>
SharedPtr<T> static_pointer_cast(const SharedPtr<U>& o) {
    return SharedPtr<T>::withControlBlock(static_cast<T*>(o.get()), o.sharedPtrCtrl());
}

template <class T, class U>
SharedPtr<T> static_pointer_cast(SharedPtr<U>&& o) {
    return SharedPtr<T>::withControlBlock(static_cast<T*>(o.get()), o.sharedPtrCtrl());
}

template <class T, class U>
bool operator==(const SharedPtr<T>& a, const SharedPtr<U>& b) noexcept {
    return a.get() == b.get();
}

template <class T, class U>
bool operator!=(const SharedPtr<T>& a, const SharedPtr<U>& b) noexcept {
    return a.get() != b.get();
}

template <class T>
bool operator==(const SharedPtr<T>& a, std::nullptr_t) noexcept {
    return a.get() == nullptr;
}

template <class T>
bool operator!=(const SharedPtr<T>& a, std::nullptr_t) noexcept {
    return a.get() != nullptr;
}

/// `std::enable_shared_from_this` 的非原子对应物。
///
/// 前置条件与 std 相同：对象此刻必须已被某个 `SharedPtr` 持有，否则
/// `shared_from_this()` 断言失败（std 在这里抛 bad_weak_ptr）。
template <class T>
class EnableSharedFromThis : public detail::EnableSharedFromThisBase {
protected:
    EnableSharedFromThis() noexcept = default;
    EnableSharedFromThis(const EnableSharedFromThis&) noexcept = default;
    EnableSharedFromThis& operator=(const EnableSharedFromThis&) noexcept = default;
    ~EnableSharedFromThis() = default;

public:
    SharedPtr<T> shared_from_this() {
        assert(this->sharedFromThisCtrl && "shared_from_this(): 对象不被任何 SharedPtr 持有");
        return SharedPtr<T>::withControlBlock(static_cast<T*>(this), this->sharedFromThisCtrl);
    }

    SharedPtr<const T> shared_from_this() const {
        assert(this->sharedFromThisCtrl && "shared_from_this(): 对象不被任何 SharedPtr 持有");
        return SharedPtr<const T>::withControlBlock(static_cast<const T*>(this),
                                                    this->sharedFromThisCtrl);
    }
};

}  // namespace cui_core
