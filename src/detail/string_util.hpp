#pragma once

// 内部字符串工具（header-only）。

#include <cstdio>
#include <string>

namespace whiz::notify::detail {

// 读取当前进程名（Linux 下取自 /proc/self/comm），
// 用作 D-Bus Notify 的 app_name 与 desktop-entry hint。
inline std::string read_proc_comm() {
#if defined(__linux__)
    std::FILE* f = std::fopen("/proc/self/comm", "r");
    if (!f) {
        return "whiz-notify";
    }
    char buffer[256]{};
    const std::size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, f);
    std::fclose(f);

    std::string name(buffer, n);
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
        name.pop_back();
    }
    return name.empty() ? "whiz-notify" : name;
#else
    return "whiz-notify";
#endif
}

} // namespace whiz::notify::detail
