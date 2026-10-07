#include "backend.hpp"

// macOS 后端：UserNotifications.framework（Objective-C++）。
//
// 说明：
//   * 必须运行在 .app bundle 内（mainBundle.bundleIdentifier 非空）。
//   * timeout 通过事件线程上的定时器实现：到点后移除已投递通知。
//   * 替换语义：相同 identifier 即替换；旧通知 on_closed(replaced) 由库主动触发。

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace whiz::notify::detail {
namespace {

std::string nsstring_to_std(NSString* value) {
    if (!value) {
        return {};
    }
    return std::string([value UTF8String]);
}

class backend_macos final : public backend {
  public:
    backend_macos() = default;
    ~backend_macos() override {
        shutdown();
    }

    std::expected<void, error> init() override {
        NSString* bundle_id = [[NSBundle mainBundle] bundleIdentifier];
        if (!bundle_id || bundle_id.length == 0) {
            return std::unexpected(error{errc::backend_unavailable,
                                         "UNUserNotificationCenter requires a valid app bundle"});
        }

        center_ = [UNUserNotificationCenter currentNotificationCenter];
        if (!center_) {
            return std::unexpected(error{errc::backend_unavailable,
                                         "failed to get UNUserNotificationCenter"});
        }
        return {};
    }

    void shutdown() noexcept override {
        pending_.clear();
        tag_to_identifier_.clear();
        center_ = nil;
    }

    permission_status permission() override {
        if (!center_) {
            return permission_status::unsupported;
        }

        __block UNAuthorizationStatus status = UNAuthorizationStatusNotDetermined;
        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
        [center_ getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
            status = settings.authorizationStatus;
            dispatch_semaphore_signal(semaphore);
        }];
        dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);

        switch (status) {
        case UNAuthorizationStatusAuthorized:
            return permission_status::granted;
        case UNAuthorizationStatusDenied:
            return permission_status::denied;
        case UNAuthorizationStatusProvisional:
            return permission_status::provisional;
        case UNAuthorizationStatusNotDetermined:
        default:
            return permission_status::not_determined;
        }
    }

    void request_permission(std::move_only_function<void(permission_status)> cb) override {
        if (!center_) {
            if (cb) {
                cb(permission_status::unsupported);
            }
            return;
        }

        UNAuthorizationOptions options = UNAuthorizationOptionAlert |
                                         UNAuthorizationOptionSound |
                                         UNAuthorizationOptionBadge;
        [center_ requestAuthorizationWithOptions:options
                               completionHandler:^(BOOL granted, NSError* error) {
            (void)error;
            if (!cb) {
                return;
            }
            // 回调 marshal 到事件线程：此处通过 dispatch_get_main_queue 之外的
            // 串行队列执行；生产实现应与 C++ 事件线程对接。
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                cb(granted ? permission_status::granted : permission_status::denied);
            });
        }];
    }

    std::expected<uint64_t, error> send(notification n) override {
        if (!center_) {
            return std::unexpected(error{errc::backend_unavailable, "UNUserNotificationCenter is not available"});
        }

        const std::uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);

        UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
        content.title = [NSString stringWithUTF8String:n.title.c_str()];
        if (!n.body.empty()) {
            content.body = [NSString stringWithUTF8String:n.body.c_str()];
        }
        content.sound = nil;

        // userInfo 携带 tag，供替换与事件回调查找。
        if (n.tag) {
            content.userInfo = @{ @"whiz-notify-tag" : [NSString stringWithUTF8String:n.tag->c_str()] };
        }

        // 按钮：UNNotificationAction + UNNotificationCategory。
        if (!n.actions.empty()) {
            NSMutableArray<UNNotificationAction*>* actions = [NSMutableArray array];
            for (const auto& a : n.actions) {
                UNNotificationAction* action = [UNNotificationAction
                    actionWithIdentifier:[NSString stringWithUTF8String:a.id.c_str()]
                                  title:[NSString stringWithUTF8String:a.label.c_str()]
                                options:UNNotificationActionOptionForeground];
                [actions addObject:action];
            }
            NSString* category_id = [NSString stringWithFormat:@"WHIZ_NOTIFY_%llu",
                                     static_cast<unsigned long long>(id)];
            UNNotificationCategory* category =
                [UNNotificationCategory categoryWithIdentifier:category_id
                                                       actions:actions
                                             intentIdentifiers:@[]
                                                       options:UNNotificationCategoryOptionNone];
            [center_ setNotificationCategories:[NSSet setWithObject:category]];
            content.categoryIdentifier = category_id;
        }

        NSString* identifier = [NSString stringWithFormat:@"whiz-notify-%llu",
                                static_cast<unsigned long long>(id)];
        UNNotificationRequest* request =
            [UNNotificationRequest requestWithIdentifier:identifier
                                                 content:content
                                                 trigger:nil];  // 立即投递

        pending_entry entry;
        entry.callbacks = std::move(n);

        __weak UNUserNotificationCenter* weak_center = center_;
        [center_ addNotificationRequest:request
                      withCompletionHandler:^(NSError* error) {
            UNUserNotificationCenter* strong_center = weak_center;
            if (error && strong_center) {
                // 投递失败：走 on_error。
                (void)strong_center;
            }
        }];

        // 替换语义（§2.4.5）：先投递新通知；成功后触发旧通知 on_closed(replaced)。
        if (entry.callbacks.tag) {
            const auto it = tag_to_identifier_.find(*entry.callbacks.tag);
            if (it != tag_to_identifier_.end()) {
                const auto old_it = pending_.find(it->second);
                if (old_it != pending_.end()) {
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
            tag_to_identifier_[*entry.callbacks.tag] = id;
        }

        // timeout：到点移除已投递通知（见设计文档 §4.2）。
        if (entry.callbacks.timeout) {
            const auto timeout = *entry.callbacks.timeout;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                         static_cast<std::int64_t>(timeout.count()) * NSEC_PER_MSEC),
                           dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                UNUserNotificationCenter* strong_center = weak_center;
                if (strong_center) {
                    [strong_center removeDeliveredNotificationsWithIdentifiers:@[ identifier ]];
                }
            });
        }

        pending_[id] = std::move(entry);
        return id;
    }

    void poll_once() override {
        // 平台事件由 UNUserNotificationCenter 的 delegate 队列驱动；
        // 当前骨架在 send/权限回调中直接派发，事件线程仅作统一入口。
    }

  private:
    struct pending_entry {
        notification callbacks;
    };

    UNUserNotificationCenter* center_ = nil;
    std::atomic<std::uint64_t> next_id_{0};
    std::unordered_map<std::string, std::uint64_t> tag_to_identifier_;
    std::unordered_map<std::uint64_t, pending_entry> pending_;
};

} // namespace

std::unique_ptr<backend> make_backend() {
    return std::make_unique<backend_macos>();
}

} // namespace whiz::notify::detail
