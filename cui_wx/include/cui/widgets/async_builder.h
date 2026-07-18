#pragma once
/// @file widgets/async_builder.h
/// @brief 协程驱动的异步构建控件。
///
/// 语义对齐上游的 `FutureBuilder` / `StreamBuilder`，但**不用回调式 builder**，
/// 而是用仓库既有的 C++20 协程（`utils/task.h` 的 `Task<T>`）表达异步流程：
///
///   auto w = cui_core::make_shared<AsyncBuilder<std::string>>(
///       [](AsyncScope<std::string>& scope) -> Task<void> {
///           auto text = co_await loadTextAsync();
///           scope.setData(std::move(text));
///       },
///       [](BuildContext& ctx, const AsyncSnapshot<std::string>& snap) {
///           return cui_core::make_shared<Text>(snap.data);
///       });
///
/// 流的写法（对齐 StreamBuilder）：
///
///   auto w = cui_core::make_shared<AsyncStreamBuilder<int>>(
///       [](AsyncScope<int>& scope, Stream<int>& stream) -> Task<void> {
///           while (auto v = co_await stream.next())
///               scope.setData(*v);
///       },
///       [](BuildContext& ctx, const AsyncSnapshot<int>& snap) { … });

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/framework/framework.h"
#include "cui/utils/future.h"
#include "cui/utils/task.h"

namespace cui {

// ── AsyncStatus / AsyncSnapshot ─────────────────────────────────────────────

/// 异步状态（对齐上游的 `ConnectionState`）。
enum class AsyncStatus {
    None,    ///< 尚未开始
    Loading, ///< 进行中
    Done,    ///< 已完成（可能带错误）
};

/// 异步快照（对齐上游的 `AsyncSnapshot<T>`）。
template <typename T>
struct AsyncSnapshot {
    AsyncStatus status = AsyncStatus::None;
    T data{};
    std::exception_ptr error;

    bool hasData() const { return status == AsyncStatus::Done && !error; }
    bool hasError() const { return error != nullptr; }
    bool isLoading() const { return status == AsyncStatus::Loading; }
};

// ── AsyncScope<T> ───────────────────────────────────────────────────────────

/// 协程与控件之间的桥：协程通过它上报状态，控件据此重建。
template <typename T>
class AsyncScope {
public:
    explicit AsyncScope(std::function<void()> onChanged)
        : onChanged_(std::move(onChanged)) {}

    /// 上报数据（触发控件重建）。
    void setData(T value) {
        snapshot_.status = AsyncStatus::Done;
        snapshot_.data = std::move(value);
        snapshot_.error = nullptr;
        notify();
    }

    /// 上报错误。
    void setError(std::exception_ptr e) {
        snapshot_.status = AsyncStatus::Done;
        snapshot_.error = std::move(e);
        notify();
    }

    /// 上报「进行中」。
    void setLoading() {
        snapshot_.status = AsyncStatus::Loading;
        notify();
    }

    const AsyncSnapshot<T>& snapshot() const { return snapshot_; }

    /// 控件已销毁时为 true —— 长耗时协程应据此提前退出。
    bool isCancelled() const { return cancelled_; }

    /// 标记取消（由控件在 dispose 时调用）。
    void cancel() { cancelled_ = true; }

private:
    void notify() {
        if (onChanged_)
            onChanged_();
    }

    std::function<void()> onChanged_;
    AsyncSnapshot<T> snapshot_;
    bool cancelled_ = false;
};

// ── AsyncBuilderState<T> ────────────────────────────────────────────────────

/// AsyncBuilder 的 State（先于 Widget 定义，createState 需要完整类型）。
template <typename T>
class AsyncBuilderState : public State {
public:
    void initState() override;
    void dispose() override;
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    std::shared_ptr<AsyncScope<T>> scope_;
    std::shared_ptr<Task<void>> task_;
};

// ── AsyncBuilder<T> ─────────────────────────────────────────────────────────

/// 协程驱动的异步构建控件（对齐上游 `FutureBuilder`）。
///
/// loader 是一次性的协程：它在控件挂载时启动，结果通过 scope 上报；
/// builder 用当前快照构建子树。
template <typename T>
class AsyncBuilder : public StatefulWidget {
public:
    using Loader = std::function<Task<void>(AsyncScope<T>&)>;
    using Builder =
        std::function<SharedPtr<Widget>(BuildContext&, const AsyncSnapshot<T>&)>;

