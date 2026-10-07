# whiz-notify 跨平台系统通知库设计文档

---

## 0. 文档信息

| 项 | 内容 |
|---|---|
| 项目名称 | **whiz-notify** |
| 命名空间 | `whiz::notify` |
| 版本 | 0.1.0 |
| 目标标准 | C++23 |
| 目标编译器 | g++-14；Windows: MSVC 19.40+；macOS: Apple Clang 16+ |
| 许可证 | AGPL-3.0-or-later |
| 集成方式 | 仅 CMake `FetchContent` |
| 公共头文件 | 单头 `whiz/notify.hpp` |
| Linux 后端依赖 | `libdbus-1` |
| 测试与 CI | 不在本项目目录内，位于仓库根目录 `test/` 下 |

---

## 1. 项目概述

`whiz-notify` 是一个面向现代桌面环境的 C++ 跨平台系统通知库，为 Windows 10/11、macOS 和 GNU/Linux 提供统一的通知发送与事件回调接口。库采用纯 C++23 编写，不依赖任何 GUI 框架（GTK/Qt/Saucer），仅通过 CMake `FetchContent` 集成到宿主工程。

### 1.1 设计目标

| 目标 | 说明 |
|---|---|
| **统一 API** | 三大平台暴露同一套 `whiz::notify::send(...)` 接口，行为语义一致。 |
| **原生协议** | Windows 走 WinRT Toast，macOS 走 `UNUserNotificationCenter`，Linux 走 `org.freedesktop.Notifications` D-Bus 规范。 |
| **零框架依赖** | 不引入 GTK4 / Qt6 / GLib。Linux 只依赖 **libdbus-1**。 |
| **权限透明** | 统一封装 AUMID（Win）、用户授权（macOS）、桌面环境管理（Linux）。 |
| **C++23 优先** | 使用 `std::expected`、`std::move_only_function`、`std::jthread`、`std::format`、`concepts`。 |
| **嵌入式** | 默认嵌入宿主进程；库内部维护一个后台事件线程，宿主无需事件泵。 |
| **单一配置类型** | 数据与回调聚合为唯一 `notification` 类型；`send()` 一次调用完成投递。 |
| **单头文件** | 全部公共 API 在 `whiz/notify.hpp` 内；实现编译为静态库。 |

### 1.2 非目标

- 不实现应用内通知 / 悬浮窗 / 通知中心自绘。
- 不提供通知历史持久化。
- 不管理应用生命周期。
- **v1 仅支持六项特性**（见 §1.3），其余一律不做，**不设路线图**。
- **不提供** `find_package` / CMake install / 系统包。
- **不提供** C++20 module。
- **不提供**项目内测试与 CI（测试/示例统一位于仓库根目录 `test/` 下）。

### 1.3 v1 功能清单（唯一定义）

| 特性 | 字段 | Win | macOS | Linux |
|---|---|---|---|---|
| 标题 | `title` | ✅ | ✅ | ✅ |
| 正文 | `body` | ✅ | ✅ | ✅ |
| 图标 | `icon` | ✅ | ✅ | ✅ |
| 按钮 | `actions` | ✅（≤3） | ✅ | ✅ |
| 超时 | `timeout` | ✅ | ✅ | ✅ |
| 替换键 | `tag` | ✅ | ✅ | ✅ |

**不包含**：紧急程度、静音、进度、输入框、附件、分组、声音选择、类别、定时投递。这些不在本库范围内。

---

## 2. 公共 API 设计

### 2.1 使用者视角

```cpp
#include <whiz/notify.hpp>
#include <cstdio>
#include <print>

int main() {
    if (whiz::notify::has_permission() == whiz::notify::permission_status::not_determined) {
        whiz::notify::request_permission([](whiz::notify::permission_status s) {
            std::println("permission -> {}", static_cast<int>(s));
        });
    }

    if (auto r = whiz::notify::send({
            .title     = "构建完成",
            .body      = "whiz-notify 已编译通过",
            .icon      = "/opt/whiz/icon.png",
            .timeout   = std::chrono::seconds{5},
            .tag       = "build-finished",
            .on_action = [](std::string_view action_id) {
                std::println("user clicked action: {}", action_id);
            },
            .on_closed = [](whiz::notify::close_reason reason) {
                std::println("notification closed, reason={}", static_cast<int>(reason));
            },
        }); !r) {
        std::println(stderr, "send failed: {}", r.error().message);
    }
}
```

