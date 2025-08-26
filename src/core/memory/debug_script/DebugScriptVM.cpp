#include "DebugScriptVM.h"
#include "string.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <atomic>
#include <mutex>
#include <type_traits>
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <sstream>
#include <fstream>
#include <memory>

#if defined(PLATFORM_WIN) //for unity
#include "Runtime/Threads/ReadWriteLock.h"
#elif defined(_MSC_VER) && _MSC_VER >= 1939
#include <shared_mutex>
#define USE_STD_SHARED_MUTEX 1
#elif defined(PLATFORM_WIN)
    || defined(UNITY_APPLE)
    || defined(PLATFORM_ANDROID)
    || defined(PLATFORM_SWITCH)
    || defined(PLATFORM_LUMIN)
    || defined(PLATFORM_PLAYSTATION) // for unity
#include "Runtime/Threads/ReadWriteLock.h"
#else
#include "rwlock.h"
#endif

#include "debug_break.h"

extern int mylog_printf(const char* fmt, ...);
extern void mylog_dump_callstack(const char* prefix, const char* file, int line);
extern void mylog_assert(bool v);

#ifndef COMPILER_BUILTIN_EXPECT
#if defined(__clang__)
#define COMPILER_BUILTIN_EXPECT(X_, Y_)              __builtin_expect((X_), (Y_))
#elif defined(__GNUC__)
#define COMPILER_BUILTIN_EXPECT(X_, Y_)              __builtin_expect((X_), (Y_))
#else
#define COMPILER_BUILTIN_EXPECT(X_, Y_)             (X_)
#endif
#endif

#ifndef OPTIMIZER_LIKELY
#define OPTIMIZER_LIKELY(EXPR_)     COMPILER_BUILTIN_EXPECT(!!(EXPR_), 1)
#endif

#ifndef OPTIMIZER_UNLIKELY
#define OPTIMIZER_UNLIKELY(EXPR_)   COMPILER_BUILTIN_EXPECT(!!(EXPR_), 0)
#endif

#ifndef LIKELY_ATTR
#if defined(_MSC_VER) && _MSC_VER >= 1939
#define LIKELY_ATTR [[likely]]
#else
#define LIKELY_ATTR
#endif
#endif

#ifndef UNLIKELY_ATTR
#if defined(_MSC_VER) && _MSC_VER >= 1939
#define UNLIKELY_ATTR [[unlikely]]
#else
#define UNLIKELY_ATTR
#endif
#endif

using namespace DebugScript;

uint32_t g_DebugScriptSerialNum = 0;
bool g_DebugScriptStarted = false;

namespace
{
    struct DebugScriptGlobalImpl;
    struct DebugScriptVMImpl;
    static DebugScriptGlobalImpl* g_pDebugScriptGlobal = nullptr;
    static thread_local DebugScriptVMImpl* g_pDebugScriptVM = nullptr;
    static thread_local bool g_IsInNewDebugScriptVM = false;
    static std::chrono::time_point<std::chrono::high_resolution_clock> g_start_time{};

    static inline void MyAssert(bool v)
    {
        mylog_assert(v);
    }
    static inline void MyDebugBreak()
    {
        if (IsDebuggerAttached_Native()) {
            DEBUG_BREAK();
        }
    }

    //this enum must sync with InsEnum in DebugScriptCompiler.cs
    enum class InsEnum
    {
        CALLEXTERN = 0,
        RET,
        JMP,
        JMPIF,
        JMPIFNOT,
        INC,
        INCFLT,
        INCV,
        INCVFLT,
        DEC,
        DECFLT,
        DECV,
        DECVFLT,
        MOV,
        MOVFLT,
        MOVSTR,
        ARRGET,
        ARRGETFLT,
        ARRGETSTR,
        ARRSET,
        ARRSETFLT,
        ARRSETSTR,
        NEG,
        NEGFLT,
        ADD,
        ADDFLT,
        ADDSTR,
        SUB,
        SUBFLT,
        MUL,
        MULFLT,
        DIV,
        DIVFLT,
        MOD,
        MODFLT,
        AND,
        OR,
        NOT,
        GT,
        GTFLT,
        GTSTR,
        GE,
        GEFLT,
        GESTR,
        EQ,
        EQFLT,
        EQSTR,
        NE,
        NEFLT,
        NESTR,
        LE,
        LEFLT,
        LESTR,
        LT,
        LTFLT,
        LTSTR,
        LSHIFT,
        RSHIFT,
        URSHIFT,
        BITAND,
        BITOR,
        BITXOR,
        BITNOT,
        INT2STR,
        FLT2STR,
        STR2INT,
        STR2FLT,
        CASTFLTINT,
        CASTSTRINT,
        CASTINTFLT,
        CASTINTSTR,
        ASINT,
        ASFLOAT,
        ASLONG,
        ASDOUBLE,
        ARGC,
        ARGV,
        ADDR,
        ADDRFLT,
        ADDRSTR,
        PTRGET,
        PTRGETFLT,
        PTRGETSTR,
        PTRSET,
        PTRSETFLT,
        PTRSETSTR,
        CASCADEPTR,
        STKIX,
        HOOKID,
        HOOKVER,
        FFIAUTO,
        FFIMANUAL,
        FFIMANUALSTACK,
        FFIMANUALDBL,
        FFIMANUALSTACKDBL,
        RESERVE5,
        RESERVE4,
        RESERVE3,
        RESERVE2,
        RESERVE1,
        CALLINTERN_FIRST = 100,
        CALLINTERN_LAST = 255,
        NUM
    };
    static const int32_t c_abs_offset_mask = 0x7fffffff;
    static const int32_t c_offset_backward_flag = static_cast<int32_t>(0x80000000);

    static inline int32_t DecodeOffset(int32_t offset)
    {
        bool back = (offset & c_offset_backward_flag) != 0;
        offset = ((offset & c_abs_offset_mask) >> 8);
        if (back)
            offset = -offset;
        return offset;
    }
    static inline InsEnum DecodeInsEnum(int32_t opcode)
    {
        return (InsEnum)(opcode & 0xff);
    }
    static inline void DecodeOperand1(int32_t operand, int32_t& num)
    {
        num = (operand & 0xffff);
    }
    static inline void DecodeOperand2(int32_t operand, int32_t& num)
    {
        operand >>= 16;
        DecodeOperand1(operand, num);
    }
    static inline void DecodeOperand1(int32_t operand, bool& isGlobal, TypeEnum& type, int32_t& index)
    {
        int32_t localOrGlobal = ((operand & 0x8000) >> 15);
        int32_t ty = ((operand & 0x6000) >> 13);
        index = (operand & 0x1fff);
        isGlobal = localOrGlobal != 0;
        type = static_cast<TypeEnum>(ty);
    }
    static inline void DecodeOperand2(int32_t operand, bool& isGlobal, TypeEnum& type, int32_t& index)
    {
        operand >>= 16;
        DecodeOperand1(operand, isGlobal, type, index);
    }
    static inline void DecodeOpcode(int32_t opcode, InsEnum& ins, int32_t& offset)
    {
        ins = (InsEnum)(opcode & 0xff);
        offset = DecodeOffset(opcode);
    }
    static inline void DecodeOpcode(int32_t opcode, InsEnum& ins, int32_t& argNum, bool& isGlobal, TypeEnum& type, int32_t& index)
    {
        ins = (InsEnum)(opcode & 0xff);
        argNum = ((opcode & 0xff00) >> 8);
        int32_t operand = opcode >> 16;
        DecodeOperand1(operand, isGlobal, type, index);
    }

    static inline uint8_t ReadByte(std::ifstream& ifs)
    {
        uint8_t v;
        ifs.read(reinterpret_cast<char*>(&v), sizeof(v));
        return v;
    }
    static inline int Read7BitEncodedInt(std::ifstream& ifs)
    {
        uint32_t num = 0u;
        uint8_t b;
        for (int i = 0; i < 28; i += 7)
        {
            b = ReadByte(ifs);
            num |= static_cast<uint32_t>((b & 0x7F) << i);
            if (static_cast<uint32_t>(b) <= 127u)
            {
                return static_cast<int>(num);
            }
        }
        b = ReadByte(ifs);
        if (static_cast<uint32_t>(b) > 15u)
        {
            return -1;
        }
        return static_cast<int>(num) | (b << 28);
    }
    static inline int32_t ReadInt32(std::ifstream& ifs)
    {
        int32_t v;
        ifs.read(reinterpret_cast<char*>(&v), sizeof(v));
        return v;
    }
    static inline int64_t ReadInt64(std::ifstream& ifs)
    {
        int64_t v;
        ifs.read(reinterpret_cast<char*>(&v), sizeof(v));
        return v;
    }
    static inline double ReadDouble(std::ifstream& ifs)
    {
        double v;
        ifs.read(reinterpret_cast<char*>(&v), sizeof(v));
        return v;
    }
    static inline std::string ReadString(std::ifstream& ifs)
    {
        int charNum = Read7BitEncodedInt(ifs);
        std::vector<char> strBuf(charNum + 1);
        ifs.read(strBuf.data(), charNum);
        std::string str{};
        str.reserve(charNum + 1);
        str.assign(strBuf.data());
        return str;
    }

    static void print_string(const char* str)
    {
        mylog_printf("%s", str);
    }
    static int myprintf(const char* fmt, ...)
    {
        const int c_buf_size = 1024 * 4 + 1;
        char buf[c_buf_size];
        va_list vl;
        va_start(vl, fmt);
        int r = std::vsnprintf(buf, c_buf_size, fmt, vl);
        va_end(vl);
        print_string(buf);
        return r;
    }
    static int script_snprintf(char* buffer, std::size_t buf_size, const char* fmt, int64_t args[]) {
        const char* p = fmt;
        int i = 0;
        int ct = 0;

        while (*p) {
            if (*p == '%') {
                char tmpFmt[64] = { *p };
                int nextTmpIx = 1;
                bool cont = true;
                while (*p && cont) {
                    ++p;
                    tmpFmt[nextTmpIx++] = *p;
                    tmpFmt[nextTmpIx] = 0;
                    switch (*p) {
                    case 'd':
                    case 'i':
                        ct += snprintf(buffer + ct, buf_size - ct, tmpFmt, args[i++]);
                        cont = false;
                        break;
                    case 'u':
                        ct += snprintf(buffer + ct, buf_size - ct, tmpFmt, args[i++]);
                        cont = false;
                        break;
                    case 'o':
                        ct += snprintf(buffer + ct, buf_size - ct, tmpFmt, args[i++]);
                        cont = false;
                        break;
                    case 'x':
                        ct += snprintf(buffer + ct, buf_size - ct, tmpFmt, args[i++]);
                        cont = false;
                        break;
                    case 'X':
                        ct += snprintf(buffer + ct, buf_size - ct, tmpFmt, args[i++]);
                        cont = false;
                        break;
                    case 'f':
                    case 'F':
                        ct += snprintf(buffer + ct, buf_size - ct, tmpFmt, *reinterpret_cast<double*>(&args[i++]));
                        cont = false;
                        break;
                    case 'e':
                    case 'E':
                        ct += snprintf(buffer + ct, buf_size - ct, tmpFmt, *reinterpret_cast<double*>(&args[i++]));
                        break;
                    case 'g':
                    case 'G':
                        ct += snprintf(buffer + ct, buf_size - ct, tmpFmt, *reinterpret_cast<double*>(&args[i++]));
                        cont = false;
                        break;
                    case 'a':
                    case 'A':
                        ct += snprintf(buffer + ct, buf_size - ct, tmpFmt, *reinterpret_cast<double*>(&args[i++]));
                        cont = false;
                        break;
                    case 'c':
                        ct += snprintf(buffer + ct, buf_size - ct, tmpFmt, args[i++]);
                        cont = false;
                        break;
                    case 's':
                        ct += snprintf(buffer + ct, buf_size - ct, tmpFmt, reinterpret_cast<const char*>(args[i++]));
                        cont = false;
                        break;
                    case 'p':
                        ct += snprintf(buffer + ct, buf_size - ct, tmpFmt, reinterpret_cast<const void*>(args[i++]));
                        cont = false;
                        break;
                    case 'n':
                        ct += snprintf(buffer + ct, buf_size - ct, "%d", ct);
                        cont = false;
                        break;
                    case '%':
                        ct += snprintf(buffer + ct, buf_size - ct, "%s", tmpFmt);
                        cont = false;
                        break;
                    }
                }
            }
            else {
                buffer[ct++] = *p;
            }
            ++p;
        }
        return ct;
    }
    static int script_printf(const char* fmt, int64_t args[]) {
        const int c_buf_size = 1024 * 4 + 1;
        char buf[c_buf_size];
        int ct = script_snprintf(buf, c_buf_size, fmt, args);
        buf[ct < c_buf_size ? ct : c_buf_size - 1] = 0;
        print_string(buf);
        return ct;
    }
    static inline int64_t dumpcascadeptr(int64_t addr, int32_t offsets[], int32_t num)
    {
        const char* pBuf = reinterpret_cast<const char*>(addr);
        if (pBuf) {
            myprintf("[DebugInfo] DumpCascadePointer, object:%p\n", pBuf);
            for (int i = 0; i < num; ++i) {
                int offset = offsets[i];
                if (offset < 0)
                    break;
                pBuf = *reinterpret_cast<const char* const*>(pBuf + offset);
                myprintf("[DebugInfo] DumpCascadePointer, offset:%x ptr:%p\n", offset, pBuf);
                if (!pBuf)
                    break;
            }
        }
        return reinterpret_cast<int64_t>(pBuf);
    }

    template<typename RetT, typename... ArgsT>
    struct RetTypeT
    {};
    template<typename RetT, typename... ArgsT>
    struct RetTypeT<RetT(ArgsT...)>
    {
        using Type = RetT;
    };
    template<typename T>
    using RetType = typename RetTypeT<T>::Type;

    template<typename... ArgsT>
    struct VariadicArgs
    {
    };
    template<typename Arg1T, typename... ArgsT>
    struct VariadicArgs<Arg1T, ArgsT...>
    {
        using Type1 = typename std::remove_cv_t<std::remove_reference_t<Arg1T>>;
    };
    template<typename Arg1T, typename Arg2T, typename... ArgsT>
    struct VariadicArgs<Arg1T, Arg2T, ArgsT...>
    {
        using Type1 = typename std::remove_cv_t<std::remove_reference_t<Arg1T>>;
        using Type2 = typename std::remove_cv_t<std::remove_reference_t<Arg2T>>;
    };

    template<typename RetT, typename... ArgsT>
    struct Arg1TypeT
    {};
    template<typename RetT, typename... ArgsT>
    struct Arg1TypeT<RetT(ArgsT...)>
    {
        using Type = typename VariadicArgs<ArgsT...>::Type1;
    };
    template<typename T>
    using Arg1Type = typename Arg1TypeT<T>::Type;

    template<typename RetT, typename... ArgsT>
    struct Arg2TypeT
    {};
    template<typename RetT, typename... ArgsT>
    struct Arg2TypeT<RetT(ArgsT...)>
    {
        using Type = typename VariadicArgs<ArgsT...>::Type2;
    };
    template<typename T>
    using Arg2Type = typename Arg2TypeT<T>::Type;

    template<typename R, typename T>
    static inline R Convert(const T& v) {}

    template<>
    [[maybe_unused]]
    inline int64_t Convert<int64_t, int64_t>(const int64_t& v)
    {
        return v;
    }
    template<>
    inline int64_t Convert<int64_t, double>(const double& v)
    {
        return static_cast<int64_t>(v);
    }
    template<>
    inline int64_t Convert<int64_t, std::string>(const std::string& v)
    {
        return std::stoll(v.c_str(), nullptr, 0);
    }

    template<>
    inline double Convert<double, int64_t>(const int64_t& v)
    {
        return static_cast<double>(v);
    }
    template<>
    [[maybe_unused]]
    inline double Convert<double, double>(const double& v)
    {
        return v;
    }
    template<>
    inline double Convert<double, std::string>(const std::string& v)
    {
        return std::stod(v.c_str(), nullptr);
    }

    template<>
    inline std::string Convert<std::string, int64_t>(const int64_t& v)
    {
        return std::to_string(v);
    }
    template<>
    inline std::string Convert<std::string, double>(const double& v)
    {
        return std::to_string(v);
    }
    template<>
    [[maybe_unused]]
    inline std::string Convert<std::string, std::string>(const std::string& v)
    {
        return v;
    }

