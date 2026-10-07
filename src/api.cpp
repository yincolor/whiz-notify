#include <whiz/notify.hpp>

#include "backend.hpp"
#include "event_loop.hpp"

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

namespace whiz::notify {
namespace {

std::mutex g_mutex;
std::unique_ptr<detail::backend> g_backend;
std::unique_ptr<detail::event_loop> g_loop;
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_shutting_down{false};

detail::backend* backend_ptr() {
    return g_backend.get();
}

detail::event_loop* loop_ptr() {
    return g_loop.get();
}

void atexit_shutdown() {
    g_shutting_down.store(true, std::memory_order_release);
    if (g_loop) {
        g_loop->stop();
    }
    if (g_backend) {
        g_backend->shutdown();
    }
}

// 惰性初始化：首次调用任意公共 API 时启动事件线程并建立平台连接。
// 同时注册 atexit 钩子（见设计文档 §5.3）。
std::expected<void, error> ensure_initialized() {
    if (g_initialized.load(std::memory_order_acquire)) {
        return {};
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized.load(std::memory_order_relaxed)) {
        return {};
    }
    if (g_shutting_down.load(std::memory_order_relaxed)) {
        return std::unexpected(error{errc::backend_unavailable, "whiz-notify is shutting down"});
    }

    auto backend = detail::make_backend();
    if (!backend) {
        return std::unexpected(error{errc::backend_unavailable, "no backend for this platform"});
    }

    if (auto result = backend->init(); !result) {
        return std::unexpected(std::move(result.error()));
    }

    auto loop = std::make_unique<detail::event_loop>();
    detail::backend* raw_backend = backend.get();
    loop->start([raw_backend] { raw_backend->poll_once(); });

    g_backend = std::move(backend);
    g_loop    = std::move(loop);

    std::atexit(&atexit_shutdown);
    g_initialized.store(true, std::memory_order_release);
    return {};
}

// send() 的同步字段校验（见设计文档 §2.4.3）。
std::expected<void, error> validate(const notification& n) {
    if (n.title.empty()) {
        return std::unexpected(error{errc::invalid_argument, "title must not be empty"});
    }

    if (n.actions.size() > 3) {
        return std::unexpected(error{errc::invalid_argument, "at most 3 actions are allowed"});
    }

    std::unordered_set<std::string> seen_ids;
    for (const auto& a : n.actions) {
        if (a.id.empty()) {
            return std::unexpected(error{errc::invalid_argument, "action id must not be empty"});
        }
        if (a.label.empty()) {
            return std::unexpected(error{errc::invalid_argument, "action label must not be empty"});
        }
        if (!seen_ids.insert(a.id).second) {
            return std::unexpected(error{errc::invalid_argument, "action ids must be unique"});
        }
    }

    if (n.timeout && (n.timeout->count() <= 0 || n.timeout->count() > INT_MAX)) {
        return std::unexpected(
            error{errc::invalid_argument, "timeout must be > 0ms and <= INT_MAX ms"});
    }

    if (n.tag && n.tag->empty()) {
        return std::unexpected(error{errc::invalid_argument, "tag must not be empty"});
    }

    return {};
}

} // namespace

std::expected<void, error> send(notification n) {
    if (auto result = validate(n); !result) {
        return std::unexpected(std::move(result.error()));
    }

    if (auto result = ensure_initialized(); !result) {
        return std::unexpected(std::move(result.error()));
    }

    detail::backend* backend = backend_ptr();
    detail::event_loop* loop = loop_ptr();
    if (!backend || !loop) {
        return std::unexpected(error{errc::backend_unavailable, "whiz-notify is not initialized"});
    }

    // backend::send 必须运行在事件线程上；submit() 会自动处理“已在事件线程”的情形。
    auto result = loop->submit([backend, n = std::move(n)]() mutable {
        return backend->send(std::move(n));
    });

    if (!result) {
        return std::unexpected(std::move(result.error()));
    }
    return {};
}

permission_status has_permission() noexcept {
    try {
        if (auto result = ensure_initialized(); !result) {
            return permission_status::unsupported;
        }
        detail::backend* backend = backend_ptr();
        if (!backend) {
            return permission_status::unsupported;
        }
        return backend->permission();
    } catch (...) {
        return permission_status::unsupported;
    }
}

void request_permission(std::move_only_function<void(permission_status)> cb) {
    auto result = ensure_initialized();
    if (!result) {
        if (cb) {
            cb(permission_status::unsupported);
        }
        return;
    }

    detail::backend* backend = backend_ptr();
    if (!backend) {
        if (cb) {
            cb(permission_status::unsupported);
        }
        return;
    }

    backend->request_permission(std::move(cb));
}

namespace platform::windows {

std::expected<void, error> register_aumid(std::string_view aumid,
                                          std::string_view display_name,
                                          std::string_view icon_path) {
#if defined(_WIN32)
    return detail::windows_register_aumid(aumid, display_name, icon_path);
#else
    (void)aumid;
    (void)display_name;
    (void)icon_path;
    return std::unexpected(error{errc::not_supported, "AUMID is Windows-only"});
#endif
}

void set_aumid(std::string_view aumid) {
#if defined(_WIN32)
    detail::windows_set_aumid(aumid);
#else
    (void)aumid;
#endif
}

} // namespace platform::windows

} // namespace whiz::notify