### 2.2 类型定义

```cpp
namespace whiz::notify {

// ---- 关闭原因 ----
enum class close_reason {
    expired,            // 超时
    dismissed,          // 用户主动关闭
    replaced,           // 被同 tag 的后继通知替换
    unknown,
};

// ---- 权限状态 ----
enum class permission_status {
    not_determined,
    granted,
    denied,
    provisional,   // macOS 临时授权
    unsupported,   // 平台无权限模型，或后端不可用（详见 §2.4）
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
    std::string                              title;     // 必填
    std::string                              body;      // 可选
    std::optional<std::string>               icon;      // 本地路径或 file:// URI
    std::vector<action>                      actions;   // 按钮，上限 3
    std::optional<std::chrono::milliseconds> timeout;   // 不填用系统默认
    std::optional<std::string>               tag;       // 替换键

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
// 便于跨平台代码条件调用；行为差异见 §2.4。
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
```

### 2.3 设计决策

**为什么把数据与回调合并到单个 `notification`？**

`notification` 是“一条待投递的通知”，其内容与事件处理天然属于同一事务。合并且使用 aggregate + designated initializers 的好处：

1. **一次构造**：不再需要 `notification_options` + `builder` 两段式。
2. **一次调用**：`send(notification)` 取代 `send(...).on_action(...).show()` 的链式流程。
3. **顺序可读**：所有字段在调用点同屏出现，内容与回调一目了然。
4. **符合标准库风格**：`std::format`、`std::ranges::sort` 等都使用“配置对象 + 自由函数”的形式，而非建造者模式。

**为什么 `notification` 是聚合体？**

因为聚合体才能使用 designated initializers（P0329R4），这是 C++20 起官方推荐的“配置对象”写法。代价是**聚合体不能声明用户构造函数**，因此字段合法性只能在 `send()` 内校验，`notification` 本身可能处于非法状态。这是**有意为之**：类型只承载数据，语义校验集中在唯一入口。

**为什么回调是公共成员，而不是链式方法？**

链式方法会让 `send(...)` 返回一个临时 `builder`，调用点会变成多段链，且需要额外维护 builder 的生命周期。成员回调更直接。若未来 `std::execution`（P2300）在三大编译器上成熟，可考虑迁移到 sender/receiver 模型；v1 不引入。

**为什么 `notification` 是 move-only？**

因为 `std::move_only_function` 不可拷贝。这反而与“投递即消耗”的语义一致：`send(notification n)` 按值接收，调用方必须 move 或传临时量。

**为什么没有 `init()` / `shutdown()`？**

- **惰性初始化**：首次调用任意公共 API 时自动启动后台线程并建立平台连接。
- **进程退出**：库通过 `atexit` 钩子在静态析构阶段触发关闭流程（§5.3）。

这消除了“忘记 `shutdown()` 是否泄漏”“`init()` 失败如何重试”等心智负担。若宿主需要在 `main` 早期显式探测后端可用性，可主动 `send()` 一条不重要的通知并检查其返回值，或调用 `has_permission()`（其语义见 §2.4）。

**`errc` 与 `std::error_code` 的关系**

本库的 `errc` **不**与 `std::error_code` 互操作，之所以采用 `errc` 命名，是贴近 `std::errc` 的标准库风格，短且约定俗成。若未来需要与 `std::error_code` 集成，可后续添加 `make_error_code(errc)` 与对应 `error_category`。

### 2.4 边界与默认行为（v1 契约）

本节明确 v1 在边界情况下的确定性行为。这些是**契约**，后端实现必须遵守。

#### 2.4.1 `has_permission()`

- 首次调用会触发惰性初始化。若初始化失败（如 Linux 无 D-Bus session、macOS 非 bundle），返回 `permission_status::unsupported`。
- **注意**：`unsupported` 同时表示“平台无权限模型”和“后端不可用”两种语义。若使用者需要区分，应改用 `send()` 并检查 `std::expected`。
- 在 Windows / Linux 上，初始化成功后恒定返回 `granted`。
- `has_permission()` 内部捕获所有异常；任何失败返回 `permission_status::unsupported`，绝不抛出。