    //this enum must sync with ApiInfo::ApiId from DebugScriptCompiler.cs
    enum class ApiEnum
    {
        Platform = 0,
        ScriptAssert,
        DumpStack,
        Printf,
        Format,
        Time,
        FloatTime,
        DumpCascadePtr,
        StringLength,
        StringContains,
        StringContainsAny,
        StringNotContains,
        StringNotContainsAny,
        StringFind,
        StringRightFind,
        SubString,
        CStrLen,
        CStrStr,
        CStrCmp,
        Malloc,
        Free,
        MemCpy,
        MemSet,
        MemSave,
        MemLoad,
        Num
    };
    struct Api
    {
        static inline void CallExternApi(int api, bool isGlobal, TypeEnum ty, int index, int argNum, int32_t firstOperand, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals, std::vector<ExternApiArgOrRetVal>& args)
        {
            //ExternApi
            ExternApiArgOrRetVal retVal{};
            retVal.Type = ty;
            retVal.IsGlobal = isGlobal;
            retVal.Index = index;

            if (static_cast<int>(args.size()) < argNum) {
                args.resize(argNum);
            }

            if (argNum > 0) {
                bool isGlobal0;
                TypeEnum ty0;
                int32_t index0;
                DecodeOperand2(firstOperand, isGlobal0, ty0, index0);
                args[0].Type = ty0;
                args[0].IsGlobal = isGlobal0;
                args[0].Index = index0;
            }

            for (int i = 1; i < argNum; i += 2) {
                ++pos;
                int operand = codes[pos];

                if (i < argNum) {
                    bool isGlobal1;
                    TypeEnum ty1;
                    int32_t index1;
                    DecodeOperand1(operand, isGlobal1, ty1, index1);
                    args[i].Type = ty1;
                    args[i].IsGlobal = isGlobal1;
                    args[i].Index = index1;
                }
                if (i + 1 < argNum) {
                    bool isGlobal2;
                    TypeEnum ty2;
                    int32_t index2;
                    DecodeOperand2(operand, isGlobal2, ty2, index2);
                    args[i + 1].Type = ty2;
                    args[i + 1].IsGlobal = isGlobal2;
                    args[i + 1].Index = index2;
                }
            }

            CppDbgScp_CallExternApi(api, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals, args.data(), argNum, retVal);
        }
        static inline int64_t Platform(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
#if defined(_MSC_VER)
            return 0; //win
#elif defined(__ANDROID__)
            return 1; //android
#elif defined(__APPLE__)
            return 2; //apple
#else
            return 3; //other
#endif
        }
        static inline int64_t ScriptAssert(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            ++pos;
            int operand = codes[pos];

            bool isGlobal;
            TypeEnum ty;
            int32_t index;
            DecodeOperand1(operand, isGlobal, ty, index);
            MyAssert(ty == TypeEnum::Int);
            int64_t cond = GetVarInt(isGlobal, index, stackBase, intLocals, intGlobals);
            if (argNum == 1) {
                if (cond == 0) {
                    MyDebugBreak();
                }
            }
            else if (argNum == 2) {
                bool isGlobal2;
                TypeEnum ty2;
                int32_t index2;
                DecodeOperand2(operand, isGlobal2, ty2, index2);
                MyAssert(ty2 == TypeEnum::String);
                const std::string& msg = GetVarString(isGlobal2, index2, stackBase, strLocals, strGlobals);
                if (cond == 0) {
                    myprintf("DebugBreak:%s\n", msg.c_str());
                    MyDebugBreak();
                }
            }
            return 0;
        }
        static inline void DumpStack(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            ++pos;
            int operand = codes[pos];

            bool isGlobal;
            TypeEnum ty;
            int32_t index;
            DecodeOperand1(operand, isGlobal, ty, index);
            MyAssert(ty == TypeEnum::String);
            const std::string& prefix = GetVarString(isGlobal, index, stackBase, strLocals, strGlobals);
            mylog_dump_callstack(prefix.c_str(), __FILE__, __LINE__);
        }
        static inline int64_t Printf(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            const std::string* pStr = nullptr;
            std::vector<int64_t> args{};
            args.reserve(argNum - 1);

            for (int i = 0; i < argNum; i += 2) {
                ++pos;
                int operand = codes[pos];

                if (i < argNum) {
                    if (i == 0) {
                        bool isGlobal;
                        TypeEnum ty;
                        int32_t index;
                        DecodeOperand1(operand, isGlobal, ty, index);
                        MyAssert(ty == TypeEnum::String);
                        pStr = &GetVarString(isGlobal, index, stackBase, strLocals, strGlobals);
                    }
                    else {
                        int64_t val = 0;

                        bool isGlobal1;
                        TypeEnum ty1;
                        int32_t index1;
                        DecodeOperand1(operand, isGlobal1, ty1, index1);
                        switch (ty1) {
                        case TypeEnum::Int: {
                            val = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
                        }break;
                        case TypeEnum::Float: {
                            double v = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
                            val = *reinterpret_cast<int64_t*>(&v);
                        }break;
                        case TypeEnum::String: {
                            const std::string& v = GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals);
                            val = reinterpret_cast<int64_t>(v.c_str());
                        }break;
                        default:
                            break;
                        }

                        args.push_back(val);
                    }
                }
                if (i + 1 < argNum) {
                    int64_t val = 0;

                    bool isGlobal2;
                    TypeEnum ty2;
                    int32_t index2;
                    DecodeOperand2(operand, isGlobal2, ty2, index2);
                    switch (ty2) {
                    case TypeEnum::Int: {
                        val = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);
                    }break;
                    case TypeEnum::Float: {
                        double v = GetVarFloat(isGlobal2, index2, stackBase, fltLocals, fltGlobals);
                        val = *reinterpret_cast<int64_t*>(&v);
                    }break;
                    case TypeEnum::String: {
                        const std::string& v = GetVarString(isGlobal2, index2, stackBase, strLocals, strGlobals);
                        val = reinterpret_cast<int64_t>(v.c_str());
                    }break;
                    default:
                        break;
                    }

                    args.push_back(val);
                }
            }

