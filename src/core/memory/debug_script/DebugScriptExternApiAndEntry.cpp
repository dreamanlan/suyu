#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <cstdint>
#include <cstddef>
#include <iomanip>
#include <cctype>

#include "DbgScpHook.h"
#include "DebugScriptVM.h"
#include "GpuCaptureManager.h"

#define DBGSCP_ON_MYUZU

#if defined(DBGSCP_ON_UNREAL)

#include "CoreMinimal.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

#endif

#if defined(_MSC_VER)
#include "windows.h"
#elif defined(__ANDROID__)
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <dlfcn.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <mach/mach_init.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <dlfcn.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <dlfcn.h>
#endif

#if defined(__APPLE__)
#include <execinfo.h>
#elif defined(UNITY_ANDROID) && UNITY_ANDROID
#include "PlatformDependent/AndroidPlayer/Source/AndroidBacktrace.h"
#elif defined(UNITY_SWITCH) && UNITY_SWITCH
#include "PlatformDependent/Switch/Source/Diagnostics/SwitchBacktrace.h"
#elif defined(UNITY_LUMIN) && UNITY_LUMIN
#include "PlatformDependent/Lumin/Source/LuminBacktrace.h"
#elif defined(UNITY_PLAYSTATION) && UNITY_PLAYSTATION
#include "PlatformDependent/SonyCommon/Player/Native/PlayStationStackTrace.h"
#elif defined(__ANDROID__)
#if __ANDROID_API__ >= 33
#include <execinfo.h>
#else
#define BACKTRACE_UNIMPLEMENTED 1

#include <iomanip>
#include <iostream>
#include <unwind.h>

namespace {
    struct BacktraceState {
        void** current;
        void** end;
    };
    static _Unwind_Reason_Code unwindCallback(struct _Unwind_Context* context, void* arg) {
        BacktraceState* state = static_cast<BacktraceState*>(arg);
        uintptr_t pc = _Unwind_GetIP(context);
        if (pc) {
            if (state->current == state->end) {
                return _URC_END_OF_STACK;
            }
            else {
                *state->current++ = reinterpret_cast<void*>(pc);
            }
        }
        return _URC_NO_REASON;
    }
    int captureBacktrace(void** buffer, size_t max) {
        BacktraceState state = { buffer, buffer + max };
        _Unwind_Backtrace(unwindCallback, &state);
        return static_cast<int>(state.current - buffer);
    }
} // namespace
#endif
#else
#define BACKTRACE_UNIMPLEMENTED 1
#if defined(_MSC_VER)
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
// for old windows before vista
void captureStack(DWORD64 stackAddr[], int maxDepth) {
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    CONTEXT context;
    RtlCaptureContext(&context);

    SymInitialize(process, NULL, TRUE);

    STACKFRAME64 stackFrame;
    ZeroMemory(&stackFrame, sizeof(STACKFRAME64));

#if defined(_M_IX86)
    DWORD machineType = IMAGE_FILE_MACHINE_I386;
    stackFrame.AddrPC.Offset = context.Eip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = context.Ebp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = context.Esp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_X64)
    DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
    stackFrame.AddrPC.Offset = context.Rip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = context.Rsp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = context.Rsp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_IA64)
    DWORD machineType = IMAGE_FILE_MACHINE_IA64;
    stackFrame.AddrPC.Offset = context.StIIP;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = context.IntSp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrBStore.Offset = context.RsBSP;
    stackFrame.AddrBStore.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = context.IntSp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
#endif

    int ix = 0;
    while (StackWalk64(machineType, process, thread, &stackFrame, &context, NULL,
        SymFunctionTableAccess64, SymGetModuleBase64, NULL) && ix < maxDepth) {
        stackAddr[ix++] = stackFrame.AddrPC.Offset;
    }

    SymCleanup(process);
}
#endif
#endif

#if defined(DBGSCP_ON_UNITY) // for unity
#include "Runtime/Logging/LogAssert.h"
int mylog_printf(const char* fmt, ...) {
    va_list vl;
    va_start(vl, fmt);
    printf_consolev(kLogTypeWarning, fmt, vl);
    va_end(vl);
    return 1;
}
#elif defined(DBGSCP_ON_UNREAL)
int mylog_printf(const char* fmt, ...) {
    const int c_buf_size = 1024 * 4 + 1;
    char buf[c_buf_size];
    va_list vl;
    va_start(vl, fmt);
    int r = std::vsnprintf(buf, c_buf_size, fmt, vl);
    va_end(vl);
    // Guard against vsnprintf returning negative (error) or exceeding buffer size
    if (r < 0) {
        r = 0;
    } else if (r >= c_buf_size) {
        r = c_buf_size - 1;
    }
    buf[r] = '\0';
    UE_LOG(LogTemp, Warning, TEXT("%s"), UTF8_TO_TCHAR(buf));
    return r;
}
#elif defined(DBGSCP_ON_MYUZU)
int mylog_printf(const char* fmt, ...) {
    const int c_buf_size = 1024 * 4 + 1;
    char buf[c_buf_size];
    va_list vl;
    va_start(vl, fmt);
    int r = std::vsnprintf(buf, c_buf_size, fmt, vl);
    va_end(vl);
    // Guard against vsnprintf returning negative (error) or exceeding buffer size
    if (r < 0) {
        r = 0;
    } else if (r >= c_buf_size) {
        r = c_buf_size - 1;
    }
    buf[r] = '\0';
    std::stringstream ss;
    ss << buf;
    Core::g_MainThreadCaller.SyncLogToView(ss.str());

    LOG_DBGSCP(Log, "{}", buf);
    return r;
}
#else
int mylog_printf(const char* fmt, ...) {
    const int c_buf_size = 1024 * 4 + 1;
    char buf[c_buf_size];
    va_list vl;
    va_start(vl, fmt);
    int r = std::vsnprintf(buf, c_buf_size, fmt, vl);
    va_end(vl);
    // Guard against vsnprintf returning negative (error) or exceeding buffer size
    if (r < 0) {
        r = 0;
    } else if (r >= c_buf_size) {
        r = c_buf_size - 1;
    }
    buf[r] = '\0';
    printf("%s", buf);
    return r;
}
#endif

void mylog_dump_callstack(const char* prefix, const char* file, int line) {
    mylog_printf("%s%s:%d\n", prefix, file, line);

#if defined(BACKTRACE_UNIMPLEMENTED) && BACKTRACE_UNIMPLEMENTED
#if defined(__ANDROID__)
    const size_t kMaxDepth = 100;
    void* buffer[kMaxDepth];
    int count = captureBacktrace(buffer, kMaxDepth);

    for (int idx = 0; idx < count; ++idx) {
        const void* addr = buffer[idx];
        const char* symbol = "";

        Dl_info info;
        if (dladdr(addr, &info) && info.dli_sname) {
            symbol = info.dli_sname;
        }

        mylog_printf(" #%02d %p %s\n", idx, addr, symbol);
    }
#elif defined(_MSC_VER)
    const size_t kMaxDepth = 100;

    HANDLE process = GetCurrentProcess();
    SymInitialize(process, NULL, TRUE);

    void* stack[kMaxDepth];
    USHORT frames = CaptureStackBackTrace(0, kMaxDepth, stack, NULL);

    SYMBOL_INFO* symbol = (SYMBOL_INFO*)malloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char));
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

    for (USHORT i = 0; i < frames; ++i) {
        SymFromAddr(process, (DWORD64)(stack[i]), 0, symbol);
        mylog_printf(" #%02d %p %s\n", i, symbol->Address, symbol->Name);
    }

    free(symbol);
