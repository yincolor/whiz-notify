#pragma once

// whiz-notify 内部头文件（不导出）：平台后端抽象接口。

#include <whiz/notify.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>

namespace whiz::notify::detail {

// 平台后端抽象（见设计文档 §3.1）。
//
// poll_once() 是事件循环钩子（内部扩展）：由事件线程周期性调用，
// 驱动平台原生事件源（Linux: D-Bus 派发；Windows: Win32 消息泵；
// macOS: dispatch queue 排空）。
struct backend {
    virtual ~backend() = default;

    virtual std::expected<void, error> init() = 0;
    virtual void shutdown() noexcept = 0;

    virtual permission_status permission() = 0;
    virtual void request_permission(
        std::move_only_function<void(permission_status)> cb) = 0;

    // 按值接收 notification：后端起投递职责，负责保存回调与平台 ID 的映射。
    // 实现必须满足设计文档 §2.4 的契约。
    virtual std::expected<uint64_t, error> send(notification n) = 0;

    virtual void poll_once() = 0;
};

std::unique_ptr<backend> make_backend();

// Windows 专用 AUMID 辅助函数（仅由 backend_windows.cpp 实现；
// api.cpp 仅在 _WIN32 下调用它们）。
std::expected<void, error> windows_register_aumid(
    std::string_view aumid,
    std::string_view display_name,
    std::string_view icon_path);
void windows_set_aumid(std::string_view aumid);

// 投递后异步错误：若用户未提供 on_error，则打印到 stderr。
inline void report_error(const error& e) {
    std::fprintf(stderr, "whiz: error: %s\n", e.message.c_str());
}

} // namespace whiz::notify::detail