            const std::string& fmt = *pStr;
            int64_t ct = script_printf(fmt.c_str(), args.data());
            return ct;
        }
        static inline std::string Format(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            const std::string* pStr = nullptr;
            std::vector<int64_t> args{};
            args.reserve(argNum - 1);
            const int c_strbuf_size = 4096;
            std::vector<int8_t> strbuf(c_strbuf_size + 1);
            char* pbuf = reinterpret_cast<char*>(strbuf.data());

            for (int i = 0; i < argNum; i += 2) {
                ++pos;
                int operand = codes[pos];

                if (i < argNum) {
                    if (i == 0) {
                        bool isGlobal;
                        TypeEnum ty;
                        int32_t index;
                        DecodeOperand1(operand, isGlobal, ty, index);
                        MyAssert(ty == TypeEnum::String);
                        pStr = &GetVarString(isGlobal, index, stackBase, strLocals, strGlobals);
                    }
                    else {
                        int64_t val = 0;

                        bool isGlobal1;
                        TypeEnum ty1;
                        int32_t index1;
                        DecodeOperand1(operand, isGlobal1, ty1, index1);
                        switch (ty1) {
                        case TypeEnum::Int: {
                            val = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
                        }break;
                        case TypeEnum::Float: {
                            double v = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
                            val = *reinterpret_cast<int64_t*>(&v);
                        }break;
                        case TypeEnum::String: {
                            const std::string& v = GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals);
                            val = reinterpret_cast<int64_t>(v.c_str());
                        }break;
                        default:
                            break;
                        }

                        args.push_back(val);
                    }
                }
                if (i + 1 < argNum) {
                    int64_t val = 0;

                    bool isGlobal2;
                    TypeEnum ty2;
                    int32_t index2;
                    DecodeOperand2(operand, isGlobal2, ty2, index2);
                    switch (ty2) {
                    case TypeEnum::Int: {
                        val = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);
                    }break;
                    case TypeEnum::Float: {
                        double v = GetVarFloat(isGlobal2, index2, stackBase, fltLocals, fltGlobals);
                        val = *reinterpret_cast<int64_t*>(&v);
                    }break;
                    case TypeEnum::String: {
                        const std::string& v = GetVarString(isGlobal2, index2, stackBase, strLocals, strGlobals);
                        val = reinterpret_cast<int64_t>(v.c_str());
                    }break;
                    default:
                        break;
                    }

                    args.push_back(val);
                }
            }

            const std::string& fmt = *pStr;
            int64_t ct = script_snprintf(pbuf, c_strbuf_size + 1, fmt.c_str(), args.data());
            MyAssert(ct <= c_strbuf_size);
            pbuf[c_strbuf_size] = 0;
            return std::string(pbuf);
        }
        static inline int64_t Time(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            auto&& duration = std::chrono::high_resolution_clock::now() - g_start_time;
            auto&& ms = std::chrono::duration_cast<std::chrono::microseconds>(duration);
            return ms.count();
        }
        static inline double FloatTime(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            using double_microseconds = std::chrono::duration<double, std::micro>;
            auto&& duration = std::chrono::high_resolution_clock::now() - g_start_time;
            auto&& ms = std::chrono::duration_cast<double_microseconds>(duration);
            return ms.count();
        }
        static inline int64_t DumpCascadePtr(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            int64_t addr = 0;
            std::vector<int32_t> args{};
            args.reserve(argNum - 1);

            for (int i = 0; i < argNum; i += 2) {
                ++pos;
                int operand = codes[pos];

                if (i < argNum) {
                    if (i == 0) {
                        bool isGlobal;
                        TypeEnum ty;
                        int32_t index;
                        DecodeOperand1(operand, isGlobal, ty, index);
                        MyAssert(ty == TypeEnum::Int);
                        addr = GetVarInt(isGlobal, index, stackBase, intLocals, intGlobals);
                    }
                    else {
                        bool isGlobal1;
                        TypeEnum ty1;
                        int32_t index1;
                        DecodeOperand1(operand, isGlobal1, ty1, index1);
                        MyAssert(ty1 == TypeEnum::Int);
                        int32_t val = static_cast<int32_t>(GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals));
                        args.push_back(val);
                    }
                }
                if (i + 1 < argNum) {
                    bool isGlobal2;
                    TypeEnum ty2;
                    int32_t index2;
                    DecodeOperand2(operand, isGlobal2, ty2, index2);
                    MyAssert(ty2 == TypeEnum::Int);
                    int32_t val = static_cast<int32_t>(GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals));
                    args.push_back(val);
                }
            }

            int64_t taddr = dumpcascadeptr(addr, args.data(), argNum - 1);
            return taddr;
        }
        static inline int64_t StringLength(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            ++pos;
            int operand = codes[pos];

            bool isGlobal;
            TypeEnum ty;
            int32_t index;
            DecodeOperand1(operand, isGlobal, ty, index);
            MyAssert(ty == TypeEnum::String);
            const std::string& str = GetVarString(isGlobal, index, stackBase, strLocals, strGlobals);

            return static_cast<int64_t>(str.length());
        }
        static inline int64_t StringContains(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            const std::string* pStr = nullptr;
            int retVal = 1;

            for (int i = 0; i < argNum; i += 2) {
                ++pos;
                int operand = codes[pos];

                if (i < argNum) {
                    if (i == 0) {
                        bool isGlobal;
                        TypeEnum ty;
                        int32_t index;
                        DecodeOperand1(operand, isGlobal, ty, index);
                        MyAssert(ty == TypeEnum::String);
                        const std::string& str = GetVarString(isGlobal, index, stackBase, strLocals, strGlobals);
                        pStr = &str;
                    }
                    else {
                        bool isGlobal1;
                        TypeEnum ty1;
                        int32_t index1;
                        DecodeOperand1(operand, isGlobal1, ty1, index1);
                        MyAssert(ty1 == TypeEnum::String);
                        const std::string& v = GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals);
                        const std::string& str = *pStr;
                        if (v.length() > 0 && str.find(v) == std::string::npos) {
                            retVal = 0;
                            break;
                        }
                    }
                }
                if (i + 1 < argNum) {
                    bool isGlobal2;
                    TypeEnum ty2;
                    int32_t index2;
                    DecodeOperand2(operand, isGlobal2, ty2, index2);
                    MyAssert(ty2 == TypeEnum::String);
                    const std::string& v = GetVarString(isGlobal2, index2, stackBase, strLocals, strGlobals);
                    const std::string& str = *pStr;
                    if (v.length() > 0 && str.find(v) == std::string::npos) {
                        retVal = 0;
                        break;
                    }
                }
            }
            return retVal;
        }
        static inline int64_t StringContainsAny(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            const std::string* pStr = nullptr;
            int retVal = 0;

            for (int i = 0; i < argNum; i += 2) {
                ++pos;
                int operand = codes[pos];

                if (i < argNum) {
                    if (i == 0) {
                        bool isGlobal;
                        TypeEnum ty;
                        int32_t index;
                        DecodeOperand1(operand, isGlobal, ty, index);
                        MyAssert(ty == TypeEnum::String);
                        const std::string& str = GetVarString(isGlobal, index, stackBase, strLocals, strGlobals);
                        pStr = &str;
                    }
                    else {
                        bool isGlobal1;
                        TypeEnum ty1;
                        int32_t index1;
                        DecodeOperand1(operand, isGlobal1, ty1, index1);
                        MyAssert(ty1 == TypeEnum::String);
                        const std::string& v = GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals);
                        const std::string& str = *pStr;
                        if (v.length() > 0 && str.find(v) != std::string::npos) {
                            retVal = 1;
                            break;
                        }
                    }
                }
                if (i + 1 < argNum) {
                    bool isGlobal2;
                    TypeEnum ty2;
                    int32_t index2;
                    DecodeOperand2(operand, isGlobal2, ty2, index2);
                    MyAssert(ty2 == TypeEnum::String);
                    const std::string& v = GetVarString(isGlobal2, index2, stackBase, strLocals, strGlobals);
                    const std::string& str = *pStr;
                    if (v.length() > 0 && str.find(v) != std::string::npos) {
                        retVal = 1;
                        break;
                    }
                }
            }
            return retVal;
        }
        static inline int64_t StringNotContains(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            const std::string* pStr = nullptr;
            int retVal = 1;

            for (int i = 0; i < argNum; i += 2) {
                ++pos;
                int operand = codes[pos];

                if (i < argNum) {
                    if (i == 0) {
                        bool isGlobal;
                        TypeEnum ty;
                        int32_t index;
                        DecodeOperand1(operand, isGlobal, ty, index);
                        MyAssert(ty == TypeEnum::String);
                        const std::string& str = GetVarString(isGlobal, index, stackBase, strLocals, strGlobals);
                        pStr = &str;
                    }
                    else {
                        bool isGlobal1;
                        TypeEnum ty1;
                        int32_t index1;
                        DecodeOperand1(operand, isGlobal1, ty1, index1);
                        MyAssert(ty1 == TypeEnum::String);
                        const std::string& v = GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals);
                        const std::string& str = *pStr;
                        if (v.length() > 0 && str.find(v) != std::string::npos) {
                            retVal = 0;
                            break;
                        }
                    }
                }
                if (i + 1 < argNum) {
                    bool isGlobal2;
                    TypeEnum ty2;
                    int32_t index2;
                    DecodeOperand2(operand, isGlobal2, ty2, index2);
                    MyAssert(ty2 == TypeEnum::String);
                    const std::string& v = GetVarString(isGlobal2, index2, stackBase, strLocals, strGlobals);
                    const std::string& str = *pStr;
                    if (v.length() > 0 && str.find(v) != std::string::npos) {
                        retVal = 0;
                        break;
                    }
                }
            }
            return retVal;
        }
        static inline int64_t StringNotContainsAny(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            const std::string* pStr = nullptr;
            int retVal = 0;

            for (int i = 0; i < argNum; i += 2) {
                ++pos;
                int operand = codes[pos];

                if (i < argNum) {
                    if (i == 0) {
                        bool isGlobal;
                        TypeEnum ty;
                        int32_t index;
                        DecodeOperand1(operand, isGlobal, ty, index);
                        MyAssert(ty == TypeEnum::String);
                        const std::string& str = GetVarString(isGlobal, index, stackBase, strLocals, strGlobals);
                        pStr = &str;
                    }
                    else {
                        bool isGlobal1;
                        TypeEnum ty1;
                        int32_t index1;
                        DecodeOperand1(operand, isGlobal1, ty1, index1);
                        MyAssert(ty1 == TypeEnum::String);
                        const std::string& v = GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals);
                        const std::string& str = *pStr;
                        if (v.length() > 0 && str.find(v) == std::string::npos) {
                            retVal = 1;
                            break;
                        }
                    }
                }
                if (i + 1 < argNum) {
                    bool isGlobal2;
                    TypeEnum ty2;
                    int32_t index2;
                    DecodeOperand2(operand, isGlobal2, ty2, index2);
                    MyAssert(ty2 == TypeEnum::String);
                    const std::string& v = GetVarString(isGlobal2, index2, stackBase, strLocals, strGlobals);
                    const std::string& str = *pStr;
                    if (v.length() > 0 && str.find(v) == std::string::npos) {
                        retVal = 1;
                        break;
                    }
                }
            }
            return retVal;
        }
        static inline int64_t StringFind(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            ++pos;
            int operand = codes[pos];

            bool isGlobal;
            TypeEnum ty;
            int32_t index;
            DecodeOperand1(operand, isGlobal, ty, index);
            MyAssert(ty == TypeEnum::String);
            const std::string& str = GetVarString(isGlobal, index, stackBase, strLocals, strGlobals);

            bool isGlobal1;
            TypeEnum ty1;
            int32_t index1;
            DecodeOperand2(operand, isGlobal1, ty1, index1);
            MyAssert(ty1 == TypeEnum::String);
            const std::string& v = GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals);

            int64_t start = 0;
            if (argNum > 2) {
                ++pos;
                int operand2 = codes[pos];

                bool isGlobal2;
                TypeEnum ty2;
                int32_t index2;
                DecodeOperand1(operand2, isGlobal2, ty2, index2);
                MyAssert(ty2 == TypeEnum::Int);
                start = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);
            }

            auto&& rpos = str.find(v, start);
            if (rpos == std::string::npos)
                return -1;
            else
                return static_cast<int64_t>(rpos);
        }
        static inline int64_t StringRightFind(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            ++pos;
            int operand = codes[pos];

            bool isGlobal;
            TypeEnum ty;
            int32_t index;
            DecodeOperand1(operand, isGlobal, ty, index);
            MyAssert(ty == TypeEnum::String);
            const std::string& str = GetVarString(isGlobal, index, stackBase, strLocals, strGlobals);

            bool isGlobal1;
            TypeEnum ty1;
            int32_t index1;
            DecodeOperand2(operand, isGlobal1, ty1, index1);
            MyAssert(ty1 == TypeEnum::String);
            const std::string& v = GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals);

            std::size_t start = std::string::npos;
            if (argNum > 2) {
                ++pos;
                int operand2 = codes[pos];

                bool isGlobal2;
                TypeEnum ty2;
                int32_t index2;
                DecodeOperand1(operand2, isGlobal2, ty2, index2);
                MyAssert(ty2 == TypeEnum::Int);
                start = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);
            }

            auto&& rpos = str.rfind(v, start);
            if (rpos == std::string::npos)
                return -1;
            else
                return static_cast<int64_t>(rpos);
        }
        static inline std::string SubString(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            ++pos;
            int operand = codes[pos];

            bool isGlobal;
            TypeEnum ty;
            int32_t index;
            DecodeOperand1(operand, isGlobal, ty, index);
            MyAssert(ty == TypeEnum::String);
            const std::string& str = GetVarString(isGlobal, index, stackBase, strLocals, strGlobals);

            bool isGlobal1;
            TypeEnum ty1;
            int32_t index1;
            DecodeOperand2(operand, isGlobal1, ty1, index1);
            MyAssert(ty1 == TypeEnum::Int);
            std::size_t off = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);

            std::size_t count = std::string::npos;
            if (argNum > 2) {
                ++pos;
                int operand2 = codes[pos];

                bool isGlobal2;
                TypeEnum ty2;
                int32_t index2;
                DecodeOperand1(operand2, isGlobal2, ty2, index2);
                MyAssert(ty2 == TypeEnum::Int);
                count = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);
            }

            auto&& rstr = str.substr(off, count);
            return rstr;
        }
        static inline int64_t CStrLen(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            ++pos;
            int operand = codes[pos];

            bool isGlobal;
            TypeEnum ty;
            int32_t index;
            DecodeOperand1(operand, isGlobal, ty, index);
            MyAssert(ty == TypeEnum::Int);
            int64_t strAddr = GetVarInt(isGlobal, index, stackBase, intLocals, intGlobals);
            if (strAddr)
                return static_cast<int64_t>(std::strlen(reinterpret_cast<const char*>(strAddr)));
            else
                return 0;
        }
        static inline int64_t CStrStr(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            ++pos;
            int operand = codes[pos];

            bool isGlobal;
            TypeEnum ty;
            int32_t index;
            DecodeOperand1(operand, isGlobal, ty, index);
            MyAssert(ty == TypeEnum::Int);
            int64_t strAddr = GetVarInt(isGlobal, index, stackBase, intLocals, intGlobals);

            bool isGlobal1;
            TypeEnum ty1;
            int32_t index1;
            DecodeOperand2(operand, isGlobal1, ty1, index1);
            MyAssert(ty1 == TypeEnum::Int);
            int64_t strAddr2 = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
            if (strAddr && strAddr2)
                return reinterpret_cast<int64_t>(std::strstr(reinterpret_cast<const char*>(strAddr), reinterpret_cast<const char*>(strAddr2)));
            else
                return 0;
        }
        static inline int64_t CStrCmp(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            ++pos;
            int operand = codes[pos];

            bool isGlobal;
            TypeEnum ty;
            int32_t index;
            DecodeOperand1(operand, isGlobal, ty, index);
            MyAssert(ty == TypeEnum::Int);
            int64_t strAddr = GetVarInt(isGlobal, index, stackBase, intLocals, intGlobals);

            bool isGlobal1;
            TypeEnum ty1;
            int32_t index1;
            DecodeOperand2(operand, isGlobal1, ty1, index1);
            MyAssert(ty1 == TypeEnum::Int);
            int64_t strAddr2 = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);

            if (strAddr && strAddr2) {
                if (argNum > 2) {
                    ++pos;
                    int operand2 = codes[pos];

                    bool isGlobal2;
                    TypeEnum ty2;
                    int32_t index2;
                    DecodeOperand1(operand2, isGlobal2, ty2, index2);
                    MyAssert(ty2 == TypeEnum::Int);
                    std::size_t count = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);

                    return std::strncmp(reinterpret_cast<const char*>(strAddr), reinterpret_cast<const char*>(strAddr2), count);
                }
                else {
                    return std::strcmp(reinterpret_cast<const char*>(strAddr), reinterpret_cast<const char*>(strAddr2));
                }
            }
            else {
                return 0;
            }
        }
        static inline int64_t Malloc(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            ++pos;
            int operand = codes[pos];

            bool isGlobal;
            TypeEnum ty;
            int32_t index;
            DecodeOperand1(operand, isGlobal, ty, index);
            MyAssert(ty == TypeEnum::Int);
            int64_t size = GetVarInt(isGlobal, index, stackBase, intLocals, intGlobals);

            void* fptr = std::malloc(static_cast<size_t>(size));
            return reinterpret_cast<int64_t>(fptr);
        }
        static inline void Free(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            ++pos;
            int operand = codes[pos];

            bool isGlobal;
            TypeEnum ty;
            int32_t index;
            DecodeOperand1(operand, isGlobal, ty, index);
            MyAssert(ty == TypeEnum::Int);
            int64_t addr = GetVarInt(isGlobal, index, stackBase, intLocals, intGlobals);
            if (addr) {
                std::free(reinterpret_cast<void*>(addr));
            }
        }
        static inline int64_t MemCpy(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            ++pos;
            int operand = codes[pos];

            bool isGlobal;
            TypeEnum ty;
            int32_t index;
            DecodeOperand1(operand, isGlobal, ty, index);
            MyAssert(ty == TypeEnum::Int);
            int64_t dest = GetVarInt(isGlobal, index, stackBase, intLocals, intGlobals);

            bool isGlobal1;
            TypeEnum ty1;
            int32_t index1;
            DecodeOperand2(operand, isGlobal1, ty1, index1);
            MyAssert(ty1 == TypeEnum::Int);
            int64_t src = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);

            ++pos;
            int operand2 = codes[pos];

            bool isGlobal2;
            TypeEnum ty2;
            int32_t index2;
            DecodeOperand1(operand2, isGlobal2, ty2, index2);
            MyAssert(ty2 == TypeEnum::Int);
            std::size_t size = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);

            if (dest && src) {
                void* fptr = std::memcpy(reinterpret_cast<void*>(dest), reinterpret_cast<const void*>(src), size);
                return reinterpret_cast<int64_t>(fptr);
            }
            else {
                return 0;
            }
        }
        static inline int64_t MemSet(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            ++pos;
            int operand = codes[pos];

            bool isGlobal;
            TypeEnum ty;
            int32_t index;
            DecodeOperand1(operand, isGlobal, ty, index);
            MyAssert(ty == TypeEnum::Int);
            int64_t dest = GetVarInt(isGlobal, index, stackBase, intLocals, intGlobals);

            bool isGlobal1;
            TypeEnum ty1;
            int32_t index1;
            DecodeOperand2(operand, isGlobal1, ty1, index1);
            MyAssert(ty1 == TypeEnum::Int);
            int64_t val = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);

            ++pos;
            int operand2 = codes[pos];

            bool isGlobal2;
            TypeEnum ty2;
            int32_t index2;
            DecodeOperand1(operand2, isGlobal2, ty2, index2);
            MyAssert(ty2 == TypeEnum::Int);
            std::size_t size = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);

            if (dest) {
                void* fptr = std::memset(reinterpret_cast<void*>(dest), static_cast<int>(val), size);
                return reinterpret_cast<int64_t>(fptr);
            }
            else {
                return 0;
            }
        }
        static inline int64_t MemSave(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            ++pos;
            int operand = codes[pos];

            bool isGlobal;
            TypeEnum ty;
            int32_t index;
            DecodeOperand1(operand, isGlobal, ty, index);
            MyAssert(ty == TypeEnum::Int);
            int64_t src = GetVarInt(isGlobal, index, stackBase, intLocals, intGlobals);

            bool isGlobal1;
            TypeEnum ty1;
            int32_t index1;
            DecodeOperand2(operand, isGlobal1, ty1, index1);
            MyAssert(ty1 == TypeEnum::Int);
            std::size_t size = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);

            ++pos;
            int operand2 = codes[pos];

            bool isGlobal2;
            TypeEnum ty2;
            int32_t index2;
            DecodeOperand1(operand2, isGlobal2, ty2, index2);
            MyAssert(ty2 == TypeEnum::String);
            const std::string& file = GetVarString(isGlobal2, index2, stackBase, strLocals, strGlobals);

            if (src) {
                std::ofstream ofile(file, std::ios::out | std::ios::binary);
                if (ofile.fail())
                    return 0;
                ofile.write(reinterpret_cast<const char*>(src), size);
                int64_t ret = static_cast<int64_t>(ofile.tellp());
                ofile.close();
                return ret;
            }
            else {
                return 0;
            }
        }
        static inline int64_t MemLoad(int argNum, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            ++pos;
            int operand = codes[pos];

            bool isGlobal;
            TypeEnum ty;
            int32_t index;
            DecodeOperand1(operand, isGlobal, ty, index);
            MyAssert(ty == TypeEnum::Int);
            int64_t dest = GetVarInt(isGlobal, index, stackBase, intLocals, intGlobals);

            bool isGlobal1;
            TypeEnum ty1;
            int32_t index1;
            DecodeOperand2(operand, isGlobal1, ty1, index1);
            MyAssert(ty1 == TypeEnum::Int);
            std::size_t size = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);

            ++pos;
            int operand2 = codes[pos];

            bool isGlobal2;
            TypeEnum ty2;
            int32_t index2;
            DecodeOperand1(operand2, isGlobal2, ty2, index2);
            MyAssert(ty2 == TypeEnum::String);
            const std::string& file = GetVarString(isGlobal2, index2, stackBase, strLocals, strGlobals);

            if (dest) {
                std::ifstream ifile(file, std::ios::out | std::ios::binary);
                if (ifile.fail())
                    return 0;
                ifile.read(reinterpret_cast<char*>(dest), size);
                int64_t ret = static_cast<int64_t>(ifile.tellg());
                ifile.close();
                return ret;
            }
            else {
                return 0;
            }
        }
    };
    static inline void DoCallIntern(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        int apiIndex = static_cast<int>(op) - static_cast<int>(InsEnum::CALLINTERN_FIRST);
        ApiEnum api = static_cast<ApiEnum>(apiIndex);
        switch (api) {
        case ApiEnum::Platform: {
            int64_t val = Api::Platform(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::ScriptAssert: {
            int64_t val = Api::ScriptAssert(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::DumpStack: {
            Api::DumpStack(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
        }break;
        case ApiEnum::Printf: {
            int64_t val = Api::Printf(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);;
        }break;
        case ApiEnum::Format: {
            std::string val = Api::Format(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::String);
            SetVarString(isGlobal, index, val, stackBase, strLocals, strGlobals);
        }break;
        case ApiEnum::Time: {
            int64_t val = Api::Time(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::FloatTime: {
            double val = Api::FloatTime(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Float);
            SetVarFloat(isGlobal, index, val, stackBase, fltLocals, fltGlobals);
        }break;
        case ApiEnum::DumpCascadePtr: {
            int64_t val = Api::DumpCascadePtr(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::StringLength: {
            int64_t val = Api::StringLength(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::StringContains: {
            int64_t val = Api::StringContains(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::StringContainsAny: {
            int64_t val = Api::StringContainsAny(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::StringNotContains: {
            int64_t val = Api::StringNotContains(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::StringNotContainsAny: {
            int64_t val = Api::StringNotContainsAny(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::StringFind: {
            int64_t val = Api::StringFind(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::StringRightFind: {
            int64_t val = Api::StringRightFind(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::SubString: {
            std::string val = Api::SubString(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::String);
            SetVarString(isGlobal, index, val, stackBase, strLocals, strGlobals);
        }break;
        case ApiEnum::CStrLen: {
            int64_t val = Api::CStrLen(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::CStrStr: {
            int64_t val = Api::CStrStr(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::CStrCmp: {
            int64_t val = Api::CStrCmp(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::Malloc: {
            int64_t val = Api::Malloc(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::Free: {
            Api::Free(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
        }break;
        case ApiEnum::MemCpy: {
            int64_t val = Api::MemCpy(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::MemSet: {
            int64_t val = Api::MemSet(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::MemSave: {
            int64_t val = Api::MemSave(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        case ApiEnum::MemLoad: {
            int64_t val = Api::MemLoad(argNum, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
            MyAssert(ty == TypeEnum::Int);
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }break;
        default: {
        }break;
        }
    }
    static inline void DoCallExtern(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals, std::vector<ExternApiArgOrRetVal>& args)
    {
        ++pos;
        int32_t operand = codes[pos];
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        int32_t apiIndex;
        DecodeOperand1(operand, apiIndex);
        MyAssert(apiIndex >= 0);
        Api::CallExternApi(apiIndex, isGlobal, ty, index, argNum, operand, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals, args);
    }
    static inline void DoRet(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals, bool& retVal)
    {
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        switch (ty) {
        case TypeEnum::Int: {
            int64_t val = GetVarInt(isGlobal, index, stackBase, intLocals, intGlobals);
            retVal = val != 0;
        }break;
        case TypeEnum::Float: {
            double val = GetVarFloat(isGlobal, index, stackBase, fltLocals, fltGlobals);
            retVal = val != 0;
        }break;
        default:
            break;
        }
    }
    static inline void DoJmp(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        int32_t enterPos = pos;
        int32_t offset;
        DecodeOpcode(opcode, op, offset);
        pos = enterPos - 1 + offset;
    }
    static inline void DoJmpIf(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        int32_t enterPos = pos;
        ++pos;
        int32_t operand = codes[pos];

        int32_t offset;
        DecodeOpcode(opcode, op, offset);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        switch (ty1) {
        case TypeEnum::Int: {
            int64_t val = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
            if (val != 0) {
                pos = enterPos - 1 + offset;
            }
        }break;
        case TypeEnum::Float: {
            double val = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
            if (val != 0) {
                pos = enterPos - 1 + offset;
            }
        }break;
        case TypeEnum::String: {
            const std::string& val = GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals);
            if (val.length() > 0) {
                pos = enterPos - 1 + offset;
            }
        }break;
        default:
            break;
        }
    }
    static inline void DoJmpIfNot(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        int32_t enterPos = pos;
        ++pos;
        int32_t operand = codes[pos];

        int32_t offset;
        DecodeOpcode(opcode, op, offset);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        switch (ty1) {
        case TypeEnum::Int: {
            int64_t val = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
            if (val == 0) {
                pos = enterPos - 1 + offset;
            }
        }break;
        case TypeEnum::Float: {
            double val = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
            if (val == 0) {
                pos = enterPos - 1 + offset;
            }
        }break;
        case TypeEnum::String: {
            const std::string& val = GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals);
            if (val.length() == 0) {
                pos = enterPos - 1 + offset;
            }
        }break;
        default:
            break;
        }
    }
    static inline void DoInc(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        MyAssert(ty == TypeEnum::Int);
        MyAssert(ty1 == TypeEnum::Int);
        int64_t val = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
        ++val;
        SetVarInt(isGlobal1, index1, val, stackBase, intLocals, intGlobals);
        SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
    }
    static inline void DoIncFlt(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        MyAssert(ty == TypeEnum::Float);
        MyAssert(ty1 == TypeEnum::Float);
        double val = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
        ++val;
        SetVarFloat(isGlobal1, index1, val, stackBase, fltLocals, fltGlobals);
        SetVarFloat(isGlobal, index, val, stackBase, fltLocals, fltGlobals);
    }
    static inline void DoIncVal(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty1 == TypeEnum::Int && ty2 == TypeEnum::Int && ty == TypeEnum::Int);
        int64_t inc = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);
        int64_t val = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
        val += inc;
        SetVarInt(isGlobal1, index1, val, stackBase, intLocals, intGlobals);
        SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
    }
    static inline void DoIncValFlt(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty1 == TypeEnum::Float && ty2 == TypeEnum::Float && ty == TypeEnum::Float);
        double inc = GetVarFloat(isGlobal2, index2, stackBase, fltLocals, fltGlobals);
        double val = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
        val += inc;
        SetVarFloat(isGlobal1, index1, val, stackBase, fltLocals, fltGlobals);
        SetVarFloat(isGlobal, index, val, stackBase, fltLocals, fltGlobals);
    }
    static inline void DoDec(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        MyAssert(ty == TypeEnum::Int);
        MyAssert(ty1 == TypeEnum::Int);
        int64_t val = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
        --val;
        SetVarInt(isGlobal1, index1, val, stackBase, intLocals, intGlobals);
        SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
    }
    static inline void DoDecFlt(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        MyAssert(ty == TypeEnum::Float);
        MyAssert(ty1 == TypeEnum::Float);
        double val = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
        --val;
        SetVarFloat(isGlobal1, index1, val, stackBase, fltLocals, fltGlobals);
        SetVarFloat(isGlobal, index, val, stackBase, fltLocals, fltGlobals);
    }
    static inline void DoDecVal(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty1 == TypeEnum::Int && ty2 == TypeEnum::Int && ty == TypeEnum::Int);
        int64_t inc = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);
        int64_t val = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
        val -= inc;
        SetVarInt(isGlobal1, index1, val, stackBase, intLocals, intGlobals);
        SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
    }
    static inline void DoDecValFlt(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty1 == TypeEnum::Float && ty2 == TypeEnum::Float && ty == TypeEnum::Float);
        double inc = GetVarFloat(isGlobal2, index2, stackBase, fltLocals, fltGlobals);
        double val = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
        val -= inc;
        SetVarFloat(isGlobal1, index1, val, stackBase, fltLocals, fltGlobals);
        SetVarFloat(isGlobal, index, val, stackBase, fltLocals, fltGlobals);
    }
    static inline void DoMov(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        MyAssert(ty1 == TypeEnum::Int && ty == TypeEnum::Int);
        int64_t val = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
        SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
    }
    static inline void DoMovFlt(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        MyAssert(ty1 == TypeEnum::Float && ty == TypeEnum::Float);
        double val = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
        SetVarFloat(isGlobal, index, val, stackBase, fltLocals, fltGlobals);
    }
    static inline void DoMovStr(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        MyAssert(ty1 == TypeEnum::String && ty == TypeEnum::String);
        const std::string& val = GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals);
        SetVarString(isGlobal, index, val, stackBase, strLocals, strGlobals);
    }
    static inline void DoArrayGet(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty1 == TypeEnum::Int && ty2 == TypeEnum::Int && ty == TypeEnum::Int);
        int32_t ix = static_cast<int32_t>(GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals));
        int64_t val = GetVarInt(isGlobal1, index1 + ix, stackBase, intLocals, intGlobals);
        SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
    }
    static inline void DoArrayGetFlt(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty1 == TypeEnum::Float && ty2 == TypeEnum::Int && ty == TypeEnum::Float);
        int32_t ix = static_cast<int32_t>(GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals));
        double val = GetVarFloat(isGlobal1, index1 + ix, stackBase, fltLocals, fltGlobals);
        SetVarFloat(isGlobal, index, val, stackBase, fltLocals, fltGlobals);
    }
    static inline void DoArrayGetStr(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty1 == TypeEnum::String && ty2 == TypeEnum::Int && ty == TypeEnum::String);
        int32_t ix = static_cast<int32_t>(GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals));
        const std::string& val = GetVarString(isGlobal1, index1 + ix, stackBase, strLocals, strGlobals);
        SetVarString(isGlobal, index, val, stackBase, strLocals, strGlobals);
    }
    static inline void DoArraySet(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty1 == TypeEnum::Int && ty2 == TypeEnum::Int && ty == TypeEnum::Int);
        int32_t ix = static_cast<int32_t>(GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals));
        int64_t val = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);
        SetVarInt(isGlobal, index + ix, val, stackBase, intLocals, intGlobals);
    }
    static inline void DoArraySetFlt(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty1 == TypeEnum::Int && ty2 == TypeEnum::Float && ty == TypeEnum::Float);
        int32_t ix = static_cast<int32_t>(GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals));
        double val = GetVarFloat(isGlobal2, index2, stackBase, fltLocals, fltGlobals);
        SetVarFloat(isGlobal, index + ix, val, stackBase, fltLocals, fltGlobals);
    }
    static inline void DoArraySetStr(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty1 == TypeEnum::Int && ty2 == TypeEnum::String && ty == TypeEnum::String);
        int32_t ix = static_cast<int32_t>(GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals));
        const std::string& val = GetVarString(isGlobal2, index2, stackBase, strLocals, strGlobals);
        SetVarString(isGlobal, index + ix, val, stackBase, strLocals, strGlobals);
    }
    static inline void DoAsInt(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);

        MyAssert(ty == TypeEnum::Int);
        MyAssert(ty1 == TypeEnum::Float);
        double val = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
        float fv = static_cast<float>(val);
        int32_t iv = *reinterpret_cast<int32_t*>(&fv);
        int64_t lv = iv;
        SetVarInt(isGlobal, index, lv, stackBase, intLocals, intGlobals);
    }
    static inline void DoAsFloat(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);

        MyAssert(ty == TypeEnum::Float);
        MyAssert(ty1 == TypeEnum::Int);
        int64_t val = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
        int32_t iv = static_cast<int32_t>(val);
        float fv = *reinterpret_cast<float*>(&iv);
        double dv = fv;
        SetVarFloat(isGlobal, index, dv, stackBase, fltLocals, fltGlobals);
    }
    static inline void DoAsLong(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);

        MyAssert(ty == TypeEnum::Int);
        MyAssert(ty1 == TypeEnum::Float);
        double val = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
        SetVarInt(isGlobal, index, *reinterpret_cast<int64_t*>(&val), stackBase, intLocals, intGlobals);
    }
    static inline void DoAsDouble(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);

        MyAssert(ty == TypeEnum::Float);
        MyAssert(ty1 == TypeEnum::Int);
        int64_t val = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
        SetVarFloat(isGlobal, index, *reinterpret_cast<double*>(&val), stackBase, fltLocals, fltGlobals);
    }
    static inline void DoArgc(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, IntGlobals& intGlobals, int32_t argc)
    {
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        MyAssert(ty == TypeEnum::Int);

        SetVarInt(isGlobal, index, argc, stackBase, intLocals, intGlobals);
    }
    static inline void DoArgv(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, IntGlobals& intGlobals, int32_t argc, int64_t argv[])
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        MyAssert(ty == TypeEnum::Int);
        MyAssert(ty1 == TypeEnum::Int);

        int64_t ix = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
        if (ix >= 0 && ix < argc) {
            int64_t val = argv[ix];
            SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
        }
        else {
            SetVarInt(isGlobal, index, 0, stackBase, intLocals, intGlobals);
        }
    }
    static inline void DoAddr(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        MyAssert(ty == TypeEnum::Int);
        MyAssert(ty1 == TypeEnum::Int);

        int64_t val = reinterpret_cast<int64_t>((isGlobal1 ? intGlobals.data() : intLocals.data()) + index1);
        SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
    }
    static inline void DoAddrFlt(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        MyAssert(ty == TypeEnum::Int);
        MyAssert(ty1 == TypeEnum::Float);

        int64_t val = reinterpret_cast<int64_t>((isGlobal1 ? fltGlobals.data() : fltLocals.data()) + index1);
        SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
    }
    static inline void DoAddrStr(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        MyAssert(ty == TypeEnum::Int);
        MyAssert(ty1 == TypeEnum::String);

        int64_t val = reinterpret_cast<int64_t>((isGlobal ? strGlobals.data() : strLocals.data()) + index1);
        SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
    }
    static inline int64_t ReadMemory(int64_t addr, int64_t offset, int64_t size)
    {
        MyAssert(addr != 0);
        int64_t retVal = 0;
        if (addr) {
            switch (size) {
            case 0:
                retVal = 0;
                break;
            case 1:
                retVal = *reinterpret_cast<int8_t*>(addr + offset);
                break;
            case 2:
                retVal = *reinterpret_cast<int16_t*>(addr + offset);
                break;
            case 3:
                retVal = *reinterpret_cast<int16_t*>(addr + offset);
                retVal |= static_cast<int64_t>(*reinterpret_cast<int8_t*>(addr + offset + sizeof(int16_t))) << (sizeof(int16_t) * 8);
                break;
            case 4:
                retVal = *reinterpret_cast<int32_t*>(addr + offset);
                break;
            case 5:
                retVal = *reinterpret_cast<int32_t*>(addr + offset);
                retVal |= static_cast<int64_t>(*reinterpret_cast<int8_t*>(addr + offset + sizeof(int32_t))) << (sizeof(int32_t) * 8);
                break;
            case 6:
                retVal = *reinterpret_cast<int32_t*>(addr + offset);
                retVal |= static_cast<int64_t>(*reinterpret_cast<int16_t*>(addr + offset + sizeof(int32_t))) << (sizeof(int32_t) * 8);
                break;
            case 7:
                retVal = *reinterpret_cast<int32_t*>(addr + offset);
                retVal |= static_cast<int64_t>(*reinterpret_cast<int16_t*>(addr + offset + sizeof(int32_t))) << (sizeof(int32_t) * 8);
                retVal |= static_cast<int64_t>(*reinterpret_cast<int8_t*>(addr + offset + sizeof(int32_t) + sizeof(int16_t))) << ((sizeof(int32_t) + sizeof(int16_t)) * 8);
                break;
            default:
                retVal = *reinterpret_cast<int64_t*>(addr + offset);
                break;
            }
        }
        return retVal;
    }
    static inline void WriteMemory(int64_t addr, int64_t offset, int64_t size, int64_t val)
    {
        MyAssert(addr != 0);
        if (addr) {
            switch (size) {
            case 0:
                break;
            case 1:
                *reinterpret_cast<int8_t*>(addr + offset) = static_cast<int8_t>(val);
                break;
            case 2:
                *reinterpret_cast<int16_t*>(addr + offset) = static_cast<int16_t>(val);
                break;
            case 3:
                *reinterpret_cast<int16_t*>(addr + offset) = static_cast<int16_t>(val & 0xffff);
                *reinterpret_cast<int8_t*>(addr + offset + sizeof(int16_t)) = static_cast<int8_t>((val & 0xff0000) >> (sizeof(int16_t) * 8));
                break;
            case 4:
                *reinterpret_cast<int32_t*>(addr + offset) = static_cast<int32_t>(val);
                break;
            case 5:
                *reinterpret_cast<int32_t*>(addr + offset) = static_cast<int32_t>(val & 0xffffffff);
                *reinterpret_cast<int8_t*>(addr + offset + sizeof(int32_t)) = static_cast<int8_t>((val & 0xff00000000) >> (sizeof(int32_t) * 8));
                break;
            case 6:
                *reinterpret_cast<int32_t*>(addr + offset) = static_cast<int32_t>(val & 0xffffffff);
                *reinterpret_cast<int16_t*>(addr + offset + sizeof(int32_t)) = static_cast<int16_t>((val & 0xffff00000000) >> (sizeof(int32_t) * 8));
                break;
            case 7:
                *reinterpret_cast<int32_t*>(addr + offset) = static_cast<int32_t>(val & 0xffffffff);
                *reinterpret_cast<int16_t*>(addr + offset + sizeof(int32_t)) = static_cast<int16_t>((val & 0xffff00000000) >> (sizeof(int32_t) * 8));
                *reinterpret_cast<int8_t*>(addr + offset + sizeof(int32_t) + sizeof(int16_t)) = static_cast<int8_t>((val & 0xff000000000000) >> ((sizeof(int32_t) + sizeof(int16_t)) * 8));
                break;
            default:
                *reinterpret_cast<int64_t*>(addr + offset) = static_cast<int64_t>(val);
                break;
            }
        }
    }
    static inline void DoPtrGet(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty == TypeEnum::Int);
        MyAssert(ty1 == TypeEnum::Int);
        MyAssert(ty2 == TypeEnum::Int);

        int64_t addr = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
        int64_t size = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);
        int64_t val = ReadMemory(addr, 0, size);
        SetVarInt(isGlobal, index, val, stackBase, intLocals, intGlobals);
    }
    static inline void DoPtrGetFlt(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty == TypeEnum::Float);
        MyAssert(ty1 == TypeEnum::Int);
        MyAssert(ty2 == TypeEnum::Int);

        int64_t addr = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
        int64_t size = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);
        double val = 0;
        MyAssert(addr != 0);
        if (addr) {
            switch (size) {
            case 4:
                val = *reinterpret_cast<float*>(addr);
                break;
            case 8:
                val = *reinterpret_cast<double*>(addr);
                break;
            default:
                int64_t ival = ReadMemory(addr, 0, size);
                val = *reinterpret_cast<double*>(&ival);
                break;
            }
        }
        SetVarFloat(isGlobal, index, val, stackBase, fltLocals, fltGlobals);
    }
    static inline void DoPtrGetStr(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty == TypeEnum::String);
        MyAssert(ty1 == TypeEnum::Int);
        MyAssert(ty2 == TypeEnum::Int);

        int64_t addr = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
        int64_t size = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);
        MyAssert(addr != 0);
        MyAssert(size == static_cast<int64_t>(sizeof(char*)));
        if (addr) {
            //dangerous!!!
            const char* tstr = *reinterpret_cast<const char**>(addr);
            if (tstr) {
                std::string val = tstr;
                SetVarString(isGlobal, index, val, stackBase, strLocals, strGlobals);
            }
            else {
                SetVarString(isGlobal, index, std::string(), stackBase, strLocals, strGlobals);
            }
        }
        else {
            SetVarString(isGlobal, index, std::string(), stackBase, strLocals, strGlobals);
        }
    }
    static inline void DoPtrSet(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty == TypeEnum::Int);
        MyAssert(ty1 == TypeEnum::Int);
        MyAssert(ty2 == TypeEnum::Int);

        int64_t addr = GetVarInt(isGlobal, index, stackBase, intLocals, intGlobals);
        int64_t size = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
        int64_t val = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);
        WriteMemory(addr, 0, size, val);
    }
    static inline void DoPtrSetFlt(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty == TypeEnum::Int);
        MyAssert(ty1 == TypeEnum::Int);
        MyAssert(ty2 == TypeEnum::Float);

        int64_t addr = GetVarInt(isGlobal, index, stackBase, intLocals, intGlobals);
        int64_t size = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
        MyAssert(addr != 0);
        if (addr) {
            double val = GetVarFloat(isGlobal2, index2, stackBase, fltLocals, fltGlobals);
            switch (size) {
            case 4:
                *reinterpret_cast<float*>(addr) = static_cast<float>(val);
                break;
            case 8:
                *reinterpret_cast<double*>(addr) = val;
                break;
            default:
                int64_t ival = *reinterpret_cast<int64_t*>(&val);
                WriteMemory(addr, 0, size, ival);
                break;
            }
        }
    }
    static inline void DoPtrSetStr(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);
        MyAssert(ty == TypeEnum::Int);
        MyAssert(ty1 == TypeEnum::Int);
        MyAssert(ty2 == TypeEnum::String);

        int64_t addr = GetVarInt(isGlobal, index, stackBase, intLocals, intGlobals);
        int64_t size = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
        MyAssert(addr != 0);
        MyAssert(size == static_cast<int64_t>(sizeof(char*)));
        if (addr) {
            //dangerous!!!
            const std::string& val = GetVarString(isGlobal2, index2, stackBase, strLocals, strGlobals);
            char* tstr = *reinterpret_cast<char**>(addr);
            if (tstr) {
#if defined(_MSC_VER) || defined(_WIN32) || defined(_WIN64)
                strcpy_s(tstr, std::strlen(tstr) + 1, val.c_str());
#else
                auto slen = std::strlen(tstr);
                strncpy(tstr, val.c_str(), slen);
                tstr[slen] = 0;
#endif
            }
        }
    }
    static inline void DoCascadePtr(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, IntGlobals& intGlobals)
    {
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        MyAssert(ty == TypeEnum::Int);

        int64_t addr = 0;
        int64_t lastSize = 0;
        for (int i = 0; i < argNum; i += 2) {
            ++pos;
            int32_t operand = codes[pos];
            if (i < argNum) {
                bool isGlobal1;
                TypeEnum ty1;
                int32_t index1;
                DecodeOperand1(operand, isGlobal1, ty1, index1);
                MyAssert(ty1 == TypeEnum::Int);
                if (i == 0) {
                    addr = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
                }
                else if (i == argNum - 1) {
                    int64_t offset = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
                    addr = ReadMemory(addr, offset, lastSize);
                }
                else {
                    int64_t offset = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
                    MyAssert(addr != 0);
                    if (addr) {
                        addr = *reinterpret_cast<int64_t*>(addr + offset);
                    }
                }
            }
            if (i + 1 < argNum) {
                bool isGlobal2;
                TypeEnum ty2;
                int32_t index2;
                DecodeOperand2(operand, isGlobal2, ty2, index2);
                MyAssert(ty2 == TypeEnum::Int);
                if (i == 0) {
                    lastSize = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);
                }
                else if (i + 1 == argNum - 1) {
                    int64_t offset = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);
                    addr = ReadMemory(addr, offset, lastSize);
                }
                else {
                    int64_t offset = GetVarInt(isGlobal2, index2, stackBase, intLocals, intGlobals);
                    MyAssert(addr != 0);
                    if (addr) {
                        addr = *reinterpret_cast<int64_t*>(addr + offset);
                    }
                }
            }
        }
        SetVarInt(isGlobal, index, addr, stackBase, intLocals, intGlobals);
    }
    static inline void DoStackIndex(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, IntGlobals& intGlobals)
    {
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        MyAssert(ty == TypeEnum::Int);

        SetVarInt(isGlobal, index, stackBase / c_max_variable_table_size, stackBase, intLocals, intGlobals);
    }
    static inline void DoHookId(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, IntGlobals& intGlobals, int32_t hookId)
    {
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        MyAssert(ty == TypeEnum::Int);

        SetVarInt(isGlobal, index, hookId, stackBase, intLocals, intGlobals);
    }
    static inline void DoHookVer(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, IntGlobals& intGlobals)
    {
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        MyAssert(ty == TypeEnum::Int);

        SetVarInt(isGlobal, index, g_DebugScriptSerialNum, stackBase, intLocals, intGlobals);
    }

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4172)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-local-addr"
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-stack-address"
#endif

    extern int64_t get_first_stack_arg_addr(int64_t r0, int64_t r1, int64_t r2, int64_t r3, int64_t r4, int64_t r5, int64_t r6, int64_t r7
        , int64_t r8, int64_t r9, int64_t r10, int64_t r11, int64_t r12, int64_t r13, int64_t r14, int64_t r15
        , int64_t r16, int64_t r17, int64_t r18, int64_t r19, int64_t r20, int64_t r21, int64_t r22, int64_t r23)
    {
        //r0 ~ r7 for register arg on arm; rdi,rsi,rdx,rcx,r8,r9 for register arg on x86_64, rcx,rdx,r8,r9 for register arg on x64, the arguments passed in memory are pushed on
        //the stack in reversed (right to left) order.
        //debug builds on msvc, all arguments are on the stack, but the compiler fakes the argument passing rules.

#if defined(_MSC_VER) && defined(_M_X64)
        //x64
        return reinterpret_cast<int64_t>(&r4);
#elif defined(_MSC_VER) && defined(_M_ARM64)
        //arm64
        return reinterpret_cast<int64_t>(&r8);
#elif defined(__GNUC__) && defined(__x86_64__)
        //x86_64 or amd64
        return reinterpret_cast<int64_t>(&r6);
#elif defined(__clang__) && defined(__x86_64__)
        //x86_64 or amd64
        return reinterpret_cast<int64_t>(&r6);
#elif defined(__APPLE__) && defined(__x86_64__)
        //x86_64 or amd64
        return reinterpret_cast<int64_t>(&r6);
#elif defined(__GNUC__) && defined(__aarch64__)
        //arm64
        return reinterpret_cast<int64_t>(&r8);
#elif defined(__clang__) && defined(__aarch64__)
        //arm64
        return reinterpret_cast<int64_t>(&r8);
#elif defined(__APPLE__) && defined(__aarch64__)
        //arm64
        return reinterpret_cast<int64_t>(&r8);
#else
        #error unknown architecture !!!
        //arm64
        return reinterpret_cast<int64_t>(&r8);
#endif
    }

    [[maybe_unused]]
    extern int64_t get_last_stack_arg_addr(int64_t r0, int64_t r1, int64_t r2, int64_t r3, int64_t r4, int64_t r5, int64_t r6, int64_t r7
        , int64_t r8, int64_t r9, int64_t r10, int64_t r11, int64_t r12, int64_t r13, int64_t r14, int64_t r15
        , int64_t r16, int64_t r17, int64_t r18, int64_t r19, int64_t r20, int64_t r21, int64_t r22, int64_t r23)
    {
        //r0 ~ r7 for register arg on arm; rdi,rsi,rdx,rcx,r8,r9 for register arg on x86_64, rcx,rdx,r8,r9 for register arg on x64, the arguments passed in memory are pushed on
        //the stack in reversed (right to left) order.
        //debug builds on msvc, all arguments are on the stack, but the compiler fakes the argument passing rules.

#if defined(_MSC_VER) && defined(_M_X64)
        //x64
        return reinterpret_cast<int64_t>(&r23);
#elif defined(_MSC_VER) && defined(_M_ARM64)
        //arm64
        return reinterpret_cast<int64_t>(&r23);
#elif defined(__GNUC__) && defined(__x86_64__)
        //x86_64 or amd64
        return reinterpret_cast<int64_t>(&r23);
#elif defined(__clang__) && defined(__x86_64__)
        //x86_64 or amd64
        return reinterpret_cast<int64_t>(&r23);
#elif defined(__APPLE__) && defined(__x86_64__)
        //x86_64 or amd64
        return reinterpret_cast<int64_t>(&r23);
#elif defined(__GNUC__) && defined(__aarch64__)
        //arm64
        return reinterpret_cast<int64_t>(&r23);
#elif defined(__clang__) && defined(__aarch64__)
        //arm64
        return reinterpret_cast<int64_t>(&r23);
#elif defined(__APPLE__) && defined(__aarch64__)
        //arm64
        return reinterpret_cast<int64_t>(&r23);
#else
#error unknown architecture !!!
//arm64
        return reinterpret_cast<int64_t>(&r23);
#endif
    }

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

    template<typename... ArgsT>
    struct FFI_Auto
    {
        inline void Call(int64_t addr, bool isGlobal, TypeEnum ty, int index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals, ArgsT... args)
        {
            switch (ty) {
            case TypeEnum::Int: {
                typedef int64_t(*Fptr)(ArgsT...);
                int64_t rval = reinterpret_cast<Fptr>(addr)(args...);
                SetVarInt(isGlobal, index, rval, stackBase, intLocals, intGlobals);
            }break;
            case TypeEnum::Float: {
                typedef double(*Fptr)(ArgsT...);
                double rval = reinterpret_cast<Fptr>(addr)(args...);
                SetVarFloat(isGlobal, index, rval, stackBase, fltLocals, fltGlobals);
            }break;
            case TypeEnum::String: {
                typedef const char* (*Fptr)(ArgsT...);
                const char* rval = reinterpret_cast<Fptr>(addr)(args...);
                SetVarString(isGlobal, index, rval, stackBase, strLocals, strGlobals);
            }break;
            default: {
                typedef void(*Fptr)(ArgsT...);
                reinterpret_cast<Fptr>(addr)(args...);
            }break;
            }
        }
    };
    template<typename... ArgsT>
    struct FFI_Manual
    {
        static const int c_max_stack_arg_num = 16;
        int64_t CallApiInt(int64_t addr, int64_t stackArgs[], int64_t stackArgNum, ArgsT... args)
        {
            typedef int64_t(*Fptr)(ArgsT...);
            int64_t ix = 0;
            int64_t* stacks = reinterpret_cast<int64_t*>(get_first_stack_arg_addr(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24));
            if (stackArgNum <= c_max_stack_arg_num) {
                for (ix = 0; ix < stackArgNum && ix < c_max_stack_arg_num; ++ix) {
                    stacks[ix] = stackArgs[ix];
                }
                return reinterpret_cast<Fptr>(addr)(args...);
            }
            return 0;
        }
        double CallApiFloat(int64_t addr, int64_t stackArgs[], int64_t stackArgNum, ArgsT... args)
        {
            typedef double(*Fptr)(ArgsT...);
            int64_t ix = 0;
            int64_t* stacks = reinterpret_cast<int64_t*>(get_first_stack_arg_addr(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24));
            if (stackArgNum <= c_max_stack_arg_num) {
                for (ix = 0; ix < stackArgNum && ix < c_max_stack_arg_num; ++ix) {
                    stacks[ix] = stackArgs[ix];
                }
                return reinterpret_cast<Fptr>(addr)(args...);
            }
            return 0;
        }
        const char* CallApiString(int64_t addr, int64_t stackArgs[], int64_t stackArgNum, ArgsT... args)
        {
            typedef const char* (*Fptr)(ArgsT...);
            int64_t ix = 0;
            int64_t* stacks = reinterpret_cast<int64_t*>(get_first_stack_arg_addr(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24));
            if (stackArgNum <= c_max_stack_arg_num) {
                for (ix = 0; ix < stackArgNum && ix < c_max_stack_arg_num; ++ix) {
                    stacks[ix] = stackArgs[ix];
                }
                return reinterpret_cast<Fptr>(addr)(args...);
            }
            return nullptr;
        }
        void CallApiVoid(int64_t addr, int64_t stackArgs[], int64_t stackArgNum, ArgsT... args)
        {
            typedef void (*Fptr)(ArgsT...);
            int64_t ix = 0;
            int64_t* stacks = reinterpret_cast<int64_t*>(get_first_stack_arg_addr(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24));
            if (stackArgNum <= c_max_stack_arg_num) {
                for (ix = 0; ix < stackArgNum && ix < c_max_stack_arg_num; ++ix) {
                    stacks[ix] = stackArgs[ix];
                }
                reinterpret_cast<Fptr>(addr)(args...);
            }
        }
        inline void Call(int64_t addr, int64_t stackArgs[], int64_t stackArgNum, bool isGlobal, TypeEnum ty, int index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals, ArgsT... args)
        {
            switch (ty) {
            case TypeEnum::Int: {
                int64_t rval = CallApiInt(addr, stackArgs, stackArgNum, args...);
                SetVarInt(isGlobal, index, rval, stackBase, intLocals, intGlobals);
            }break;
            case TypeEnum::Float: {
                double rval = CallApiFloat(addr, stackArgs, stackArgNum, args...);
                SetVarFloat(isGlobal, index, rval, stackBase, fltLocals, fltGlobals);
            }break;
            case TypeEnum::String: {
                const char* rval = CallApiString(addr, stackArgs, stackArgNum, args...);
                SetVarString(isGlobal, index, rval, stackBase, strLocals, strGlobals);
            }break;
            default: {
                CallApiVoid(addr, stackArgs, stackArgNum, args...);
            }break;
            }
        }
    };
    template<typename... ArgsT>
    struct FFI_ManualStack
    {
        static const int c_max_stack_arg_num = 16;
        int64_t CallApiInt(int64_t addr, int64_t stackArgAddr, int64_t stackArgSize, ArgsT... args)
        {
            typedef int64_t(*Fptr)(ArgsT...);
            int64_t* stacks = reinterpret_cast<int64_t*>(get_first_stack_arg_addr(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24));
            if (stackArgSize <= static_cast<int64_t>(c_max_stack_arg_num * sizeof(int64_t))) {
                std::memcpy(reinterpret_cast<char*>(stacks), reinterpret_cast<const void*>(stackArgAddr), stackArgSize);
                return reinterpret_cast<Fptr>(addr)(args...);
            }
            return 0;
        }
        double CallApiFloat(int64_t addr, int64_t stackArgAddr, int64_t stackArgSize, ArgsT... args)
        {
            typedef double(*Fptr)(ArgsT...);
            int64_t* stacks = reinterpret_cast<int64_t*>(get_first_stack_arg_addr(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24));
            if (stackArgSize <= static_cast<int64_t>(c_max_stack_arg_num * sizeof(int64_t))) {
                std::memcpy(reinterpret_cast<char*>(stacks), reinterpret_cast<const void*>(stackArgAddr), stackArgSize);
                return reinterpret_cast<Fptr>(addr)(args...);
            }
            return 0;
        }
        const char* CallApiString(int64_t addr, int64_t stackArgAddr, int64_t stackArgSize, ArgsT... args)
        {
            typedef const char* (*Fptr)(ArgsT...);
            int64_t* stacks = reinterpret_cast<int64_t*>(get_first_stack_arg_addr(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24));
            if (stackArgSize <= static_cast<int64_t>(c_max_stack_arg_num * sizeof(int64_t))) {
                std::memcpy(reinterpret_cast<char*>(stacks), reinterpret_cast<const void*>(stackArgAddr), stackArgSize);
                return reinterpret_cast<Fptr>(addr)(args...);
            }
            return nullptr;
        }
        void CallApiVoid(int64_t addr, int64_t stackArgAddr, int64_t stackArgSize, ArgsT... args)
        {
            typedef void (*Fptr)(ArgsT...);
            int64_t* stacks = reinterpret_cast<int64_t*>(get_first_stack_arg_addr(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24));
            if (stackArgSize <= static_cast<int64_t>(c_max_stack_arg_num * sizeof(int64_t))) {
                std::memcpy(reinterpret_cast<char*>(stacks), reinterpret_cast<const void*>(stackArgAddr), stackArgSize);
                reinterpret_cast<Fptr>(addr)(args...);
            }
        }
        inline void Call(int64_t addr, int64_t stackArgAddr, int64_t stackArgSize, bool isGlobal, TypeEnum ty, int index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals, ArgsT... args)
        {
            switch (ty) {
            case TypeEnum::Int: {
                int64_t rval = CallApiInt(addr, stackArgAddr, stackArgSize, args...);
                SetVarInt(isGlobal, index, rval, stackBase, intLocals, intGlobals);
            }break;
            case TypeEnum::Float: {
                double rval = CallApiFloat(addr, stackArgAddr, stackArgSize, args...);
                SetVarFloat(isGlobal, index, rval, stackBase, fltLocals, fltGlobals);
            }break;
            case TypeEnum::String: {
                const char* rval = CallApiString(addr, stackArgAddr, stackArgSize, args...);
                SetVarString(isGlobal, index, rval, stackBase, strLocals, strGlobals);
            }break;
            default: {
                CallApiVoid(addr, stackArgAddr, stackArgSize, args...);
            }break;
            }
        }
    };
    template<typename... ArgsT>
    struct TypesHolder
    {
        using type = TypesHolder<ArgsT...>;
    };
    template<typename TypeT, typename... ArgsT>
    struct TypesHolder<TypeT, TypesHolder<ArgsT...>>
    {
        using type = TypesHolder<TypeT, ArgsT...>;
    };
    template<>
    struct TypesHolder<>
    {
        using type = TypesHolder<>;
    };
    template<typename TypeT, int K>
    struct RepeatType1
    {
        using holder_type = typename TypesHolder<TypeT, typename RepeatType1<TypeT, K - 1>::holder_type>::type;
    };
    template<typename TypeT>
    struct RepeatType1<TypeT, 0>
    {
        using holder_type = TypesHolder<>;
    };
    template<typename Type1T, typename Type2T, int K2, int K1>
    struct RepeatType2
    {
        using holder_type = typename TypesHolder<Type1T, typename RepeatType2<Type1T, Type2T, K2, K1 - 1>::holder_type>::type;
    };
    template<typename Type1T, typename Type2T, int K2>
    struct RepeatType2<Type1T, Type2T, K2, 0>
    {
        using holder_type = typename TypesHolder<Type2T, typename RepeatType2<Type1T, Type2T, K2 - 1, 0>::holder_type>::type;
    };
    template<typename Type1T, typename Type2T>
    struct RepeatType2<Type1T, Type2T, 0, 0>
    {
        using holder_type = typename TypesHolder<>::type;
    };
    template<template<typename... ArgsT> class VariadicT>
    struct VariadicFactory
    {
        template<typename... ArgsT>
        static inline VariadicT<ArgsT...> Build(TypesHolder<ArgsT...> holder)
        {
            return VariadicT<ArgsT...>{};
        }
    };