#### 2.4.2 `request_permission()`

- 在无权限模型的平台（Windows / Linux）：**立即**（在调用线程同步）以 `permission_status::unsupported` 调用回调，然后返回。
- 在 macOS：`requestAuthorizationWithOptions:`，回调被 marshal 到事件线程。
- 若事件线程尚未启动（首次调用），先完成惰性初始化再触发回调。
- 若回调为空（`nullptr`），静默忽略，不报错。

#### 2.4.3 `send()` 的同步校验

`send()` 在投递前**同步**校验以下字段，任一不满足即返回 `invalid_argument`：

| 字段 | 规则 |
|---|---|
| `title` | 不得为空字符串 |
| `actions` | `size() <= 3`；每个 `action.id` 非空；同一 `notification` 内 `action.id` 不得重复 |
| `timeout` | 若存在，必须 `> 0ms`；且 `<= INT_MAX` 毫秒（Linux `expire_timeout` 为 `int`） |
| `tag` | 若存在，不得为空字符串 |
| `actions[i].label` | 不得为空字符串 |

`icon`、`body` 不做同步校验；不合法的路径由后端在投递时决定忽略或报 `io_error`（详见 §6.3 静默降级）。

#### 2.4.4 缺少 `on_error` 时的默认行为

若使用者未提供 `on_error`（即字段为空），投递后的**异步错误**按下述处理：

- 打印到 `stderr`，格式：`whiz: error: <message>`
- 不抛出异常，不终止进程
- 库继续运行

#### 2.4.5 同 `tag` 替换时的 `on_closed` 语义

当相同 `tag` 的新通知替换旧通知时：

- 后端**主动**触发旧通知的 `on_closed(close_reason::replaced)`，并从内部 `id → callbacks` 映射中移除该条目。
- 后端随后若收到平台发来的旧通知关闭事件，**忽略**之，保证 `on_closed` 对同一通知至多触发一次（符合 §3.2 不变量 4）。
- 若旧通知没有 `on_closed`，仅移除映射，不打印。
- 替换顺序：先投递新通知；新通知投递成功后，再触发旧通知的 `on_closed(replaced)` 并移除映射。若新通知投递失败，旧通知及其回调保持不变。

#### 2.4.6 `platform::windows` 在非 Windows 平台的行为

- `register_aumid(...)`：返回 `std::unexpected{error{errc::not_supported, "AUMID is Windows-only"}}`。
- `set_aumid(...)`：静默忽略。

声明保留（不做 `#ifdef` 剥离），以便跨平台代码统一书写、条件调用。

---

## 3. 架构

```
┌──────────────────────────────────────────────────────────┐
│                    公共 API（whiz::notify）                │
│  send() / has_permission() / request_permission()          │
└───────────────────────────┬──────────────────────────────┘
                            │
┌───────────────────────────▼──────────────────────────────┐
│                     Core（平台无关）                       │
│  选项校验 │ 回调注册表 │ 事件循环(jthread)                 │
└───────────────────────────┬──────────────────────────────┘
                            │  backend 接口
        ┌───────────────────┼───────────────────┐
        ▼                   ▼                   ▼
┌───────────────┐   ┌───────────────┐   ┌───────────────┐
│ Windows 后端   │   │ macOS 后端     │   │ Linux 后端     │
│ WinRT Toast   │   │ UNUserNotif.  │   │ libdbus-1     │
└───────────────┘   └───────────────┘   └───────────────┘
```

### 3.1 后端抽象接口

```cpp
// src/backend.hpp（内部头，不导出）
namespace whiz::notify::detail {

struct backend {
    virtual ~backend() = default;

    virtual std::expected<void, error> init() = 0;
    virtual void shutdown() noexcept = 0;

    virtual permission_status permission() = 0;
    virtual void request_permission(
        std::move_only_function<void(permission_status)>) = 0;

    // 按值接收 notification：后端起投递职责，负责保存回调与平台 ID 的映射。
    // 实现必须满足 §2.4 的契约。
    virtual std::expected<uint64_t, error> send(notification n) = 0;
};

std::unique_ptr<backend> make_backend();

} // namespace whiz::notify::detail
```

> 由于 `notification` 是 move-only，后端按值接收是自然选择；后端在其内部维护 `id → callbacks` 的映射。

