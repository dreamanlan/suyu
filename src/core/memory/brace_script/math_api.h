#pragma once

#define BRACE_SUPPORTED_PLATFORM

#ifdef BRACE_SUPPORTED_PLATFORM
#include "BraceScript.h"
#include "brace_object.h"
#include "pinv.h"
#include <cmath>
#include <limits>
#include <random>

#define PI 3.1415926

namespace BraceScriptInterpreter
{
    class SqrtExp final : public Brace::SimpleBraceApiBase
    {
    public:
        SqrtExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected sqrt(number) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::sqrt(v));
        }
    };
    class CbrtExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CbrtExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected cbrt(number) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::cbrt(v));
        }
    };
    class PowExp final : public Brace::SimpleBraceApiBase
    {
    public:
        PowExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 2) {
                auto& argInfo = argInfos[0];
                auto& argInfo2 = argInfos[1];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING && argInfo2.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo2.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected pow(base, exp) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            auto& argInfo2 = argInfos[1];
            double b = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            double e = Brace::VarGetF64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::pow(b, e));
        }
    };
    class HypotExp final : public Brace::SimpleBraceApiBase
    {
    public:
        HypotExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 2) {
                auto& argInfo = argInfos[0];
                auto& argInfo2 = argInfos[1];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING && argInfo2.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo2.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            else if (argInfos.size() == 3) {
                auto& argInfo = argInfos[0];
                auto& argInfo2 = argInfos[1];
                auto& argInfo3 = argInfos[2];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING && argInfo2.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo2.Type < Brace::BRACE_DATA_TYPE_STRING && argInfo3.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo3.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected hypot(x, y) or hypot(x, y, z) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            auto& argInfo2 = argInfos[1];
            double x = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            double y = Brace::VarGetF64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex);
            if (argInfos.size() == 3) {
                auto& argInfo3 = argInfos[2];
                double z = Brace::VarGetF64((argInfo3.IsGlobal ? gvars : lvars), argInfo3.Type, argInfo3.VarIndex);
                Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::hypot(x, y, z)); //need c++17
                //Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::hypot(x, y));
            }
            else {
                Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::hypot(x, y));
            }
        }
    };
    class AbsExp final : public Brace::SimpleBraceApiBase
    {
    public:
        AbsExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected abs(number) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::abs(v));
        }
    };
    class CeilExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CeilExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected ceil(number) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::ceil(v));
        }
    };
    class FloorExp final : public Brace::SimpleBraceApiBase
    {
    public:
        FloorExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected floor(number) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::floor(v));
        }
    };
    class SinExp final : public Brace::SimpleBraceApiBase
    {
    public:
        SinExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected sin(number) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::sin(v));
        }
    };
    class CosExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CosExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected cos(number) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::cos(v));
        }
    };
    class TanExp final : public Brace::SimpleBraceApiBase
    {
    public:
        TanExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected tan(number) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::tan(v));
        }
    };
    class AsinExp final : public Brace::SimpleBraceApiBase
    {
    public:
        AsinExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected asin(number) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::asin(v));
        }
    };
    class AcosExp final : public Brace::SimpleBraceApiBase
    {
    public:
        AcosExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected acos(number) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::acos(v));
        }
    };
    class AtanExp final : public Brace::SimpleBraceApiBase
    {
    public:
        AtanExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected atan(number) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::atan(v));
        }
    };
    class Atan2Exp final : public Brace::SimpleBraceApiBase
    {
    public:
        Atan2Exp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 2) {
                auto& argInfo = argInfos[0];
                auto& argInfo2 = argInfos[1];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING && argInfo2.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo2.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected atan2(y, x) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            auto& argInfo2 = argInfos[1];
            double y = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            double x = Brace::VarGetF64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::atan2(y, x));
        }
    };
    class Deg2RadExp final : public Brace::SimpleBraceApiBase
    {
    public:
        Deg2RadExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected deg2rad(number) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v * PI / 180.0);
        }
    };
    class Rad2DegExp final : public Brace::SimpleBraceApiBase
    {
    public:
        Rad2DegExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected rad2deg(number) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v * 180.0 / PI);
        }
    };
    class RandIntExp final : public Brace::SimpleBraceApiBase
    {
    public:
        RandIntExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 2) {
                auto& argInfo = argInfos[0];
                auto& argInfo2 = argInfos[1];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING && argInfo2.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo2.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_INT64;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            else if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_INT64;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            else if (argInfos.size() == 0) {
                resultInfo.Type = Brace::BRACE_DATA_TYPE_INT64;
                resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                resultInfo.Name = GenTempVarName();
                resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                return true;
            }
            std::stringstream ss;
            ss << "expected randint() or randint(max_num) or randint(min_num, max_num) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            std::default_random_engine engine;
            if (argInfos.size() >= 1) {
                auto& argInfo = argInfos[0];
                int64_t v1 = Brace::VarGetI64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                if (argInfos.size() == 2) {
                    auto& argInfo2 = argInfos[1];
                    int64_t v2 = Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex);
                    std::uniform_int_distribution<int64_t> dist(v1, v2);
                    Brace::VarSetInt64((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, dist(engine));
                }
                else {
                    std::uniform_int_distribution<int64_t> dist(0, v1);
                    Brace::VarSetInt64((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, dist(engine));
                }
            }
            else {
                std::uniform_int_distribution<int64_t> dist(0, std::numeric_limits<int64_t>::max());
                Brace::VarSetInt64((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, dist(engine));
            }
        }
    };
    class RandFloatExp final : public Brace::SimpleBraceApiBase
    {
    public:
        RandFloatExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 2) {
                auto& argInfo = argInfos[0];
                auto& argInfo2 = argInfos[1];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING && argInfo2.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo2.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            else if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type > Brace::BRACE_DATA_TYPE_BOOL && argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            else if (argInfos.size() == 0) {
                resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                resultInfo.Name = GenTempVarName();
                resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                return true;
            }
            std::stringstream ss;
            ss << "expected randfloat() or randfloat(max_num) or randfloat(min_num, max_num) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            std::default_random_engine engine;
            if (argInfos.size() >= 1) {
                auto& argInfo = argInfos[0];
                double v1 = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                if (argInfos.size() == 2) {
                    auto& argInfo2 = argInfos[1];
                    double v2 = Brace::VarGetF64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex);
                    std::uniform_real_distribution<double> dist(v1, v2);
                    Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, dist(engine));
                }
                else {
                    std::uniform_real_distribution<double> dist(0, v1);
                    Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, dist(engine));
                }
            }
            else {
                std::uniform_real_distribution<double> dist(0, 1);
                Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, dist(engine));
            }
        }
    };
    class MaxExp final : public Brace::SimpleBraceApiBase
    {
    public:
        MaxExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            for(auto&& argInfo : argInfos) {
                if (argInfo.Type <= Brace::BRACE_DATA_TYPE_BOOL || argInfo.Type >= Brace::BRACE_DATA_TYPE_STRING) {
                    std::stringstream ss;
                    ss << "expected max(number, ...) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            double mv = 0;
            bool first = true;
            for (auto&& argInfo : argInfos) {
                double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                if (first) {
                    mv = v;
                    first = false;
                }
                else {
                    if (mv < v)
                        mv = v;
                }
            }
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, mv);
        }
    };
    class MinExp final : public Brace::SimpleBraceApiBase
    {
    public:
        MinExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            for (auto&& argInfo : argInfos) {
                if (argInfo.Type <= Brace::BRACE_DATA_TYPE_BOOL || argInfo.Type >= Brace::BRACE_DATA_TYPE_STRING) {
                    std::stringstream ss;
                    ss << "expected min(number, ...) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            double mv = DBL_MAX;
            bool first = true;
            for (auto&& argInfo : argInfos) {
                double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                if (first) {
                    mv = v;
                    first = false;
                }
                else {
                    if (mv > v)
                        mv = v;
                }
            }
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, mv);
        }
    };
    class SumExp final : public Brace::SimpleBraceApiBase
    {
    public:
        SumExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            for (auto&& argInfo : argInfos) {
                if (argInfo.Type <= Brace::BRACE_DATA_TYPE_BOOL || argInfo.Type >= Brace::BRACE_DATA_TYPE_STRING) {
                    std::stringstream ss;
                    ss << "expected sum(number, ...) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            double mv = 0;
            for (auto&& argInfo : argInfos) {
                double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                mv += v;
            }
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, mv);
        }
    };
    class AvgExp final : public Brace::SimpleBraceApiBase
    {
    public:
        AvgExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            for (auto&& argInfo : argInfos) {
                if (argInfo.Type <= Brace::BRACE_DATA_TYPE_BOOL || argInfo.Type >= Brace::BRACE_DATA_TYPE_STRING) {
                    std::stringstream ss;
                    ss << "expected avg(number, ...) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            double mv = 0;
            for (auto&& argInfo : argInfos) {
                double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                mv += v;
            }
            int ct = static_cast<int>(argInfos.size());
            if (ct > 0)
                mv /= ct;
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, mv);
        }
    };
    class DevSqExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DevSqExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            for (auto&& argInfo : argInfos) {
                if (argInfo.Type <= Brace::BRACE_DATA_TYPE_BOOL || argInfo.Type >= Brace::BRACE_DATA_TYPE_STRING) {
                    std::stringstream ss;
                    ss << "expected devsq(number, ...) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            double mv = 0;
            double avg = 0;
            for (auto&& argInfo : argInfos) {
                double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                avg += v;
            }
            int ct = static_cast<int>(argInfos.size());
            if (ct > 0)
                avg /= ct;
            for (auto&& argInfo : argInfos) {
                double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                mv += (v - avg) * (v - avg);
            }
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, mv);
        }
    };

    class ArrayMaxExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ArrayMaxExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                int objTypeId = argInfo.ObjectTypeId;
                if (objTypeId > CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY && objTypeId < CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected arraymax(int_array) or arraymax(float_array) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            auto& objPtr = Brace::VarGetObject((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            int objTypeId = argInfo.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                using ArrayType = ArrayT<int64_t>;
                DoCalc<ArrayType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                using ArrayType = ArrayT<double>;
                DoCalc<ArrayType>(gvars, lvars, resultInfo, objPtr);
            }break;
            }

        }
    private:
        template<typename ArrayType>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& resultInfo, const std::shared_ptr<void>& arrayWrap)const
        {
            using value_type = typename ArrayType::value_type;
            auto* arrayObj = static_cast<ArrayType*>(arrayWrap.get());
            if (nullptr != arrayObj) {
                auto& arr = *arrayObj;
                double mv = 0;
                int ct = static_cast<int>(arr.size());
                for (int ix = 0; ix < ct; ++ix) {
                    double v = static_cast<double>(arr[ix]);
                    if (ix == 0) {
                        mv = v;
                    }
                    else {
                        if (mv < v)
                            mv = v;
                    }
                }
                Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, mv);
            }
        }
    };
    class ArrayMinExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ArrayMinExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                int objTypeId = argInfo.ObjectTypeId;
                if (objTypeId > CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY && objTypeId < CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected arraymin(int_array) or arraymin(float_array) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            auto& objPtr = Brace::VarGetObject((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            int objTypeId = argInfo.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                using ArrayType = ArrayT<int64_t>;
                DoCalc<ArrayType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                using ArrayType = ArrayT<double>;
                DoCalc<ArrayType>(gvars, lvars, resultInfo, objPtr);
            }break;
            }

        }
    private:
        template<typename ArrayType>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& resultInfo, const std::shared_ptr<void>& arrayWrap)const
        {
            using value_type = typename ArrayType::value_type;
            auto* arrayObj = static_cast<ArrayType*>(arrayWrap.get());
            if (nullptr != arrayObj) {
                auto& arr = *arrayObj;
                double mv = 0;
                int ct = static_cast<int>(arr.size());
                for (int ix = 0; ix < ct; ++ix) {
                    double v = static_cast<double>(arr[ix]);
                    if (ix == 0) {
                        mv = v;
                    }
                    else {
                        if (mv > v)
                            mv = v;
                    }
                }
                Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, mv);
            }
        }
    };
    class ArraySumExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ArraySumExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                int objTypeId = argInfo.ObjectTypeId;
                if (objTypeId > CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY && objTypeId < CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected arraysum(int_array) or arraysum(float_array) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            auto& objPtr = Brace::VarGetObject((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            int objTypeId = argInfo.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                using ArrayType = ArrayT<int64_t>;
                DoCalc<ArrayType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                using ArrayType = ArrayT<double>;
                DoCalc<ArrayType>(gvars, lvars, resultInfo, objPtr);
            }break;
            }

        }
    private:
        template<typename ArrayType>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& resultInfo, const std::shared_ptr<void>& arrayWrap)const
        {
            using value_type = typename ArrayType::value_type;
            auto* arrayObj = static_cast<ArrayType*>(arrayWrap.get());
            if (nullptr != arrayObj) {
                auto& arr = *arrayObj;
                double mv = 0;
                int ct = static_cast<int>(arr.size());
                for (int ix = 0; ix < ct; ++ix) {
                    mv += arr[ix];
                }
                Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, mv);
            }
        }
    };
    class ArrayAvgExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ArrayAvgExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                int objTypeId = argInfo.ObjectTypeId;
                if (objTypeId > CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY && objTypeId < CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected arrayavg(int_array) or arrayavg(float_array) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            auto& objPtr = Brace::VarGetObject((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            int objTypeId = argInfo.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                using ArrayType = ArrayT<int64_t>;
                DoCalc<ArrayType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                using ArrayType = ArrayT<double>;
                DoCalc<ArrayType>(gvars, lvars, resultInfo, objPtr);
            }break;
            }

        }
    private:
        template<typename ArrayType>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& resultInfo, const std::shared_ptr<void>& arrayWrap)const
        {
            using value_type = typename ArrayType::value_type;
            auto* arrayObj = static_cast<ArrayType*>(arrayWrap.get());
            if (nullptr != arrayObj) {
                auto& arr = *arrayObj;
                double avg = 0;
                int ct = static_cast<int>(arr.size());
                for (int ix = 0; ix < ct; ++ix) {
                    avg += arr[ix];
                }
                if (ct > 0)
                    avg /= ct;
                Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, avg);
            }
        }
    };
    class ArrayDevSqExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ArrayDevSqExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                int objTypeId = argInfo.ObjectTypeId;
                if (objTypeId > CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY && objTypeId < CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected arraydevsq(int_array) or arraydevsq(float_array) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            auto& objPtr = Brace::VarGetObject((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            int objTypeId = argInfo.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                using ArrayType = ArrayT<int64_t>;
                DoCalc<ArrayType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                using ArrayType = ArrayT<double>;
                DoCalc<ArrayType>(gvars, lvars, resultInfo, objPtr);
            }break;
            }

        }
    private:
        template<typename ArrayType>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& resultInfo, const std::shared_ptr<void>& arrayWrap)const
        {
            using value_type = typename ArrayType::value_type;
            auto* arrayObj = static_cast<ArrayType*>(arrayWrap.get());
            if (nullptr != arrayObj) {
                auto& arr = *arrayObj;
                double mv = 0;
                double avg = 0;
                int ct = static_cast<int>(arr.size());
                for (int ix = 0; ix < ct; ++ix) {
                    avg += arr[ix];
                }
                if (ct > 0)
                    avg /= ct;
                for (int ix = 0; ix < ct; ++ix) {
                    double v = static_cast<double>(arr[ix]);
                    mv += (v - avg) * (v - avg);
                }
                Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, mv);
            }
        }
    };

    class HashtableMaxExp final : public Brace::SimpleBraceApiBase
    {
    public:
        HashtableMaxExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                int objTypeId = argInfo.ObjectTypeId;
                switch (objTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE:
                case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE:
                case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE:
                case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE:
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected hashtablemax(int_int_hash) or hashtablemax(str_int_hash) or hashtablemax(int_float_hash) or hashtablemax(str_float_hash) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            auto& objPtr = Brace::VarGetObject((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            int objTypeId = argInfo.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                using HashType = HashtableT<std::string, int64_t>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                using HashType = HashtableT<std::string, double>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                using HashType = HashtableT<int64_t, int64_t>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                using HashType = HashtableT<int64_t, double>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            }
        }
    private:
        template<typename HashType>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& resultInfo, const std::shared_ptr<void>& hashWrap)const
        {
            using key_type = typename HashType::key_type;
            using val_type = typename HashType::mapped_type;
            auto* hashObj = static_cast<HashType*>(hashWrap.get());
            if (nullptr != hashObj) {
                auto& hash = *hashObj;
                double mv = 0;
                bool first = true;
                for (auto&& pair : hash) {
                    double v = static_cast<double>(pair.second);
                    if (first) {
                        mv = v;
                        first = false;
                    }
                    else {
                        if (mv < v)
                            mv = v;
                    }
                }
                Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, mv);
            }
        }
    };
    class HashtableMinExp final : public Brace::SimpleBraceApiBase
    {
    public:
        HashtableMinExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                int objTypeId = argInfo.ObjectTypeId;
                switch (objTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE:
                case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE:
                case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE:
                case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE:
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected hashtablemin(int_int_hash) or hashtablemin(str_int_hash) or hashtablemin(int_float_hash) or hashtablemin(str_float_hash) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            auto& objPtr = Brace::VarGetObject((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            int objTypeId = argInfo.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                using HashType = HashtableT<std::string, int64_t>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                using HashType = HashtableT<std::string, double>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                using HashType = HashtableT<int64_t, int64_t>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                using HashType = HashtableT<int64_t, double>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            }
        }
    private:
        template<typename HashType>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& resultInfo, const std::shared_ptr<void>& hashWrap)const
        {
            using key_type = typename HashType::key_type;
            using val_type = typename HashType::mapped_type;
            auto* hashObj = static_cast<HashType*>(hashWrap.get());
            if (nullptr != hashObj) {
                auto& hash = *hashObj;
                double mv = 0;
                bool first = true;
                for (auto&& pair : hash) {
                    double v = static_cast<double>(pair.second);
                    if (first) {
                        mv = v;
                        first = false;
                    }
                    else {
                        if (mv > v)
                            mv = v;
                    }
                }
                Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, mv);
            }
        }
    };
    class HashtableSumExp final : public Brace::SimpleBraceApiBase
    {
    public:
        HashtableSumExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                int objTypeId = argInfo.ObjectTypeId;
                switch (objTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE:
                case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE:
                case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE:
                case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE:
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected hashtablesum(int_int_hash) or hashtablesum(str_int_hash) or hashtablesum(int_float_hash) or hashtablesum(str_float_hash) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            auto& objPtr = Brace::VarGetObject((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            int objTypeId = argInfo.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                using HashType = HashtableT<std::string, int64_t>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                using HashType = HashtableT<std::string, double>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                using HashType = HashtableT<int64_t, int64_t>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                using HashType = HashtableT<int64_t, double>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            }
        }
    private:
        template<typename HashType>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& resultInfo, const std::shared_ptr<void>& hashWrap)const
        {
            using key_type = typename HashType::key_type;
            using val_type = typename HashType::mapped_type;
            auto* hashObj = static_cast<HashType*>(hashWrap.get());
            if (nullptr != hashObj) {
                auto& hash = *hashObj;
                double mv = 0;
                for (auto&& pair : hash) {
                    mv += pair.second;
                }
                Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, mv);
            }
        }
    };
    class HashtableAvgExp final : public Brace::SimpleBraceApiBase
    {
    public:
        HashtableAvgExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                int objTypeId = argInfo.ObjectTypeId;
                switch (objTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE:
                case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE:
                case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE:
                case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE:
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected hashtableavg(int_int_hash) or hashtableavg(str_int_hash) or hashtableavg(int_float_hash) or hashtableavg(str_float_hash) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            auto& objPtr = Brace::VarGetObject((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            int objTypeId = argInfo.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                using HashType = HashtableT<std::string, int64_t>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                using HashType = HashtableT<std::string, double>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                using HashType = HashtableT<int64_t, int64_t>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                using HashType = HashtableT<int64_t, double>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            }
        }
    private:
        template<typename HashType>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& resultInfo, const std::shared_ptr<void>& hashWrap)const
        {
            using key_type = typename HashType::key_type;
            using val_type = typename HashType::mapped_type;
            auto* hashObj = static_cast<HashType*>(hashWrap.get());
            if (nullptr != hashObj) {
                auto& hash = *hashObj;
                double avg = 0;
                int ct = static_cast<int>(hash.size());
                for (auto&& pair : hash) {
                    avg += pair.second;
                }
                if (ct > 0)
                    avg /= ct;
                Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, avg);
            }
        }
    };
    class HashtableDevSqExp final : public Brace::SimpleBraceApiBase
    {
    public:
        HashtableDevSqExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                int objTypeId = argInfo.ObjectTypeId;
                switch(objTypeId){
                case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE:
                case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE:
                case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE:
                case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE:
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected hashtabledevsq(int_int_hash) or hashtabledevsq(str_int_hash) or hashtabledevsq(int_float_hash) or hashtabledevsq(str_float_hash) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            auto& argInfo = argInfos[0];
            auto& objPtr = Brace::VarGetObject((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            int objTypeId = argInfo.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                using HashType = HashtableT<std::string, int64_t>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                using HashType = HashtableT<std::string, double>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                using HashType = HashtableT<int64_t, int64_t>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                using HashType = HashtableT<int64_t, double>;
                DoCalc<HashType>(gvars, lvars, resultInfo, objPtr);
            }break;
            }
        }
    private:
        template<typename HashType>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& resultInfo, const std::shared_ptr<void>& hashWrap)const
        {
            using key_type = typename HashType::key_type;
            using val_type = typename HashType::mapped_type;
            auto* hashObj = static_cast<HashType*>(hashWrap.get());
            if (nullptr != hashObj) {
                auto& hash = *hashObj;
                double mv = 0;
                double avg = 0;
                int ct = static_cast<int>(hash.size());
                for (auto&& pair : hash) {
                    avg += pair.second;
                }
                if (ct > 0)
                    avg /= ct;
                for (auto&& pair : hash) {
                    double v = static_cast<double>(pair.second);
                    mv += (v - avg) * (v - avg);
                }
                Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, mv);
            }
        }
    };

    class LinearRegressionExp final : public Brace::SimpleBraceApiBase
    {
    public:
        LinearRegressionExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 2 || argInfos.size() == 3) {
                auto& argInfo = argInfos[0];
                auto& argInfo2 = argInfos[1];
                bool arg3Check = true;
                if (argInfos.size() == 3) {
                    auto& argInfo3 = argInfos[2];
                    if (argInfo3.Type >= Brace::BRACE_DATA_TYPE_BOOL && argInfo3.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                        arg3Check = true;
                    }
                    else {
                        arg3Check = false;
                    }
                }
                int objTypeId = argInfo.ObjectTypeId;
                int objTypeId2 = argInfo2.ObjectTypeId;
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
                if (nullptr != pInfo && pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_OBJ_ARRAY && pInfo->FirstTypeParamObjTypeId() == CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY && objTypeId2 == CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY && arg3Check) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                    resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected linearregression(array<:array<:double:>:>, array<:double:>) or linearregression(array<:array<:double:>:>, array<:double:>, bool_debug) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }

        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            using FloatArray = ArrayT<double>;

            auto& argInfo = argInfos[0];
            auto& argInfo2 = argInfos[1];
            bool isDebug = false;
            if (argInfos.size() == 3) {
                auto& argInfo3 = argInfos[2];
                isDebug = Brace::VarGetBoolean((argInfo3.IsGlobal ? gvars : lvars), argInfo3.Type, argInfo3.VarIndex);
            }
            auto& objPtr = Brace::VarGetObject((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            auto& objPtr2 = Brace::VarGetObject((argInfo2.IsGlobal ? gvars : lvars), argInfo2.VarIndex);
            auto* arrayXsPtr = static_cast<ObjectArray*>(objPtr.get());
            auto* arrayYPtr = static_cast<FloatArray*>(objPtr2.get());
            auto* arrPtr = new FloatArray();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(arrPtr));

            auto& xsArr = *arrayXsPtr;
            auto& yArr = *arrayYPtr;
            auto& resultArr = *arrPtr;

            int num = static_cast<int>(yArr.size());
            int dim = static_cast<int>(xsArr.size());

            if (num > 0 && dim > 0) {
                matrix::Matrix x(num, dim + 1);
                matrix::Matrix y(num, 1);
                for (int j = 0; j <= dim; ++j) {
                    if (j == 0) {
                        for (int i = 0; i < num; ++i) {
                            x(i, j) = 1;
                        }
                    }
                    else {
                        auto& col = xsArr[j - 1];
                        auto* colArr = static_cast<FloatArray*>(col.get());
                        for (int i = 0; i < num; ++i) {
                            x(i, j) = (*colArr)[i];
                        }
                    }
                }
                for (int i = 0; i < num; ++i) {
                    y(i, 0) = yArr[i];
                }

                if (isDebug)
                    matrix::Helper::LogRef() = std::bind(&LinearRegressionExp::LogMatrix, this, std::placeholders::_1, std::placeholders::_2);
                else
                    matrix::Helper::LogRef() = nullptr;

                matrix::Helper::Log(x, "x:");
                matrix::Helper::Log(y, "y:");

                matrix::Matrix pinv{};
                if (matrix::geninv(x, pinv)) {

                    matrix::Helper::Log(pinv, "pinv:");

                    matrix::Matrix check = x * pinv * x;
                    matrix::Helper::Log(check, "check:");

                    matrix::Matrix betas = pinv * y;

                    matrix::Helper::Log(betas, "beta:");

                    matrix::Matrix ny = x * betas;

                    matrix::Helper::Log(ny, "ny:");

                    double mv = 0;
                    double avg = 0;

                    for (auto&& v : yArr) {
                        avg += v;
                    }
                    avg /= num;

                    for (auto&& v : yArr) {
                        mv += (v - avg) * (v - avg);
                    }

                    double mv2 = 0;
                    for (int ix = 0; ix < num; ++ix) {
                        double v = yArr[ix];
                        double vi = ny(ix, 0);
                        mv2 += (v - vi) * (v - vi);
                    }

                    for (int ix = 0; ix <= dim; ++ix) {
                        double beta = betas(ix, 0);
                        resultArr.push_back(beta);
                    }

                    double sigmasqr = num != 2 ? mv2 / (num - 2) : DBL_MAX;
                    double rsqr = mv != 0 ? 1 - mv2 / mv : 0;
                    resultArr.push_back(sigmasqr);
                    resultArr.push_back(rsqr);
                }
            }
        }
    private:
        void LogMatrix(const matrix::Matrix& m, const char* tag) const
        {
            std::stringstream ss;
            ss << tag;
            LogInfo(ss.str());
            for (std::size_t i = 0; i < m.GetM(); ++i) {
                ss.str("");
                for (std::size_t j = 0; j < m.GetN(); ++j) {
                    if (j > 0)
                        ss << " ";
                    ss << m(i, j);
                }
                LogInfo(ss.str());
            }
        }
    };
}
#endif //BRACE_SUPPORTED_PLATFORM