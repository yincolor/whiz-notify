#pragma once

// 内部 MPSC 任务队列：多生产者、单消费者（事件线程）。
//
// v1 采用“互斥锁 + deque”的简单实现，保证正确性；
// 跨线程唤醒由 event_loop 的平台事件源（eventfd / 事件对象 /
// dispatch_semaphore）负责，队列本身不负责唤醒。

#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace whiz::notify::detail {

template <class T>
class mpsc_queue {
  public:
    void push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(value));
    }

    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop_front();
        return value;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

  private:
    mutable std::mutex mutex_;
    std::deque<T> queue_;
};

} // namespace whiz::notify::detail
