#include "backend.hpp"
#include "detail/string_util.hpp"

#include <dbus/dbus.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace whiz::notify::detail {
namespace {

constexpr const char* k_service   = "org.freedesktop.Notifications";
constexpr const char* k_path      = "/org/freedesktop/Notifications";
constexpr const char* k_interface = "org.freedesktop.Notifications";

class backend_linux;

DBusHandlerResult signal_filter(DBusConnection*, DBusMessage*, void*);

class backend_linux final : public backend {
  public:
    backend_linux() = default;
    ~backend_linux() override {
        shutdown();
    }

    std::expected<void, error> init() override {
        dbus_threads_init_default();

        DBusError dbus_error;
        dbus_error_init(&dbus_error);

        conn_ = dbus_bus_get(DBUS_BUS_SESSION, &dbus_error);
        if (dbus_error_is_set(&dbus_error) || !conn_) {
            std::string message = dbus_error.message ? dbus_error.message
                                                     : "cannot connect to the D-Bus session bus";
            dbus_error_free(&dbus_error);
            return std::unexpected(error{errc::backend_unavailable, std::move(message)});
        }
        dbus_error_free(&dbus_error);

        dbus_connection_set_exit_on_disconnect(conn_, FALSE);
        dbus_connection_add_filter(conn_, signal_filter, this, nullptr);

        // 订阅 org.freedesktop.Notifications 的两个事件信号。
        add_match("type='signal',interface='org.freedesktop.Notifications',member='NotificationClosed'");
        add_match("type='signal',interface='org.freedesktop.Notifications',member='ActionInvoked'");

        return {};
    }

    void shutdown() noexcept override {
        if (!conn_) {
            return;
        }
        dbus_connection_remove_filter(conn_, signal_filter, this);
        dbus_connection_unref(conn_);
        conn_ = nullptr;

        tag_to_id_.clear();
        pending_.clear();
    }

    permission_status permission() override {
        // Linux 桌面环境无“通知权限”模型，初始化成功后恒定 granted（§2.4.1）。
        return permission_status::granted;
    }

    void request_permission(std::move_only_function<void(permission_status)> cb) override {
        // 无权限模型的平台：立即在调用线程同步回调 unsupported（§2.4.2）。
        if (cb) {
            cb(permission_status::unsupported);
        }
    }

    std::expected<uint64_t, error> send(notification n) override {
        if (!conn_) {
            return std::unexpected(error{errc::backend_unavailable, "D-Bus connection is not available"});
        }

        std::uint32_t replaces_id = 0;
        if (n.tag) {
            const auto it = tag_to_id_.find(*n.tag);
            if (it != tag_to_id_.end()) {
                replaces_id = it->second;
            }
        }

        DBusMessage* message =
            dbus_message_new_method_call(k_service, k_path, k_interface, "Notify");
        if (!message) {
            return std::unexpected(error{errc::io_error, "failed to create D-Bus Notify message"});
        }

        DBusMessageIter args;
        dbus_message_iter_init_append(message, &args);

        const std::string app_name = read_proc_comm();
        const char* app_name_c     = app_name.c_str();
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &app_name_c);

        dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &replaces_id);