    AsyncBuilder(Loader loader, Builder builder, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), loader_(std::move(loader)),
          builder_(std::move(builder)) {}

    const Loader& loader() const { return loader_; }
    const Builder& builder() const { return builder_; }

    std::unique_ptr<State> createState() override {
        return std::make_unique<AsyncBuilderState<T>>();
    }

private:
    Loader loader_;
    Builder builder_;
};

// ── AsyncStreamBuilderState<T> ──────────────────────────────────────────────

/// AsyncStreamBuilder 的 State。
template <typename T>
class AsyncStreamBuilderState : public State {
public:
    void initState() override;
    void dispose() override;
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    std::shared_ptr<AsyncScope<T>> scope_;
    std::shared_ptr<Stream<T>> stream_;
    std::shared_ptr<Task<void>> task_;
};

// ── AsyncStreamBuilder<T> ───────────────────────────────────────────────────

/// 协程驱动的流式构建控件（对齐上游 `StreamBuilder`）。
///
/// loader 拿到 scope 与一个 Stream：在协程里 `co_await stream.next()` 逐项
/// 消费，每收到一项就 scope.setData()，控件随之重建。
template <typename T>
class AsyncStreamBuilder : public StatefulWidget {
public:
    using Loader = std::function<Task<void>(AsyncScope<T>&, Stream<T>&)>;
    using Builder =
        std::function<SharedPtr<Widget>(BuildContext&, const AsyncSnapshot<T>&)>;

    AsyncStreamBuilder(Loader loader, Builder builder, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), loader_(std::move(loader)),
          builder_(std::move(builder)) {}

    const Loader& loader() const { return loader_; }
    const Builder& builder() const { return builder_; }

    std::unique_ptr<State> createState() override {
        return std::make_unique<AsyncStreamBuilderState<T>>();
    }

private:
    Loader loader_;
    Builder builder_;
};

// ── 实现 ────────────────────────────────────────────────────────────────────

template <typename T>
void AsyncBuilderState<T>::initState() {
    scope_ = std::make_shared<AsyncScope<T>>([this] {
        if (mounted())
            setState();
    });
    scope_->setLoading();

    auto* w = static_cast<AsyncBuilder<T>*>(widget());
    if (w && w->loader()) {
        // 惰性启动的协程：显式 start()，让协程体在 GUI 线程执行。
        task_ = std::make_shared<Task<void>>(w->loader()(*scope_));
        task_->start();
    }
}

template <typename T>
void AsyncBuilderState<T>::dispose() {
    if (scope_)
        scope_->cancel();
    task_.reset();  // 释放协程帧
}

template <typename T>
SharedPtr<Widget> AsyncBuilderState<T>::build(BuildContext& context) {
    auto* w = static_cast<AsyncBuilder<T>*>(widget());
    if (!w || !w->builder() || !scope_)
        return nullptr;
    return w->builder()(context, scope_->snapshot());
}

template <typename T>
void AsyncStreamBuilderState<T>::initState() {
    scope_ = std::make_shared<AsyncScope<T>>([this] {
        if (mounted())
            setState();
    });
    scope_->setLoading();

    stream_ = std::make_shared<Stream<T>>();
    auto* w = static_cast<AsyncStreamBuilder<T>*>(widget());
    if (w && w->loader()) {
        task_ = std::make_shared<Task<void>>(w->loader()(*scope_, *stream_));
        task_->start();
    }
}

template <typename T>
void AsyncStreamBuilderState<T>::dispose() {
    if (scope_)
        scope_->cancel();
    if (stream_)
        stream_->close();
    task_.reset();
}

template <typename T>
SharedPtr<Widget> AsyncStreamBuilderState<T>::build(BuildContext& context) {
    auto* w = static_cast<AsyncStreamBuilder<T>*>(widget());
    if (!w || !w->builder() || !scope_)
        return nullptr;
    return w->builder()(context, scope_->snapshot());
}

}  // namespace cui_core
