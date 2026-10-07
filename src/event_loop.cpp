#include "event_loop.hpp"

#include <cstdint>
#include <utility>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <dispatch/dispatch.h>
#else
#  include <cerrno>
#  include <poll.h>
#  include <pthread.h>
#  include <sys/eventfd.h>
#  include <unistd.h>
#endif

namespace whiz::notify::detail {

// 平台唤醒源：post() 后从事件线程的等待中提前唤醒。
//
//   Linux  : eventfd（poll 等待）
//   Windows: 自动重置事件对象
//   macOS  : dispatch_semaphore
struct event_loop::event_source {
#if defined(_WIN32)
    HANDLE event = nullptr;
#elif defined(__APPLE__)
    dispatch_semaphore_t semaphore = nullptr;
#else
    int fd = -1;
#endif

    event_source() {
#if defined(_WIN32)
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
#elif defined(__APPLE__)
        semaphore = dispatch_semaphore_create(0);
#else
        fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
#endif
    }

    ~event_source() {
#if defined(_WIN32)
        if (event) {
            CloseHandle(event);
        }
#elif defined(__APPLE__)
        if (semaphore) {
            dispatch_release(semaphore);
        }
#else
        if (fd >= 0) {
            ::close(fd);
        }
#endif
    }

    event_source(const event_source&)            = delete;
    event_source& operator=(const event_source&) = delete;

    void notify() {
#if defined(_WIN32)
        if (event) {
            SetEvent(event);
        }
#elif defined(__APPLE__)
        if (semaphore) {
            dispatch_semaphore_signal(semaphore);
        }
#else
        if (fd >= 0) {
            const std::uint64_t one = 1;
            (void)::write(fd, &one, sizeof(one));
        }
#endif
    }

    void wait_for(int timeout_ms) {
#if defined(_WIN32)
        if (event) {
            WaitForSingleObject(event, static_cast<DWORD>(timeout_ms));
        } else {
            Sleep(static_cast<DWORD>(timeout_ms));
        }
#elif defined(__APPLE__)
        if (semaphore) {
            dispatch_semaphore_wait(
                semaphore,
                dispatch_time(DISPATCH_TIME_NOW, static_cast<std::int64_t>(timeout_ms) * NSEC_PER_MSEC));
        }
#else
        if (fd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return;
        }
        struct pollfd pfd {};
        pfd.fd     = fd;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, timeout_ms);
        if (ready > 0) {
            std::uint64_t value = 0;
            (void)::read(fd, &value, sizeof(value));
        }
#endif
    }
};

event_loop::event_loop() = default;

event_loop::~event_loop() {
    stop();
}

void event_loop::start(std::move_only_function<void()> poll) {
    if (thread_.joinable()) {
        return;
    }
    poll_        = std::move(poll);
    wake_        = std::make_unique<event_source>();
    done_future_ = done_.get_future();
    thread_      = std::jthread([this](std::stop_token token) { run(token); });
}

void event_loop::post(std::move_only_function<void()> task) {
    if (!task) {
        return;
    }
    queue_.push(std::move(task));
    if (wake_) {
        wake_->notify();
    }
}

void event_loop::stop() {
    if (!thread_.joinable()) {
        return;
    }

    thread_.request_stop();
    if (wake_) {
        wake_->notify();
    }

    if (done_future_.valid() &&
        done_future_.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        // 事件线程 100ms 内未退出（如用户回调阻塞）：放弃等待。
        // std::jthread 无 detach()，这里将其移入堆对象，避免其析构函数
        // 在静态析构阶段阻塞 join。进程结束时由 OS 回收线程资源。
        auto* leaked = new std::jthread(std::move(thread_));
        (void)leaked;
        return;
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

void event_loop::run(std::stop_token stop_token) {
    thread_id_.store(std::this_thread::get_id(), std::memory_order_relaxed);

#if defined(__linux__)
    // 命名事件线程为 "whiz-evt"（仅诊断用途，失败不影响功能）。
    pthread_setname_np(pthread_self(), "whiz-evt");
#endif

    while (!stop_token.stop_requested()) {
        while (auto task = queue_.try_pop()) {
            std::invoke(std::move(*task));
        }

        if (poll_) {
            poll_();
        }

        if (stop_token.stop_requested()) {
            break;
        }

        if (wake_) {
            wake_->wait_for(50);
        }
    }

    // 退出前排空一次剩余任务。
    while (auto task = queue_.try_pop()) {
        std::invoke(std::move(*task));
    }

    try {
        done_.set_value();
    } catch (...) {
    }
}

} // namespace whiz::notify::detail