#define ARRAY_TO_ARGS0(arr)
#define ARRAY_TO_ARGS1(arr) , arr[0]
#define ARRAY_TO_ARGS2(arr) ARRAY_TO_ARGS1(arr), arr[1]
#define ARRAY_TO_ARGS3(arr) ARRAY_TO_ARGS2(arr), arr[2]
#define ARRAY_TO_ARGS4(arr) ARRAY_TO_ARGS3(arr), arr[3]
#define ARRAY_TO_ARGS5(arr) ARRAY_TO_ARGS4(arr), arr[4]
#define ARRAY_TO_ARGS6(arr) ARRAY_TO_ARGS5(arr), arr[5]
#define ARRAY_TO_ARGS7(arr) ARRAY_TO_ARGS6(arr), arr[6]
#define ARRAY_TO_ARGS8(arr) ARRAY_TO_ARGS7(arr), arr[7]
#define ARRAY_TO_ARGS9(arr) ARRAY_TO_ARGS8(arr), arr[8]
#define ARRAY_TO_ARGS10(arr) ARRAY_TO_ARGS9(arr), arr[9]
#define ARRAY_TO_ARGS11(arr) ARRAY_TO_ARGS10(arr), arr[10]
#define ARRAY_TO_ARGS12(arr) ARRAY_TO_ARGS11(arr), arr[11]
#define ARRAY_TO_ARGS13(arr) ARRAY_TO_ARGS12(arr), arr[12]
#define ARRAY_TO_ARGS14(arr) ARRAY_TO_ARGS13(arr), arr[13]
#define ARRAY_TO_ARGS15(arr) ARRAY_TO_ARGS14(arr), arr[14]
#define ARRAY_TO_ARGS16(arr) ARRAY_TO_ARGS15(arr), arr[15]
#define ARRAY_TO_ARGS17(arr) ARRAY_TO_ARGS16(arr), arr[16]
#define ARRAY_TO_ARGS18(arr) ARRAY_TO_ARGS17(arr), arr[17]
#define ARRAY_TO_ARGS19(arr) ARRAY_TO_ARGS18(arr), arr[18]
#define ARRAY_TO_ARGS20(arr) ARRAY_TO_ARGS19(arr), arr[19]
#define ARRAY_TO_ARGS21(arr) ARRAY_TO_ARGS20(arr), arr[20]
#define ARRAY_TO_ARGS22(arr) ARRAY_TO_ARGS21(arr), arr[21]
#define ARRAY_TO_ARGS23(arr) ARRAY_TO_ARGS22(arr), arr[22]
#define ARRAY_TO_ARGS24(arr) ARRAY_TO_ARGS23(arr), arr[23]
#define ARRAY_TO_ARGS(arr, num) ARRAY_TO_ARGS##num(arr)

    static inline void DoFFIAuto(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);

        const int c_max_count = 25;
        int64_t addr = 0;
        int64_t args[c_max_count - 1];

        int num = std::min(argNum, c_max_count);

        for (int i = 0; i < argNum; i += 2) {
            int32_t operand = codes[++pos];
            if (i < num) {
                bool isGlobal0;
                TypeEnum ty0;
                int32_t index0;
                DecodeOperand1(operand, isGlobal0, ty0, index0);
                if (i == 0) {
                    MyAssert(ty0 == TypeEnum::Int);
                    addr = GetVarInt(isGlobal0, index0, stackBase, intLocals, intGlobals);
                }
                else {
                    if (ty0 == TypeEnum::Int) {
                        args[i - 1] = GetVarInt(isGlobal0, index0, stackBase, intLocals, intGlobals);
                    }
                    else {
                        args[i - 1] = reinterpret_cast<int64_t>(GetVarString(isGlobal0, index0, stackBase, strLocals, strGlobals).c_str());
                    }
                }
            }
            if (i + 1 < num) {
                bool isGlobal1;
                TypeEnum ty1;
                int32_t index1;
                DecodeOperand2(operand, isGlobal1, ty1, index1);
                MyAssert(ty1 == TypeEnum::Int || ty1 == TypeEnum::String);
                if (ty1 == TypeEnum::Int) {
                    args[i] = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
                }
                else {
                    args[i] = reinterpret_cast<int64_t>(GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals).c_str());
                }
            }
        }

        switch (num) {
        case 1:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 0>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 0));
            break;
        case 2:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 1>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 1));
            break;
        case 3:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 2>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 2));
            break;
        case 4:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 3>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 3));
            break;
        case 5:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 4>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 4));
            break;
        case 6:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 5>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 5));
            break;
        case 7:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 6>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 6));
            break;
        case 8:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 7>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 7));
            break;
        case 9:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 8>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 8));
            break;
        case 10:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 9>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 9));
            break;
        case 11:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 10>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 10));
            break;
        case 12:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 11>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 11));
            break;
        case 13:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 12>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 12));
            break;
        case 14:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 13>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 13));
            break;
        case 15:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 14>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 14));
            break;
        case 16:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 15>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 15));
            break;
        case 17:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 16>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 16));
            break;
        case 18:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 17>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 17));
            break;
        case 19:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 18>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 18));
            break;
        case 20:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 19>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 19));
            break;
        case 21:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 20>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 20));
            break;
        case 22:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 21>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 21));
            break;
        case 23:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 22>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 22));
            break;
        case 24:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 23>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 23));
            break;
        case 25:
            VariadicFactory<FFI_Auto>::Build(typename RepeatType1<int64_t, 24>::holder_type()).Call(addr, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, 24));
            break;
        }
    }
    static inline void DoFFIManual(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);

        int32_t operand0 = codes[++pos];
        int32_t intNum;
        DecodeOperand1(operand0, intNum);
        int32_t fltNum;
        DecodeOperand2(operand0, fltNum);

        const int c_max_count = 8;
        int64_t addr = 0;
        int64_t args[c_max_count];
        float fargs[c_max_count];
        int64_t stacks[c_max_count * 4];

        int inum = std::min(intNum + 1, c_max_count + 1);
        int fnum = std::min(fltNum, c_max_count);

        for (int i = 0; i < argNum; i += 2) {
            int32_t operand = codes[++pos];
            if (i < inum) {
                bool isGlobal0;
                TypeEnum ty0;
                int32_t index0;
                DecodeOperand1(operand, isGlobal0, ty0, index0);
                if (i == 0) {
                    MyAssert(ty0 == TypeEnum::Int);
                    addr = GetVarInt(isGlobal0, index0, stackBase, intLocals, intGlobals);
                }
                else {
                    if (ty0 == TypeEnum::Int) {
                        args[i - 1] = GetVarInt(isGlobal0, index0, stackBase, intLocals, intGlobals);
                    }
                    else {
                        args[i - 1] = reinterpret_cast<int64_t>(GetVarString(isGlobal0, index0, stackBase, strLocals, strGlobals).c_str());
                    }
                }
            }
            else if (i >= 1 + intNum && i < 1 + intNum + fnum) {
                bool isGlobal0;
                TypeEnum ty0;
                int32_t index0;
                DecodeOperand1(operand, isGlobal0, ty0, index0);
                MyAssert(ty0 == TypeEnum::Float);
                fargs[i - 1 - intNum] = static_cast<float>(GetVarFloat(isGlobal0, index0, stackBase, fltLocals, fltGlobals));
            }
            else if (i >= 1 + intNum + fltNum && i < argNum) {
                bool isGlobal0;
                TypeEnum ty0;
                int32_t index0;
                DecodeOperand1(operand, isGlobal0, ty0, index0);
                if (ty0 == TypeEnum::Int) {
                    stacks[i - 1 - intNum - fltNum] = GetVarInt(isGlobal0, index0, stackBase, intLocals, intGlobals);
                }
                else if (ty0 == TypeEnum::Float) {
                    float fv = static_cast<float>(GetVarFloat(isGlobal0, index0, stackBase, fltLocals, fltGlobals));
                    stacks[i - 1 - intNum - fltNum] = *reinterpret_cast<int32_t*>(&fv);
                }
                else {
                    stacks[i - 1 - intNum - fltNum] = reinterpret_cast<int64_t>(GetVarString(isGlobal0, index0, stackBase, strLocals, strGlobals).c_str());
                }
            }
            if (i + 1 < inum) {
                bool isGlobal1;
                TypeEnum ty1;
                int32_t index1;
                DecodeOperand2(operand, isGlobal1, ty1, index1);
                MyAssert(ty1 == TypeEnum::Int || ty1 == TypeEnum::String);
                if (ty1 == TypeEnum::Int) {
                    args[i] = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
                }
                else {
                    args[i] = reinterpret_cast<int64_t>(GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals).c_str());
                }
            }
            else if (i + 1 >= 1 + intNum && i + 1 < 1 + intNum + fnum) {
                bool isGlobal1;
                TypeEnum ty1;
                int32_t index1;
                DecodeOperand2(operand, isGlobal1, ty1, index1);
                MyAssert(ty1 == TypeEnum::Float);
                fargs[i - intNum] = static_cast<float>(GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals));
            }
            else if (i + 1 >= 1 + intNum + fltNum && i + 1 < argNum) {
                bool isGlobal1;
                TypeEnum ty1;
                int32_t index1;
                DecodeOperand2(operand, isGlobal1, ty1, index1);
                if (ty1 == TypeEnum::Int) {
                    stacks[i - intNum - fltNum] = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
                }
                else if (ty1 == TypeEnum::Float) {
                    float fv = static_cast<float>(GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals));
                    stacks[i - intNum - fltNum] = *reinterpret_cast<int32_t*>(&fv);
                }
                else {
                    stacks[i - intNum - fltNum] = reinterpret_cast<int64_t>(GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals).c_str());
                }
            }
        }

