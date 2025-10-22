#pragma once

#if defined(_MSC_VER)

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN64) && _WIN64
    __declspec(dllimport) BOOL __stdcall IsDebuggerPresent();
#else
    __declspec(dllimport) int __stdcall IsDebuggerPresent();
#endif

#ifdef __cplusplus
}
#endif

static inline bool IsDebuggerAttached_Native()
{
    return IsDebuggerPresent() != 0;
}

#define DEBUG_BREAK() __debugbreak()

#elif defined(__ANDROID__)

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>

static inline bool IsDebuggerAttached_Native()
{
    // try reading /proc/self/status into memory
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0)
        return false;

    char buf[4096];
    ssize_t size = read(fd, buf, sizeof(buf) - 1);
    [[maybe_unused]]int rc = close(fd);
    assert(rc == 0);
    if (size <= 0)
        return false;
    buf[size] = '\0';

    // try finding TracerPid key, and skip after it
    const char keyword[11] = "TracerPid:";
    const char* substring = strstr(buf, keyword);
    if (!substring)
        return false;
    substring += sizeof(keyword) - 1;

    // non zero TracerPid means that there is a tracer present
    // so we check that first non-space character is in [1, 9]
    for (; substring <= buf + size; ++substring) {
        char c = *substring;
        if (isspace(c)) {
            continue;
        } else {
            return (c >= '1' && c <= '9');
        }
    }

    return false;
}

#ifdef __cplusplus
extern "C" {
#endif
int raise(int sig);
#ifdef __cplusplus
}
#endif

#define DETAIL_BASELIB_SIGTRAP 5
#define DEBUG_BREAK() raise(DETAIL_BASELIB_SIGTRAP)

#elif defined(__APPLE__)

#include <sys/sysctl.h>
#include <string.h>
#include <unistd.h>

static inline bool IsDebuggerAttached_Native()
{
    kinfo_proc info = {};

    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};

    size_t size = sizeof(info);
    sysctl(mib, sizeof(mib) / sizeof(*mib), &info, &size, nullptr, 0);

    return (info.kp_proc.p_flag & P_TRACED) != 0;
}

#define DEBUG_BREAK() __builtin_debugtrap()

#elif defined(__GNUC__)

// Some versions of GCC do provide __builtin_debugtrap, but it seems to be unreliable.
// See https://github.com/scottt/debugbreak/issues/13
#if defined(__i386__) || defined(__x86_64__)
#define DEBUG_BREAK() __asm__ volatile("int $0x03")
#elif defined(__thumb__)
#define DEBUG_BREAK() __asm__ volatile(".inst 0xde01")
#elif defined(__arm__) && !defined(__thumb__)
#define DEBUG_BREAK() __asm__ volatile(".inst 0xe7f001f0")
#elif defined(__aarch64__)
#define DEBUG_BREAK() __asm__ volatile(".inst 0xd4200000")
#endif

#endif
