#include "platform.h"

#if defined(_WIN32)
  #include <windows.h>
#elif defined(__APPLE__)
  #include <mach-o/dyld.h>
  #include <climits>
#else
  #include <unistd.h>
  #include <climits>
#endif

namespace platform {

std::string exe_path() {
#if defined(_WIN32)
    wchar_t wbuf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, wbuf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, &s[0], len, nullptr, nullptr);
    return s;
#elif defined(__APPLE__)
    char buf[PATH_MAX]; uint32_t sz = sizeof buf;
    if (_NSGetExecutablePath(buf, &sz) != 0) return {};   // buf too small -> give up
    return std::string(buf);
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::string(buf);
#endif
}

std::string exe_dir() {
    std::string p = exe_path();
    if (p.empty()) return ".";
#if defined(_WIN32)
    auto slash = p.find_last_of("/\\");
#else
    auto slash = p.find_last_of('/');
#endif
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

} // namespace platform