#define CASE_K1_MANUAL(K1, K2) case K1: VariadicFactory<FFI_Manual>::Build(typename RepeatType2<int64_t, float, K2, K1>::holder_type()).Call(addr, stacks, argNum - 1 - intNum - fltNum, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, K1) ARRAY_TO_ARGS(fargs, K2)); break;

#define CALL_FFI_MANUAL(K2) \
        switch (inum - 1) {\
        CASE_K1_MANUAL(0, K2)\
        CASE_K1_MANUAL(1, K2)\
        CASE_K1_MANUAL(2, K2)\
        CASE_K1_MANUAL(3, K2)\
        CASE_K1_MANUAL(4, K2)\
        CASE_K1_MANUAL(5, K2)\
        CASE_K1_MANUAL(6, K2)\
        CASE_K1_MANUAL(7, K2)\
        CASE_K1_MANUAL(8, K2)\
        };

#define CASE_K2_MANUAL(K2) case K2: CALL_FFI_MANUAL(K2) break;

        ///*
        switch (fnum) {
            CASE_K2_MANUAL(0)
                CASE_K2_MANUAL(1)
                CASE_K2_MANUAL(2)
                CASE_K2_MANUAL(3)
                CASE_K2_MANUAL(4)
                CASE_K2_MANUAL(5)
                CASE_K2_MANUAL(6)
                CASE_K2_MANUAL(7)
                CASE_K2_MANUAL(8)
        }
        //*/
    }
    static inline void DoFFIManualStack(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);

        int32_t operand0 = codes[++pos];
        int32_t intNum;
        DecodeOperand1(operand0, intNum);
        int32_t fltNum;
        DecodeOperand2(operand0, fltNum);

        const int c_max_count = 8;
        int64_t addr = 0;
        int64_t args[c_max_count];
        float fargs[c_max_count];
        int64_t stacks[c_max_count * 4];

        int inum = std::min(intNum + 1, c_max_count + 1);
        int fnum = std::min(fltNum, c_max_count);

        for (int i = 0; i < argNum; i += 2) {
            int32_t operand = codes[++pos];
            if (i < inum) {
                bool isGlobal0;
                TypeEnum ty0;
                int32_t index0;
                DecodeOperand1(operand, isGlobal0, ty0, index0);
                if (i == 0) {
                    MyAssert(ty0 == TypeEnum::Int);
                    addr = GetVarInt(isGlobal0, index0, stackBase, intLocals, intGlobals);
                }
                else {
                    if (ty0 == TypeEnum::Int) {
                        args[i - 1] = GetVarInt(isGlobal0, index0, stackBase, intLocals, intGlobals);
                    }
                    else {
                        args[i - 1] = reinterpret_cast<int64_t>(GetVarString(isGlobal0, index0, stackBase, strLocals, strGlobals).c_str());
                    }
                }
            }
            else if (i >= 1 + intNum && i < 1 + intNum + fnum) {
                bool isGlobal0;
                TypeEnum ty0;
                int32_t index0;
                DecodeOperand1(operand, isGlobal0, ty0, index0);
                MyAssert(ty0 == TypeEnum::Float);
                fargs[i - 1 - intNum] = static_cast<float>(GetVarFloat(isGlobal0, index0, stackBase, fltLocals, fltGlobals));
            }
            else if (i >= 1 + intNum + fltNum && i < argNum) {
                bool isGlobal0;
                TypeEnum ty0;
                int32_t index0;
                DecodeOperand1(operand, isGlobal0, ty0, index0);
                if (ty0 == TypeEnum::Int) {
                    stacks[i - 1 - intNum - fltNum] = GetVarInt(isGlobal0, index0, stackBase, intLocals, intGlobals);
                }
                else if (ty0 == TypeEnum::Float) {
                    float fv = static_cast<float>(GetVarFloat(isGlobal0, index0, stackBase, fltLocals, fltGlobals));
                    stacks[i - 1 - intNum - fltNum] = *reinterpret_cast<int32_t*>(&fv);
                }
                else {
                    stacks[i - 1 - intNum - fltNum] = reinterpret_cast<int64_t>(GetVarString(isGlobal0, index0, stackBase, strLocals, strGlobals).c_str());
                }
            }
            if (i + 1 < inum) {
                bool isGlobal1;
                TypeEnum ty1;
                int32_t index1;
                DecodeOperand2(operand, isGlobal1, ty1, index1);
                MyAssert(ty1 == TypeEnum::Int || ty1 == TypeEnum::String);
                if (ty1 == TypeEnum::Int) {
                    args[i] = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
                }
                else {
                    args[i] = reinterpret_cast<int64_t>(GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals).c_str());
                }
            }
            else if (i + 1 >= 1 + intNum && i + 1 < 1 + intNum + fnum) {
                bool isGlobal1;
                TypeEnum ty1;
                int32_t index1;
                DecodeOperand2(operand, isGlobal1, ty1, index1);
                MyAssert(ty1 == TypeEnum::Float);
                fargs[i - intNum] = static_cast<float>(GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals));
            }
            else if (i + 1 >= 1 + intNum + fltNum && i + 1 < argNum) {
                bool isGlobal1;
                TypeEnum ty1;
                int32_t index1;
                DecodeOperand2(operand, isGlobal1, ty1, index1);
                if (ty1 == TypeEnum::Int) {
                    stacks[i - intNum - fltNum] = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
                }
                else if (ty1 == TypeEnum::Float) {
                    float fv = static_cast<float>(GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals));
                    stacks[i - intNum - fltNum] = *reinterpret_cast<int32_t*>(&fv);
                }
                else {
                    stacks[i - intNum - fltNum] = reinterpret_cast<int64_t>(GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals).c_str());
                }
            }
        }

