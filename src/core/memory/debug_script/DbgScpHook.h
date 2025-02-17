#pragma once
#include <cstdint>
#include <cstdio>
#include <array>
#include <type_traits>
#include "DebugScriptVM.h"

static inline void CheckFuncHook(const char* name, int32_t& hook_id, uint32_t& serial_num)
{
    if (g_DebugScriptStarted) {
        if (serial_num < g_DebugScriptSerialNum) {
            if (hook_id >= 0) {
                hook_id = -1;
            }
            hook_id = DebugScriptVM::FindHook(name);
            serial_num = g_DebugScriptSerialNum;
        }
    }
}

template<typename... ArgsT>
struct HookWrap
{
    HookWrap(int hookId, bool& retry, ArgsT&... args) :m_Break(false), m_HookId(hookId), m_Retry(retry), m_Args{reinterpret_cast<int64_t>(&args)...}
    {
        m_Break = DebugScriptVM::RunHookOnEnter(m_HookId, static_cast<int32_t>(m_Args.size()), m_Args.data());
    }
    ~HookWrap()
    {
        if (!m_Break) {
            m_Retry = DebugScriptVM::RunHookOnExit(m_HookId, static_cast<int32_t>(m_Args.size()), m_Args.data());
        }
    }
    bool IsBreak()const { return m_Break; }

    bool m_Break;
    int m_HookId;
    bool& m_Retry;
    std::array<int64_t, sizeof...(ArgsT)> m_Args;
};

template<typename... ArgsT>
static inline HookWrap<ArgsT...> CreateHookWrap(int hookId, ArgsT&... args)
{
    bool retry = false;
    return HookWrap<ArgsT...>(hookId, retry, args...);
}
template<typename... ArgsT>
static inline HookWrap<ArgsT...> CreateHookWrap(bool& retry, int hookId, ArgsT&... args)
{
    return HookWrap<ArgsT...>(hookId, retry, args...);
}

#if PLATFORM_WIN || PLATFORM_WINRT || PLATFORM_XBOXONE || _MSC_VER || _WIN32 || _WIN64
#define HOOK_FUNC_SIG __FUNCSIG__
#else
#define HOOK_FUNC_SIG __PRETTY_FUNCTION__
#endif

/*
//for handwriting
thread_local static int32_t s_hook_id = -1;
thread_local static uint32_t s_serial_num = 0;
CheckFuncHook("func_sig_str", s_hook_id, s_serial_num);
auto&& placeHolder = CreateHookWrap(s_hook_id, arg1, arg2);
if (placeHolder.IsBreak())return;
*/

/*
//for handwriting
thread_local static int32_t s_hook_id = -1;
thread_local static uint32_t s_serial_num = 0;
CheckFuncHook("func_sig_str", s_hook_id, s_serial_num);
auto&& placeHolder = CreateHookWrap(s_hook_id, obj, arg1, arg2);
if (placeHolder.IsBreak())return;
*/

/*
//for handwriting
thread_local static int32_t s_hook_id = -1;
thread_local static uint32_t s_serial_num = 0;
CheckFuncHook("func_sig_str", s_hook_id, s_serial_num);
RetT retVal{};
auto&& placeHolder = CreateHookWrap(s_hook_id, retVal, arg1, arg2);
if (placeHolder.IsBreak())return retVal;
*/

/*
//for handwriting
thread_local static int32_t s_hook_id = -1;
thread_local static uint32_t s_serial_num = 0;
CheckFuncHook("func_sig_str", s_hook_id, s_serial_num);
RetT retVal{};
auto&& placeHolder = CreateHookWrap(s_hook_id, retVal, obj, arg1, arg2);
if (placeHolder.IsBreak())return retVal;
*/

//Maybe we should replace ', ##' with ' __VA_OPT(, ) ' on C++20, which is the standard way
#define DBGSCP_HOOK(func_sig_str, ret_type, ...)   \
thread_local static int32_t s_hook_id = -1;\
thread_local static uint32_t s_serial_num = 0;\
CheckFuncHook(func_sig_str, s_hook_id, s_serial_num);\
ret_type h_ret_val{};\
auto&& placeHolder = CreateHookWrap(s_hook_id, h_ret_val, ##__VA_ARGS__);\
if (placeHolder.IsBreak())return h_ret_val;

#define BEGIN_DBGSCP_HOOK()    \
        auto _hook_func_ = [&]() {

#define END_DBGSCP_HOOK(func_sig_str, ret_type, ...)  \
};\
thread_local static int32_t s_hook_id = -1;\
thread_local static uint32_t s_serial_num = 0;\
CheckFuncHook(func_sig_str, s_hook_id, s_serial_num);\
bool retry{};\
ret_type h_ret_val{};\
do\
{\
    auto&& placeHolder = CreateHookWrap(retry, s_hook_id, h_ret_val, ##__VA_ARGS__);\
    if (placeHolder.IsBreak()) {\
        return h_ret_val;\
    }\
    else\
    {\
        h_ret_val = _hook_func_();\
    }\
} while (false);\
if (retry) {\
    h_ret_val = _hook_func_();\
}\
return h_ret_val;

#define DBGSCP_HOOK_VOID(func_sig_str, ...)   \
thread_local static int32_t s_hook_id = -1;\
thread_local static uint32_t s_serial_num = 0;\
CheckFuncHook(func_sig_str, s_hook_id, s_serial_num);\
auto&& placeHolder = CreateHookWrap(s_hook_id, ##__VA_ARGS__);\
if (placeHolder.IsBreak())return;

#define BEGIN_DBGSCP_HOOK_VOID()    \
        auto _hook_func_ = [&]() {

#define END_DBGSCP_HOOK_VOID(func_sig_str, ...)  \
};\
thread_local static int32_t s_hook_id = -1;\
thread_local static uint32_t s_serial_num = 0;\
CheckFuncHook(func_sig_str, s_hook_id, s_serial_num);\
bool retry{};\
do\
{\
    auto&& placeHolder = CreateHookWrap(retry, s_hook_id, ##__VA_ARGS__);\
    if (placeHolder.IsBreak()) {\
        return;\
    }\
    else\
    {\
        _hook_func_();\
    }\
} while (false);\
if (retry) {\
    _hook_func_();\
}