#endif
#else
    const size_t kMaxDepth = 100;
    void* stackAddr[kMaxDepth];
    int stackDepth = (int)backtrace(stackAddr, kMaxDepth);
    char** stackSymbol = backtrace_symbols(stackAddr, stackDepth);

    for (int i = 1, n = stackDepth; i < n; ++i)
        mylog_printf(" #%02d %p %s\n", i - 1, stackAddr[i], stackSymbol[i]);
    free(stackSymbol);
#endif
}
void mylog_assert(bool v) {
#if defined(UNITY_WIN) && UNITY_WIN // for unity
    DebugAssert(v);
#elif defined(_MSC_VER)
    _ASSERT(v);
#elif defined(DBGSCP_ON_UNITY) // for unity
    DebugAssert(v);
#else
    assert(v);
#endif
}

static inline int64_t GetProcessId() {
#if defined(_MSC_VER)
    return static_cast<int64_t>(GetCurrentProcessId());
#elif defined(__APPLE__) || defined(__ANDROID__)
    return static_cast<int64_t>(getpid());
#else
    return 0;
#endif
}
static inline int64_t GetThreadId() {
#if defined(_MSC_VER)
    return static_cast<int64_t>(GetCurrentThreadId());
#elif defined(__ANDROID__)
    return static_cast<int64_t>(gettid());
#elif defined(__APPLE__)
    //thread_t mach_tid = mach_thread_self(); // returns a send right (thread_t)
    //int64_t tid = static_cast<int64_t>(mach_tid);
    //mach_port_deallocate(mach_task_self(), mach_tid);
    //return tid;

    //pthread_t pt = pthread_self();
    //return static_cast<int64_t>(pthread_mach_thread_np(pt));

    /*
    thread_t mach_tid = mach_thread_self();
    thread_identifier_info_data_t tidinfo;
    mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
    kern_return_t kr = thread_info(mach_tid, THREAD_IDENTIFIER_INFO,
                                   reinterpret_cast<thread_info_t>(&tidinfo), &count);
    uint64_t ti_thread_handle = 0, ti_thread_id = 0;
    if (kr == KERN_SUCCESS) {
        ti_thread_handle = tidinfo.thread_handle;
        ti_thread_id = tidinfo.thread_id;
    }
    mach_port_deallocate(mach_task_self(), mach_tid);
    return static_cast<int64_t>(ti_thread_id);
    */

    uint64_t pthread_uid = 0;
    pthread_threadid_np(NULL, &pthread_uid);
    return static_cast<int64_t>(pthread_uid);
#else
    return 0;
#endif
}

static void get_errno_message(int err, char* buf, size_t buflen) {
    if (buflen == 0)
        return;
#if defined(_WIN32)
    if (strerror_s(buf, buflen, err) != 0) {
        // fallback
        snprintf(buf, buflen, "Unknown error %d", err);
    }
#else
#if defined(GLIBC) && defined(_GNU_SOURCE)
    char* msg = strerror_r(err, buf, buflen);
    if (msg) {
        strncpy(buf, msg, buflen);
        buf[buflen - 1] = '\0';
    } else {
        snprintf(buf, buflen, "Unknown error %d", err);
    }
#else
    if (strerror_r(err, buf, buflen) != 0) {
        snprintf(buf, buflen, "Unknown error %d", err);
    }
#endif
#endif
}
static FILE* open_file_with_error(const char* path, const char* mode, int& err, char* errbuf,
                                  size_t errbufSize) {
    err = 0;
    if (errbuf && errbufSize > 0)
        errbuf[0] = '\0';

#if defined(_WIN32)
    FILE* f = NULL;
    errno_t er = fopen_s(&f, path, mode);
    if (er != 0) {
        err = (int)er;
        if (errbuf)
            get_errno_message(err, errbuf, errbufSize);
        return NULL;
    }
    return f;
#else
    FILE* f = fopen(path, mode);
    if (!f) {
        err = errno;
        if (errbuf)
            get_errno_message(err, errbuf, errbufSize);
        return NULL;
    }
    return f;
#endif
}

struct WatchPointCommandInfo {
    short cmd;  // 0--nothing 1--add 2--remove
    short flag; // 0--nothing 1--read 2--write 3--readwrite
    int size;
    int64_t addr;
    int64_t tid;
};

static std::recursive_mutex g_WatchPointMutex{};
WatchPointCommandInfo g_WatchPointCommandInfo{0, 0, 0, 0, 0};

static void DbgScp_SetWatchPoint(short cmd, short flag, int size, int64_t addr, int64_t tid) {
    std::lock_guard<std::recursive_mutex> lock(g_WatchPointMutex);

    g_WatchPointCommandInfo.cmd = cmd;
    g_WatchPointCommandInfo.flag = flag;
    g_WatchPointCommandInfo.size = size;
    g_WatchPointCommandInfo.addr = addr;
    g_WatchPointCommandInfo.tid = tid;
}
static short DbgScp_GetWatchPoint(short& flag, int& size, int64_t& addr, int64_t& tid) {
    std::lock_guard<std::recursive_mutex> lock(g_WatchPointMutex);

    flag = g_WatchPointCommandInfo.flag;
    size = g_WatchPointCommandInfo.size;
    addr = g_WatchPointCommandInfo.addr;
    tid = g_WatchPointCommandInfo.tid;

    return g_WatchPointCommandInfo.cmd;
}

static const int c_max_log_file_num = 16;
static const int c_max_log_file_size = 1536 * 1024 * 1024;
static const std::streamsize c_log_buffer_size = 8 * 1024 * 1024;
static int g_LogIndex = 0;
static bool g_FirstLog = true;
static uint64_t g_LogSize = 0;

static inline std::vector<char>& GetSwapBufferRef() {
    static std::vector<char> s_SwapBuffer(c_log_buffer_size);
    return s_SwapBuffer;
}
static inline std::stringstream& GetLogBufferRef() {
    static std::stringstream s_LogBuffer;
    return s_LogBuffer;
}
static inline std::recursive_mutex& GetLogBufferMutexRef() {
    static std::recursive_mutex s_LogBufferMutex;
    return s_LogBufferMutex;
}
static inline std::string* GetLogFilesRef() {
#if defined(DBGSCP_COMPILER_TEST)
    static std::string s_LogFile[c_max_log_file_num] = {"dbgscp_log_0.txt",
                                                        "dbgscp_log_1.txt",
                                                        "dbgscp_log_2.txt",
                                                        "dbgscp_log_3.txt"
                                                        "dbgscp_log_4.txt",
                                                        "dbgscp_log_5.txt",
                                                        "dbgscp_log_6.txt",
                                                        "dbgscp_log_7.txt",
                                                        "dbgscp_log_8.txt",
                                                        "dbgscp_log_9.txt",
                                                        "dbgscp_log_10.txt",
                                                        "dbgscp_log_11.txt",
                                                        "dbgscp_log_12.txt",
                                                        "dbgscp_log_13.txt",
                                                        "dbgscp_log_14.txt",
                                                        "dbgscp_log_15.txt"};
#else
    static std::string s_LogFile[c_max_log_file_num];
#endif
    return s_LogFile;
}