        const std::string app_icon = n.icon ? *n.icon : std::string{};
        const char* app_icon_c     = app_icon.c_str();
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &app_icon_c);

        const char* summary_c = n.title.c_str();
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &summary_c);

        const char* body_c = n.body.c_str();
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &body_c);

        // actions：扁平 [id1, label1, id2, label2, ...]。
        DBusMessageIter actions_array;
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &actions_array);
        for (const auto& a : n.actions) {
            const char* id_c    = a.id.c_str();
            const char* label_c = a.label.c_str();
            dbus_message_iter_append_basic(&actions_array, DBUS_TYPE_STRING, &id_c);
            dbus_message_iter_append_basic(&actions_array, DBUS_TYPE_STRING, &label_c);
        }
        dbus_message_iter_close_container(&args, &actions_array);

        // hints：v1 仅写 desktop-entry（来自 /proc/self/comm）。
        DBusMessageIter hints_array;
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &hints_array);
        {
            DBusMessageIter entry;
            DBusMessageIter variant;
            dbus_message_iter_open_container(&hints_array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);

            const char* key = "desktop-entry";
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);

            const char* value = app_name_c;
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
            dbus_message_iter_close_container(&entry, &variant);

            dbus_message_iter_close_container(&hints_array, &entry);
        }
        dbus_message_iter_close_container(&args, &hints_array);

        const std::int32_t expire_timeout =
            n.timeout ? static_cast<std::int32_t>(n.timeout->count()) : -1;
        dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &expire_timeout);

        DBusError dbus_error;
        dbus_error_init(&dbus_error);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn_, message, -1, &dbus_error);
        dbus_message_unref(message);

        if (!reply) {
            const bool service_unknown =
                dbus_error_is_set(&dbus_error) &&
                dbus_error.name != nullptr &&
                std::strcmp(dbus_error.name, DBUS_ERROR_SERVICE_UNKNOWN) == 0;

            std::string error_message = dbus_error.message ? dbus_error.message : "Notify failed";
            dbus_error_free(&dbus_error);

            if (service_unknown) {
                // 无桌面环境（无通知守护进程）：send() 本身已成功返回，
                // 投递后错误经 on_error 回调报告；未提供 on_error 则打印 stderr（§4.3 / §2.4.4）。
                const error async_error{errc::backend_unavailable, std::move(error_message)};
                if (n.on_error) {
                    try {
                        n.on_error(async_error);
                    } catch (...) {
                        std::fprintf(stderr, "whiz: error: on_error callback threw an exception\n");
                    }
                } else {
                    report_error(async_error);
                }
                return std::uint64_t{0};
            }

            return std::unexpected(error{errc::io_error, std::move(error_message)});
        }
        dbus_error_free(&dbus_error);

        std::uint32_t id = 0;
        if (!dbus_message_get_args(reply, &dbus_error,
                                   DBUS_TYPE_UINT32, &id,
                                   DBUS_TYPE_INVALID)) {
            std::string error_message = dbus_error.message ? dbus_error.message
                                                           : "Notify returned no id";
            dbus_error_free(&dbus_error);
            dbus_message_unref(reply);
            return std::unexpected(error{errc::io_error, std::move(error_message)});
        }
        dbus_message_unref(reply);

        // 替换语义（§2.4.5）：先投递新通知；成功后触发旧通知 on_closed(replaced) 并移除映射。
        if (n.tag) {
            const auto it = tag_to_id_.find(*n.tag);
            if (it != tag_to_id_.end()) {
                const std::uint32_t old_id = it->second;
                auto old_it                = pending_.find(old_id);
                if (old_it != pending_.end()) {
                    notification old = std::move(old_it->second);
                    pending_.erase(old_it);
                    if (old.on_closed) {
                        try {
                            old.on_closed(close_reason::replaced);
                        } catch (...) {
                            std::fprintf(stderr, "whiz: error: on_closed callback threw an exception\n");
                        }
                    }
                }
            }
            tag_to_id_[*n.tag] = id;
        }

        pending_[id] = std::move(n);
        return id;
    }

    void poll_once() override {
        if (!conn_) {
            return;
        }
        // 非阻塞读写 + 派发；信号经 signal_filter 处理。
        dbus_connection_read_write_dispatch(conn_, 0);
    }

    DBusHandlerResult handle_signal(DBusMessage* message) {
        if (!dbus_message_is_signal(message, k_interface, "NotificationClosed") &&
            !dbus_message_is_signal(message, k_interface, "ActionInvoked")) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }

        const char* member = dbus_message_get_member(message);
        if (member == nullptr) {
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (std::strcmp(member, "NotificationClosed") == 0) {
            std::uint32_t id     = 0;
            std::uint32_t reason = 0;
            if (dbus_message_get_args(message, nullptr,
                                      DBUS_TYPE_UINT32, &id,
                                      DBUS_TYPE_UINT32, &reason,
                                      DBUS_TYPE_INVALID)) {
                on_closed_signal(id, reason);
            }
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (std::strcmp(member, "ActionInvoked") == 0) {
            std::uint32_t id          = 0;
            const char* action_key    = nullptr;
            if (dbus_message_get_args(message, nullptr,
                                      DBUS_TYPE_UINT32, &id,
                                      DBUS_TYPE_STRING, &action_key,
                                      DBUS_TYPE_INVALID)) {
                on_action_signal(id, action_key ? action_key : "");
            }
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        return DBUS_HANDLER_RESULT_HANDLED;
    }

  private:
    void add_match(const char* rule) {
        DBusError dbus_error;
        dbus_error_init(&dbus_error);
        dbus_bus_add_match(conn_, rule, &dbus_error);
        if (dbus_error_is_set(&dbus_error)) {
            std::fprintf(stderr, "whiz: warning: dbus_bus_add_match failed: %s\n",
                         dbus_error.message ? dbus_error.message : "unknown");
            dbus_error_free(&dbus_error);
        }
    }

    void on_closed_signal(std::uint32_t id, std::uint32_t reason) {
        const auto it = pending_.find(id);
        if (it == pending_.end()) {
            // 已被同 tag 替换而移除（§2.4.5），忽略平台随后发来的关闭事件。
            return;
        }

        notification n = std::move(it->second);
        pending_.erase(it);

        if (!n.on_closed) {
            return;
        }

        close_reason close_reason_value = close_reason::unknown;
        switch (reason) {
        case 1: // expired
            close_reason_value = close_reason::expired;
            break;
        case 2: // dismissed by user
            close_reason_value = close_reason::dismissed;
            break;
        default:
            close_reason_value = close_reason::unknown;
            break;
        }

        try {
            n.on_closed(close_reason_value);
        } catch (...) {
            std::fprintf(stderr, "whiz: error: on_closed callback threw an exception\n");
        }
    }

    void on_action_signal(std::uint32_t id, std::string_view action_key) {
        const auto it = pending_.find(id);
        if (it == pending_.end()) {
            return;
        }
        if (!it->second.on_action) {
            return;
        }

        // on_action 可多次触发（§3.2 不变量 4），不移除映射。
        try {
            it->second.on_action(action_key);
        } catch (...) {
            std::fprintf(stderr, "whiz: error: on_action callback threw an exception\n");
        }
    }

    DBusConnection* conn_ = nullptr;
    std::unordered_map<std::string, std::uint32_t> tag_to_id_;
    std::unordered_map<std::uint32_t, notification> pending_;
};

DBusHandlerResult signal_filter(DBusConnection*, DBusMessage* message, void* user_data) {
    if (!message || !user_data) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    return static_cast<backend_linux*>(user_data)->handle_signal(message);
}

} // namespace

std::unique_ptr<backend> make_backend() {
    return std::make_unique<backend_linux>();
}

} // namespace whiz::notify::detail