#define CASE_K1_MANUALSTACK(K1, K2) case K1: VariadicFactory<FFI_ManualStack>::Build(typename RepeatType2<int64_t, float, K2, K1>::holder_type()).Call(addr, stacks[0], stacks[1], isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, K1) ARRAY_TO_ARGS(fargs, K2)); break;

#define CALL_FFI_MANUALSTACK(K2) \
        switch (inum - 1) {\
        CASE_K1_MANUALSTACK(0, K2)\
        CASE_K1_MANUALSTACK(1, K2)\
        CASE_K1_MANUALSTACK(2, K2)\
        CASE_K1_MANUALSTACK(3, K2)\
        CASE_K1_MANUALSTACK(4, K2)\
        CASE_K1_MANUALSTACK(5, K2)\
        CASE_K1_MANUALSTACK(6, K2)\
        CASE_K1_MANUALSTACK(7, K2)\
        CASE_K1_MANUALSTACK(8, K2)\
        }

#define CASE_K2_MANUALSTACK(K2) case K2: CALL_FFI_MANUALSTACK(K2) break;

        ///*
        switch (fnum) {
            CASE_K2_MANUALSTACK(0)
                CASE_K2_MANUALSTACK(1)
                CASE_K2_MANUALSTACK(2)
                CASE_K2_MANUALSTACK(3)
                CASE_K2_MANUALSTACK(4)
                CASE_K2_MANUALSTACK(5)
                CASE_K2_MANUALSTACK(6)
                CASE_K2_MANUALSTACK(7)
                CASE_K2_MANUALSTACK(8)
        }
        //*/
    }
    static inline void DoFFIManualDbl(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);

        int32_t operand0 = codes[++pos];
        int32_t intNum;
        DecodeOperand1(operand0, intNum);
        int32_t fltNum;
        DecodeOperand2(operand0, fltNum);

        const int c_max_count = 8;
        int64_t addr = 0;
        int64_t args[c_max_count];
        double fargs[c_max_count];
        int64_t stacks[c_max_count * 4];

        int inum = std::min(intNum + 1, c_max_count + 1);
        int fnum = std::min(fltNum, c_max_count);

        for (int i = 0; i < argNum; i += 2) {
            int32_t operand = codes[++pos];
            if (i < inum) {
                bool isGlobal0;
                TypeEnum ty0;
                int32_t index0;
                DecodeOperand1(operand, isGlobal0, ty0, index0);
                if (i == 0) {
                    MyAssert(ty0 == TypeEnum::Int);
                    addr = GetVarInt(isGlobal0, index0, stackBase, intLocals, intGlobals);
                }
                else {
                    if (ty0 == TypeEnum::Int) {
                        args[i - 1] = GetVarInt(isGlobal0, index0, stackBase, intLocals, intGlobals);
                    }
                    else {
                        args[i - 1] = reinterpret_cast<int64_t>(GetVarString(isGlobal0, index0, stackBase, strLocals, strGlobals).c_str());
                    }
                }
            }
            else if (i >= 1 + intNum && i < 1 + intNum + fnum) {
                bool isGlobal0;
                TypeEnum ty0;
                int32_t index0;
                DecodeOperand1(operand, isGlobal0, ty0, index0);
                MyAssert(ty0 == TypeEnum::Float);
                fargs[i - 1 - intNum] = GetVarFloat(isGlobal0, index0, stackBase, fltLocals, fltGlobals);
            }
            else if (i >= 1 + intNum + fltNum && i < argNum) {
                bool isGlobal0;
                TypeEnum ty0;
                int32_t index0;
                DecodeOperand1(operand, isGlobal0, ty0, index0);
                if (ty0 == TypeEnum::Int) {
                    stacks[i - 1 - intNum - fltNum] = GetVarInt(isGlobal0, index0, stackBase, intLocals, intGlobals);
                }
                else if (ty0 == TypeEnum::Float) {
                    double fv = GetVarFloat(isGlobal0, index0, stackBase, fltLocals, fltGlobals);
                    stacks[i - 1 - intNum - fltNum] = *reinterpret_cast<int64_t*>(&fv);
                }
                else {
                    stacks[i - 1 - intNum - fltNum] = reinterpret_cast<int64_t>(GetVarString(isGlobal0, index0, stackBase, strLocals, strGlobals).c_str());
                }
            }
            if (i + 1 < inum) {
                bool isGlobal1;
                TypeEnum ty1;
                int32_t index1;
                DecodeOperand2(operand, isGlobal1, ty1, index1);
                MyAssert(ty1 == TypeEnum::Int || ty1 == TypeEnum::String);
                if (ty1 == TypeEnum::Int) {
                    args[i] = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
                }
                else {
                    args[i] = reinterpret_cast<int64_t>(GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals).c_str());
                }
            }
            else if (i + 1 >= 1 + intNum && i + 1 < 1 + intNum + fnum) {
                bool isGlobal1;
                TypeEnum ty1;
                int32_t index1;
                DecodeOperand2(operand, isGlobal1, ty1, index1);
                MyAssert(ty1 == TypeEnum::Float);
                fargs[i - intNum] = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
            }
            else if (i + 1 >= 1 + intNum + fltNum && i + 1 < argNum) {
                bool isGlobal1;
                TypeEnum ty1;
                int32_t index1;
                DecodeOperand2(operand, isGlobal1, ty1, index1);
                if (ty1 == TypeEnum::Int) {
                    stacks[i - intNum - fltNum] = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
                }
                else if (ty1 == TypeEnum::Float) {
                    double fv = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
                    stacks[i - intNum - fltNum] = *reinterpret_cast<int64_t*>(&fv);
                }
                else {
                    stacks[i - intNum - fltNum] = reinterpret_cast<int64_t>(GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals).c_str());
                }
            }
        }

#define CASE_K1_MANUALDBL(K1, K2) case K1: VariadicFactory<FFI_Manual>::Build(typename RepeatType2<int64_t, double, K2, K1>::holder_type()).Call(addr, stacks, argNum - 1 - intNum - fltNum, isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, K1) ARRAY_TO_ARGS(fargs, K2)); break;

#define CALL_FFI_MANUALDBL(K2) \
        switch (inum - 1) {\
        CASE_K1_MANUALDBL(0, K2)\
        CASE_K1_MANUALDBL(1, K2)\
        CASE_K1_MANUALDBL(2, K2)\
        CASE_K1_MANUALDBL(3, K2)\
        CASE_K1_MANUALDBL(4, K2)\
        CASE_K1_MANUALDBL(5, K2)\
        CASE_K1_MANUALDBL(6, K2)\
        CASE_K1_MANUALDBL(7, K2)\
        CASE_K1_MANUALDBL(8, K2)\
        };

#define CASE_K2_MANUALDBL(K2) case K2: CALL_FFI_MANUALDBL(K2) break;

        ///*
        switch (fnum) {
            CASE_K2_MANUALDBL(0)
                CASE_K2_MANUALDBL(1)
                CASE_K2_MANUALDBL(2)
                CASE_K2_MANUALDBL(3)
                CASE_K2_MANUALDBL(4)
                CASE_K2_MANUALDBL(5)
                CASE_K2_MANUALDBL(6)
                CASE_K2_MANUALDBL(7)
                CASE_K2_MANUALDBL(8)
        }
        //*/
    }
    static inline void DoFFIManualStackDbl(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);

        int32_t operand0 = codes[++pos];
        int32_t intNum;
        DecodeOperand1(operand0, intNum);
        int32_t fltNum;
        DecodeOperand2(operand0, fltNum);

        const int c_max_count = 8;
        int64_t addr = 0;
        int64_t args[c_max_count];
        double fargs[c_max_count];
        int64_t stacks[c_max_count * 4];

        int inum = std::min(intNum + 1, c_max_count + 1);
        int fnum = std::min(fltNum, c_max_count);

        for (int i = 0; i < argNum; i += 2) {
            int32_t operand = codes[++pos];
            if (i < inum) {
                bool isGlobal0;
                TypeEnum ty0;
                int32_t index0;
                DecodeOperand1(operand, isGlobal0, ty0, index0);
                if (i == 0) {
                    MyAssert(ty0 == TypeEnum::Int);
                    addr = GetVarInt(isGlobal0, index0, stackBase, intLocals, intGlobals);
                }
                else {
                    if (ty0 == TypeEnum::Int) {
                        args[i - 1] = GetVarInt(isGlobal0, index0, stackBase, intLocals, intGlobals);
                    }
                    else {
                        args[i - 1] = reinterpret_cast<int64_t>(GetVarString(isGlobal0, index0, stackBase, strLocals, strGlobals).c_str());
                    }
                }
            }
            else if (i >= 1 + intNum && i < 1 + intNum + fnum) {
                bool isGlobal0;
                TypeEnum ty0;
                int32_t index0;
                DecodeOperand1(operand, isGlobal0, ty0, index0);
                MyAssert(ty0 == TypeEnum::Float);
                fargs[i - 1 - intNum] = GetVarFloat(isGlobal0, index0, stackBase, fltLocals, fltGlobals);
            }
            else if (i >= 1 + intNum + fltNum && i < argNum) {
                bool isGlobal0;
                TypeEnum ty0;
                int32_t index0;
                DecodeOperand1(operand, isGlobal0, ty0, index0);
                if (ty0 == TypeEnum::Int) {
                    stacks[i - 1 - intNum - fltNum] = GetVarInt(isGlobal0, index0, stackBase, intLocals, intGlobals);
                }
                else if (ty0 == TypeEnum::Float) {
                    double fv = GetVarFloat(isGlobal0, index0, stackBase, fltLocals, fltGlobals);
                    stacks[i - 1 - intNum - fltNum] = *reinterpret_cast<int64_t*>(&fv);
                }
                else {
                    stacks[i - 1 - intNum - fltNum] = reinterpret_cast<int64_t>(GetVarString(isGlobal0, index0, stackBase, strLocals, strGlobals).c_str());
                }
            }
            if (i + 1 < inum) {
                bool isGlobal1;
                TypeEnum ty1;
                int32_t index1;
                DecodeOperand2(operand, isGlobal1, ty1, index1);
                MyAssert(ty1 == TypeEnum::Int || ty1 == TypeEnum::String);
                if (ty1 == TypeEnum::Int) {
                    args[i] = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
                }
                else {
                    args[i] = reinterpret_cast<int64_t>(GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals).c_str());
                }
            }
            else if (i + 1 >= 1 + intNum && i + 1 < 1 + intNum + fnum) {
                bool isGlobal1;
                TypeEnum ty1;
                int32_t index1;
                DecodeOperand2(operand, isGlobal1, ty1, index1);
                MyAssert(ty1 == TypeEnum::Float);
                fargs[i - intNum] = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
            }
            else if (i + 1 >= 1 + intNum + fltNum && i + 1 < argNum) {
                bool isGlobal1;
                TypeEnum ty1;
                int32_t index1;
                DecodeOperand2(operand, isGlobal1, ty1, index1);
                if (ty1 == TypeEnum::Int) {
                    stacks[i - intNum - fltNum] = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
                }
                else if (ty1 == TypeEnum::Float) {
                    double fv = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
                    stacks[i - intNum - fltNum] = *reinterpret_cast<int64_t*>(&fv);
                }
                else {
                    stacks[i - intNum - fltNum] = reinterpret_cast<int64_t>(GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals).c_str());
                }
            }
        }

#define CASE_K1_MANUALSTACKDBL(K1, K2) case K1: VariadicFactory<FFI_ManualStack>::Build(typename RepeatType2<int64_t, double, K2, K1>::holder_type()).Call(addr, stacks[0], stacks[1], isGlobal, ty, index, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals ARRAY_TO_ARGS(args, K1) ARRAY_TO_ARGS(fargs, K2)); break;

#define CALL_FFI_MANUALSTACKDBL(K2) \
        switch (inum - 1) {\
        CASE_K1_MANUALSTACKDBL(0, K2)\
        CASE_K1_MANUALSTACKDBL(1, K2)\
        CASE_K1_MANUALSTACKDBL(2, K2)\
        CASE_K1_MANUALSTACKDBL(3, K2)\
        CASE_K1_MANUALSTACKDBL(4, K2)\
        CASE_K1_MANUALSTACKDBL(5, K2)\
        CASE_K1_MANUALSTACKDBL(6, K2)\
        CASE_K1_MANUALSTACKDBL(7, K2)\
        CASE_K1_MANUALSTACKDBL(8, K2)\
        }