static int DbgScp_FlushLog_NoLock(const char* pstr, size_t len) {
    int err = -1;
    if (g_LogIndex < c_max_log_file_num) {
        char errmsg[256];
        FILE* fp = open_file_with_error(GetLogFilesRef()[g_LogIndex].c_str(),
                                        g_FirstLog ? "wt" : "at", err, errmsg, sizeof(errmsg));
        if (fp) {
            auto&& pos = GetLogBufferRef().tellp();
            g_LogSize += pos;

            std::streampos curPos = 0;
            GetLogBufferRef().seekg(curPos, std::ios::beg);
            while (curPos < pos) {
                std::streamsize size = pos - curPos;
                if (size > c_log_buffer_size) {
                    size = c_log_buffer_size;
                }
                GetLogBufferRef().read(GetSwapBufferRef().data(), size);
                fwrite(GetSwapBufferRef().data(), 1, size, fp);
                curPos += size;
            }
            if (pstr) {
                fwrite(pstr, 1, len, fp);
                g_LogSize += len;
            }
            fclose(fp);
            GetLogBufferRef().str("");
            g_FirstLog = false;
            err = 0;

            if (g_LogSize >= c_max_log_file_size) {
                g_FirstLog = true;
                g_LogIndex = (g_LogIndex + 1) % c_max_log_file_num;
                g_LogSize = 0;
            }
        } else {
            mylog_printf("open file failed: %s, error:%s\n", GetLogFilesRef()[g_LogIndex].c_str(),
                         errmsg);
        }
    }
    return err;
}
static int DbgScp_FlushLog() {
    std::lock_guard<std::recursive_mutex> lock(GetLogBufferMutexRef());

    return DbgScp_FlushLog_NoLock(nullptr, 0);
}
static int DbgScp_WriteLog(const std::string& str) {
    std::lock_guard<std::recursive_mutex> lock(GetLogBufferMutexRef());

    int r = 0;
    auto&& pos = GetLogBufferRef().tellp();
    size_t sizeInBuffer = pos + static_cast<std::streamoff>(str.length());
    if (sizeInBuffer > c_log_buffer_size) {
        r = DbgScp_FlushLog_NoLock(str.c_str(), str.length());
    } else {
        GetLogBufferRef() << str;
        r = static_cast<int>(sizeInBuffer);
    }
    return r;
}
static void DbgScp_LogCallstack(const char* prefix, const char* file, int line) {
    const int c_buf_size = 1024 * 4 + 1;
    char buf[c_buf_size];
    snprintf(buf, c_buf_size, "%s%s:%d\n", prefix, file, line);
    DbgScp_WriteLog(buf);

#if defined(BACKTRACE_UNIMPLEMENTED) && BACKTRACE_UNIMPLEMENTED
#if defined(__ANDROID__)
    const size_t kMaxDepth = 100;
    void* buffer[kMaxDepth];
    int count = captureBacktrace(buffer, kMaxDepth);

    for (int idx = 0; idx < count; ++idx) {
        const void* addr = buffer[idx];
        const char* symbol = "";

        Dl_info info;
        if (dladdr(addr, &info) && info.dli_sname) {
            symbol = info.dli_sname;
        }

        snprintf(buf, c_buf_size, " #%02d %p %s\n", idx, addr, symbol);
        DbgScp_WriteLog(buf);
    }
#endif
#else
    const size_t kMaxDepth = 100;
    void* stackAddr[kMaxDepth];
    int stackDepth = (int)backtrace(stackAddr, kMaxDepth);
    char** stackSymbol = backtrace_symbols(stackAddr, stackDepth);

    for (int i = 1, n = stackDepth; i < n; ++i) {
        snprintf(buf, c_buf_size, " #%02d %p %s\n", i - 1, stackAddr[i], stackSymbol[i]);
        DbgScp_WriteLog(buf);
    }
    free(stackSymbol);
#endif
}

static inline std::unordered_map<int64_t, int64_t>& GetMemoryFlagsRef() {
    static std::unordered_map<int64_t, int64_t> s_MemoryFlags;
    return s_MemoryFlags;
}
static inline std::recursive_mutex& GetMemoryFlagMutexRef() {
    static std::recursive_mutex s_MemoryFlagMutex;
    return s_MemoryFlagMutex;
}

static inline bool DbgScp_AddMemoryFlag(int64_t addr, int64_t flag) {
    std::lock_guard<std::recursive_mutex> lock(GetMemoryFlagMutexRef());

    auto&& r = GetMemoryFlagsRef().insert(std::make_pair(addr, flag));
    return r.second;
}
static inline bool DbgScp_RemoveMemoryFlag(int64_t addr) {
    std::lock_guard<std::recursive_mutex> lock(GetMemoryFlagMutexRef());

    auto&& r = GetMemoryFlagsRef().erase(addr);
    return r > 0;
}
static inline bool DbgScp_GetMemoryFlag(int64_t addr, int64_t& flag) {
    std::lock_guard<std::recursive_mutex> lock(GetMemoryFlagMutexRef());

    bool r = false;
    auto&& it = GetMemoryFlagsRef().find(addr);
    if (it != GetMemoryFlagsRef().end()) {
        flag = it->second;
        r = true;
    }
    return r;
}

extern "C" void FlushDbgScpLog() {
    DbgScp_FlushLog();
}

[[maybe_unused]]
static inline void DbgScp_Set(int cmd, int a, double b, const char* c) {
    BEGIN_DBGSCP_HOOK_VOID()

    mylog_printf("DbgScp_Set cmd:%d a:%d b:%f c:%s\n", cmd, a, b, c);

    END_DBGSCP_HOOK_VOID("DbgScp_Set", cmd, a, b, c)
}
[[maybe_unused]]
static inline int DbgScp_Get(int cmd, int a, double b, const char* c) {
    BEGIN_DBGSCP_HOOK()

    mylog_printf("DbgScp_Get cmd:%d a:%d b:%f c:%s\n", cmd, a, b, c);
    return 0;

    END_DBGSCP_HOOK("DbgScp_Get", int, cmd, a, b, c)
}

[[maybe_unused]]
static inline int TestMacro1(int a, double b, const char* c) {
    DBGSCP_HOOK("TestMacro1", int, a, b, c)
    mylog_printf("TestMacro1 a:%d b:%f c:%s\n", a, b, c);
    return 0;
}
[[maybe_unused]]
static inline int TestMacro2(int a, double b, const char* c) {
    BEGIN_DBGSCP_HOOK()
    mylog_printf("TestMacro2 a:%d b:%f c:%s\n", a, b, c);
    return 0;
    END_DBGSCP_HOOK("TestMacro2", int, a, b, c)
}
[[maybe_unused]]
static inline void TestMacro3(int a, double b, const char* c) {
    DBGSCP_HOOK_VOID("TestMacro3", a, b, c)
    mylog_printf("TestMacro3 a:%d b:%f c:%s\n", a, b, c);
}
[[maybe_unused]]
static inline void TestMacro4(int a, double b, const char* c) {
    BEGIN_DBGSCP_HOOK_VOID()
    mylog_printf("TestMacro4 a:%d b:%f c:%s\n", a, b, c);
    END_DBGSCP_HOOK_VOID("TestMacro4", a, b, c)
}

int TestFFI0(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, float f1, float f2,
             int64_t sv1, int64_t sv2) {
    mylog_printf("%d %d %d %d %d %d %d %d %f %f %lld %lld\n", a1, a2, a3, a4, a5, a6, a7, a8, f1,
                 f2, sv1, sv2);
    return 0;
}
int64_t TestFFI1(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6, int64_t a7,
                 int64_t a8, double f1, double f2, int64_t sv1, int64_t sv2) {
    mylog_printf("%lld %lld %lld %lld %lld %lld %lld %lld %f %f %lld %lld\n", a1, a2, a3, a4, a5,
                 a6, a7, a8, f1, f2, sv1, sv2);
    return 1;
}

