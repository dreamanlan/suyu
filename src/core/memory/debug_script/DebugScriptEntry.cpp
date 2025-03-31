#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdio.h>
#include "DbgScpHook.h"
#include "core/core.h"

#if defined(PLATFORM_WIN) || defined(_MSC_VER)
#include "windows.h"
#elif defined(PLATFORM_ANDROID) || defined(__ANDROID__)
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(UNITY_POSIX) || defined(__APPLE__)
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if defined(UNITY_APPLE) || defined(__APPLE__)
#include <execinfo.h>
#elif defined(PLATFORM_ANDROID)
#include "PlatformDependent/AndroidPlayer/Source/AndroidBacktrace.h"
#elif defined(PLATFORM_SWITCH)
#include "PlatformDependent/Switch/Source/Diagnostics/SwitchBacktrace.h"
#elif defined(PLATFORM_LUMIN)
#include "PlatformDependent/Lumin/Source/LuminBacktrace.h"
#elif defined(PLATFORM_PLAYSTATION)
#include "PlatformDependent/SonyCommon/Player/Native/PlayStationStackTrace.h"
#elif defined(__ANDROID__)
#if __ANDROID_API__ >= 33
#include <execinfo.h>
#else
#define BACKTRACE_UNIMPLEMENTED 1

#include <iostream>
#include <iomanip>
#include <dlfcn.h>
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
        } else {
            *state->current++ = reinterpret_cast<void*>(pc);
        }
    }
    return _URC_NO_REASON;
}
int captureBacktrace(void** buffer, size_t max) {
    BacktraceState state = {buffer, buffer + max};
    _Unwind_Backtrace(unwindCallback, &state);
    return static_cast<int>(state.current - buffer);
}
}
#endif
#else
#define BACKTRACE_UNIMPLEMENTED 1
#if _MSC_VER
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
//for old windows before vista
void captureStack(DWORD64 stackAddr[], int maxDepth) {
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    CONTEXT context;
    RtlCaptureContext(&context);

    SymInitialize(process, NULL, TRUE);

    STACKFRAME64 stackFrame;
    ZeroMemory(&stackFrame, sizeof(STACKFRAME64));

#ifdef _M_IX86
    DWORD machineType = IMAGE_FILE_MACHINE_I386;
    stackFrame.AddrPC.Offset = context.Eip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = context.Ebp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = context.Esp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
#elif _M_X64
    DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
    stackFrame.AddrPC.Offset = context.Rip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = context.Rsp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = context.Rsp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
#elif _M_IA64
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

#if defined(PLATFORM_WIN)
|| defined(UNITY_APPLE) || defined(PLATFORM_ANDROID) || defined(PLATFORM_SWITCH) ||
    defined(PLATFORM_LUMIN) ||
    defined(PLATFORM_PLAYSTATION) // for unity
#include "Runtime/Logging/LogAssert.h"
int mylog_printf(const char* fmt, ...) {
    va_list vl;
    va_start(vl, fmt);
    printf_consolev(kLogTypeWarning, fmt, vl);
    va_end(vl);
    return 1;
}
#else
int mylog_printf(const char* fmt, ...) {
    const int c_buf_size = 1024 * 4 + 1;
    char buf[c_buf_size];
    va_list vl;
    va_start(vl, fmt);
    int r = std::vsnprintf(buf, c_buf_size, fmt, vl);
    va_end(vl);
    std::stringstream ss;
    ss << buf;
    Core::g_MainThreadCaller.SyncLogToView(ss.str());

    LOG_INFO(Log, "{}", buf);
    return r;
}
#endif

void mylog_dump_callstack(const char* prefix, const char* file, int line) {
    mylog_printf("%s%s:%d\n", prefix, file, line);

#if BACKTRACE_UNIMPLEMENTED
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
#elif _MSC_VER
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
#if defined(PLATFORM_WIN) // for unity
    DebugAssert(v);
#elif defined(_MSC_VER)
    _ASSERT(v);
#elif defined(UNITY_APPLE)
    || defined(PLATFORM_ANDROID)
    || defined(PLATFORM_SWITCH)
    || defined(PLATFORM_LUMIN)
    || defined(PLATFORM_PLAYSTATION) // for unity
    DebugAssert(v);
#else
    assert(v);
#endif
}

static inline int64_t GetProcessId()
{
#if defined(PLATFORM_WIN) || defined(_MSC_VER)
    return static_cast<int64_t>(GetCurrentProcessId());
#elif defined(__APPLE__) || defined(__ANDROID__) || defined(UNITY_POSIX)
    return static_cast<int64_t>(getpid());
#endif
    return 0;
}
static inline int64_t GetThreadId()
{
#if defined(PLATFORM_WIN) || defined(_MSC_VER)
    return static_cast<int64_t>(GetCurrentThreadId());
#elif defined(PLATFORM_ANDROID) || defined(__ANDROID__)
    return static_cast<int64_t>(gettid());
#elif defined(UNITY_POSIX) || defined(__APPLE__)
    return reinterpret_cast<int64_t>(pthread_self());
#endif
    return 0;
}

struct WatchPointCommandInfo
{
    short cmd;//0--nothing 1--add 2--remove
    short flag;//0--nothing 1--read 2--write 3--readwrite
    int size;
    int64_t addr;
    int64_t tid;
};

static std::recursive_mutex g_WatchPointMutex{};
WatchPointCommandInfo g_WatchPointCommandInfo{0, 0, 0, 0, 0};

static inline void DbgScp_SetWatchPoint(short cmd, short flag, int size, int64_t addr,
                                        int64_t tid) {
    std::lock_guard<std::recursive_mutex> lock(g_WatchPointMutex);

    g_WatchPointCommandInfo.cmd = cmd;
    g_WatchPointCommandInfo.flag = flag;
    g_WatchPointCommandInfo.size = size;
    g_WatchPointCommandInfo.addr = addr;
    g_WatchPointCommandInfo.tid = tid;
}
static inline short DbgScp_GetWatchPoint(short& flag, int& size, int64_t& addr, int64_t& tid) {
    std::lock_guard<std::recursive_mutex> lock(g_WatchPointMutex);

    flag = g_WatchPointCommandInfo.flag;
    size = g_WatchPointCommandInfo.size;
    addr = g_WatchPointCommandInfo.addr;
    tid = g_WatchPointCommandInfo.tid;

    return g_WatchPointCommandInfo.cmd;
}

static const int c_max_log_file_num = 16;
static const int c_max_log_file_size = 1024 * 1024 * 1024;
static const std::streamsize c_log_buffer_size = 8 * 1024 * 1024;
static std::vector<char> g_SwapBuffer(c_log_buffer_size);
static std::stringstream g_LogBuffer{};
static std::recursive_mutex g_LogBufferMutex{};
static std::string g_LogFile[c_max_log_file_num] = {"dbgscp_log_0.txt",
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
static int g_LogIndex = 0;
static bool g_FirstLog = true;
static uint64_t g_LogSize = 0;

static inline bool DbgScp_FlushLog_NoLock(const char* pstr, size_t len) {
    bool r = false;
    if (g_LogIndex < c_max_log_file_num) {
        FILE* fp = fopen(g_LogFile[g_LogIndex].c_str(), g_FirstLog ? "wt" : "at");
        if (fp) {
            auto&& pos = g_LogBuffer.tellp();
            g_LogSize += pos;

            std::streampos curPos = 0;
            g_LogBuffer.seekg(curPos, std::ios::beg);
            while (curPos < pos) {
                std::streamsize size = pos - curPos;
                if (size > c_log_buffer_size) {
                    size = c_log_buffer_size;
                }
                g_LogBuffer.read(g_SwapBuffer.data(), size);
                fwrite(g_SwapBuffer.data(), 1, size, fp);
                curPos += size;
            }
            if (pstr) {
                fwrite(pstr, 1, len, fp);
                g_LogSize += len;
            }
            fclose(fp);
            g_LogBuffer.str("");
            g_FirstLog = false;
            r = true;

            if (g_LogSize >= c_max_log_file_size) {
                g_FirstLog = true;
                ++g_LogIndex;
                g_LogSize = 0;
            }
        }
    }
    return r;
}
static inline bool DbgScp_FlushLog() {
    std::lock_guard<std::recursive_mutex> lock(g_LogBufferMutex);

    return DbgScp_FlushLog_NoLock(nullptr, 0);
}
static inline bool DbgScp_WriteLog(const std::string& str) {
    std::lock_guard<std::recursive_mutex> lock(g_LogBufferMutex);

    bool r = true;
    auto&& pos = g_LogBuffer.tellp();
    if (pos + static_cast<std::streamoff>(str.length()) > c_log_buffer_size) {
        r = DbgScp_FlushLog_NoLock(str.c_str(), str.length());
    } else {
        g_LogBuffer << str;
    }
    return r;
}
static inline void DbgScp_LogCallstack(const char* prefix, const char* file, int line) {
    const int c_buf_size = 1024 * 4 + 1;
    char buf[c_buf_size];
    snprintf(buf, c_buf_size, "%s%s:%d\n", prefix, file, line);
    DbgScp_WriteLog(buf);

#if BACKTRACE_UNIMPLEMENTED
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

static std::unordered_map<int64_t, int64_t> g_MemoryFlags{};
static std::recursive_mutex g_MemoryFlagMutex{};

static inline bool DbgScp_AddMemoryFlag(int64_t addr, int64_t flag) {
    std::lock_guard<std::recursive_mutex> lock(g_MemoryFlagMutex);

    auto&& r = g_MemoryFlags.insert(std::make_pair(addr, flag));
    return r.second;
}
static inline bool DbgScp_RemoveMemoryFlag(int64_t addr) {
    std::lock_guard<std::recursive_mutex> lock(g_MemoryFlagMutex);

    auto&& r = g_MemoryFlags.erase(addr);
    return r > 0;
}
static inline bool DbgScp_GetMemoryFlag(int64_t addr, int64_t& flag) {
    std::lock_guard<std::recursive_mutex> lock(g_MemoryFlagMutex);

    bool r = false;
    auto&& it = g_MemoryFlags.find(addr);
    if (it != g_MemoryFlags.end()) {
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
    mylog_printf("%d %d %d %d %d %d %d %d %f %f %lld %lld\n", a1, a2, a3, a4, a5, a6, a7, a8, f1, f2, sv1,
           sv2);
    return 0;
}
int64_t TestFFI1(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6, int64_t a7,
                 int64_t a8, double f1, double f2, int64_t sv1, int64_t sv2) {
    mylog_printf("%lld %lld %lld %lld %lld %lld %lld %lld %f %f %lld %lld\n", a1, a2, a3, a4, a5, a6, a7,
           a8, f1, f2, sv1, sv2);
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

static inline size_t GetPagetSize() {
#if defined(PLATFORM_WIN)
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return static_cast<size_t>(sysInfo.dwPageSize);
#elif defined(__APPLE__) || defined(__ANDROID__) || defined(UNITY_POSIX)
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
#if defined(PLATFORM_WIN) || defined(_MSC_VER)
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
#elif UNITY_POSIX
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

enum class ExternApiEnum {
    TestFFI = c_extern_api_start_id,
    GetPID,
    GetTID,
    MemoryProtect,
    WriteLog,
    FlushLog,
    LogStack,
    AddMemoryFlag,
    RemoveMemoryFlag,
    GetMemoryFlag,
    SetWatchPoint,
    GetWatchPoint,
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
        bool r = DbgScp_WriteLog(str);
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, r ? 1 : 0, stackBase, intLocals,
                               intGlobals);
    }
    static inline void FlushLog(
        int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals,
        DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals,
        DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals,
        const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal) {
        bool r = DbgScp_FlushLog();
        DebugScript::SetVarInt(retVal.IsGlobal, retVal.Index, r ? 1 : 0, stackBase, intLocals,
                               intGlobals);
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
    case ExternApiEnum::GetPID:
        ExternApi::GetPID(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
                          strGlobals, args, argNum, retVal);
        break;
    case ExternApiEnum::GetTID:
        ExternApi::GetTID(stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals,
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
    default:
        break;
    }
}

void LoadDbgScp(const std::string& log_path, const std::string& load_path) {
    for (int i = 0; i < c_max_log_file_num; ++i) {
        if (g_LogFile[i].empty()) {
            auto&& path = fmt::format("{}/dbgscp_log_{}.txt", log_path.c_str(), i);
            g_LogFile[i] = path.c_str();
        }
    }
    std::string data_file = load_path + "/bytecode.dat";
    DebugScriptGlobal::Reset();
    bool r = DebugScriptGlobal::Load(data_file.c_str());
    DebugScriptGlobal::Start();
    mylog_printf("LoadDbgScp: %s %d\n", data_file.c_str(), r ? 1 : 0);
}
void PauseDbgScp() {
    DebugScriptGlobal::Pause();
    mylog_printf("DebugScriptGlobal::Pause\n");
}
void ResumeDbgScp() {
    DebugScriptGlobal::Resume();
    mylog_printf("DebugScriptGlobal::Resume\n");
}