### 3.2 关键不变量

1. **单事件线程**：所有平台事件、所有用户回调，都在同一个 `std::jthread` 上派发。
2. **投递线程无关**：`send()` 可从任意线程调用。
3. **回调不重入**：事件线程串行执行用户回调；用户回调内可安全调用 `whiz::notify::send()`。
4. **`on_closed` 至多触发一次**；`on_action` 可多次。
5. **字段合法性统一在 `send()` 校验**：`notification` 本身可以是非法状态。
6. **`has_permission()` / `request_permission()` 可在任意线程调用**；语义分别见 §2.4.1 与 §2.4.2。

---

## 4. 平台后端设计

### 4.1 Windows（WinRT Toast）

- **API**：`winrt::Windows::UI::Notifications::ToastNotificationManager`。
- **线程**：事件线程以 STA 模式 `winrt::init_apartment`，运行 Win32 消息泵。
- **AUMID**：未打包应用需先 `register_aumid(...)`；打包应用跳过。
- **模板**：`ToastGeneric`，承载 title / body / icon / actions。
- **tag / replace**：映射到 `ToastNotification::Tag` + `Group`。
- **超时**：WinRT Toast 本身无“自动消失超时”字段；`timeout` 通过 `ToastNotification` 的 `ExpirationTime`（系统在指定时间后从 Action Center 移除）近似实现。`timeout` 为空时不设 `ExpirationTime`。
- **图标**：v1 仅 `file://`。
- **按钮**：`ToastButton`，上限 3（Windows 硬限制）。
- **权限**：恒定 `granted`。
- **替换语义**：使用相同 `Tag` + `Group` 时系统自动替换；旧通知的 `on_closed(replaced)` 由库在投递新通知时主动触发（§2.4.5）。

### 4.2 macOS（UNUserNotificationCenter）

- **API**：`UserNotifications.framework`（Objective-C++）。
- **Bundle 要求**：`[NSBundle mainBundle].bundleIdentifier` 为空则 `init()` 返回 `backend_unavailable`。
- **授权**：`requestAuthorizationWithOptions:`，回调 marshal 到 C++ 事件线程。无权限模型相关操作时立即回调 `unsupported`。
- **内容**：`UNMutableNotificationContent`；`userInfo` 携带 tag。
- **按钮**：`UNNotificationAction` + `UNNotificationCategory`。
- **超时**：`UNUserNotificationCenter` 无"自动消失"字段；`timeout` 通过事件线程上的定时器实现：投递时记录 `identifier` 与到期时间，到点调用 `removeDeliveredNotificationsWithIdentifiers:` 移除。`timeout` 为空时不注册定时器。
- **关闭原因**：由 response identifier 判定。
- **替换语义**：相同 `identifier` 即替换；旧通知的 `on_closed(replaced)` 由库在投递新通知时主动触发（§2.4.5）。
- **投递**：使用 `nil` trigger 立即投递（不延迟）；`timeout` 与投递延迟无关。
- **前台行为**：不覆盖系统默认（前台不显示横幅）。

### 4.3 Linux（libdbus-1 + org.freedesktop.Notifications）

- **依赖**：`libdbus-1`（`dbus-devel` / `libdbus-1-dev`）。
- **连接**：`dbus_bus_get(DBUS_BUS_SESSION, &err)`；失败 → `init()` 返回 `backend_unavailable`。
- **发送**：`org.freedesktop.Notifications.Notify`：

  ```
  Notify(app_name, replaces_id, app_icon, summary, body,
         actions, hints, expire_timeout) -> uint32
  ```

  - `actions`：扁平 `[id1, label1, id2, label2, ...]`。
  - `hints`：`{string, variant}` 数组；v1 仅写 `desktop-entry`（来自 `/proc/self/comm`）。
  - `expire_timeout`：毫秒，`int` 类型；由 §2.4.3 校验保证 `<= INT_MAX`。无 `timeout` 时传 `-1`（后端默认）。

- **替换逻辑**：库维护 `std::unordered_map<std::string, uint32_t>`（tag → 通知 id）。相同 tag 时取旧 id 作为 `replaces_id`；同时按 §2.4.5 主动触发旧通知的 `on_closed(replaced)` 并清除映射。
- **事件订阅**：`dbus_bus_add_match` 订阅：
  - `NotificationClosed(uint32 id, uint32 reason)`
  - `ActionInvoked(uint32 id, string action_key)`
