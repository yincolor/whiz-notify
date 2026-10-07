#pragma once

// whiz-notify 内部头文件（不导出）：单一事件线程与跨线程任务投递。

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

#include "detail/mpsc_queue.hpp"

namespace whiz::notify::detail {

// 单一事件线程（std::jthread，线程名 "whiz-evt"）。
//
// 事件线程串行执行：
//   1. 排空内部任务队列（send 投递、权限回调等）；
//   2. 调用后端 poll_once() 驱动平台原生事件源；
//   3. 在平台唤醒源上等待最多 50ms（有新任务时被提前唤醒）。
//
// 因此用户回调（on_action / on_closed / on_error）全部在事件线程上执行，
// 满足设计文档 §3.2 的关键不变量。
class event_loop {
  public:
    event_loop();
    ~event_loop();

    event_loop(const event_loop&)            = delete;
    event_loop& operator=(const event_loop&) = delete;

    // 启动事件线程；poll 为平台事件派发钩子（通常调用 backend::poll_once）。
    void start(std::move_only_function<void()> poll);

    // 从任意线程投递任务；空任务静默忽略。
    void post(std::move_only_function<void()> task);

    // 投递任务并阻塞等待结果。若已在事件线程上，则直接同步执行，
    // 保证用户回调内调用 send() 不会死锁。
    template <class F>
    auto submit(F&& f) -> std::invoke_result_t<F> {
        using R = std::invoke_result_t<F>;
        if (on_event_thread()) {
            if constexpr (std::is_void_v<R>) {
                std::invoke(std::forward<F>(f));
            } else {
                return std::invoke(std::forward<F>(f));
            }
        }

        std::promise<R> promise;
        std::future<R> future = promise.get_future();
        post([&promise, f = std::forward<F>(f)]() mutable {
            if constexpr (std::is_void_v<R>) {
                std::invoke(std::move(f));
                promise.set_value();
            } else {
                promise.set_value(std::invoke(std::move(f)));
            }
        });
        return future.get();
    }

    // 当前线程是否为事件线程。
    [[nodiscard]] bool on_event_thread() const noexcept {
        return std::this_thread::get_id() == thread_id_.load(std::memory_order_relaxed);
    }

    // 请求停止事件线程；最多等待 100ms，超时则放弃等待（见设计文档 §5.3）。
    void stop();

  private:
    void run(std::stop_token stop_token);

    struct event_source;

    std::unique_ptr<event_source> wake_;
    mpsc_queue<std::move_only_function<void()>> queue_;
    std::move_only_function<void()> poll_;

    std::jthread thread_;
    std::atomic<std::thread::id> thread_id_{};

    std::promise<void> done_;
    std::future<void> done_future_;
};

} // namespace whiz::notify::detail