[[maybe_unused]]
static inline std::vector<std::string> string_split(const std::string& input, char delimiter,
                                                    int max_fields) {
    std::istringstream input_stream(input);
    std::vector<std::string> tokens;
    std::string token;

    while (std::getline(input_stream, token, delimiter)) {
        if (token.length() > 0) {
            tokens.push_back(token);
        }
        if (static_cast<int>(tokens.size()) >= max_fields) {
            break;
        }
    }

    return tokens;
}

static inline int64_t test_find_loaded_segments(int64_t pid, const std::string& so,
                                                const std::string& attr, bool incFirst,
                                                std::string& match,
                                                std::vector<std::string>& fields) {
    const char* txt = "72c9804000-72c9900000 r--p 00000000 fe:0c 31612032                       "
                      "/vendor/lib64/hw/vulkan.adreno.so\n"
                      "72c9900000-72c9ba1000 r-xp 000fc000 fe:0c 31612032                       "
                      "/vendor/lib64/hw/vulkan.adreno.so\n"
                      "72c9ba1000-72c9bb1000 r--p 0039d000 fe:0c 31612032                       "
                      "/vendor/lib64/hw/vulkan.adreno.so\n"
                      "72c9bb1000-72c9bd2000 rw-p 003ac000 fe:0c 31612032                       "
                      "/vendor/lib64/hw/vulkan.adreno.so\n"
                      "754670a000-754671e000 r--p 00000000 fe:09 19286849                       "
                      "/system/lib64/libvulkan.so\n"
                      "754671e000-7546737000 r-xp 00014000 fe:09 19286849                       "
                      "/system/lib64/libvulkan.so\n"
                      "7546737000-7546739000 r--p 0002d000 fe:09 19286849                       "
                      "/system/lib64/libvulkan.so\n"
                      "7546739000-754673a000 rw-p 0002e000 fe:09 19286849                       "
                      "/system/lib64/libvulkan.so\n";
    std::istringstream input_stream(txt);

    std::string line;
    while (std::getline(input_stream, line)) {
        // 72d2e1b000-72d2f17000 r--p 00000000 fe:0c 31612032 /vendor/lib64/hw/vulkan.adreno.so
        if (line.find(so) != std::string::npos) {
            auto&& strs = string_split(line, ' ', 3);
            if (strs.size() >= 3) {
                if (strs[1] == attr) {
                    int64_t offset = std::stoll(strs[2], nullptr, 16);
                    if (incFirst || offset > 0) {
                        size_t pos = strs[0].find_first_of('-');
                        auto&& startStr = strs[0].substr(0, pos);
                        match = line;
                        fields.assign(strs.cbegin(), strs.cend());
                        return std::stoll(startStr, nullptr, 16);
                    }
                }
            }
        }
    }
    return 0;
}

static std::unordered_map<std::string, void*> g_Lib2Addresses{};

static inline void* LoadDynamicLibrary(const std::string& str) {
    void* ptr = 0;
    auto&& it = g_Lib2Addresses.find(str);
    if (it == g_Lib2Addresses.end()) {
#if defined(_MSC_VER)
        ptr = LoadLibraryA(str.c_str());
#else
        ptr = dlopen(str.c_str(), RTLD_LAZY);
#endif
        if (ptr) {
            g_Lib2Addresses.insert(std::make_pair(str, ptr));
        }
    } else {
        ptr = it->second;
    }
    return ptr;
}
static inline void* LookupSymbol(void* module, const std::string& str) {
#if defined(_MSC_VER)
    return GetProcAddress(reinterpret_cast<HMODULE>(module), str.c_str());
#else
    return dlsym(module, str.c_str());
#endif
}
static inline void* LoadAndLookupSymbol(const std::string& lib, const std::string& proc) {
    void* ptr = 0;
    auto&& it = g_Lib2Addresses.find(lib);
    if (it == g_Lib2Addresses.end()) {
#if defined(_MSC_VER)
        ptr = LoadLibraryA(lib.c_str());
#else
        ptr = dlopen(lib.c_str(), RTLD_LAZY);
#endif
        if (ptr) {
            g_Lib2Addresses.insert(std::make_pair(lib, ptr));
        }
    } else {
        ptr = it->second;
    }
    if (ptr) {
#if defined(_MSC_VER)
        auto&& p = GetProcAddress(reinterpret_cast<HMODULE>(ptr), proc.c_str());
#else
        auto&& p = dlsym(ptr, proc.c_str());
#endif
        return p;
    }
    return nullptr;
}
static inline void UnloadDynamicLibrary(void* module) {
#if defined(_MSC_VER)
    FreeLibrary(reinterpret_cast<HMODULE>(module));
#else
    dlclose(module);
#endif
}
static inline bool UnloadDynamicLibrary(const std::string& str) {
    bool r = false;
    auto&& it = g_Lib2Addresses.find(str);
    if (it != g_Lib2Addresses.end()) {
#if defined(_MSC_VER)
        FreeLibrary(reinterpret_cast<HMODULE>(it->second));
#else
        dlclose(it->second);
#endif
        g_Lib2Addresses.erase(it);
        r = true;
    }
    return r;
}
static inline int64_t find_loaded_segments(int64_t pid, const std::string& so,
                                           const std::string& attr, bool incFirst,
                                           std::string& match, std::vector<std::string>& fields) {
    std::ostringstream path;
    path << "/proc/" << pid << "/maps";

    std::string proc_file = path.str();
    std::ifstream maps_file(proc_file);
    if (!maps_file.is_open()) {
        mylog_printf("[seg]:can't open '%s'\n", proc_file.c_str());
        // return 0;
        return test_find_loaded_segments(pid, so, attr, incFirst, match, fields);
    }

    std::string line;
    while (std::getline(maps_file, line)) {
        // 72d2e1b000-72d2f17000 r--p 00000000 fe:0c 31612032 /vendor/lib64/hw/vulkan.adreno.so
        if (line.find(so) != std::string::npos) {
            auto&& strs = string_split(line, ' ', 3);
            if (strs.size() >= 3) {
                if (strs[1] == attr) {
                    int64_t offset = std::stoll(strs[2], nullptr, 16);
                    if (incFirst || offset > 0) {
                        size_t pos = strs[0].find_first_of('-');
                        auto&& startStr = strs[0].substr(0, pos);
                        match = line;
                        fields.assign(strs.cbegin(), strs.cend());
                        return std::stoll(startStr, nullptr, 16);
                    }
                }
            } else {
                mylog_printf("[seg]:unexpected format: '%s'\n", line.c_str());
            }
        }
    }
    return 0;
}
static inline size_t GetPagetSize() {
#if defined(_MSC_VER)
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return static_cast<size_t>(sysInfo.dwPageSize);
#elif defined(__APPLE__) || defined(__ANDROID__)
    return static_cast<size_t>(getpagesize());
#else
    return 4096;
#endif
}
static inline int64_t AlignToPageSize(int64_t addr, size_t pageSize) {
    return static_cast<int64_t>(static_cast<size_t>(addr) & ~(pageSize - 1));
}
static inline size_t RoundToPageSize(size_t size, size_t pageSize) {
    return (size + pageSize - 1) & ~(pageSize - 1);
}
static inline void SetMemoryProtect(int64_t addr, size_t size, size_t pageSize, int32_t quickflag,
                                    int32_t rawflag) {
    addr = AlignToPageSize(addr, pageSize);
    size = RoundToPageSize(size, pageSize);
    /*
#define PAGE_NOACCESS           0x01
#define PAGE_READONLY           0x02
#define PAGE_READWRITE          0x04
#define PAGE_WRITECOPY          0x08
#define PAGE_EXECUTE            0x10
#define PAGE_EXECUTE_READ       0x20
#define PAGE_EXECUTE_READWRITE  0x40
#define PAGE_EXECUTE_WRITECOPY  0x80
#define PAGE_GUARD             0x100
#define PAGE_NOCACHE           0x200
#define PAGE_WRITECOMBINE      0x400
    */
    /*
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define PROT_SEM 0x8
#define PROT_NONE 0x0
#define PROT_GROWSDOWN 0x01000000
#define PROT_GROWSUP 0x02000000
    */
#if defined(_MSC_VER)
    DWORD flag = rawflag;
    if (quickflag >= 0) {
        switch (quickflag) {
        case 0:
            flag = PAGE_NOACCESS;
            break;
        case 1:
            flag = PAGE_READONLY;
            break;
        case 2:
            flag = PAGE_READWRITE;
            break;
        case 3:
            flag = PAGE_EXECUTE_READWRITE;
            break;
        }
    }
    DWORD oldProtect;
    VirtualProtect(reinterpret_cast<void*>(addr), size, flag, &oldProtect);
#elif defined(__ANDROID__) || defined(__APPLE__)
    int flag = rawflag;
    if (quickflag >= 0) {
        switch (quickflag) {
        case 0:
            flag = PROT_NONE;
            break;
        case 1:
            flag = PROT_READ;
            break;
        case 2:
            flag = PROT_READ | PROT_WRITE;
            break;
        case 3:
            flag = PROT_READ | PROT_WRITE | PROT_EXEC;
            break;
        }
    }
    mprotect(reinterpret_cast<char*>(addr), size, flag);
#endif
}