- **事件循环**（`std::jthread` 内）：

  ```
  while (!stop_requested)
      dbus_connection_read_write_dispatch(conn, /*timeout_ms=*/50);
  ```

  跨线程任务投递通过内部 `mpsc_queue` + `eventfd` 唤醒。

- **线程安全**：`init()` 时调用 `dbus_threads_init_default()`；所有 D-Bus 调用只在事件线程执行。
- **app_name**：从 `/proc/self/comm` 读取。
- **无桌面环境**：`dbus_bus_get` 成功但 `Notify` 返回 `ServiceUnknown` → 通过 `on_error` 回调报告 `backend_unavailable`；`send()` 本身已成功返回（投递后异步错误，见 §6.1）。

### 4.4 后端依赖总览

| 平台 | 库 | 依赖 |
|---|---|---|
| Windows | WinRT | `WindowsApp.lib` |
| macOS | UserNotifications.framework | Foundation |
| Linux | **libdbus-1** | `dbus-1`（pkg-config: `dbus-1`） |

---

## 5. 线程与事件模型

### 5.1 事件线程

- 单个 `std::jthread`，命名 `"whiz-evt"`。
- 循环：
  - **Linux**：`dbus_connection_read_write_dispatch(conn, 50)` + 内部任务队列。
  - **Windows**：`MsgWaitForMultipleObjectsEx` + `PeekMessage` / `DispatchMessage`。
  - **macOS**：自定义 `dispatch_queue` + `dispatch_semaphore_wait`。
- 跨线程任务投递：内部 `mpsc_queue<task>` + 事件源唤醒（Linux: `eventfd`；Windows: `PostThreadMessage`；macOS: `dispatch_source_t`）。

### 5.2 用户回调执行保证

- 在事件线程上同步执行。
- 抛异常 → 捕获并打印 `stderr`，库继续。
- 阻塞 → 仅警告，不强制中断。

### 5.3 关闭流程

```
（首次调用任意公共 API）
  ├─ 惰性初始化：启动 jthread、建立平台连接、注册 atexit 钩子

（进程退出 / 静态析构）
  ├─ atexit 钩子触发
  ├─ 投递 shutdown task 到事件线程
  ├─ 事件线程：关闭平台连接、清空队列、退出循环
  ├─ jthread::request_stop()
  ├─ 等待事件线程退出，超时 100ms（条件变量 / std::promise::wait_for）
  └─ 超时则 detach 并标记已关闭
```

不提供公共的 `init()` / `shutdown()`。如宿主需要在 `main` 内提前释放资源（罕见场景），可借助 C++ 作用域（例如在有限作用域内使用通知），库随后依赖 `atexit` 完成收尾。

---

## 6. 错误处理策略

### 6.1 错误分类

| 阶段 | 传播方式 |
|---|---|
| 惰性初始化 / 平台连接 | `send()` 首次调用的返回 `std::expected<void, error>` |
| `send()` 同步失败 | `std::expected<void, error>`（含 §2.4.3 校验） |
| 投递后系统拒绝 | `on_error` 回调；若未提供，打印 `stderr`（§2.4.4） |
| 权限请求结果 | `request_permission` 回调 |

### 6.2 错误来源映射

| errc | Windows | macOS | Linux |
|---|---|---|---|
| `invalid_argument` | title 空 / 按钮超 3 个 / id 重复 / timeout 非法 | 同左 | summary 空 / 同左 |
| `permission_denied` | — | `UNAuthorizationStatusDenied` | — |
| `not_supported` | 选项不支持 / 非 Windows 平台调用 `register_aumid` | 同左 | hint 不支持 |
| `backend_unavailable` | AUMID 未注册 | 非 bundle | 无 D-Bus session / 服务缺失 |
| `io_error` | WinRT 异常 | NSError | libdbus 返回错误 |

### 6.3 静默降级

可选字段（icon / actions）在后端不支持时**不报错**，仅忽略并打印 `whiz: warning: ...`。

---

## 7. 目录结构

