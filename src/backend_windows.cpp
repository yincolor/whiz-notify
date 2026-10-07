#include "backend.hpp"

// Windows 后端：WinRT Toast 通知。
//
// 说明：
//   * 事件线程以 STA 模式运行（winrt::init_apartment），并泵 Win32 消息。
//   * 每个通知使用唯一内部 Tag（whiz-notify-<id>）；用户 tag 通过
//     tag_to_id_ 映射实现替换语义（见设计文档 §2.4.5）。
//   * 未打包应用必须先调用 platform::windows::register_aumid + set_aumid。

#include <windows.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace whiz::notify::detail {
namespace {

using winrt::Windows::Data::Xml::Dom::XmlDocument;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::UI::Notifications::ToastDismissalReason;
using winrt::Windows::UI::Notifications::ToastFailedEventArgs;
using winrt::Windows::UI::Notifications::ToastNotification;
using winrt::Windows::UI::Notifications::ToastNotificationManager;
using winrt::Windows::UI::Notifications::ToastNotifier;
using winrt::Windows::UI::Notifications::ToastDismissedEventArgs;

std::string g_default_aumid;

std::wstring to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring xml_escape(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (const wchar_t ch : text) {
        switch (ch) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        case L'\'': out += L"&apos;"; break;
        default: out += ch; break;
        }
    }
    return out;
}

std::wstring build_toast_xml(const notification& n) {
    std::wstring xml;
    xml += L"<toast>";
    xml += L"<visual><binding template=\"ToastGeneric\">";
    xml += L"<text>";
    xml += xml_escape(to_wide(n.title));
    xml += L"</text>";

    if (!n.body.empty()) {
        xml += L"<text>";
        xml += xml_escape(to_wide(n.body));
        xml += L"</text>";
    }

    if (n.icon) {
        // WinRT Toast 图标仅支持 file:// URI。
        xml += L"<image placement=\"appLogoOverride\" src=\"";
        xml += xml_escape(to_wide(*n.icon));
        xml += L"\" />";
    }

    xml += L"</binding></visual>";

    if (!n.actions.empty()) {
        xml += L"<actions>";
        for (const auto& a : n.actions) {
            xml += L"<action content=\"";
            xml += xml_escape(to_wide(a.label));
            xml += L"\" arguments=\"";
            xml += xml_escape(to_wide(a.id));
            xml += L"\" activationType=\"foreground\" />";
        }
        xml += L"</actions>";
    }

    xml += L"</toast>";
    return xml;
}

class backend_windows final : public backend {
  public:
    backend_windows() = default;
    ~backend_windows() override {
        shutdown();
    }

    std::expected<void, error> init() override {
        // WinRT STA 公寓在事件线程首次 poll_once/send 时初始化。
        return {};
    }

    void shutdown() noexcept override {
        pending_.clear();
        tag_to_id_.clear();
    }

    permission_status permission() override {
        // Windows 无通知权限模型，初始化成功后恒定 granted（§2.4.1）。
        return permission_status::granted;
    }

    void request_permission(std::move_only_function<void(permission_status)> cb) override {
        if (cb) {
            cb(permission_status::unsupported);
        }
    }