// addr        : start address of memory
// totalBytes  : total number of bytes to format
// elementSize : number of bytes per element (1, 2, 4, or 8)
// perLine     : number of elements per line
//
// Return: formatted memory dump as a single string, lines separated by '\n'.
std::string DumpMemoryFormattedToString(const void* addr,
    size_t      totalBytes,
    size_t      elementSize,
    size_t      perLine) {
    std::ostringstream oss;
    oss.setf(std::ios::hex, std::ios::basefield);
    oss.setf(std::ios::right, std::ios::adjustfield);
    oss.fill('0');

    if (!addr || totalBytes == 0 || perLine == 0) {
        return oss.str();
    }
    if (!(elementSize == 1 || elementSize == 2 || elementSize == 4 || elementSize == 8)) {
        return oss.str(); // unsupported element size
    }

    const uint8_t* base = static_cast<const uint8_t*>(addr);
    const size_t   bytesPerLine = elementSize * perLine;

    size_t offset = 0;
    bool firstLine = true;

    while (offset < totalBytes) {
        size_t lineBytes = totalBytes - offset;
        if (lineBytes > bytesPerLine) {
            lineBytes = bytesPerLine;
        }

        const uint8_t* linePtr = base + offset;

        // Add newline between lines (do not start with '\n')
        if (!firstLine) {
            oss << '\n';
        }
        firstLine = false;

        // 1. Print line start address (64-bit, hex, width 16)
        {
            std::uintptr_t addrValue = reinterpret_cast<std::uintptr_t>(linePtr);
            oss << std::setw(16) << addrValue << ' ';
        }

        // 2. Print elements in hex, each element width = elementSize * 2
        //    If remaining bytes are not enough for a full element, pad with spaces.
        size_t printedBytes = 0;
        for (size_t i = 0; i < perLine; ++i) {
            if (printedBytes + elementSize <= lineBytes) {
                const uint8_t* elemPtr = linePtr + printedBytes;

                // Print element as elementSize bytes, high byte first
                // i.e. elemPtr[elementSize - 1] ... elemPtr[0]
                for (size_t b = 0; b < elementSize; ++b) {
                    size_t idx = elementSize - 1 - b;
                    oss << std::setw(2)
                        << static_cast<unsigned>(elemPtr[idx]);
                }
                oss << ' ';

                printedBytes += elementSize;
            }
            else {
                // Not enough data for a full element: output spaces for this element
                size_t hexChars = elementSize * 2;
                for (size_t c = 0; c < hexChars; ++c) {
                    oss << ' ';
                }
                oss << ' ';
            }
        }

        // 3. ASCII section: one character per byte actually present on this line
        oss << ' ';

        // Switch to normal character output (no width, no hex formatting needed)
        for (size_t i = 0; i < lineBytes; ++i) {
            unsigned char c = linePtr[i];
            if (c >= 32 && c <= 126) {
                oss << static_cast<char>(c);
            }
            else {
                oss << '.';
            }
        }

        offset += lineBytes;
    }

    return oss.str();
}


static bool s_DbgScpLoaded = false;
static bool g_bFrameCapturing = false;

bool IsDbgScpLoaded() {
    return s_DbgScpLoaded;
}
void InitGpuCaptureManager() {
#if __APPLE__
    GpuCaptureManager::Instance().Init(GpuCaptureBackend::MetalXcode);
#elif defined(__OHOS__)
    GpuCaptureManager::Instance().Init(GpuCaptureBackend::HuaweiSquid);
#else
    GpuCaptureManager::Instance().Init(GpuCaptureBackend::RenderDoc);
#endif
    g_bFrameCapturing = false;
}
void ShutdownGpuCaptureManager() {
    GpuCaptureManager::Instance().Shutdown();
    g_bFrameCapturing = false;
}
void EndFrameCaptureIfCapturing() {
    if (g_bFrameCapturing) {
        if (GpuCaptureManager::Instance().EndFrameCapture())
            g_bFrameCapturing = false;
    }
}

enum class ExternApiEnum {
    TestFFI = c_extern_api_start_id,
    LoadLib,
    GetProc,
    LoadLibAndGetProc,
    FreeLib,
    FreeLibByPath,
    GetPID,
    GetTID,
    FindSegment,
    MemoryProtect,
    WriteLog,
    FlushLog,
    LogStack,
    AddMemoryFlag,
    RemoveMemoryFlag,
    GetMemoryFlag,
    SetWatchPoint,
    GetWatchPoint,
    FormatMemory,
    TriggerFrameCapture,
    StartFrameCapture,
    EndFrameCapture,
    Num
};