```
whiz-notify/
├── CMakeLists.txt
├── LICENSE                     # AGPL-3.0-or-later 全文
├── README.md
├── DESIGN.md                   # 设计文档（项目内唯一文档 md）
├── include/whiz/
│   └── notify.hpp              # 单头文件，全部公共 API
└── src/
    ├── api.cpp
    ├── event_loop.cpp
    ├── event_loop.hpp
    ├── backend.hpp
    ├── backend_windows.cpp
    ├── backend_macos.mm
    ├── backend_linux.cpp
    └── detail/
        ├── mpsc_queue.hpp
        └── string_util.hpp
```

> **测试与示例不在本项目目录内**，统一放在仓库根目录 `test/` 下（`test/examples/whiz-notify/`）。
> 本项目不内置 CI；如需集成测试，可在仓库 `test/` 目录中通过 FetchContent / add_subdirectory 拉取本库进行。

### 7.1 `whiz/notify.hpp` 必备标准库头

单头文件应自包含，至少包含以下标准库头：

```cpp
#include <chrono>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
```

实现文件按需补充 `<thread>`、`<mutex>`、`<atomic>` 等。

---

## 8. 构建与集成

### 8.1 CMake 顶层结构

```cmake
cmake_minimum_required(VERSION 3.28)
project(whiz-notify VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(whiz-notify STATIC 
    src/api.cpp
    src/event_loop.cpp
)

target_include_directories(whiz-notify PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_features(whiz-notify PUBLIC cxx_std_23)

if (WIN32)
    target_sources(whiz-notify PRIVATE src/backend_windows.cpp)
    target_link_libraries(whiz-notify PRIVATE windowsapp)
elseif (APPLE)
    enable_language(OBJCXX)
    target_sources(whiz-notify PRIVATE src/backend_macos.mm)
    target_link_libraries(whiz-notify PRIVATE
        "-framework UserNotifications"
        "-framework Foundation")
else()
    target_sources(whiz-notify PRIVATE src/backend_linux.cpp)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
    target_link_libraries(whiz-notify PRIVATE PkgConfig::DBUS)
endif()
```

### 8.2 使用者：FetchContent（唯一集成方式）

```cmake
include(FetchContent)
FetchContent_Declare(whiz-notify
    GIT_REPOSITORY https://github.com/your-org/whiz-notify.git
    GIT_TAG        v0.1.0)
FetchContent_MakeAvailable(whiz-notify)

target_link_libraries(my_app PRIVATE whiz-notify)
```

### 8.3 编译选项

- 库自身：`-Wall -Wextra -Wpedantic -Werror`（Debug）。
- 无内置测试/示例缓存选项；示例代码位于仓库根目录 `test/examples/whiz-notify/`，不参与本项目主构建。

### 8.4 平台依赖清单

| 平台 | 依赖 |
|---|---|
| Windows 10/11 | `WindowsApp.lib`（WinRT），MSVC 19.40+ |
| macOS 12+ | `UserNotifications.framework`，Apple Clang 16+ |
| Linux | `dbus-1`（`dbus-devel` / `libdbus-1-dev`），GCC 14+ |

---

## 9. 版本与稳定性

- 语义化版本 `MAJOR.MINOR.PATCH`。
- v0.x：API 可能在小版本内变动。
- v1.0 起：公共头中的类型、函数签名、枚举值遵循 semver。
- ABI 不稳定：单头 + 静态库，建议整体重建。
- 弃用：`[[deprecated]]` 保留至少一个 minor 版本。

---

## 10. 平台差异说明

这些是**平台本身**的行为差异，库不试图抹平，使用者在跨平台时需要知情：

| 方面 | Windows | macOS | Linux |
|---|---|---|---|
| 按钮上限 | 3 | 无硬限制（系统 UI 折叠） | 由后端决定 |
| 超时语义 | `ExpirationTime`，从 Action Center 移除 | `removeDeliveredNotificationsWithIdentifiers:` | `expire_timeout` |
| 替换（tag） | `Tag` + `Group` | `identifier` 相同即替换 | `replaces_id` |
| 图标 | `file://` | bundle 内资源或路径 | `app_icon` 名称或路径 |
| 回调时机 | 应用运行且 STA 存活时 | 应用运行且 bundle 有效时 | 事件循环运行期间 |

---

## 11. 许可证：AGPL-3.0-or-later 的下游影响