#define CASE_K2_MANUALSTACKDBL(K2) case K2: CALL_FFI_MANUALSTACKDBL(K2) break;

        ///*
        switch (fnum) {
            CASE_K2_MANUALSTACKDBL(0)
                CASE_K2_MANUALSTACKDBL(1)
                CASE_K2_MANUALSTACKDBL(2)
                CASE_K2_MANUALSTACKDBL(3)
                CASE_K2_MANUALSTACKDBL(4)
                CASE_K2_MANUALSTACKDBL(5)
                CASE_K2_MANUALSTACKDBL(6)
                CASE_K2_MANUALSTACKDBL(7)
                CASE_K2_MANUALSTACKDBL(8)
        }
        //*/
    }

    //unary or binary operation
    struct NegOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool argIsGlobal, TypeEnum argTy, int argIndex, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && argTy == TypeEnum::Int);
            int64_t val = GetVarInt(argIsGlobal, argIndex, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, -val, stackBase, intLocals, intGlobals);
        }
    };
    struct NegFltOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool argIsGlobal, TypeEnum argTy, int argIndex, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Float && argTy == TypeEnum::Float);
            double val = GetVarFloat(argIsGlobal, argIndex, stackBase, fltLocals, fltGlobals);
            SetVarFloat(isGlobal, index, -val, stackBase, fltLocals, fltGlobals);
        }
    };
    struct AddOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 + val2, stackBase, intLocals, intGlobals);
        }
    };
    struct AddFltOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Float && arg1Ty == TypeEnum::Float && arg2Ty == TypeEnum::Float);
            double val1 = GetVarFloat(arg1IsGlobal, arg1Index, stackBase, fltLocals, fltGlobals);
            double val2 = GetVarFloat(arg2IsGlobal, arg2Index, stackBase, fltLocals, fltGlobals);
            SetVarFloat(isGlobal, index, val1 + val2, stackBase, fltLocals, fltGlobals);
        }
    };
    struct AddStrOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::String && arg1Ty == TypeEnum::String && arg2Ty == TypeEnum::String);
            const std::string& val1 = GetVarString(arg1IsGlobal, arg1Index, stackBase, strLocals, strGlobals);
            const std::string& val2 = GetVarString(arg2IsGlobal, arg2Index, stackBase, strLocals, strGlobals);
            SetVarString(isGlobal, index, val1 + val2, stackBase, strLocals, strGlobals);
        }
    };
    struct SubOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 - val2, stackBase, intLocals, intGlobals);
        }
    };
    struct SubFltOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Float && arg1Ty == TypeEnum::Float && arg2Ty == TypeEnum::Float);
            double val1 = GetVarFloat(arg1IsGlobal, arg1Index, stackBase, fltLocals, fltGlobals);
            double val2 = GetVarFloat(arg2IsGlobal, arg2Index, stackBase, fltLocals, fltGlobals);
            SetVarFloat(isGlobal, index, val1 - val2, stackBase, fltLocals, fltGlobals);
        }
    };
    struct MulOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 * val2, stackBase, intLocals, intGlobals);
        }
    };
    struct MulFltOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Float && arg1Ty == TypeEnum::Float && arg2Ty == TypeEnum::Float);
            double val1 = GetVarFloat(arg1IsGlobal, arg1Index, stackBase, fltLocals, fltGlobals);
            double val2 = GetVarFloat(arg2IsGlobal, arg2Index, stackBase, fltLocals, fltGlobals);
            SetVarFloat(isGlobal, index, val1 * val2, stackBase, fltLocals, fltGlobals);
        }
    };
    struct DivOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 / val2, stackBase, intLocals, intGlobals);
        }
    };
    struct DivFltOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Float && arg1Ty == TypeEnum::Float && arg2Ty == TypeEnum::Float);
            double val1 = GetVarFloat(arg1IsGlobal, arg1Index, stackBase, fltLocals, fltGlobals);
            double val2 = GetVarFloat(arg2IsGlobal, arg2Index, stackBase, fltLocals, fltGlobals);
            SetVarFloat(isGlobal, index, val1 / val2, stackBase, fltLocals, fltGlobals);
        }
    };
    struct ModOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 % val2, stackBase, intLocals, intGlobals);
        }
    };
    struct ModFltOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Float && arg1Ty == TypeEnum::Float && arg2Ty == TypeEnum::Float);
            double val1 = GetVarFloat(arg1IsGlobal, arg1Index, stackBase, fltLocals, fltGlobals);
            double val2 = GetVarFloat(arg2IsGlobal, arg2Index, stackBase, fltLocals, fltGlobals);
            SetVarFloat(isGlobal, index, std::fmod(val1, val2), stackBase, fltLocals, fltGlobals);
        }
    };
    struct AndOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 && val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct OrOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 || val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct NotOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool argIsGlobal, TypeEnum argTy, int argIndex, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && argTy == TypeEnum::Int);
            int64_t val = GetVarInt(argIsGlobal, argIndex, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val ? 0 : 1, stackBase, intLocals, intGlobals);
        }
    };
    struct GTOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 > val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct GTFltOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Float && arg2Ty == TypeEnum::Float);
            double val1 = GetVarFloat(arg1IsGlobal, arg1Index, stackBase, fltLocals, fltGlobals);
            double val2 = GetVarFloat(arg2IsGlobal, arg2Index, stackBase, fltLocals, fltGlobals);
            SetVarInt(isGlobal, index, val1 > val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct GTStrOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::String && arg2Ty == TypeEnum::String);
            const std::string& val1 = GetVarString(arg1IsGlobal, arg1Index, stackBase, strLocals, strGlobals);
            const std::string& val2 = GetVarString(arg2IsGlobal, arg2Index, stackBase, strLocals, strGlobals);
            SetVarInt(isGlobal, index, val1 > val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct GEOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 >= val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct GEFltOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Float && arg2Ty == TypeEnum::Float);
            double val1 = GetVarFloat(arg1IsGlobal, arg1Index, stackBase, fltLocals, fltGlobals);
            double val2 = GetVarFloat(arg2IsGlobal, arg2Index, stackBase, fltLocals, fltGlobals);
            SetVarInt(isGlobal, index, val1 >= val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct GEStrOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::String && arg2Ty == TypeEnum::String);
            const std::string& val1 = GetVarString(arg1IsGlobal, arg1Index, stackBase, strLocals, strGlobals);
            const std::string& val2 = GetVarString(arg2IsGlobal, arg2Index, stackBase, strLocals, strGlobals);
            SetVarInt(isGlobal, index, val1 >= val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct EQOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 == val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct EQFltOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Float && arg2Ty == TypeEnum::Float);
            double val1 = GetVarFloat(arg1IsGlobal, arg1Index, stackBase, fltLocals, fltGlobals);
            double val2 = GetVarFloat(arg2IsGlobal, arg2Index, stackBase, fltLocals, fltGlobals);
            SetVarInt(isGlobal, index, val1 == val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct EQStrOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::String && arg2Ty == TypeEnum::String);
            const std::string& val1 = GetVarString(arg1IsGlobal, arg1Index, stackBase, strLocals, strGlobals);
            const std::string& val2 = GetVarString(arg2IsGlobal, arg2Index, stackBase, strLocals, strGlobals);
            SetVarInt(isGlobal, index, val1 == val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct NEOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 != val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct NEFltOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Float && arg2Ty == TypeEnum::Float);
            double val1 = GetVarFloat(arg1IsGlobal, arg1Index, stackBase, fltLocals, fltGlobals);
            double val2 = GetVarFloat(arg2IsGlobal, arg2Index, stackBase, fltLocals, fltGlobals);
            SetVarInt(isGlobal, index, val1 != val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct NEStrOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::String && arg2Ty == TypeEnum::String);
            const std::string& val1 = GetVarString(arg1IsGlobal, arg1Index, stackBase, strLocals, strGlobals);
            const std::string& val2 = GetVarString(arg2IsGlobal, arg2Index, stackBase, strLocals, strGlobals);
            SetVarInt(isGlobal, index, val1 != val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct LEOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 <= val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct LEFltOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Float && arg2Ty == TypeEnum::Float);
            double val1 = GetVarFloat(arg1IsGlobal, arg1Index, stackBase, fltLocals, fltGlobals);
            double val2 = GetVarFloat(arg2IsGlobal, arg2Index, stackBase, fltLocals, fltGlobals);
            SetVarInt(isGlobal, index, val1 <= val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct LEStrOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::String && arg2Ty == TypeEnum::String);
            const std::string& val1 = GetVarString(arg1IsGlobal, arg1Index, stackBase, strLocals, strGlobals);
            const std::string& val2 = GetVarString(arg2IsGlobal, arg2Index, stackBase, strLocals, strGlobals);
            SetVarInt(isGlobal, index, val1 <= val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct LTOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 < val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct LTFltOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Float && arg2Ty == TypeEnum::Float);
            double val1 = GetVarFloat(arg1IsGlobal, arg1Index, stackBase, fltLocals, fltGlobals);
            double val2 = GetVarFloat(arg2IsGlobal, arg2Index, stackBase, fltLocals, fltGlobals);
            SetVarInt(isGlobal, index, val1 < val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct LTStrOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::String && arg2Ty == TypeEnum::String);
            const std::string& val1 = GetVarString(arg1IsGlobal, arg1Index, stackBase, strLocals, strGlobals);
            const std::string& val2 = GetVarString(arg2IsGlobal, arg2Index, stackBase, strLocals, strGlobals);
            SetVarInt(isGlobal, index, val1 < val2 ? 1 : 0, stackBase, intLocals, intGlobals);
        }
    };
    struct LShiftOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 << static_cast<int>(val2), stackBase, intLocals, intGlobals);
        }
    };
    struct RShiftOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 >> static_cast<int>(val2), stackBase, intLocals, intGlobals);
        }
    };
    struct URShiftOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, static_cast<int64_t>(static_cast<uint64_t>(val1) >> static_cast<int>(val2)), stackBase, intLocals, intGlobals);
        }
    };
    struct BitAndOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 & val2, stackBase, intLocals, intGlobals);
        }
    };
    struct BitOrOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 | val2, stackBase, intLocals, intGlobals);
        }
    };
    struct BitXorOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool arg1IsGlobal, TypeEnum arg1Ty, int arg1Index, bool arg2IsGlobal, TypeEnum arg2Ty, int arg2Index, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && arg1Ty == TypeEnum::Int && arg2Ty == TypeEnum::Int);
            int64_t val1 = GetVarInt(arg1IsGlobal, arg1Index, stackBase, intLocals, intGlobals);
            int64_t val2 = GetVarInt(arg2IsGlobal, arg2Index, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, val1 ^ val2, stackBase, intLocals, intGlobals);
        }
    };
    struct BitNotOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool argIsGlobal, TypeEnum argTy, int argIndex, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty == TypeEnum::Int && argTy == TypeEnum::Int);
            int64_t val = GetVarInt(argIsGlobal, argIndex, stackBase, intLocals, intGlobals);
            SetVarInt(isGlobal, index, ~val, stackBase, intLocals, intGlobals);
        }
    };

    struct Int2StrOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool isGlobal1, TypeEnum ty1, int index1, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty1 == TypeEnum::Int && ty == TypeEnum::String);
            int64_t ival = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
            std::string res = Convert<std::string>(ival);
            SetVarString(isGlobal, index, res, stackBase, strLocals, strGlobals);
        }
    };
    struct Flt2StrOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool isGlobal1, TypeEnum ty1, int index1, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty1 == TypeEnum::Float && ty == TypeEnum::String);
            double fval = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
            std::string res = Convert<std::string>(fval);
            SetVarString(isGlobal, index, res, stackBase, strLocals, strGlobals);
        }
    };
    struct Str2IntOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool isGlobal1, TypeEnum ty1, int index1, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty1 == TypeEnum::String && ty == TypeEnum::Int);
            const std::string& sval = GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals);
            int64_t res = Convert<int64_t>(sval);
            SetVarInt(isGlobal, index, res, stackBase, intLocals, intGlobals);
        }
    };
    struct Str2FltOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool isGlobal1, TypeEnum ty1, int index1, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty1 == TypeEnum::String && ty == TypeEnum::Float);
            const std::string& sval = GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals);
            double res = Convert<double>(sval);
            SetVarFloat(isGlobal, index, res, stackBase, fltLocals, fltGlobals);
        }
    };
    struct CastFltIntOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool isGlobal1, TypeEnum ty1, int index1, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty1 == TypeEnum::Float && ty == TypeEnum::Int);
            double fval = GetVarFloat(isGlobal1, index1, stackBase, fltLocals, fltGlobals);
            int64_t res = Convert<int64_t>(fval);
            SetVarInt(isGlobal, index, res, stackBase, intLocals, intGlobals);
        }
    };
    struct CastStrIntOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool isGlobal1, TypeEnum ty1, int index1, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty1 == TypeEnum::String && ty == TypeEnum::Int);
            const std::string& sval = GetVarString(isGlobal1, index1, stackBase, strLocals, strGlobals);
            int64_t res = reinterpret_cast<int64_t>(sval.c_str());
            SetVarInt(isGlobal, index, res, stackBase, intLocals, intGlobals);
        }
    };
    struct CastIntFltOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool isGlobal1, TypeEnum ty1, int index1, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty1 == TypeEnum::Int && ty == TypeEnum::Float);
            int64_t ival = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
            double res = Convert<double>(ival);
            SetVarFloat(isGlobal, index, res, stackBase, fltLocals, fltGlobals);
        }
    };
    struct CastIntStrOperation
    {
        static inline void Calc(bool isGlobal, TypeEnum ty, int index, bool isGlobal1, TypeEnum ty1, int index1, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
        {
            MyAssert(ty1 == TypeEnum::Int && ty == TypeEnum::String);
            int64_t ival = GetVarInt(isGlobal1, index1, stackBase, intLocals, intGlobals);
            const char* res = reinterpret_cast<const char*>(ival);
            SetVarString(isGlobal, index, res, stackBase, strLocals, strGlobals);
        }
    };

    template<typename OperationT>
    static inline void DoUnary(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);

        OperationT::Calc(isGlobal, ty, index, isGlobal1, ty1, index1, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
    }
    template<typename OperationT>
    static inline void DoBinary(int32_t opcode, InsEnum op, const std::vector<int32_t>& codes, int32_t& pos, int32_t stackBase, IntLocals& intLocals, FloatLocals& fltLocals, StringLocals& strLocals, IntGlobals& intGlobals, FloatGlobals& fltGlobals, StringGlobals& strGlobals)
    {
        ++pos;
        int32_t operand = codes[pos];

        int32_t argNum;
        bool isGlobal;
        TypeEnum ty;
        int32_t index;
        DecodeOpcode(opcode, op, argNum, isGlobal, ty, index);
        bool isGlobal1;
        TypeEnum ty1;
        int32_t index1;
        DecodeOperand1(operand, isGlobal1, ty1, index1);
        bool isGlobal2;
        TypeEnum ty2;
        int32_t index2;
        DecodeOperand2(operand, isGlobal2, ty2, index2);

        OperationT::Calc(isGlobal, ty, index, isGlobal1, ty1, index1, isGlobal2, ty2, index2, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
    }

#define DoUnaryNEG DoUnary<NegOperation>
#define DoUnaryNEGFLT DoUnary<NegFltOperation>
#define DoUnaryNOT DoUnary<NotOperation>
#define DoUnaryBITNOT DoUnary<BitNotOperation>
#define DoUnaryINT2STR DoUnary<Int2StrOperation>
#define DoUnaryFLT2STR DoUnary<Flt2StrOperation>
#define DoUnarySTR2INT DoUnary<Str2IntOperation>
#define DoUnarySTR2FLT DoUnary<Str2FltOperation>
#define DoUnaryCASTFLTINT DoUnary<CastFltIntOperation>
#define DoUnaryCASTSTRINT DoUnary<CastStrIntOperation>
#define DoUnaryCASTINTFLT DoUnary<CastIntFltOperation>
#define DoUnaryCASTINTSTR DoUnary<CastIntStrOperation>
#define DoBinaryADD DoBinary<AddOperation>
#define DoBinaryADDFLT DoBinary<AddFltOperation>
#define DoBinaryADDSTR DoBinary<AddStrOperation>
#define DoBinarySUB DoBinary<SubOperation>
#define DoBinarySUBFLT DoBinary<SubFltOperation>
#define DoBinaryMUL DoBinary<MulOperation>
#define DoBinaryMULFLT DoBinary<MulFltOperation>
#define DoBinaryDIV DoBinary<DivOperation>
#define DoBinaryDIVFLT DoBinary<DivFltOperation>
#define DoBinaryMOD DoBinary<ModOperation>
#define DoBinaryMODFLT DoBinary<ModFltOperation>
#define DoBinaryAND DoBinary<AndOperation>
#define DoBinaryOR DoBinary<OrOperation>
#define DoBinaryGT DoBinary<GTOperation>
#define DoBinaryGTFLT DoBinary<GTFltOperation>
#define DoBinaryGTSTR DoBinary<GTStrOperation>
#define DoBinaryGE DoBinary<GEOperation>
#define DoBinaryGEFLT DoBinary<GEFltOperation>
#define DoBinaryGESTR DoBinary<GEStrOperation>
#define DoBinaryEQ DoBinary<EQOperation>
#define DoBinaryEQFLT DoBinary<EQFltOperation>
#define DoBinaryEQSTR DoBinary<EQStrOperation>
#define DoBinaryNE DoBinary<NEOperation>
#define DoBinaryNEFLT DoBinary<NEFltOperation>
#define DoBinaryNESTR DoBinary<NEStrOperation>
#define DoBinaryLE DoBinary<LEOperation>
#define DoBinaryLEFLT DoBinary<LEFltOperation>
#define DoBinaryLESTR DoBinary<LEStrOperation>
#define DoBinaryLT DoBinary<LTOperation>
#define DoBinaryLTFLT DoBinary<LTFltOperation>
#define DoBinaryLTSTR DoBinary<LTStrOperation>
#define DoBinaryLSHIFT DoBinary<LShiftOperation>
#define DoBinaryRSHIFT DoBinary<RShiftOperation>
#define DoBinaryURSHIFT DoBinary<URShiftOperation>
#define DoBinaryBITAND DoBinary<BitAndOperation>
#define DoBinaryBITOR DoBinary<BitOrOperation>
#define DoBinaryBITXOR DoBinary<BitXorOperation>

    struct HookData
    {
        std::vector<int32_t> m_OnEnter{};
        std::vector<int32_t> m_OnExit{};
    };

    struct DebugScriptGlobalImpl
    {
        int32_t m_NextIntConstIndex{ c_max_variable_table_size - 1 };
        int32_t m_NextFloatConstIndex{ c_max_variable_table_size - 1 };
        int32_t m_NextStringConstIndex{ c_max_variable_table_size - 1 };

        IntGlobals m_IntGlobals{};
        FloatGlobals m_FloatGlobals{};
        StringGlobals m_StringGlobals{};
        int32_t m_NextIntGlobalIndex{};
        int32_t m_NextFloatGlobalIndex{};
        int32_t m_NextStringGlobalIndex{};

        std::unordered_map<std::string, int32_t> m_HookMap{};
        std::vector<HookData> m_HookDatas{};

        std::atomic_uint32_t m_RuntimeVersion{};
#if USE_STD_SHARED_MUTEX
        using read_write_lock = std::shared_mutex;
        using write_locker = std::lock_guard<std::shared_mutex>;
        struct read_locker
        {
            read_locker(std::shared_mutex& mutex, bool lock):m_shared_lock(mutex, std::defer_lock_t())
            {
                if (lock)
                    m_shared_lock.lock();
            }
            ~read_locker()
            {}
            std::shared_lock<std::shared_mutex> m_shared_lock;
        };
#else
        using read_write_lock = ReadWriteLock;
        using write_locker = typename ReadWriteLock::AutoWriteLock;
        struct read_locker
        {
            read_locker(ReadWriteLock& mutex, bool lock) :m_mutex(mutex), m_lock(lock)
            {
                if (lock)
                    m_mutex.ReadLock();
            }
            ~read_locker()
            {
                if (m_lock)
                    m_mutex.ReadUnlock();
            }
            ReadWriteLock& m_mutex;
            bool m_lock;
        };
#endif
        read_write_lock m_Mutex{};

        void Reset()
        {
            write_locker lock(m_Mutex);

            g_DebugScriptStarted = false;
            g_DebugScriptSerialNum = ++m_RuntimeVersion;

            g_start_time = std::chrono::high_resolution_clock::now();

            m_NextIntConstIndex = c_max_variable_table_size - 1;
            m_NextFloatConstIndex = c_max_variable_table_size - 1;
            m_NextStringConstIndex = c_max_variable_table_size - 1;
            m_IntGlobals.fill(0);
            m_FloatGlobals.fill(0);
            m_StringGlobals.fill(std::string());
            m_NextIntGlobalIndex = 0;
            m_NextFloatGlobalIndex = 0;
            m_NextStringGlobalIndex = 0;

            m_HookMap.clear();
            m_HookDatas.clear();
        }
        void AllocConstInt(int64_t val)
        {
            write_locker lock(m_Mutex);

            if (m_NextIntConstIndex > 0) {
                m_IntGlobals[m_NextIntConstIndex--] = val;
            }
        }
        void AllocConstFloat(double val)
        {
            write_locker lock(m_Mutex);

            if (m_NextFloatConstIndex > 0) {
                m_FloatGlobals[m_NextFloatConstIndex--] = val;
            }
        }
        void AllocConstString(const char* val)
        {
            write_locker lock(m_Mutex);

            if (m_NextStringConstIndex > 0) {
                m_StringGlobals[m_NextStringConstIndex--] = val;
            }
        }
        void AllocGlobalInt(int64_t val)
        {
            write_locker lock(m_Mutex);

            if (m_NextIntGlobalIndex < static_cast<int32_t>(m_IntGlobals.size())) {
                m_IntGlobals[m_NextIntGlobalIndex++] = val;
            }
        }
        void AllocGlobalFloat(double val)
        {
            write_locker lock(m_Mutex);

            if (m_NextFloatGlobalIndex < static_cast<int32_t>(m_FloatGlobals.size())) {
                m_FloatGlobals[m_NextFloatGlobalIndex++] = val;
            }
        }
        void AllocGlobalString(const char* val)
        {
            write_locker lock(m_Mutex);

            if (m_NextStringGlobalIndex < static_cast<int32_t>(m_StringGlobals.size())) {
                m_StringGlobals[m_NextStringGlobalIndex++] = val;
            }
        }
        int32_t AddHook(const char* name, std::vector<int32_t>&& onEnter, std::vector<int32_t>&& onExit)
        {
            write_locker lock(m_Mutex);

            HookData newData{};
            newData.m_OnEnter.swap(onEnter);
            newData.m_OnExit.swap(onExit);
            int32_t hookId = static_cast<int32_t>(m_HookDatas.size());
            m_HookMap.insert(std::make_pair(std::string(name), hookId));
            m_HookDatas.push_back(std::move(newData));
            return hookId;
        }
        int32_t AddHook(const char* name, int32_t* onEnter, int32_t onEnterSize, int32_t* onExit, int32_t onExitSize)
        {
            std::vector<int32_t> onEnterVec{};
            std::vector<int32_t> onExitVec{};
            onEnterVec.assign(onEnter, onEnter + onEnterSize);
            onExitVec.assign(onExit, onExit + onExitSize);
            return AddHook(name, std::move(onEnterVec), std::move(onExitVec));
        }
        int32_t ShareWith(int32_t hookId, const char* other)
        {
            if (hookId >= 0 && hookId < static_cast<int32_t>(m_HookDatas.size())) {
                write_locker lock(m_Mutex);

                m_HookMap.insert(std::make_pair(std::string(other), hookId));
                return hookId;
            }
            return -1;
        }
        void Start()
        {
            g_DebugScriptStarted = true;
        }
        void Pause()
        {
            g_DebugScriptStarted = false;
        }
        void Resume()
        {
            g_DebugScriptStarted = true;
        }
        bool Load(const char* file)
        {
            std::ifstream ifs(file, std::ios::in | std::ios::binary);
            if (ifs.fail())
                return false;

            //tag:DSBC 0x43425344
            int tag = ReadInt32(ifs);
            if (tag != 0x43425344)
                return false;

            //str table offset
            int offset = ReadInt32(ifs);

            auto&& pos = ifs.tellg();
            ifs.seekg(offset, std::ios::beg);

            //string count
            int strNum = ReadInt32(ifs);
            std::vector<std::string> strTable{};
            for (int i = 0; i < strNum; ++i) {
                std::string str = ReadString(ifs);
                strTable.push_back(std::move(str));
            }

            ifs.seekg(pos, std::ios::beg);

            //int consts
            int intConstNum = ReadInt32(ifs);
            for (int i = 0; i < intConstNum; ++i) {
                int64_t v = ReadInt64(ifs);
                AllocConstInt(v);
            }

            //float consts
            int floatConstNum = ReadInt32(ifs);
            for (int i = 0; i < floatConstNum; ++i) {
                double v = ReadDouble(ifs);
                AllocConstFloat(v);
            }

            //string consts
            int strConstNum = ReadInt32(ifs);
            for (int i = 0; i < strConstNum; ++i) {
                int ix = ReadInt32(ifs);
                if (ix >= 0 && ix < static_cast<int>(strTable.size())) {
                    AllocConstString(strTable[ix].c_str());
                }
                else {
                    AllocConstString("");
                }
            }

            //int globals
            int intGlobalNum = ReadInt32(ifs);
            for (int i = 0; i < intGlobalNum; ++i) {
                int64_t v = ReadInt64(ifs);
                AllocGlobalInt(v);
            }

            //float globals
            int floatGlobalNum = ReadInt32(ifs);
            for (int i = 0; i < floatGlobalNum; ++i) {
                double v = ReadDouble(ifs);
                AllocGlobalFloat(v);
            }

            //string globals
            int strGlobalNum = ReadInt32(ifs);
            for (int i = 0; i < strGlobalNum; ++i) {
                int ix = ReadInt32(ifs);
                if (ix >= 0 && ix < static_cast<int>(strTable.size())) {
                    AllocGlobalString(strTable[ix].c_str());
                }
                else {
                    AllocGlobalString("");
                }
            }

            //hook count
            int hookNum = ReadInt32(ifs);
            for (int i = 0; i < hookNum; ++i) {
                //hook name
                int nameIx = ReadInt32(ifs);
                const char* name = nameIx >= 0 && nameIx < static_cast<int>(strTable.size()) ? strTable[nameIx].c_str() : "";
                //onEnter count
                int onEnterNum = ReadInt32(ifs);
                std::vector<int32_t> onEnter(onEnterNum);
                ifs.read(reinterpret_cast<char*>(onEnter.data()), onEnterNum * sizeof(int32_t));
                int onExitNum = ReadInt32(ifs);
                std::vector<int32_t> onExit(onExitNum);
                ifs.read(reinterpret_cast<char*>(onExit.data()), onExitNum * sizeof(int32_t));

                int32_t hookId = AddHook(name, std::move(onEnter), std::move(onExit));
                myprintf("hook:%s id:%d\n", name, hookId);

                //shader name count
                int shareNameNum = ReadInt32(ifs);
                for (int ix = 0; ix < shareNameNum; ++ix) {
                    int shareNameIx = ReadInt32(ifs);
                    const char* other = shareNameIx >= 0 && shareNameIx < static_cast<int>(strTable.size()) ? strTable[shareNameIx].c_str() : "";
                    ShareWith(hookId, other);
                    myprintf("share with:%s id:%d\n", other, hookId);
                }
            }

            ifs.close();
            return true;
        }
        read_write_lock& GetReadWriteLock()
        {
            return m_Mutex;
        }

        //these methods must be protected by read_locker
        int32_t FindHook(const std::string& name)const
        {
            auto&& it = m_HookMap.find(name);
            if (it != m_HookMap.end())
                return it->second;
            return -1;
        }
    };

    struct DebugScriptVMImpl
    {
        struct AutoStackAllocator
        {
            AutoStackAllocator(int32_t& depth) :m_Depth(depth)
            {
                ++m_Depth;
            }
            inline int32_t StackBase()const
            {
                return (m_Depth - 1) * c_max_variable_table_size;
            }
            ~AutoStackAllocator() {
                --m_Depth;
            }
        private:
            int32_t& m_Depth;
        };

        int32_t m_StackDepth{0};
        IntLocals m_IntLocals{};
        FloatLocals m_FloatLocals{};
        StringLocals m_StringLocals{};

        std::vector<ExternApiArgOrRetVal> m_ExternApiArgs{};

        int32_t FindHook(const std::string& name)const
        {
            DebugScriptGlobalImpl::read_locker lock(g_pDebugScriptGlobal->GetReadWriteLock(), m_StackDepth != 0);

            int32_t id = g_pDebugScriptGlobal->FindHook(name);
            return id;
        }
        bool CanRun()const
        {
            return g_DebugScriptStarted;
        }
        bool RunOnEnter(int32_t id, int32_t argc, int64_t argv[])
        {
            UNLIKELY_ATTR
            if (OPTIMIZER_UNLIKELY(CanRun()))
            {
                DebugScriptGlobalImpl::read_locker lock(g_pDebugScriptGlobal->GetReadWriteLock(), m_StackDepth != 0);

                if (id < 0 || id >= static_cast<int32_t>(g_pDebugScriptGlobal->m_HookDatas.size()))
                    return false;
                const HookData& data = g_pDebugScriptGlobal->m_HookDatas[id];
                return Run(data.m_OnEnter, id, argc, argv);
            }
            return false;
        }
        bool RunOnExit(int32_t id, int32_t argc, int64_t argv[])
        {
            UNLIKELY_ATTR
            if (OPTIMIZER_UNLIKELY(CanRun()))
            {
                DebugScriptGlobalImpl::read_locker lock(g_pDebugScriptGlobal->GetReadWriteLock(), m_StackDepth != 0);

                if (id < 0 || id >= static_cast<int32_t>(g_pDebugScriptGlobal->m_HookDatas.size()))
                    return false;
                const HookData& data = g_pDebugScriptGlobal->m_HookDatas[id];
                return Run(data.m_OnExit, id, argc, argv);
            }
            return false;
        }

        bool Run(const std::vector<int32_t>& codes, int32_t id, int32_t argc, int64_t argv[])
        {
            auto&& intGlobals = g_pDebugScriptGlobal->m_IntGlobals;
            auto&& fltGlobals = g_pDebugScriptGlobal->m_FloatGlobals;
            auto&& strGlobals = g_pDebugScriptGlobal->m_StringGlobals;

            auto&& intLocals = m_IntLocals;
            auto&& fltLocals = m_FloatLocals;
            auto&& strLocals = m_StringLocals;

            bool ret = false;
            AutoStackAllocator autoStk(m_StackDepth);
            int32_t stackBase = autoStk.StackBase();
            if (stackBase + c_max_variable_table_size <= c_max_local_variable_table_size) {
                for (int32_t pos = 0; pos < static_cast<int32_t>(codes.size()); ++pos) {
                    int32_t opcode = codes[pos];
                    InsEnum op = DecodeInsEnum(opcode);
                    switch (op) {
                    case InsEnum::CALLEXTERN:
                        DoCallExtern(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals, m_ExternApiArgs);
                        break;
                    case InsEnum::RET:
                        DoRet(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals, ret);
                        return ret;
                    case InsEnum::JMP:
                        DoJmp(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::JMPIF:
                        DoJmpIf(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::JMPIFNOT:
                        DoJmpIfNot(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::INC:
                        DoInc(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::INCFLT:
                        DoIncFlt(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::INCV:
                        DoIncVal(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::INCVFLT:
                        DoIncValFlt(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::DEC:
                        DoDec(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::DECFLT:
                        DoDecFlt(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::DECV:
                        DoDecVal(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::DECVFLT:
                        DoDecValFlt(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::MOV:
                        DoMov(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::MOVFLT:
                        DoMovFlt(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::MOVSTR:
                        DoMovStr(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::ARRGET:
                        DoArrayGet(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::ARRGETFLT:
                        DoArrayGetFlt(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::ARRGETSTR:
                        DoArrayGetStr(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::ARRSET:
                        DoArraySet(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::ARRSETFLT:
                        DoArraySetFlt(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::ARRSETSTR:
                        DoArraySetStr(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::NEG:
                        DoUnaryNEG(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::NEGFLT:
                        DoUnaryNEGFLT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::ADD:
                        DoBinaryADD(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::ADDFLT:
                        DoBinaryADDFLT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::ADDSTR:
                        DoBinaryADDSTR(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::SUB:
                        DoBinarySUB(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::SUBFLT:
                        DoBinarySUBFLT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::MUL:
                        DoBinaryMUL(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::MULFLT:
                        DoBinaryMULFLT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::DIV:
                        DoBinaryDIV(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::DIVFLT:
                        DoBinaryDIVFLT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::MOD:
                        DoBinaryMOD(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::MODFLT:
                        DoBinaryMODFLT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::AND:
                        DoBinaryAND(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::OR:
                        DoBinaryOR(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::NOT:
                        DoUnaryNOT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::GT:
                        DoBinaryGT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::GTFLT:
                        DoBinaryGTFLT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::GTSTR:
                        DoBinaryGTSTR(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::GE:
                        DoBinaryGE(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::GEFLT:
                        DoBinaryGEFLT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::GESTR:
                        DoBinaryGESTR(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::EQ:
                        DoBinaryEQ(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::EQFLT:
                        DoBinaryEQFLT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::EQSTR:
                        DoBinaryEQSTR(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::NE:
                        DoBinaryNE(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::NEFLT:
                        DoBinaryNEFLT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::NESTR:
                        DoBinaryNESTR(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::LE:
                        DoBinaryLE(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::LEFLT:
                        DoBinaryLEFLT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::LESTR:
                        DoBinaryLESTR(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::LT:
                        DoBinaryLT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::LTFLT:
                        DoBinaryLTFLT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::LTSTR:
                        DoBinaryLTSTR(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::LSHIFT:
                        DoBinaryLSHIFT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::RSHIFT:
                        DoBinaryRSHIFT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::URSHIFT:
                        DoBinaryURSHIFT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::BITAND:
                        DoBinaryBITAND(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::BITOR:
                        DoBinaryBITOR(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::BITXOR:
                        DoBinaryBITXOR(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::BITNOT:
                        DoUnaryBITNOT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::INT2STR:
                        DoUnaryINT2STR(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::FLT2STR:
                        DoUnaryFLT2STR(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::STR2INT:
                        DoUnarySTR2INT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::STR2FLT:
                        DoUnarySTR2FLT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::CASTFLTINT:
                        DoUnaryCASTFLTINT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::CASTSTRINT:
                        DoUnaryCASTSTRINT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::CASTINTFLT:
                        DoUnaryCASTINTFLT(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::CASTINTSTR:
                        DoUnaryCASTINTSTR(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::ASINT:
                        DoAsInt(opcode, op, codes, pos, stackBase, intLocals, fltLocals, intGlobals, fltGlobals);
                        break;
                    case InsEnum::ASFLOAT:
                        DoAsFloat(opcode, op, codes, pos, stackBase, intLocals, fltLocals, intGlobals, fltGlobals);
                        break;
                    case InsEnum::ASLONG:
                        DoAsLong(opcode, op, codes, pos, stackBase, intLocals, fltLocals, intGlobals, fltGlobals);
                        break;
                    case InsEnum::ASDOUBLE:
                        DoAsDouble(opcode, op, codes, pos, stackBase, intLocals, fltLocals, intGlobals, fltGlobals);
                        break;
                    case InsEnum::ARGC:
                        DoArgc(opcode, op, codes, pos, stackBase, intLocals, intGlobals, argc);
                        break;
                    case InsEnum::ARGV:
                        DoArgv(opcode, op, codes, pos, stackBase, intLocals, intGlobals, argc, argv);
                        break;
                    case InsEnum::ADDR:
                        DoAddr(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::ADDRFLT:
                        DoAddrFlt(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::ADDRSTR:
                        DoAddrStr(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::PTRGET:
                        DoPtrGet(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::PTRGETFLT:
                        DoPtrGetFlt(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::PTRGETSTR:
                        DoPtrGetStr(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::PTRSET:
                        DoPtrSet(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::PTRSETFLT:
                        DoPtrSetFlt(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::PTRSETSTR:
                        DoPtrSetStr(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::CASCADEPTR:
                        DoCascadePtr(opcode, op, codes, pos, stackBase, intLocals, intGlobals);
                        break;
                    case InsEnum::STKIX:
                        DoStackIndex(opcode, op, codes, pos, stackBase, intLocals, intGlobals);
                        break;
                    case InsEnum::HOOKID:
                        DoHookId(opcode, op, codes, pos, stackBase, intLocals, intGlobals, id);
                        break;
                    case InsEnum::HOOKVER:
                        DoHookVer(opcode, op, codes, pos, stackBase, intLocals, intGlobals);
                        break;
                    case InsEnum::FFIAUTO:
                        DoFFIAuto(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::FFIMANUAL:
                        DoFFIManual(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::FFIMANUALSTACK:
                        DoFFIManualStack(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::FFIMANUALDBL:
                        DoFFIManualDbl(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    case InsEnum::FFIMANUALSTACKDBL:
                        DoFFIManualStackDbl(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        break;
                    default:
                        if (op >= InsEnum::CALLINTERN_FIRST && op <= InsEnum::CALLINTERN_LAST) {
                            DoCallIntern(opcode, op, codes, pos, stackBase, intLocals, fltLocals, strLocals, intGlobals, fltGlobals, strGlobals);
                        }
                        break;
                    }
                }
            }
            return ret;
        }
    };
    struct AutoMark
    {
        AutoMark(bool& flag) :m_Flag(flag)
        {
            m_Flag = true;
        }
        ~AutoMark()
        {
            m_Flag = false;
        }
    private:
        bool& m_Flag;
    };

    DebugScriptGlobalImpl* GetDebugScriptGlobal()
    {
        if (nullptr == g_pDebugScriptGlobal) {
            g_pDebugScriptGlobal = new DebugScriptGlobalImpl();
        }
        return g_pDebugScriptGlobal;
    }

    DebugScriptVMImpl* GetDebugScriptVM()
    {
        if (g_IsInNewDebugScriptVM) {
            return nullptr;
        }
        if (nullptr == g_pDebugScriptVM) {
            AutoMark mark(g_IsInNewDebugScriptVM);
            g_pDebugScriptVM = new DebugScriptVMImpl();
        }
        return g_pDebugScriptVM;
    }
}

void DebugScriptGlobal::Reset()
{
    GetDebugScriptGlobal()->Reset();
}
void DebugScriptGlobal::AllocConstInt(int64_t val)
{
    GetDebugScriptGlobal()->AllocConstInt(val);
}
void DebugScriptGlobal::AllocConstFloat(double val)
{
    GetDebugScriptGlobal()->AllocConstFloat(val);
}
void DebugScriptGlobal::AllocConstString(const char* val)
{
    GetDebugScriptGlobal()->AllocConstString(val);
}
void DebugScriptGlobal::AllocGlobalInt(int64_t val)
{
    GetDebugScriptGlobal()->AllocGlobalInt(val);
}
void DebugScriptGlobal::AllocGlobalFloat(double val)
{
    GetDebugScriptGlobal()->AllocGlobalFloat(val);
}
void DebugScriptGlobal::AllocGlobalString(const char* val)
{
    GetDebugScriptGlobal()->AllocGlobalString(val);
}
int32_t DebugScriptGlobal::AddHook(const char* name, int32_t* onEnter, int32_t onEnterSize, int32_t* onExit, int32_t onExitSize)
{
    return GetDebugScriptGlobal()->AddHook(name, onEnter, onEnterSize, onExit, onExitSize);
}
int32_t DebugScriptGlobal::ShareWith(int32_t hookId, const char* other)
{
    return GetDebugScriptGlobal()->ShareWith(hookId, other);
}
void DebugScriptGlobal::Start()
{
    GetDebugScriptGlobal()->Start();
}
void DebugScriptGlobal::Pause()
{
    GetDebugScriptGlobal()->Pause();
}
void DebugScriptGlobal::Resume()
{
    GetDebugScriptGlobal()->Resume();
}
bool DebugScriptGlobal::Load(const char* file)
{
    return GetDebugScriptGlobal()->Load(file);
}

int32_t DebugScriptVM::FindHook(const char* name)
{
    auto&& vm = GetDebugScriptVM();
    UNLIKELY_ATTR
    if (OPTIMIZER_UNLIKELY(!vm))
        return -1;
    return vm->FindHook(name);
}
bool DebugScriptVM::CanRun()
{
    auto&& vm = GetDebugScriptVM();
    UNLIKELY_ATTR
    if (OPTIMIZER_UNLIKELY(!vm))
        return false;
    return vm->CanRun();
}
bool DebugScriptVM::RunHookOnEnter(int32_t id, int32_t argc, int64_t argv[])
{
    auto&& vm = GetDebugScriptVM();
    UNLIKELY_ATTR
    if (OPTIMIZER_UNLIKELY(!vm))
        return false;
    return vm->RunOnEnter(id, argc, argv);
}
bool DebugScriptVM::RunHookOnExit(int32_t id, int32_t argc, int64_t argv[])
{
    auto&& vm = GetDebugScriptVM();
    UNLIKELY_ATTR
    if (OPTIMIZER_UNLIKELY(!vm))
        return false;
    return vm->RunOnExit(id, argc, argv);
}