struct ExternApi {
    static inline void TestFFI(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        int64_t ix = DebugScript::GetVarInt(args[0].IsGlobal, args[0].Index, stackBase, intLocals,
                                            intGlobals);
        int64_t addr = 0;
        switch (ix) {
        case 0:
            addr = reinterpret_cast<int64_t>(TestFFI0);
            break;
        case 1:
            addr = reinterpret_cast<int64_t>(TestFFI1);
            break;
        }
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, addr, stackBase, intLocals,
                               intGlobals);
    }
    static inline void LoadLib(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        auto&& str = DebugScript::GetVarString(args[0].IsGlobal, args[0].Index, stackBase,
                                               strLocals, strGlobals);
        void* ptr = LoadDynamicLibrary(str);
        int64_t addr = reinterpret_cast<int64_t>(ptr);
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, addr, stackBase, intLocals,
                               intGlobals);
    }
    static inline void GetProc(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        int64_t addr = DebugScript::GetVarInt(args[0].IsGlobal, args[0].Index, stackBase, intLocals,
                                              intGlobals);
        auto&& str = DebugScript::GetVarString(args[1].IsGlobal, args[1].Index, stackBase,
                                               strLocals, strGlobals);
        void* ptr = LookupSymbol(reinterpret_cast<void*>(addr), str);
        int64_t rval = reinterpret_cast<int64_t>(ptr);
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, rval, stackBase, intLocals,
                               intGlobals);
    }
    static inline void LoadLibAndGetProc(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        int64_t rval = 0;
        auto&& lib = DebugScript::GetVarString(args[0].IsGlobal, args[0].Index, stackBase,
                                               strLocals, strGlobals);
        auto&& proc = DebugScript::GetVarString(args[1].IsGlobal, args[1].Index, stackBase,
                                                strLocals, strGlobals);
        void* ptr = LoadAndLookupSymbol(lib, proc);
        rval = reinterpret_cast<int64_t>(ptr);
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, rval, stackBase, intLocals,
                               intGlobals);
    }
    static inline void FreeLib(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        int64_t addr = DebugScript::GetVarInt(args[0].IsGlobal, args[0].Index, stackBase, intLocals,
                                              intGlobals);
        UnloadDynamicLibrary(reinterpret_cast<void*>(addr));
        int64_t rval = 1;
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, rval, stackBase, intLocals,
                               intGlobals);
    }
    static inline void FreeLibByPath(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        auto&& str = DebugScript::GetVarString(args[0].IsGlobal, args[0].Index, stackBase,
                                               strLocals, strGlobals);
        UnloadDynamicLibrary(str);
        int64_t rval = 1;
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, rval, stackBase, intLocals,
                               intGlobals);
    }
    static inline void GetPID(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        int64_t rval = GetProcessId();
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, rval, stackBase, intLocals,
                               intGlobals);
    }
    static inline void GetTID(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        int64_t rval = GetThreadId();
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, rval, stackBase, intLocals,
                               intGlobals);
    }
    static inline void FindSegment(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        int64_t pid = DebugScript::GetVarInt(args[0].IsGlobal, args[0].Index, stackBase, intLocals,
                                             intGlobals);
        auto&& so = DebugScript::GetVarString(args[1].IsGlobal, args[1].Index, stackBase, strLocals,
                                              strGlobals);
        auto&& attr = DebugScript::GetVarString(args[2].IsGlobal, args[2].Index, stackBase,
                                                strLocals, strGlobals);
        bool incFirst = DebugScript::GetVarInt(args[3].IsGlobal, args[3].Index, stackBase,
                                               intLocals, intGlobals) != 0;
        std::string match;
        std::vector<std::string> fields;
        int64_t rval = find_loaded_segments(pid, so, attr, incFirst, match, fields);
        if (rval) {
            mylog_printf("find segment: '%s'\n", match.c_str());
            if (argNum > 4) {
                for (int ix = 0; ix < static_cast<int>(fields.size()); ++ix) {
                    DebugScript::SetVarString(args[4].IsGlobal, args[4].Index + ix, fields[ix],
                                              stackBase, strLocals, strGlobals);
                }
            }
        }
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, rval, stackBase, intLocals,
                               intGlobals);
    }
    static inline void MemoryProtect(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        int64_t addr = DebugScript::GetVarInt(args[0].IsGlobal, args[0].Index, stackBase, intLocals,
                                              intGlobals);
        size_t size = static_cast<size_t>(DebugScript::GetVarInt(args[1].IsGlobal, args[1].Index,
                                                                 stackBase, intLocals, intGlobals));
        int quickflag = static_cast<int>(DebugScript::GetVarInt(args[2].IsGlobal, args[2].Index,
                                                                stackBase, intLocals, intGlobals));
        int rawflag = 0;
        if (argNum > 3) {
            rawflag = static_cast<int>(DebugScript::GetVarInt(args[3].IsGlobal, args[3].Index,
                                                              stackBase, intLocals, intGlobals));
        }
        size_t pageSize = GetPagetSize();
        SetMemoryProtect(addr, size, pageSize, quickflag, rawflag);
    }
    static inline void WriteLog(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        const std::string& str = DebugScript::GetVarString(args[0].IsGlobal, args[0].Index,
                                                           stackBase, strLocals, strGlobals);
        int r = DbgScp_WriteLog(str);
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, r, stackBase, intLocals, intGlobals);
    }
    static inline void FlushLog(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        int r = DbgScp_FlushLog();
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, r, stackBase, intLocals, intGlobals);
    }
    static inline void LogStack(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        const std::string& str = DebugScript::GetVarString(args[0].IsGlobal, args[0].Index,
                                                           stackBase, strLocals, strGlobals);
        DbgScp_LogCallstack(str.c_str(), __FILE__, __LINE__);
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, 1, stackBase, intLocals, intGlobals);
    }
    static inline void AddMemoryFlag(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        int64_t addr = DebugScript::GetVarInt(args[0].IsGlobal, args[0].Index, stackBase, intLocals,
                                              intGlobals);
        int64_t flag = DebugScript::GetVarInt(args[1].IsGlobal, args[1].Index, stackBase, intLocals,
                                              intGlobals);
        bool r = DbgScp_AddMemoryFlag(addr, flag);
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, r ? 1 : 0, stackBase, intLocals,
                               intGlobals);
    }
    static inline void RemoveMemoryFlag(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        int64_t addr = DebugScript::GetVarInt(args[0].IsGlobal, args[0].Index, stackBase, intLocals,
                                              intGlobals);
        bool r = DbgScp_RemoveMemoryFlag(addr);
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, r ? 1 : 0, stackBase, intLocals,
                               intGlobals);
    }
    static inline void GetMemoryFlag(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        int64_t addr = DebugScript::GetVarInt(args[0].IsGlobal, args[0].Index, stackBase, intLocals,
                                              intGlobals);
        int64_t flag = 0;
        bool r = DbgScp_GetMemoryFlag(addr, flag);
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, r ? 1 : 0, stackBase, intLocals,
                               intGlobals);
        if (argNum > 1) {
            DebugScript::SetVarInt(args[1].IsGlobal, args[1].Index, flag, stackBase, intLocals,
                                   intGlobals);
        }
    }
    static inline void SetWatchPoint(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        int64_t cmd = DebugScript::GetVarInt(args[0].IsGlobal, args[0].Index, stackBase, intLocals,
                                             intGlobals);
        int64_t flag = DebugScript::GetVarInt(args[1].IsGlobal, args[1].Index, stackBase, intLocals,
                                              intGlobals);
        int64_t size = DebugScript::GetVarInt(args[2].IsGlobal, args[2].Index, stackBase, intLocals,
                                              intGlobals);
        int64_t addr = DebugScript::GetVarInt(args[3].IsGlobal, args[3].Index, stackBase, intLocals,
                                              intGlobals);
        int64_t tid = DebugScript::GetVarInt(args[4].IsGlobal, args[4].Index, stackBase, intLocals,
                                             intGlobals);
        DbgScp_SetWatchPoint(static_cast<short>(cmd), static_cast<short>(flag),
                             static_cast<int>(size), addr, tid);
    }
    static inline void GetWatchPoint(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        short flag = 0;
        int size = 0;
        int64_t addr = 0;
        int64_t tid = 0;
        short cmd = DbgScp_GetWatchPoint(flag, size, addr, tid);
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, cmd, stackBase, intLocals,
                               intGlobals);
        if (argNum > 1) {
            DebugScript::SetVarInt(args[1].IsGlobal, args[1].Index, flag, stackBase, intLocals,
                                   intGlobals);
        }
        if (argNum > 2) {
            DebugScript::SetVarInt(args[2].IsGlobal, args[2].Index, size, stackBase, intLocals,
                                   intGlobals);
        }
        if (argNum > 3) {
            DebugScript::SetVarInt(args[3].IsGlobal, args[3].Index, addr, stackBase, intLocals,
                                   intGlobals);
        }
        if (argNum > 4) {
            DebugScript::SetVarInt(args[4].IsGlobal, args[4].Index, tid, stackBase, intLocals,
                                   intGlobals);
        }
    }
    static inline void FormatMemory(int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals, DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals, DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals, const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal)
    {
        int64_t addr = DebugScript::GetVarInt(args[0].IsGlobal, args[0].Index, stackBase, intLocals, intGlobals);
        int64_t total_bytes = DebugScript::GetVarInt(args[1].IsGlobal, args[1].Index, stackBase, intLocals, intGlobals);
        int64_t element_size = 1;
        int64_t element_per_line = 32;
        if (argNum > 2) {
            element_size = DebugScript::GetVarInt(args[2].IsGlobal, args[2].Index, stackBase, intLocals, intGlobals);
            element_per_line /= element_size;
        }
        if (argNum > 3) {
            element_per_line = DebugScript::GetVarInt(args[3].IsGlobal, args[3].Index, stackBase, intLocals, intGlobals);
        }
        std::string str = DumpMemoryFormattedToString(reinterpret_cast<void*>(addr), total_bytes, element_size, element_per_line);
        DebugScript::SetVarString(retVal.IsGlobal, retVal.Index, str, stackBase, strLocals, strGlobals);
    }
    static inline void TriggerFrameCapture(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        int r = 1;
        GpuCaptureManager::Instance().TriggerCapture();
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, r, stackBase, intLocals, intGlobals);
    }
    static inline void StartFrameCapture(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        bool r = GpuCaptureManager::Instance().StartFrameCapture();
        if (r) {
            g_bFrameCapturing = true;
        }
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, r ? 1 : 0, stackBase, intLocals, intGlobals);
    }
    static inline void EndFrameCapture(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        bool r = GpuCaptureManager::Instance().EndFrameCapture();
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, r ? 1 : 0, stackBase, intLocals,
                               intGlobals);
    }
};