选择 **AGPL-3.0-or-later** 意味着：

1. **强 copyleft**：任何链接本库的宿主程序，若**通过网络向用户提供服务**，必须向这些用户提供完整对应源码（AGPL §13）。
2. **静态链接传染**：库以静态库形式被 FetchContent 编入宿主，宿主整体被视为衍生作品，必须以 AGPL 兼容许可发布。
3. **对商业闭源不友好**：闭源软件**不能**直接使用本库，除非获得作者单独的商业许可，或完全满足 AGPL 条款。
4. **FetchContent 不改变许可**：源码被拉入并编译，仍受 AGPL 约束。

建议在 `README.md` 顶部明确写出这段“下游须知”。

---

## 附录 A：完整最小示例

```cpp
// test/examples/whiz-notify/minimal.cpp
#include <whiz/notify.hpp>
#include <cstdio>
#include <print>
#include <thread>

int main() {
    if (auto r = whiz::notify::send({
            .title     = "构建完成",
            .body      = "耗时 3.2 秒，0 错误",
            .timeout   = std::chrono::seconds{5},
            .tag       = "build",
            .on_action = [](std::string_view id) {
                std::println("[callback] action: {}", id);
            },
            .on_closed = [](whiz::notify::close_reason r) {
                std::println("[callback] closed: {}", static_cast<int>(r));
            },
            .on_error = [](const whiz::notify::error& e) {
                std::println(stderr, "[error] {}: {}",
                             static_cast<int>(e.code), e.message);
            },
        }); !r) {
        std::println(stderr, "[send] {}: {}",
                     static_cast<int>(r.error().code), r.error().message);
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::seconds{2});
}
```

---

## 附录 B：Windows 未打包应用完整示例

```cpp
// test/examples/whiz-notify/windows_unpackaged.cpp
#include <whiz/notify.hpp>
#include <cstdio>
#include <print>

int main() {
    // 1) 注册 AUMID（仅未打包应用需要，一次即可）
    if (auto r = whiz::notify::platform::windows::register_aumid(
            "MyCompany.MyApp",
            "My App",
            "C:/Program Files/MyApp/icon.ico");
        !r) {
        std::println(stderr, "register_aumid failed: {}", r.error().message);
        return 1;
    }
    whiz::notify::platform::windows::set_aumid("MyCompany.MyApp");

    // 2) 正常发通知
    if (auto r = whiz::notify::send({
            .title = "Hello",
            .body  = "from unpackaged app",
        }); !r) {
        std::println(stderr, "send failed: {}", r.error().message);
        return 1;
    }
}
```

---

## 附录 C：术语表

| 术语 | 说明 |
|---|---|
| AUMID | Application User Model ID，Windows 应用身份标识 |
| Bundle | macOS `.app` 目录结构 |
| D-Bus | Linux 桌面进程间通信总线 |
| libdbus-1 | D-Bus 的参考 C 实现，本库 Linux 后端依赖 |
| STA | Single-Threaded Apartment，Windows COM 线程模型 |
| tag | 用户提供的字符串，用于替换同 tag 的通知 |
| aggregate | C++20 起的聚合体，可用 designated initializers 直接构造 |
| designated initializer | C++20 起的 `.field = value` 初始化语法 |

---

## 最终确认项

- 项目名称：**whiz-notify**。
- 命名空间：`whiz::notify`（含 `whiz::notify::platform::windows`）。
- 功能集：**仅**六项（标题、正文、图标、按钮、超时、tag），无路线图。
- 公共类型：`notification`（唯一配置类型）、`action`、`close_reason`、`permission_status`、`errc`、`error`。
- 公共函数：`send`、`has_permission`、`request_permission`，以及 `platform::windows` 下的 `register_aumid` / `set_aumid`。
- 返回值：所有**返回值的**公共函数均标记 `[[nodiscard]]`；错误用 `std::expected` 或 `on_error`。
- 边界契约：见 §2.4。
- 集成：仅 FetchContent。
- 头文件：单头 `whiz/notify.hpp`，自包含标准库头见 §7.1。
- 许可：AGPL-3.0-or-later。
- Linux 后端：libdbus-1。
- 测试与 CI：**不在本项目目录内**，位于仓库根目录 `test/` 下。