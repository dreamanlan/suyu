#pragma once
#include <stdint.h>
#include <string>
#include <array>
#include "DebugScriptExport.h"

class DebugScriptGlobal
{
public:
    static void Reset();
    static void AllocConstInt(int64_t val);
    static void AllocConstFloat(double val);
    static void AllocConstString(const char* val);
    static void AllocGlobalInt(int64_t val);
    static void AllocGlobalFloat(double val);
    static void AllocGlobalString(const char* val);
    static int32_t AddHook(const char* name, int32_t* onEnter, int32_t onEnterSize, int32_t* onExit, int32_t onExitSize);
    static int32_t ShareWith(int32_t hookId, const char* other);
    static void Start();
    static void Pause();
    static void Resume();
    static bool Load(const char* file);
};

namespace DebugScript
{
    static const int c_max_variable_table_size = 8192;
    static const int c_max_stack_depth = 2; //enable reentry once
    static const int c_max_local_variable_table_size = c_max_variable_table_size * c_max_stack_depth;

    using IntGlobals = std::array<int64_t, c_max_variable_table_size>;
    using FloatGlobals = std::array<double, c_max_variable_table_size>;
    using StringGlobals = std::array<std::string, c_max_variable_table_size>;

    using IntLocals = std::array<int64_t, c_max_local_variable_table_size>;
    using FloatLocals = std::array<double, c_max_local_variable_table_size>;
    using StringLocals = std::array<std::string, c_max_local_variable_table_size>;

    enum class TypeEnum
    {
        NotUse = 0,
        Int,
        Float,
        String
    };

    static inline int64_t GetVarInt(bool isGlobal, int32_t index, int32_t stackBase, IntLocals& intLocals, IntGlobals& intGlobals)
    {
        return isGlobal ? intGlobals[index] : intLocals[stackBase + index];
    }
    static inline double GetVarFloat(bool isGlobal, int32_t index, int32_t stackBase, FloatLocals& fltLocals, FloatGlobals& fltGlobals)
    {
        return isGlobal ? fltGlobals[index] : fltLocals[stackBase + index];
    }
    static inline const std::string& GetVarString(bool isGlobal, int32_t index, int32_t stackBase, StringLocals& strLocals, StringGlobals& strGlobals)
    {
        return isGlobal ? strGlobals[index] : strLocals[stackBase + index];
    }
    static inline void SetVarInt(bool isGlobal, int32_t index, int64_t val, int32_t stackBase, IntLocals& intLocals, IntGlobals& intGlobals)
    {
        (isGlobal ? intGlobals[index] : intLocals[stackBase + index]) = val;
    }
    static inline void SetVarFloat(bool isGlobal, int32_t index, double val, int32_t stackBase, FloatLocals& fltLocals, FloatGlobals& fltGlobals)
    {
        (isGlobal ? fltGlobals[index] : fltLocals[stackBase + index]) = val;
    }
    static inline void SetVarString(bool isGlobal, int32_t index, const std::string& val, int32_t stackBase, StringLocals& strLocals, StringGlobals& strGlobals)
    {
        (isGlobal ? strGlobals[index] : strLocals[stackBase + index]) = val;
    }
}

static const int c_extern_api_start_id = 0;
struct ExternApiArgOrRetVal
{
    DebugScript::TypeEnum Type;
    bool IsGlobal;
    int Index;
};

extern void CppDbgScp_CallExternApi(int api, int32_t stackBase, DebugScript::IntLocals& intLocals, DebugScript::FloatLocals& fltLocals, DebugScript::StringLocals& strLocals, DebugScript::IntGlobals& intGlobals, DebugScript::FloatGlobals& fltGlobals, DebugScript::StringGlobals& strGlobals, const ExternApiArgOrRetVal args[], int32_t argNum, const ExternApiArgOrRetVal& retVal);
