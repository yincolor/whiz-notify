# whiz-notify

跨平台系统通知库（C++23 + CMake），为 Windows 10/11、macOS 和 GNU/Linux 提供统一的
通知发送与事件回调接口。库不依赖任何 GUI 框架（GTK/Qt/Saucer），仅通过 CMake
`FetchContent` 集成到宿主工程。

> ## 下游须知（AGPL-3.0-or-later）
>
> 本项目采用 **AGPL-3.0-or-later** 许可：
>
> 1. **强 copyleft**：任何链接本库的宿主程序，若**通过网络向用户提供服务**，
>    必须向这些用户提供完整对应源码（AGPL §13）。
> 2. **静态链接传染**：本库以静态库形式被 FetchContent 编入宿主，宿主整体被视为
>    衍生作品，必须以 AGPL 兼容许可发布。
> 3. **对商业闭源不友好**：闭源软件**不能**直接使用本库，除非获得作者单独的
>    商业许可，或完全满足 AGPL 条款。
> 4. **FetchContent 不改变许可**：源码被拉入并编译，仍受 AGPL 约束。

## 特性

- 跨平台：Windows 10/11、macOS 12+、GNU/Linux
- 统一 API：`whiz::notify::send(...)` 一个入口完成投递
- 原生协议：Windows WinRT Toast、macOS `UNUserNotificationCenter`、Linux
  `org.freedesktop.Notifications` D-Bus
- 零框架依赖：Linux 仅依赖 `libdbus-1`
- C++23：`std::expected`、`std::move_only_function`、`std::jthread`、`std::format` 等
- 嵌入式：内部维护一个后台事件线程（`whiz-evt`），宿主无需事件泵
- 单头文件：全部公共 API 在 `whiz/notify.hpp`

## v1 功能范围

仅支持六项特性（设计文档 §1.3）：

| 特性 | 字段 |
|---|---|
| 标题 | `title` |
| 正文 | `body` |
| 图标 | `icon` |
| 按钮 | `actions`（≤3） |
| 超时 | `timeout` |
| 替换键 | `tag` |

不包含紧急程度、静音、进度、输入框、附件、分组、声音选择、类别、定时投递。

## 环境要求

- C++23 编译器：GCC 14+ / Apple Clang 16+ / MSVC 19.40+
- CMake 3.28+
- Linux 开发包：`libdbus-1-dev`（`pkg-config: dbus-1`）
- Windows：`WindowsApp.lib`（WinRT）
- macOS：`UserNotifications.framework`

## 构建库

```bash
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

产物为 `libwhiz-notify.a`（Windows 下为 `.lib`）。

## 最小示例

```cpp
#include <whiz/notify.hpp>
#include <print>

int main() {
    if (auto r = whiz::notify::send({
            .title = "构建完成",
            .body  = "whiz-notify 已编译通过",
            .tag   = "build-finished",
            .on_closed = [](whiz::notify::close_reason reason) {
                std::println("closed: {}", static_cast<int>(reason));
            },
        });
        !r) {
        std::println("send failed: {}", r.error().message);
        return 1;
    }
}
```

## 集成（仅 FetchContent）

```cmake
include(FetchContent)
FetchContent_Declare(whiz-notify
    GIT_REPOSITORY https://github.com/your-org/whiz-notify.git
    GIT_TAG        v0.1.0)
FetchContent_MakeAvailable(whiz-notify)

target_link_libraries(my_app PRIVATE whiz-notify::whiz-notify)
```

> 不提供 `find_package` / CMake install / 系统包。

## 示例

示例代码不在本项目目录内，统一位于仓库根目录 `test/examples/whiz-notify/` 下。

## 文档

- 设计文档：[DESIGN.md](DESIGN.md)

## 许可证

本项目采用 [GNU Affero General Public License v3.0 (AGPL-3.0-or-later)](LICENSE)。