    std::expected<uint64_t, error> send(notification n) override {
        try {
            ensure_apartment();

            const std::uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);

            XmlDocument document;
            document.LoadXml(build_toast_xml(n));

            ToastNotification toast(document);
            toast.Tag(winrt::hstring(L"whiz-notify-" + std::to_wstring(id)));
            toast.Group(winrt::hstring(L"whiz-notify"));

            if (n.timeout) {
                using winrt::Windows::Foundation::TimeSpan;
                // TimeSpan 单位为 100ns；1ms = 10000 ticks。
                const TimeSpan span{static_cast<std::int64_t>(n.timeout->count()) * 10000};
                toast.ExpirationTime(winrt::clock::now() + span);
            }

            const auto notifier = make_notifier();
            if (!notifier) {
                return std::unexpected(error{errc::backend_unavailable,
                                             "AUMID is not registered (call platform::windows::register_aumid)"});
            }

            pending_entry entry;
            entry.callbacks = std::move(n);
            entry.toast     = toast;

            const std::uint64_t captured_id = id;
            entry.dismissed_token = toast.Dismissed(
                [this, captured_id](ToastNotification const&, ToastDismissedEventArgs const& args) {
                    on_dismissed(captured_id, args.Reason());
                });
            entry.activated_token = toast.Activated(
                [this, captured_id](ToastNotification const&, IInspectable const& args) {
                    on_activated(captured_id, args);
                });
            entry.failed_token = toast.Failed(
                [this, captured_id](ToastNotification const&, ToastFailedEventArgs const& args) {
                    on_failed(captured_id, args);
                });

            notifier.Show(toast);

            // 替换语义（§2.4.5）：先投递新通知；成功后处理旧通知。
            if (entry.callbacks.tag) {
                const auto it = tag_to_id_.find(*entry.callbacks.tag);
                if (it != tag_to_id_.end()) {
                    const std::uint64_t old_id = it->second;
                    auto old_it                = pending_.find(old_id);
                    if (old_it != pending_.end()) {
                        hide_notification(old_it->second);
                        notification old_callbacks = std::move(old_it->second.callbacks);
                        pending_.erase(old_it);
                        if (old_callbacks.on_closed) {
                            try {
                                old_callbacks.on_closed(close_reason::replaced);
                            } catch (...) {
                                std::fprintf(stderr, "whiz: error: on_closed callback threw an exception\n");
                            }
                        }
                    }
                }
                tag_to_id_[*entry.callbacks.tag] = id;
            }

            pending_[id] = std::move(entry);
            return id;
        } catch (const winrt::hresult_error& e) {
            return std::unexpected(error{errc::io_error, winrt::to_string(e.message())});
        } catch (...) {
            return std::unexpected(error{errc::unknown, "WinRT toast send failed"});
        }
    }

    void poll_once() override {
        try {
            ensure_apartment();
        } catch (...) {
            return;
        }
        pump_messages();
    }

  private:
    struct pending_entry {
        notification callbacks;
        ToastNotification toast{nullptr};
        winrt::event_token dismissed_token{};
        winrt::event_token activated_token{};
        winrt::event_token failed_token{};
    };

    static void ensure_apartment() {
        static std::atomic<bool> initialized{false};
        if (initialized.load(std::memory_order_acquire)) {
            return;
        }
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        initialized.store(true, std::memory_order_release);
    }

    static ToastNotifier make_notifier() {
        if (g_default_aumid.empty()) {
            return ToastNotificationManager::CreateToastNotifier();
        }
        return ToastNotificationManager::CreateToastNotifier(winrt::hstring(to_wide(g_default_aumid)));
    }

    void hide_notification(const pending_entry& entry) {
        if (!entry.toast) {
            return;
        }
        try {
            make_notifier().Hide(entry.toast);
        } catch (...) {
        }
    }

    static void pump_messages() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    void on_dismissed(std::uint64_t id, ToastDismissalReason reason) {
        const auto it = pending_.find(id);
        if (it == pending_.end()) {
            return;
        }

        pending_entry entry = std::move(it->second);
        pending_.erase(it);

        if (!entry.callbacks.on_closed) {
            return;
        }

        close_reason cr = close_reason::unknown;
        switch (reason) {
        case ToastDismissalReason::TimedOut:
            cr = close_reason::expired;
            break;
        case ToastDismissalReason::UserCanceled:
            cr = close_reason::dismissed;
            break;
        default:
            cr = close_reason::unknown;
            break;
        }

        try {
            entry.callbacks.on_closed(cr);
        } catch (...) {
            std::fprintf(stderr, "whiz: error: on_closed callback threw an exception\n");
        }
    }

    void on_activated(std::uint64_t id, IInspectable const& args) {
        const auto it = pending_.find(id);
        if (it == pending_.end() || !it->second.callbacks.on_action) {
            return;
        }

        std::string action_id = winrt::to_string(args.as<winrt::Windows::UI::Notifications::ToastActivatedEventArgs>().Arguments());
        try {
            it->second.callbacks.on_action(action_id);
        } catch (...) {
            std::fprintf(stderr, "whiz: error: on_action callback threw an exception\n");
        }
    }

    void on_failed(std::uint64_t id, ToastFailedEventArgs const& args) {
        const auto it = pending_.find(id);
        if (it == pending_.end()) {
            return;
        }

        const error e{errc::io_error, winrt::to_string(args.ErrorCode().message())};
        if (it->second.callbacks.on_error) {
            try {
                it->second.callbacks.on_error(e);
            } catch (...) {
                std::fprintf(stderr, "whiz: error: on_error callback threw an exception\n");
            }
        } else {
            report_error(e);
        }

        pending_.erase(it);
    }

    std::atomic<std::uint64_t> next_id_{0};
    std::unordered_map<std::string, std::uint64_t> tag_to_id_;
    std::unordered_map<std::uint64_t, pending_entry> pending_;
};

} // namespace

std::expected<void, error> windows_register_aumid(std::string_view aumid,
                                                  std::string_view display_name,
                                                  std::string_view icon_path) {
    const std::wstring key = L"Software\\Classes\\AppUserModelId\\" + to_wide(aumid);

    HKEY key_handle = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                        &key_handle, nullptr) != ERROR_SUCCESS) {
        return std::unexpected(error{errc::io_error, "failed to create AUMID registry key"});
    }

    const std::wstring display_name_wide = to_wide(display_name);
    const LSTATUS dn_result =
        RegSetValueExW(key_handle, L"DisplayName", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(display_name_wide.c_str()),
                       static_cast<DWORD>((display_name_wide.size() + 1) * sizeof(wchar_t)));

    LSTATUS icon_result = ERROR_SUCCESS;
    if (!icon_path.empty()) {
        const std::wstring icon_wide = to_wide(icon_path);
        icon_result =
            RegSetValueExW(key_handle, L"IconUri", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(icon_wide.c_str()),
                           static_cast<DWORD>((icon_wide.size() + 1) * sizeof(wchar_t)));
    }

    RegCloseKey(key_handle);

    if (dn_result != ERROR_SUCCESS || icon_result != ERROR_SUCCESS) {
        return std::unexpected(error{errc::io_error, "failed to write AUMID registry values"});
    }
    return {};
}

void windows_set_aumid(std::string_view aumid) {
    g_default_aumid = std::string(aumid);
}

std::unique_ptr<backend> make_backend() {
    return std::make_unique<backend_windows>();
}

} // namespace whiz::notify::detail