void CppDbgScp_CallExternApi(int api, int32_t stackBase, DebugScript::IntLocals& intLocals,
                             DebugScript::FloatLocals& fltLocals,
                             DebugScript::StringLocals& strLocals,
                             DebugScript::IntGlobals& intGlobals,
                             DebugScript::FloatGlobals& fltGlobals,
                             DebugScript::StringGlobals& strGlobals,
                             const ExternApiArgOrRetVal args[], int32_t argNum,
                             const ExternApiArgOrRetVal& retVal) {
    switch (static_cast<ExternApiEnum>(api)) {
    case ExternApiEnum::TestFFI:
        ExternApi::TestFFI(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                           strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::LoadLib:
        ExternApi::LoadLib(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                           strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::GetProc:
        ExternApi::GetProc(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                           strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::LoadLibAndGetProc:
        ExternApi::LoadLibAndGetProc(stackBase, intLocals, fltLocals, strLocals, intGlobals,
                          fltGlobals, strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::FreeLib:
        ExternApi::FreeLib(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                           strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::FreeLibByPath:
        ExternApi::FreeLibByPath(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                          strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::GetPID:
        ExternApi::GetPID(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                          strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::GetTID:
        ExternApi::GetTID(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                          strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::FindSegment:
        ExternApi::FindSegment(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                          strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::MemoryProtect:
        ExternApi::MemoryProtect(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                          strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::WriteLog:
        ExternApi::WriteLog(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                            strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::FlushLog:
        ExternApi::FlushLog(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                            strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::LogStack:
        ExternApi::LogStack(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                            strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::AddMemoryFlag:
        ExternApi::AddMemoryFlag(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                                 strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::RemoveMemoryFlag:
        ExternApi::RemoveMemoryFlag(stackBase, intLocals, fltLocals, strLocals, intGlobals,
                                    fltGlobals, strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::GetMemoryFlag:
        ExternApi::GetMemoryFlag(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                                 strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::SetWatchPoint:
        ExternApi::SetWatchPoint(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                                 strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::GetWatchPoint:
        ExternApi::GetWatchPoint(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                                 strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::FormatMemory:
        ExternApi::FormatMemory(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
            strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::TriggerFrameCapture:
        ExternApi::TriggerFrameCapture(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                            strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::StartFrameCapture:
        ExternApi::StartFrameCapture(stackBase, intLocals, fltLocals, strLocals, intGlobals,
                                       fltGlobals, strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::EndFrameCapture:
        ExternApi::EndFrameCapture(stackBase, intLocals, fltLocals, strLocals, intGlobals,
                                       fltGlobals, strGlobals, args, argNum, retVal);
        break;
    default:
        break;
    }
}

#if defined(DBGSCP_COMPILER_TEST)

extern "C" {
    __declspec(dllexport) void CppDbgScp_ResetVM() {
        DebugScriptGlobal::Reset();
    }
    __declspec(dllexport) void CppDbgScp_AllocConstInt(int64_t val) {
        DebugScriptGlobal::AllocConstInt(val);
    }
    __declspec(dllexport) void CppDbgScp_AllocConstFloat(double val) {
        DebugScriptGlobal::AllocConstFloat(val);
    }
    __declspec(dllexport) void CppDbgScp_AllocConstString(const char* val) {
        DebugScriptGlobal::AllocConstString(val);
    }
    __declspec(dllexport) void CppDbgScp_AllocGlobalInt(int64_t val) {
        DebugScriptGlobal::AllocGlobalInt(val);
    }
    __declspec(dllexport) void CppDbgScp_AllocGlobalFloat(double val) {
        DebugScriptGlobal::AllocGlobalFloat(val);
    }
    __declspec(dllexport) void CppDbgScp_AllocGlobalString(const char* val) {
        DebugScriptGlobal::AllocGlobalString(val);
    }
    __declspec(dllexport) int32_t CppDbgScp_AddHook(const char* name, int32_t* enterCodes,
        int enterCodeNum, int32_t* exitCodes,
        int exitCodeNum) {
        return DebugScriptGlobal::AddHook(name, enterCodes, enterCodeNum, exitCodes, exitCodeNum);
    }
    __declspec(dllexport) int32_t CppDbgScp_ShareWith(int32_t hookId, const char* other) {
        return DebugScriptGlobal::ShareWith(hookId, other);
    }
    __declspec(dllexport) void CppDbgScp_StartVM() {
        DebugScriptGlobal::Start();
    }
    __declspec(dllexport) void CppDbgScp_PauseVM() {
        DebugScriptGlobal::Pause();
    }
    __declspec(dllexport) void CppDbgScp_ResumeVM() {
        DebugScriptGlobal::Resume();
    }

    __declspec(dllexport) void CppDbgScp_Load(const char* file) {
        DebugScriptGlobal::Reset();
        DebugScriptGlobal::Load(file);
        DebugScriptGlobal::Start();
    }

    __declspec(dllexport) void DbgScp_Set_Export(int cmd, int a, double b, const char* c) {
        DbgScp_Set(cmd, a, b, c);
    }
    __declspec(dllexport) int DbgScp_Get_Export(int cmd, int a, double b, const char* c) {
        return DbgScp_Get(cmd, a, b, c);
    }

    __declspec(dllexport) int Test1_Export(int a, double b, const char* c) {
        thread_local static int32_t s_hook_id = -1;
        thread_local static uint32_t s_serial_num = 0;
        CheckFuncHook(__FUNCTION__, s_hook_id, s_serial_num);
        int h_ret_val{};
        auto&& placeHolder = CreateHookWrap(s_hook_id, h_ret_val, a, b, c);
        if (placeHolder.IsBreak())
            return h_ret_val;
        mylog_printf("Test1 a:%d b:%f c:%s\n", a, b, c);
        return 0;
    }
    __declspec(dllexport) int Test2_Export(int a, double b, const char* c) {
        auto f = [&]() {
            mylog_printf("Test2 a:%d b:%f c:%s\n", a, b, c);
            return 0;
            };
        thread_local static int32_t s_hook_id = -1;
        thread_local static uint32_t s_serial_num = 0;
        CheckFuncHook(__FUNCTION__, s_hook_id, s_serial_num);
        bool retry{};
        int h_ret_val{};
        do {
            auto&& placeHolder = CreateHookWrap(retry, s_hook_id, h_ret_val, a, b, c);
            if (placeHolder.IsBreak()) {
                return h_ret_val;
            }
            else {
                h_ret_val = f();
            }
        }
        while (false);
        if (retry) {
            h_ret_val = f();
        }
        return h_ret_val;
    }
    __declspec(dllexport) void Test3_Export(int a, double b, const char* c) {
        static int32_t s_hook_id = -1;
        static uint32_t s_serial_num = 0;
        CheckFuncHook(__FUNCTION__, s_hook_id, s_serial_num);
        auto&& placeHolder = CreateHookWrap(s_hook_id, a, b, c);
        if (placeHolder.IsBreak())
            return;
        mylog_printf("Test3 a:%d b:%f c:%s\n", a, b, c);
    }
    __declspec(dllexport) void Test4_Export(int a, double b, const char* c) {
        auto f = [&]() { mylog_printf("Test4 a:%d b:%f c:%s\n", a, b, c); };
        static int32_t s_hook_id = -1;
        static uint32_t s_serial_num = 0;
        CheckFuncHook(__FUNCTION__, s_hook_id, s_serial_num);
        bool retry{};
        do {
            auto&& placeHolder = CreateHookWrap(retry, s_hook_id, a, b, c);
            if (placeHolder.IsBreak()) {
                return;
            }
            else {
                f();
            }
        }
        while (false);
        if (retry) {
            f();
        }
    }

    __declspec(dllexport) int TestMacro1_Export(int a, double b, const char* c) {
        return TestMacro1(a, b, c);
    }
    __declspec(dllexport) int TestMacro2_Export(int a, double b, const char* c) {
        return TestMacro2(a, b, c);
    }
    __declspec(dllexport) void TestMacro3_Export(int a, double b, const char* c) {
        TestMacro3(a, b, c);
    }
    __declspec(dllexport) void TestMacro4_Export(int a, double b, const char* c) {
        TestMacro4(a, b, c);
    }
}

#elif defined(DBGSCP_ON_UNITY)

void LoadDbgScp(const core::string& log_path, const core::string& load_path)
{
    s_DbgScpLoaded = true;
    for (int i = 0; i < c_max_log_file_num; ++i) {
        if (GetLogFilesRef()[i].empty()) {
            auto&& path = Format("%s/dbgscp_log_%d.txt", log_path.c_str(), i);
            GetLogFilesRef()[i] = path.c_str();
            printf_console("LoadDbgScp, LogFile: %d %s\n", i, path.c_str());
        }
    }
#if PLATFORM_ANDROID
    const char* c_data_file = "/data/local/tmp/bytecode.dat";
#else
    auto&& dataPath = Format("%s/bytecode.dat", load_path.c_str());
    const char* c_data_file = dataPath.c_str();
#endif
    DebugScriptGlobal::Reset();
    bool r = DebugScriptGlobal::Load(c_data_file);
    DebugScriptGlobal::Start();
    printf_console("LoadDbgScp: %s %d\n", c_data_file, r ? 1 : 0);
}
void PauseDbgScp()
{
    DebugScriptGlobal::Pause();
    printf_console("DebugScriptGlobal::Pause\n");
}
void ResumeDbgScp()
{
    DebugScriptGlobal::Resume();
    printf_console("DebugScriptGlobal::Resume\n");
}
void DbgScp_Set_Extern(int cmd, int a, double b, const char* c)
{
    DbgScp_Set(cmd, a, b, c);
}
int DbgScp_Get_Extern(int cmd, int a, double b, const char* c)
{
    return DbgScp_Get(cmd, a, b, c);
}

#elif defined(DBGSCP_ON_UNREAL)

void LoadDbgScp(const FString& log_path, const FString& load_path)
{
    s_DbgScpLoaded = true;
    for (int i = 0; i < c_max_log_file_num; ++i) {
        if (GetLogFilesRef()[i].empty()) {
            auto&& path = FPaths::Combine(log_path, FString::Printf(TEXT("dbgscp_log_%d.txt"), i));
            GetLogFilesRef()[i] = TCHAR_TO_UTF8(*path);
            UE_LOG(LogTemp, Log, TEXT("LoadDbgScp, LogFile: %d %s\n"), i, TCHAR_TO_UTF8(*path));
        }
    }
#if PLATFORM_ANDROID
    const char* c_data_file = "/data/local/tmp/bytecode.dat";
#else
    auto&& dataPath = FPaths::Combine(load_path, TEXT("bytecode.dat"));
    FTCHARToUTF8 Converter(*dataPath);
    const char* c_data_file = Converter.Get();
#endif
    DebugScriptGlobal::Reset();
    bool r = DebugScriptGlobal::Load(c_data_file);
    DebugScriptGlobal::Start();
    UE_LOG(LogTemp, Log, TEXT("LoadDbgScp: %s %d\n"), c_data_file, r ? 1 : 0);
}
void PauseDbgScp()
{
    DebugScriptGlobal::Pause();
    UE_LOG(LogTemp, Log, TEXT("DebugScriptGlobal::Pause\n"));
}
void ResumeDbgScp()
{
    DebugScriptGlobal::Resume();
    UE_LOG(LogTemp, Log, TEXT("DebugScriptGlobal::Resume\n"));
}
void DbgScp_Set_Extern(int cmd, int a, double b, const char* c)
{
    DbgScp_Set(cmd, a, b, c);
}
int DbgScp_Get_Extern(int cmd, int a, double b, const char* c)
{
    return DbgScp_Get(cmd, a, b, c);
}

#else

void LoadDbgScp(const std::string& log_path, const std::string& load_path)
{
    s_DbgScpLoaded = true;
    const int c_path_capacity_max = 1025;
    for (int i = 0; i < c_max_log_file_num; ++i) {
        if (GetLogFilesRef()[i].empty()) {
            char strBuf[c_path_capacity_max];
            int len = snprintf(strBuf, c_path_capacity_max, "%s/dbgscp_log_%d.txt", log_path.c_str(), i);
            // Guard against snprintf returning negative (error) or exceeding buffer size
            if (len < 0) {
                len = 0;
            } else if (len >= c_path_capacity_max - 1) {
                len = c_path_capacity_max - 1;
            }
            strBuf[len] = 0;
            GetLogFilesRef()[i] = strBuf;
        }
    }
    std::string data_file = load_path + "/bytecode.dat";
    DebugScriptGlobal::Reset();
    bool r = DebugScriptGlobal::Load(data_file.c_str());
    DebugScriptGlobal::Start();
    mylog_printf("LoadDbgScp: %s %d\n", data_file.c_str(), r ? 1 : 0);
}
void PauseDbgScp()
{
    DebugScriptGlobal::Pause();
    mylog_printf("DebugScriptGlobal::Pause\n");
}
void ResumeDbgScp()
{
    DebugScriptGlobal::Resume();
    mylog_printf("DebugScriptGlobal::Resume\n");
}
void DbgScp_Set_Extern(int cmd, int a, double b, const char* c)
{
    DbgScp_Set(cmd, a, b, c);
}
int DbgScp_Get_Extern(int cmd, int a, double b, const char* c)
{
    return DbgScp_Get(cmd, a, b, c);
}

#endif
