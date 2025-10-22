#pragma once
#include <stdint.h>
#include <string>
#include <array>

class DebugScriptVM
{
public:
    static uint32_t GetSerialNum();
    static bool IsStarted();
public:
    static int32_t FindHook(const char* name);
    static bool CanRun();
    static bool RunHookOnEnter(int32_t id, int32_t argc, int64_t argv[]);
    static bool RunHookOnExit(int32_t id, int32_t argc, int64_t argv[]);
};
