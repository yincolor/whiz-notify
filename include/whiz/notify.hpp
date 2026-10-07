#pragma once

// whiz-notify 跨平台系统通知库 —— 公共 API 单头文件。
//
// 本头文件是库的全部公共接口，自包含（见设计文档 §7.1）。
// 命名空间：whiz::notify（含 whiz::notify::platform::windows）。
//
// 实现编译为静态库；使用者通过 CMake FetchContent 集成，见 README.md。

#include <chrono>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whiz::notify {

// ---- 关闭原因 ----
enum class close_reason {
    expired,    // 超时
    dismissed,  // 用户主动关闭
    replaced,   // 被同 tag 的后继通知替换
    unknown,
};

// ---- 权限状态 ----
enum class permission_status {
    not_determined,
    granted,
    denied,
    provisional,  // macOS 临时授权
    unsupported,  // 平台无权限模型，或后端不可用
};

// ---- 错误 ----
enum class errc {
    invalid_argument,
    permission_denied,
    not_supported,
    backend_unavailable,
    io_error,
    unknown,
};

struct error {
    errc        code;
    std::string message;
};

// ---- 按钮 ----
struct action {
    std::string id;
    std::string label;
};

// ---- 通知（数据 + 回调，唯一配置类型） ----
//
// 说明：
//   * 本类型是 aggregate，使用 designated initializers 构造。
//   * 因含 std::move_only_function 成员，本类型 move-only。
//   * 本类型自身不保证字段合法（如 title 非空、actions.size() <= 3）；
//     合法性由 send() 统一校验。
//   * designated initializers 必须按声明顺序出现；未列出的字段默认构造。
//   * 未指定的事件回调字段为空（== nullptr），库按“未提供”处理。
struct notification {
    // -- 内容 --
    std::string                              title;    // 必填
    std::string                              body;     // 可选
    std::optional<std::string>               icon;     // 本地路径或 file:// URI
    std::vector<action>                      actions;  // 按钮，上限 3
    std::optional<std::chrono::milliseconds> timeout;  // 不填用系统默认
    std::optional<std::string>               tag;      // 替换键

    // -- 事件（可选） --
    std::move_only_function<void(std::string_view)> on_action;
    std::move_only_function<void(close_reason)>     on_closed;
    std::move_only_function<void(const error&)>     on_error;
};

// ---- 唯一入口 ----
[[nodiscard]] std::expected<void, error> send(notification n);

// ---- 权限接口 ----
[[nodiscard]] permission_status has_permission() noexcept;
void request_permission(std::move_only_function<void(permission_status)> cb);

// ---- Windows AUMID 辅助接口 ----
//
// 可见性：本命名空间在三大平台上均声明（无 #ifdef 包裹），
// 便于跨平台代码条件调用；行为差异见设计文档 §2.4。
namespace platform::windows {

// 将 AUMID 写入 HKCU\Software\Classes\AppUserModelId\<aumid>。
// 未打包（非 MSIX）应用必须在首次发通知前调用一次。
// 打包应用无需调用。
// 非 Windows 平台：返回 not_supported。
[[nodiscard]] std::expected<void, error> register_aumid(
    std::string_view aumid,
    std::string_view display_name,
    std::string_view icon_path = {});

// 设置库默认使用的 AUMID（必须与 register_aumid 的 aumid 一致）。
// 非 Windows 平台：静默忽略。
void set_aumid(std::string_view aumid);

} // namespace platform::windows

} // namespace whiz::notify
