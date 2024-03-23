#ifndef BRACE_SCRIPT_INTERPRETER_INC
#include "brace_script_interpreter.h"
#include "BraceScript.h"
#include "BraceCoroutine.h"
#include "BraceAny.h"
#include "brace_object.h"
#endif

namespace BraceScriptInterpreter
{
    class CastExp final : public Brace::AbstractBraceApi
    {
    public:
        CastExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter), m_AssignPtr(nullptr), m_ResultInfo()
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //cast(exp, type)
            if (data.GetParamNum() != 2)
                return false;
            auto* type = data.GetParam(1);
            auto typeInfo = ParseParamTypeInfo(*type);

            Brace::OperandLoadtimeInfo info;
            info.Type = typeInfo.Type;
            info.ObjectTypeId = typeInfo.ObjectTypeId;
            auto expExecutor = LoadHelper(*data.GetParam(0), info);
            if (Brace::DataTypeInfo::IsSameType(info, typeInfo)) {
                resultInfo = info;
                std::swap(executor, expExecutor);
                return true;
            }
            else if (!Brace::IsObjectType(typeInfo.Type) && !Brace::IsObjectType(info.Type)) {
                auto fptr = Brace::GetVarAssignPtr(typeInfo.Type, false, info.Type, false);
                if (nullptr != fptr) {
                    m_AssignPtr = fptr;
                    resultInfo.Type = typeInfo.Type;
                    resultInfo.ObjectTypeId = typeInfo.ObjectTypeId;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);

                    m_ExpInfo = info;
                    std::swap(m_Exp, expExecutor);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &CastExp::Execute);
                }
            }
            std::stringstream ss;
            ss << "expected cast(exp, type), line: " << data.GetLine();
            LogError(ss.str());
            executor = nullptr;
            return false;
        }
    private:
        int Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)
        {
            if (!m_Exp.isNull())
                m_Exp(gvars, lvars);
            (*m_AssignPtr)((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, (m_ExpInfo.IsGlobal ? gvars : lvars), m_ExpInfo.VarIndex);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    private:
        Brace::VarAssignPtr m_AssignPtr;
        Brace::OperandRuntimeInfo m_ExpInfo;
        Brace::BraceApiExecutor m_Exp;
        Brace::OperandRuntimeInfo m_ResultInfo;
    };
    class TypeTagExp final : public Brace::AbstractBraceApi
    {
    public:
        TypeTagExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter)
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //typetag(type) or typetag(exp)
            if (data.GetParamNum() != 1) {
                std::stringstream ss;
                ss << "expected typetag(type) or typetag(exp), line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            auto* typeOrExp = data.GetParam(0);
            auto typeInfo = ParseParamTypeInfo(*typeOrExp);
            if (Brace::IsUnknownType(typeInfo.Type) || typeInfo.Type == Brace::BRACE_DATA_TYPE_OBJECT && typeInfo.ObjectTypeId <= 0) {
                Brace::OperandLoadtimeInfo loadInfo;
                LoadHelper(*typeOrExp, loadInfo);
                typeInfo.Type = loadInfo.Type;
                typeInfo.ObjectTypeId = loadInfo.ObjectTypeId;
            }

            resultInfo.Type = typeInfo.Type;
            resultInfo.ObjectTypeId = typeInfo.ObjectTypeId;
            resultInfo.Name = "loadtimevar";
            resultInfo.VarIndex = INVALID_INDEX;

            executor = nullptr;
            return true;
        }
    };
    class TypeIdExp final : public Brace::AbstractBraceApi
    {
    public:
        TypeIdExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter)
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //typeid(type) or typeid(exp)
            if (data.GetParamNum() != 1) {
                std::stringstream ss;
                ss << "expected typeid(type) or typeid(exp), line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            int type = Brace::BRACE_DATA_TYPE_UNKNOWN;
            auto* typeOrExp = data.GetParam(0);
            auto typeInfo = ParseParamTypeInfo(*typeOrExp);
            if (Brace::IsUnknownType(typeInfo.Type) || typeInfo.Type == Brace::BRACE_DATA_TYPE_OBJECT && typeInfo.ObjectTypeId <= 0) {
                Brace::OperandLoadtimeInfo loadInfo;
                LoadHelper(*typeOrExp, loadInfo);
                type = loadInfo.Type;
            }
            else {
                type = typeInfo.Type;
            }

            std::string varId = std::to_string(type);
            auto* info = GetConstInfo(DslData::ValueData::VALUE_TYPE_NUM, varId);
            if (nullptr != info) {
                resultInfo.Type = info->Type;
                resultInfo.ObjectTypeId = info->ObjectTypeId;
                resultInfo.VarIndex = info->VarIndex;
                resultInfo.IsGlobal = true;
                resultInfo.IsTempVar = false;
                resultInfo.IsConst = true;
                resultInfo.Name = varId;
            }
            else {
                resultInfo.VarIndex = AllocConst(DslData::ValueData::VALUE_TYPE_NUM, varId, resultInfo.Type, resultInfo.ObjectTypeId);
                resultInfo.IsGlobal = true;
                resultInfo.IsTempVar = false;
                resultInfo.IsConst = true;
                resultInfo.Name = varId;
            }
            executor = nullptr;
            return true;
        }
    };
    class ObjTypeIdExp final : public Brace::AbstractBraceApi
    {
    public:
        ObjTypeIdExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter)
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //objtypeid(type) or objtypeid(exp)
            if (data.GetParamNum() != 1) {
                std::stringstream ss;
                ss << "expected objtypeid(type) or objtypeid(exp), line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            int type = Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
            auto* typeOrExp = data.GetParam(0);
            auto typeInfo = ParseParamTypeInfo(*typeOrExp);
            if (Brace::IsUnknownType(typeInfo.Type) || typeInfo.Type == Brace::BRACE_DATA_TYPE_OBJECT && typeInfo.ObjectTypeId <= Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ) {
                Brace::OperandLoadtimeInfo loadInfo;
                LoadHelper(*typeOrExp, loadInfo);
                type = loadInfo.ObjectTypeId;
            }
            else {
                type = typeInfo.ObjectTypeId;
            }

            std::string varId = std::to_string(type);
            auto* info = GetConstInfo(DslData::ValueData::VALUE_TYPE_NUM, varId);
            if (nullptr != info) {
                resultInfo.Type = info->Type;
                resultInfo.ObjectTypeId = info->ObjectTypeId;
                resultInfo.VarIndex = info->VarIndex;
                resultInfo.IsGlobal = true;
                resultInfo.IsTempVar = false;
                resultInfo.IsConst = true;
                resultInfo.Name = varId;
            }
            else {
                resultInfo.VarIndex = AllocConst(DslData::ValueData::VALUE_TYPE_NUM, varId, resultInfo.Type, resultInfo.ObjectTypeId);
                resultInfo.IsGlobal = true;
                resultInfo.IsTempVar = false;
                resultInfo.IsConst = true;
                resultInfo.Name = varId;
            }
            executor = nullptr;
            return true;
        }
    };
    class GetObjTypeNameExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetObjTypeNameExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type >= Brace::BRACE_DATA_TYPE_INT8 && argInfo.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected getobjtypename(objtypeid), line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo = argInfos[0];
            int objTypeId = static_cast<int>(Brace::VarGetI64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex));
            const std::string& v = g_ObjectInfoMgr.GetBraceObjectTypeName(objTypeId);
            Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
        }
    };
    class GetObjCategoryExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetObjCategoryExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type >= Brace::BRACE_DATA_TYPE_INT8 && argInfo.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected getobjcategory(objtypeid), line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo = argInfos[0];
            int objTypeId = static_cast<int>(Brace::VarGetI64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex));
            int v = g_ObjectInfoMgr.GetBraceObjectCategory(objTypeId);
            Brace::VarSetInt32((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
        }
    };
    class GetTypeParamCountExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetTypeParamCountExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type >= Brace::BRACE_DATA_TYPE_INT8 && argInfo.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected gettypeparamcount(objtypeid), line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo = argInfos[0];
            int objTypeId = static_cast<int>(Brace::VarGetI64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex));
            int v = g_ObjectInfoMgr.GetBraceObjectTypeParamCount(objTypeId);
            Brace::VarSetInt32((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
        }
    };
    class GetTypeParamTypeExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetTypeParamTypeExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 2) {
                auto& argInfo = argInfos[0];
                auto& argInfo2 = argInfos[1];
                if (argInfo.Type >= Brace::BRACE_DATA_TYPE_INT8 && argInfo.Type <= Brace::BRACE_DATA_TYPE_UINT64 && argInfo2.Type >= Brace::BRACE_DATA_TYPE_INT8 && argInfo2.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected gettypeparamtype(objtypeid, index), line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo = argInfos[0];
            auto& argInfo2 = argInfos[1];
            int objTypeId = static_cast<int>(Brace::VarGetI64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex));
            int index = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            int v = g_ObjectInfoMgr.GetBraceObjectTypeParamType(objTypeId, index);
            Brace::VarSetInt32((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
        }
    };
    class GetTypeParamObjTypeIdExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetTypeParamObjTypeIdExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 2) {
                auto& argInfo = argInfos[0];
                auto& argInfo2 = argInfos[1];
                if (argInfo.Type >= Brace::BRACE_DATA_TYPE_INT8 && argInfo.Type <= Brace::BRACE_DATA_TYPE_UINT64 && argInfo2.Type >= Brace::BRACE_DATA_TYPE_INT8 && argInfo2.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected gettypeparamobjtypeid(objtypeid, index), line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo = argInfos[0];
            auto& argInfo2 = argInfos[1];
            int objTypeId = static_cast<int>(Brace::VarGetI64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex));
            int index = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            int v = g_ObjectInfoMgr.GetBraceObjectTypeParamObjTypeId(objTypeId, index);
            Brace::VarSetInt32((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
        }
    };
    class SwapExp final : public Brace::AbstractBraceApi
    {
    public:
        SwapExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter), m_Var1Info(), m_Var2Info()
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //swap(var1, var2)
            if (data.GetParamNum() != 2) {
                std::stringstream ss;
                ss << "expected swap(var1, var2), line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            DslData::ISyntaxComponent* param1 = data.GetParam(0);
            DslData::ISyntaxComponent* param2 = data.GetParam(1);
            if (param1->GetSyntaxType() != DslData::ISyntaxComponent::TYPE_VALUE || param2->GetSyntaxType() != DslData::ISyntaxComponent::TYPE_VALUE) {
                std::stringstream ss;
                ss << "expected swap(var1, var2), var1 and var2 must be local var or global var, line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            const std::string& varId1 = param1->GetId();
            const std::string& varId2 = param2->GetId();
            bool var1IsGlobal = varId1.length() > 0 && varId1[0] == '@';
            bool var2IsGlobal = varId2.length() > 0 && varId2[0] == '@';
            Brace::VarInfo* varInfo1 = nullptr;
            Brace::VarInfo* varInfo2 = nullptr;
            if (var1IsGlobal)
                varInfo1 = GetGlobalVarInfo(varId1);
            else
                varInfo1 = GetVarInfo(varId1);
            if (var2IsGlobal)
                varInfo2 = GetGlobalVarInfo(varId2);
            else
                varInfo2 = GetVarInfo(varId2);
            if (nullptr == varInfo1) {
                std::stringstream ss;
                ss << "can't find var " << varId1 << ", line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            if (nullptr == varInfo2) {
                std::stringstream ss;
                ss << "can't find var " << varId2 << ", line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            if (varInfo1->Type != varInfo2->Type || varInfo1->ObjectTypeId != varInfo2->ObjectTypeId) {
                std::stringstream ss;
                ss << varId1 << " and " << varId2 << " must be same type, line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            if (varInfo1->Type == Brace::BRACE_DATA_TYPE_REF) {
                auto& ref1 = func.VarInitInfo.ReferenceVars[varInfo1->VarIndex];
                auto& ref2 = func.VarInitInfo.ReferenceVars[varInfo2->VarIndex];
                if (ref1.Type != ref2.Type || ref1.ObjectTypeId != ref2.ObjectTypeId) {
                    std::stringstream ss;
                    ss << varId1 << " and " << varId2 << " must be same type, line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            m_Var1Info.Type = static_cast<int8_t>(varInfo1->Type);
            m_Var1Info.ObjectTypeId = varInfo1->ObjectTypeId;
            m_Var1Info.VarIndex = static_cast<int16_t>(varInfo1->VarIndex);
            m_Var1Info.IsGlobal = (var1IsGlobal ? 1 : 0);

            m_Var2Info.Type = static_cast<int8_t>(varInfo2->Type);
            m_Var2Info.ObjectTypeId = varInfo2->ObjectTypeId;
            m_Var2Info.VarIndex = static_cast<int16_t>(varInfo2->VarIndex);
            m_Var2Info.IsGlobal = (var2IsGlobal ? 1 : 0);

            executor.attach(this, &SwapExp::Execute);
            return true;
        }
    private:
        int Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            //todo:split by tuple (var, global/local vars, type) for performance, maybe 104 functions, a lot of work..
            auto& vars1 = (m_Var1Info.IsGlobal ? gvars : lvars);
            auto& vars2 = (m_Var2Info.IsGlobal ? gvars : lvars);
            int index1 = m_Var1Info.VarIndex;
            int index2 = m_Var2Info.VarIndex;
            int type = m_Var1Info.Type;

            DoSwap(type, vars1, vars2, index1, index2);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    private:
        void DoSwap(int type, Brace::VariableInfo& vars1, Brace::VariableInfo& vars2, int index1, int index2)const
        {
            switch (type) {
            case Brace::BRACE_DATA_TYPE_BOOL: {
                bool v1 = Brace::VarGetBool(vars1, index1);
                bool v2 = Brace::VarGetBool(vars2, index2);
                Brace::VarSetBool(vars1, index1, v2);
                Brace::VarSetBool(vars2, index2, v1);
            }break;
            case Brace::BRACE_DATA_TYPE_INT8: {
                int8_t v1 = Brace::VarGetInt8(vars1, index1);
                int8_t v2 = Brace::VarGetInt8(vars2, index2);
                Brace::VarSetInt8(vars1, index1, v2);
                Brace::VarSetInt8(vars2, index2, v1);
            }break;
            case Brace::BRACE_DATA_TYPE_UINT8: {
                uint8_t v1 = Brace::VarGetUInt8(vars1, index1);
                uint8_t v2 = Brace::VarGetUInt8(vars2, index2);
                Brace::VarSetUInt8(vars1, index1, v2);
                Brace::VarSetUInt8(vars2, index2, v1);
            }break;
            case Brace::BRACE_DATA_TYPE_INT16: {
                int16_t v1 = Brace::VarGetInt16(vars1, index1);
                int16_t v2 = Brace::VarGetInt16(vars2, index2);
                Brace::VarSetInt16(vars1, index1, v2);
                Brace::VarSetInt16(vars2, index2, v1);
            }break;
            case Brace::BRACE_DATA_TYPE_UINT16: {
                uint16_t v1 = Brace::VarGetUInt16(vars1, index1);
                uint16_t v2 = Brace::VarGetUInt16(vars2, index2);
                Brace::VarSetUInt16(vars1, index1, v2);
                Brace::VarSetUInt16(vars2, index2, v1);
            }break;
            case Brace::BRACE_DATA_TYPE_INT32: {
                int32_t v1 = Brace::VarGetInt32(vars1, index1);
                int32_t v2 = Brace::VarGetInt32(vars2, index2);
                Brace::VarSetInt32(vars1, index1, v2);
                Brace::VarSetInt32(vars2, index2, v1);
            }break;
            case Brace::BRACE_DATA_TYPE_UINT32: {
                uint32_t v1 = Brace::VarGetUInt32(vars1, index1);
                uint32_t v2 = Brace::VarGetUInt32(vars2, index2);
                Brace::VarSetUInt32(vars1, index1, v2);
                Brace::VarSetUInt32(vars2, index2, v1);
            }break;
            case Brace::BRACE_DATA_TYPE_INT64: {
                int64_t v1 = Brace::VarGetInt64(vars1, index1);
                int64_t v2 = Brace::VarGetInt64(vars2, index2);
                Brace::VarSetInt64(vars1, index1, v2);
                Brace::VarSetInt64(vars2, index2, v1);
            }break;
            case Brace::BRACE_DATA_TYPE_UINT64: {
                uint64_t v1 = Brace::VarGetUInt64(vars1, index1);
                uint64_t v2 = Brace::VarGetUInt64(vars2, index2);
                Brace::VarSetUInt64(vars1, index1, v2);
                Brace::VarSetUInt64(vars2, index2, v1);
            }break;
            case Brace::BRACE_DATA_TYPE_FLOAT: {
                float v1 = Brace::VarGetFloat(vars1, index1);
                float v2 = Brace::VarGetFloat(vars2, index2);
                Brace::VarSetFloat(vars1, index1, v2);
                Brace::VarSetFloat(vars2, index2, v1);
            }break;
            case Brace::BRACE_DATA_TYPE_DOUBLE: {
                double v1 = Brace::VarGetDouble(vars1, index1);
                double v2 = Brace::VarGetDouble(vars2, index2);
                Brace::VarSetDouble(vars1, index1, v2);
                Brace::VarSetDouble(vars2, index2, v1);
            }break;
            case Brace::BRACE_DATA_TYPE_STRING: {
                std::string v1 = Brace::VarGetString(vars1, index1);
                const std::string& v2 = Brace::VarGetString(vars2, index2);
                Brace::VarSetString(vars1, index1, v2);
                Brace::VarSetString(vars2, index2, v1);
            }break;
            case Brace::BRACE_DATA_TYPE_OBJECT: {
                auto v1 = Brace::VarGetObject(vars1, index1);
                auto& v2 = Brace::VarGetObject(vars2, index2);
                Brace::VarSetObject(vars1, index1, v2);
                Brace::VarSetObject(vars2, index2, v1);
            }break;
            case Brace::BRACE_DATA_TYPE_REF: {
                auto& ref1 = Brace::VarGetRef(vars1, index1);
                auto& ref2 = Brace::VarGetRef(vars2, index2);

                DoSwap(ref1.Type, *ref1.Vars, *ref2.Vars, ref1.VarIndex, ref2.VarIndex);
            }break;
            }
        }
    private:
        Brace::OperandRuntimeInfo m_Var1Info;
        Brace::OperandRuntimeInfo m_Var2Info;
    };

    class CppObjectMemberCallProvider final : public AbstractMemberCallApiProvider
    {
        friend class MemberCallExp;
    protected:
        virtual bool LoadMemberCall(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& objInfo, Brace::BraceApiExecutor&& obj, std::string&& member, std::vector<Brace::OperandLoadtimeInfo>&& argInfos, std::vector<Brace::BraceApiExecutor>&& args, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor)override
        {
            if (member == "toString") {
                if (argInfos.size() > 0) {
                    std::stringstream ss;
                    ss << "expected object.ToString(), line: " << data.GetLine();
                    LogError(ss.str());
                    executor = nullptr;
                    return false;
                }
                resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
                resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                resultInfo.Name = GenTempVarName();
                resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);

                m_ResultInfo = resultInfo;
                switch (objInfo.ObjectTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO:
                    executor.attach(this, &CppObjectMemberCallProvider::ExecuteMemModifyInfoToString);
                    break;
                }

                m_ObjInfo = objInfo;
                std::swap(m_Obj, obj);
                std::swap(m_Member, member);
                return true;
            }
            else {
                std::stringstream ss;
                ss << "unknown method '" << member << "', line: " << data.GetLine();
                LogError(ss.str());
                executor = nullptr;
                return false;
            }
        }
    private:
        int ExecuteMemModifyInfoToString(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            auto& ptr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            auto* pObj = static_cast<Core::Memory::MemoryModifyInfo*>(ptr.get());
            Brace::VarSetString((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, "");
            if (nullptr != pObj) {
                std::stringstream ss;
                ss << std::hex << pObj->addr.GetValue() << "," << pObj->type;
                ss << std::hex;
                switch (pObj->type) {
                case Core::Memory::MemoryModifyInfo::type_u8:
                    ss << "," << static_cast<u16>(pObj->u8Val);
                    ss << "," << static_cast<u16>(pObj->u8OldVal);
                    break;
                case Core::Memory::MemoryModifyInfo::type_u16:
                    ss << "," << pObj->u16Val;
                    ss << "," << pObj->u16OldVal;
                    break;
                case Core::Memory::MemoryModifyInfo::type_u32:
                    ss << "," << pObj->u32Val;
                    ss << "," << pObj->u32OldVal;
                    break;
                case Core::Memory::MemoryModifyInfo::type_u64:
                    ss << "," << pObj->u64Val;
                    ss << "," << pObj->u64OldVal;
                    break;
                }
                ss << std::dec;
                ss << "," << pObj->size;
                Brace::VarSetString((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, ss.str());
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    protected:
        CppObjectMemberCallProvider(Brace::BraceScript& interpreter) :AbstractMemberCallApiProvider(interpreter), m_ObjInfo(), m_Obj(), m_ArgInfos(), m_Args(), m_ArgObjInfos(), m_ResultInfo(), m_ResultObjInfo(nullptr), m_Member(), m_ArgIteratorIndex(INVALID_INDEX)
        {}
    protected:
        Brace::OperandRuntimeInfo m_ObjInfo;
        Brace::BraceApiExecutor m_Obj;
        std::vector<Brace::OperandRuntimeInfo> m_ArgInfos;
        std::vector<Brace::BraceApiExecutor> m_Args;
        std::vector<BraceObjectInfo*> m_ArgObjInfos;
        Brace::OperandRuntimeInfo m_ResultInfo;
        BraceObjectInfo* m_ResultObjInfo;
        std::string m_Member;

        int m_ArgIteratorIndex;
    };
    class CppObjectMemberSetProvider final : public AbstractMemberSetApiProvider
    {
        friend class MemberSetExp;
    protected:
        virtual bool LoadMemberSet(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& objInfo, Brace::BraceApiExecutor&& obj, std::string&& member, Brace::OperandLoadtimeInfo&& argInfo, Brace::BraceApiExecutor&& arg, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor)override
        {
            if (member == "type") {
                if (argInfo.Type < Brace::BRACE_DATA_TYPE_INT8 || argInfo.Type > Brace::BRACE_DATA_TYPE_UINT64) {
                    std::stringstream ss;
                    ss << "object.type must assigned integer value, line: " << data.GetLine();
                    LogError(ss.str());
                    executor = nullptr;
                    return false;
                }
                switch (objInfo.ObjectTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO:
                    executor.attach(this, &CppObjectMemberSetProvider::ExecuteSetMemModifyInfoType);
                    break;
                }

                m_ObjInfo = objInfo;
                std::swap(m_Obj, obj);
                m_ArgInfo = argInfo;
                std::swap(m_Arg, arg);
                std::swap(m_Member, member);
                return true;
            }
            else if (member == "addr") {
                if (argInfo.Type < Brace::BRACE_DATA_TYPE_INT8 || argInfo.Type > Brace::BRACE_DATA_TYPE_UINT64) {
                    std::stringstream ss;
                    ss << "object.addr must assigned integer value, line: " << data.GetLine();
                    LogError(ss.str());
                    executor = nullptr;
                    return false;
                }
                switch (objInfo.ObjectTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO:
                    executor.attach(this, &CppObjectMemberSetProvider::ExecuteSetMemModifyInfoAddr);
                    break;
                }

                m_ObjInfo = objInfo;
                std::swap(m_Obj, obj);
                m_ArgInfo = argInfo;
                std::swap(m_Arg, arg);
                std::swap(m_Member, member);
                return true;
            }
            else if (member == "val") {
                if (argInfo.Type < Brace::BRACE_DATA_TYPE_INT8 || argInfo.Type > Brace::BRACE_DATA_TYPE_UINT64) {
                    std::stringstream ss;
                    ss << "object.val must assigned integer value, line: " << data.GetLine();
                    LogError(ss.str());
                    executor = nullptr;
                    return false;
                }
                switch (objInfo.ObjectTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO:
                    executor.attach(this, &CppObjectMemberSetProvider::ExecuteSetMemModifyInfoVal);
                    break;
                }

                m_ObjInfo = objInfo;
                std::swap(m_Obj, obj);
                m_ArgInfo = argInfo;
                std::swap(m_Arg, arg);
                std::swap(m_Member, member);
                return true;
            }
            else if (member == "oldVal") {
                if (argInfo.Type < Brace::BRACE_DATA_TYPE_INT8 || argInfo.Type > Brace::BRACE_DATA_TYPE_UINT64) {
                    std::stringstream ss;
                    ss << "object.oldVal must assigned integer value, line: " << data.GetLine();
                    LogError(ss.str());
                    executor = nullptr;
                    return false;
                }
                switch (objInfo.ObjectTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO:
                    executor.attach(this, &CppObjectMemberSetProvider::ExecuteSetMemModifyInfoOldVal);
                    break;
                }

                m_ObjInfo = objInfo;
                std::swap(m_Obj, obj);
                m_ArgInfo = argInfo;
                std::swap(m_Arg, arg);
                std::swap(m_Member, member);
                return true;
            }
            else if (member == "size") {
                if (argInfo.Type < Brace::BRACE_DATA_TYPE_INT8 || argInfo.Type > Brace::BRACE_DATA_TYPE_UINT64) {
                    std::stringstream ss;
                    ss << "object.size must assigned integer value, line: " << data.GetLine();
                    LogError(ss.str());
                    executor = nullptr;
                    return false;
                }
                switch (objInfo.ObjectTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO:
                    executor.attach(this, &CppObjectMemberSetProvider::ExecuteSetMemModifyInfoSize);
                    break;
                }

                m_ObjInfo = objInfo;
                std::swap(m_Obj, obj);
                m_ArgInfo = argInfo;
                std::swap(m_Arg, arg);
                std::swap(m_Member, member);
                return true;
            }
            else {
                std::stringstream ss;
                ss << "unknown writable property '" << member << "', line: " << data.GetLine();
                LogError(ss.str());
                executor = nullptr;
                return false;
            }
        }
    private:
        int ExecuteSetMemModifyInfoAddr(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            if (!m_Arg.isNull())
                m_Arg(gvars, lvars);
            auto& ptr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            auto* pObj = static_cast<Core::Memory::MemoryModifyInfo*>(ptr.get());
            if (nullptr != pObj) {
                uint64_t addr = static_cast<uint64_t>(Brace::VarGetI64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex));
                pObj->addr = Common::ProcessAddress(addr);
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteSetMemModifyInfoType(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            if (!m_Arg.isNull())
                m_Arg(gvars, lvars);
            auto& ptr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            auto* pObj = static_cast<Core::Memory::MemoryModifyInfo*>(ptr.get());
            if (nullptr != pObj) {
                int type = static_cast<int>(Brace::VarGetI64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex));
                pObj->type = type;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteSetMemModifyInfoVal(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            if (!m_Arg.isNull())
                m_Arg(gvars, lvars);
            auto& ptr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            auto* pObj = static_cast<Core::Memory::MemoryModifyInfo*>(ptr.get());
            if (nullptr != pObj) {
                uint64_t val = static_cast<uint64_t>(Brace::VarGetI64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex));
                pObj->u64Val = val;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteSetMemModifyInfoOldVal(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            if (!m_Arg.isNull())
                m_Arg(gvars, lvars);
            auto& ptr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            auto* pObj = static_cast<Core::Memory::MemoryModifyInfo*>(ptr.get());
            if (nullptr != pObj) {
                uint64_t val = static_cast<uint64_t>(Brace::VarGetI64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex));
                pObj->u64OldVal = val;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteSetMemModifyInfoSize(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            if (!m_Arg.isNull())
                m_Arg(gvars, lvars);
            auto& ptr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            auto* pObj = static_cast<Core::Memory::MemoryModifyInfo*>(ptr.get());
            if (nullptr != pObj) {
                uint64_t size = static_cast<uint64_t>(Brace::VarGetI64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex));
                pObj->size = size;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    protected:
        CppObjectMemberSetProvider(Brace::BraceScript& interpreter) :AbstractMemberSetApiProvider(interpreter), m_ObjInfo(), m_Obj(), m_ArgInfo(), m_Arg(), m_ArgIsStruct(false), m_Member()
        {}
    protected:
        Brace::OperandRuntimeInfo m_ObjInfo;
        Brace::BraceApiExecutor m_Obj;
        Brace::OperandRuntimeInfo m_ArgInfo;
        Brace::BraceApiExecutor m_Arg;
        bool m_ArgIsStruct;
        std::string m_Member;
    };
    class CppObjectMemberGetProvider final : public AbstractMemberGetApiProvider
    {
        friend class MemberGetExp;
    protected:
        virtual bool LoadMemberGet(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& objInfo, Brace::BraceApiExecutor&& obj, std::string&& member, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor)override
        {
            if (member == "addr") {
                resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
                resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                resultInfo.IsGlobal = false;
                resultInfo.Name = GenTempVarName();
                resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);

                switch (objInfo.ObjectTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO:
                    executor.attach(this, &CppObjectMemberGetProvider::ExecuteGetMemModifyInfoAddr);
                    break;
                }

                m_ObjInfo = objInfo;
                std::swap(m_Obj, obj);
                m_ResultInfo = resultInfo;
                return true;
            }
            else if (member == "type") {
                resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
                resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                resultInfo.IsGlobal = false;
                resultInfo.Name = GenTempVarName();
                resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);

                switch (objInfo.ObjectTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO:
                    executor.attach(this, &CppObjectMemberGetProvider::ExecuteGetMemModifyInfoType);
                    break;
                }

                m_ObjInfo = objInfo;
                std::swap(m_Obj, obj);
                m_ResultInfo = resultInfo;
                return true;
            }
            else if (member == "val") {
                resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
                resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                resultInfo.IsGlobal = false;
                resultInfo.Name = GenTempVarName();
                resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);

                switch (objInfo.ObjectTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO:
                    executor.attach(this, &CppObjectMemberGetProvider::ExecuteGetMemModifyInfoVal);
                    break;
                }

                m_ObjInfo = objInfo;
                std::swap(m_Obj, obj);
                m_ResultInfo = resultInfo;
                return true;
            }
            else if (member == "oldVal") {
                resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
                resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                resultInfo.IsGlobal = false;
                resultInfo.Name = GenTempVarName();
                resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);

                switch (objInfo.ObjectTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO:
                    executor.attach(this, &CppObjectMemberGetProvider::ExecuteGetMemModifyInfoOldVal);
                    break;
                }

                m_ObjInfo = objInfo;
                std::swap(m_Obj, obj);
                m_ResultInfo = resultInfo;
                return true;
            }
            else if (member == "size") {
                resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
                resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                resultInfo.IsGlobal = false;
                resultInfo.Name = GenTempVarName();
                resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);

                switch (objInfo.ObjectTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO:
                    executor.attach(this, &CppObjectMemberGetProvider::ExecuteGetMemModifyInfoSize);
                    break;
                }

                m_ObjInfo = objInfo;
                std::swap(m_Obj, obj);
                m_ResultInfo = resultInfo;
                return true;
            }
            else {
                std::stringstream ss;
                ss << "unknown property '" << member << "', line: " << data.GetLine();
                LogError(ss.str());
                executor = nullptr;
                return false;
            }
        }
    private:
        int ExecuteGetMemModifyInfoAddr(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            auto& ptr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            auto* pObj = static_cast<Core::Memory::MemoryModifyInfo*>(ptr.get());
            Brace::VarSetUInt64((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, 0);
            if (nullptr != pObj) {
                Brace::VarSetUInt64((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, pObj->addr.GetValue());
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteGetMemModifyInfoType(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            auto& ptr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            auto* pObj = static_cast<Core::Memory::MemoryModifyInfo*>(ptr.get());
            Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, 0);
            if (nullptr != pObj) {
                Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, pObj->type);
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteGetMemModifyInfoVal(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            auto& ptr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            auto* pObj = static_cast<Core::Memory::MemoryModifyInfo*>(ptr.get());
            Brace::VarSetUInt64((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, 0);
            if (nullptr != pObj) {
                uint64_t val = 0;
                switch (pObj->type) {
                case Core::Memory::MemoryModifyInfo::type_u8:
                    val = pObj->u8Val;
                    break;
                case Core::Memory::MemoryModifyInfo::type_u16:
                    val = pObj->u16Val;
                    break;
                case Core::Memory::MemoryModifyInfo::type_u32:
                    val = pObj->u32Val;
                    break;
                case Core::Memory::MemoryModifyInfo::type_u64:
                    val = pObj->u64Val;
                    break;
                }
                Brace::VarSetUInt64((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, val);
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteGetMemModifyInfoOldVal(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            auto& ptr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            auto* pObj = static_cast<Core::Memory::MemoryModifyInfo*>(ptr.get());
            Brace::VarSetUInt64((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, 0);
            if (nullptr != pObj) {
                uint64_t val = 0;
                switch (pObj->type) {
                case Core::Memory::MemoryModifyInfo::type_u8:
                    val = pObj->u8OldVal;
                    break;
                case Core::Memory::MemoryModifyInfo::type_u16:
                    val = pObj->u16OldVal;
                    break;
                case Core::Memory::MemoryModifyInfo::type_u32:
                    val = pObj->u32OldVal;
                    break;
                case Core::Memory::MemoryModifyInfo::type_u64:
                    val = pObj->u64OldVal;
                    break;
                }
                Brace::VarSetUInt64((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, val);
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteGetMemModifyInfoSize(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            auto& ptr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            auto* pObj = static_cast<Core::Memory::MemoryModifyInfo*>(ptr.get());
            Brace::VarSetUInt64((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, 0);
            if (nullptr != pObj) {
                Brace::VarSetUInt64((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, pObj->size);
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    protected:
        CppObjectMemberGetProvider(Brace::BraceScript& interpreter) :AbstractMemberGetApiProvider(interpreter), m_ObjInfo(), m_Obj(), m_ResultInfo(), m_ResultObjInfo(nullptr), m_Member()
        {}
    protected:
        Brace::OperandRuntimeInfo m_ObjInfo;
        Brace::BraceApiExecutor m_Obj;
        Brace::OperandRuntimeInfo m_ResultInfo;
        BraceObjectInfo* m_ResultObjInfo;
        std::string m_Member;
    };

    class StructMemberCallProvider final : public AbstractMemberCallApiProvider
    {
        friend class MemberCallExp;
    protected:
        virtual bool LoadMemberCall(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& objInfo, Brace::BraceApiExecutor&& obj, std::string&& member, std::vector<Brace::OperandLoadtimeInfo>&& argInfos, std::vector<Brace::BraceApiExecutor>&& args, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor)override
        {
            return false;
        }
    protected:
        StructMemberCallProvider(Brace::BraceScript& interpreter) :AbstractMemberCallApiProvider(interpreter), m_ObjInfo(), m_Obj(), m_ArgInfos(), m_Args(), m_ResultInfo(), m_Member()
        {}
    protected:
        Brace::OperandRuntimeInfo m_ObjInfo;
        Brace::BraceApiExecutor m_Obj;
        std::vector<Brace::OperandRuntimeInfo> m_ArgInfos;
        std::vector<Brace::BraceApiExecutor> m_Args;
        Brace::OperandRuntimeInfo m_ResultInfo;
        std::string m_Member;
    };
    class StructMemberSetProvider final : public AbstractMemberSetApiProvider
    {
        friend class MemberSetExp;
    protected:
        virtual bool LoadMemberSet(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& objInfo, Brace::BraceApiExecutor&& obj, std::string&& member, Brace::OperandLoadtimeInfo&& argInfo, Brace::BraceApiExecutor&& arg, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor)override
        {
            auto& fields = braceObjInfo.FieldTable.Fields;
            auto it = std::find_if(fields.begin(), fields.end(), [&](auto& v) { return v.Name == member; });
            if (it != fields.end()) {
                if ((Brace::IsStringType(it->Type.Type) && Brace::IsStringType(argInfo.Type)) || (!Brace::IsStringType(it->Type.Type) && CanAssign(it->Type.Type, it->Type.ObjectTypeId, argInfo.Type, argInfo.ObjectTypeId))) {
                    m_ObjInfo = objInfo;
                    std::swap(m_Obj, obj);
                    m_ArgInfo = argInfo;
                    std::swap(m_Arg, arg);
                    m_FieldInfo = *it;
                    executor.attach(this, &StructMemberSetProvider::Execute);

                    return true;
                }
            }
            std::stringstream ss;
            ss << "struct member " << member << " set error, line: " << data.GetLine();
            LogError(ss.str());
            executor = nullptr;
            return false;
        }
    private:
        int Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            if (!m_Arg.isNull())
                m_Arg(gvars, lvars);
            auto& optr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            auto* pObj = static_cast<StructObj*>(optr.get());
            if (nullptr != pObj) {
                char* p = reinterpret_cast<char*>(pObj->GetMemory());
                switch (m_FieldInfo.Type.Type) {
                case Brace::BRACE_DATA_TYPE_BOOL: {
                    bool v = Brace::VarGetBoolean((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex);
                    *(p + m_FieldInfo.Offset) = (v ? 1 : 0);
                }break;
                case Brace::BRACE_DATA_TYPE_INT8: {
                    int8_t v = static_cast<int8_t>(Brace::VarGetI64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex));
                    *reinterpret_cast<int8_t*>(p + m_FieldInfo.Offset) = v;
                }break;
                case Brace::BRACE_DATA_TYPE_UINT8: {
                    uint8_t v = static_cast<uint8_t>(Brace::VarGetU64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex));
                    *reinterpret_cast<uint8_t*>(p + m_FieldInfo.Offset) = v;
                }break;
                case Brace::BRACE_DATA_TYPE_INT16: {
                    int16_t v = static_cast<int16_t>(Brace::VarGetI64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex));
                    *reinterpret_cast<int16_t*>(p + m_FieldInfo.Offset) = v;
                }break;
                case Brace::BRACE_DATA_TYPE_UINT16: {
                    uint16_t v = static_cast<uint16_t>(Brace::VarGetU64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex));
                    *reinterpret_cast<uint16_t*>(p + m_FieldInfo.Offset) = v;
                }break;
                case Brace::BRACE_DATA_TYPE_INT32: {
                    int32_t v = static_cast<int32_t>(Brace::VarGetI64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex));
                    *reinterpret_cast<int32_t*>(p + m_FieldInfo.Offset) = v;
                }break;
                case Brace::BRACE_DATA_TYPE_UINT32: {
                    uint32_t v = static_cast<uint32_t>(Brace::VarGetU64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex));
                    *reinterpret_cast<uint32_t*>(p + m_FieldInfo.Offset) = v;
                }break;
                case Brace::BRACE_DATA_TYPE_INT64: {
                    int64_t v = Brace::VarGetI64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex);
                    *reinterpret_cast<int64_t*>(p + m_FieldInfo.Offset) = v;
                }break;
                case Brace::BRACE_DATA_TYPE_UINT64: {
                    uint64_t v = Brace::VarGetU64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex);
                    *reinterpret_cast<uint64_t*>(p + m_FieldInfo.Offset) = v;
                }break;
                case Brace::BRACE_DATA_TYPE_FLOAT: {
                    float v = static_cast<float>(Brace::VarGetF64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex));
                    *reinterpret_cast<float*>(p + m_FieldInfo.Offset) = v;
                }break;
                case Brace::BRACE_DATA_TYPE_DOUBLE: {
                    double v = Brace::VarGetF64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex);
                    *reinterpret_cast<double*>(p + m_FieldInfo.Offset) = v;
                }break;
                case Brace::BRACE_DATA_TYPE_STRING: {
                    std::string sv = Brace::VarGetString((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.VarIndex);
                    char* v = nullptr;
                    if (m_FieldInfo.IsPtr) {
                        v = *reinterpret_cast<char**>(p + m_FieldInfo.Offset);
                    }
                    else {
                        v = reinterpret_cast<char*>(p + m_FieldInfo.Offset);
                    }
                    size_t size = sv.length();
                    if (size > static_cast<size_t>(m_FieldInfo.Size))
                        size = static_cast<size_t>(m_FieldInfo.Size);
                    std::strncpy(v, sv.c_str(), size);
                    std::string* pStr = pObj->GetCachedStrField(m_FieldInfo.Offset);
                    if (nullptr == pStr)
                        pObj->CacheStrField(m_FieldInfo.Offset, std::string(v, size));
                    else
                        pStr->assign(sv, size);
                }break;
                case Brace::BRACE_DATA_TYPE_OBJECT: {
                    auto& ptr = Brace::VarGetObject((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.VarIndex);
                    if (ptr.get()) {
                        auto* pFieldInfo = m_FieldInfo.BraceObjInfo;
                        if (nullptr != pFieldInfo) {
                            if (pFieldInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STRUCT) {
                                void* v = nullptr;
                                if (m_FieldInfo.IsPtr) {
                                    v = *reinterpret_cast<void**>(p + m_FieldInfo.Offset);
                                }
                                else {
                                    v = reinterpret_cast<void*>(p + m_FieldInfo.Offset);
                                }
                                StructObj* pSrc = static_cast<StructObj*>(ptr.get());
                                std::memcpy(v, pSrc->GetMemory(), m_FieldInfo.Size);
                                auto* pExists = pObj->GetCachedObjField(m_FieldInfo.Offset);
                                if (nullptr == pExists) {
                                    StructObj* pWrap = new StructObj();
                                    pWrap->SetMemory(pFieldInfo, v);
                                    std::shared_ptr<void> sptr(pWrap);
                                    pObj->CacheObjField(m_FieldInfo.Offset, sptr);
                                }
                            }
                        }
                    }
                }break;
                }
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    protected:
        StructMemberSetProvider(Brace::BraceScript& interpreter) :AbstractMemberSetApiProvider(interpreter), m_ObjInfo(), m_Obj(), m_ArgInfo(), m_Arg(), m_FieldInfo()
        {}
    protected:
        Brace::OperandRuntimeInfo m_ObjInfo;
        Brace::BraceApiExecutor m_Obj;
        Brace::OperandRuntimeInfo m_ArgInfo;
        Brace::BraceApiExecutor m_Arg;
        FieldInfo m_FieldInfo;
    };
    class StructMemberGetProvider final : public AbstractMemberGetApiProvider
    {
        friend class MemberGetExp;
    protected:
        virtual bool LoadMemberGet(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& objInfo, Brace::BraceApiExecutor&& obj, std::string&& member, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor)override
        {
            if (member == "StructName") {
                resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
                resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                resultInfo.IsGlobal = false;
                resultInfo.Name = GenTempVarName();
                resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);

                executor.attach(this, &StructMemberGetProvider::ExecuteGetStructName);

                m_ObjInfo = objInfo;
                std::swap(m_Obj, obj);
                m_ResultInfo = resultInfo;
                return true;
            }
            else if (member == "MemAddr") {
                resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
                resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                resultInfo.IsGlobal = false;
                resultInfo.Name = GenTempVarName();
                resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);

                executor.attach(this, &StructMemberGetProvider::ExecuteGetMemoryAddr);

                m_ObjInfo = objInfo;
                std::swap(m_Obj, obj);
                m_ResultInfo = resultInfo;
                return true;
            }
            else {
                auto& fields = braceObjInfo.FieldTable.Fields;
                auto it = std::find_if(fields.begin(), fields.end(), [&](auto& v) { return v.Name == member; });
                if (it != fields.end()) {
                    resultInfo.Type = it->Type.Type;
                    resultInfo.ObjectTypeId = it->Type.ObjectTypeId;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);

                    m_ObjInfo = objInfo;
                    std::swap(m_Obj, obj);
                    m_FieldInfo = *it;
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &StructMemberGetProvider::Execute);

                    return true;
                }
            }
            std::stringstream ss;
            ss << "struct member " << member << " get error, line: " << data.GetLine();
            LogError(ss.str());
            executor = nullptr;
            return false;
        }
    private:
        int ExecuteGetStructName(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            auto& ptr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            auto* pObj = static_cast<StructObj*>(ptr.get());
            if (nullptr != pObj) {
                auto* pInfo = pObj->GetObjectInfo();
                if (nullptr != pInfo) {
                    Brace::VarSetString((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, pInfo->TypeName);
                }
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteGetMemoryAddr(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            auto& ptr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            auto* pObj = static_cast<StructObj*>(ptr.get());
            if (nullptr != pObj) {
                Brace::VarSetUInt64((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, reinterpret_cast<uint64_t>(pObj));
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            auto& optr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            auto* pObj = static_cast<StructObj*>(optr.get());
            if (nullptr != pObj) {
                char* p = reinterpret_cast<char*>(pObj->GetMemory());
                switch (m_FieldInfo.Type.Type) {
                case Brace::BRACE_DATA_TYPE_BOOL: {
                    bool v = *(p + m_FieldInfo.Offset) != 0;
                    Brace::VarSetBool((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }break;
                case Brace::BRACE_DATA_TYPE_INT8: {
                    int8_t v = *reinterpret_cast<int8_t*>(p + m_FieldInfo.Offset);
                    Brace::VarSetInt8((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }break;
                case Brace::BRACE_DATA_TYPE_UINT8: {
                    uint8_t v = *reinterpret_cast<uint8_t*>(p + m_FieldInfo.Offset);
                    Brace::VarSetUInt8((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }break;
                case Brace::BRACE_DATA_TYPE_INT16: {
                    int16_t v = *reinterpret_cast<int16_t*>(p + m_FieldInfo.Offset);
                    Brace::VarSetInt16((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }break;
                case Brace::BRACE_DATA_TYPE_UINT16: {
                    uint16_t v = *reinterpret_cast<uint16_t*>(p + m_FieldInfo.Offset);
                    Brace::VarSetUInt16((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }break;
                case Brace::BRACE_DATA_TYPE_INT32: {
                    int32_t v = *reinterpret_cast<int32_t*>(p + m_FieldInfo.Offset);
                    Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }break;
                case Brace::BRACE_DATA_TYPE_UINT32: {
                    uint32_t v = *reinterpret_cast<uint32_t*>(p + m_FieldInfo.Offset);
                    Brace::VarSetUInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }break;
                case Brace::BRACE_DATA_TYPE_INT64: {
                    int64_t v = *reinterpret_cast<int64_t*>(p + m_FieldInfo.Offset);
                    Brace::VarSetInt64((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }break;
                case Brace::BRACE_DATA_TYPE_UINT64: {
                    uint64_t v = *reinterpret_cast<uint64_t*>(p + m_FieldInfo.Offset);
                    Brace::VarSetUInt64((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }break;
                case Brace::BRACE_DATA_TYPE_FLOAT: {
                    float v = *reinterpret_cast<float*>(p + m_FieldInfo.Offset);
                    Brace::VarSetFloat((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }break;
                case Brace::BRACE_DATA_TYPE_DOUBLE: {
                    double v = *reinterpret_cast<double*>(p + m_FieldInfo.Offset);
                    Brace::VarSetDouble((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }break;
                case Brace::BRACE_DATA_TYPE_STRING: {
                    auto* ptr = pObj->GetCachedStrField(m_FieldInfo.Offset);
                    if (ptr) {
                        Brace::VarSetString((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, *ptr);
                    }
                    else {
                        const char* v = nullptr;
                        if (m_FieldInfo.IsPtr) {
                            v = *reinterpret_cast<const char**>(p + m_FieldInfo.Offset);
                        }
                        else {
                            v = reinterpret_cast<const char*>(p + m_FieldInfo.Offset);
                        }
                        std::string str(v, m_FieldInfo.Size);
                        Brace::VarSetString((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, str);
                    }
                }break;
                case Brace::BRACE_DATA_TYPE_OBJECT: {
                    auto* ptr = pObj->GetCachedObjField(m_FieldInfo.Offset);
                    if (ptr) {
                        Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, *ptr);
                    }
                    else {
                        auto* pFieldInfo = m_FieldInfo.BraceObjInfo;
                        if (nullptr != pFieldInfo) {
                            if (pFieldInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STRUCT) {
                                void* v = nullptr;
                                if (m_FieldInfo.IsPtr) {
                                    v = *reinterpret_cast<void**>(p + m_FieldInfo.Offset);
                                }
                                else {
                                    v = reinterpret_cast<void*>(p + m_FieldInfo.Offset);
                                }
                                StructObj* pWrap = new StructObj();
                                pWrap->SetMemory(pFieldInfo, v);
                                std::shared_ptr<void> sptr(pWrap);
                                pObj->CacheObjField(m_FieldInfo.Offset, sptr);
                                Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, sptr);
                            }
                        }
                    }
                }break;
                }
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    protected:
        StructMemberGetProvider(Brace::BraceScript& interpreter) :AbstractMemberGetApiProvider(interpreter), m_ObjInfo(), m_Obj(), m_ResultInfo(), m_FieldInfo()
        {}
    protected:
        Brace::OperandRuntimeInfo m_ObjInfo;
        Brace::BraceApiExecutor m_Obj;
        Brace::OperandRuntimeInfo m_ResultInfo;
        FieldInfo m_FieldInfo;
    };

    class StructExp final : public Brace::AbstractBraceApi
    {
    public:
        StructExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter)
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //struct(name){ a : int32; b : int32; ... };
            if (data.IsHighOrder()) {
                bool ret = true;
                auto& callData = data.GetLowerOrderFunction();
                const std::string& name = callData.GetParamId(0);
                int structId = g_ObjectInfoMgr.GetObjectTypeId(name);
                if (structId == Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN) {
                    structId = g_ObjectInfoMgr.AddNewObjectTypeId(name);
                }
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(structId);
                if (nullptr == pInfo) {
                    pInfo = g_ObjectInfoMgr.AddBraceObjectInfo(structId, BRACE_OBJECT_CATEGORY_STRUCT, name);
                }
                auto& fieldTable = pInfo->FieldTable;
                fieldTable.Size = 0;
                fieldTable.Fields.clear();
                for (int ix = 0; ix < data.GetParamNum(); ++ix) {
                    auto* pSyntax = data.GetParam(ix);
                    if (pSyntax->GetSyntaxType() == DslData::ISyntaxComponent::TYPE_FUNCTION && pSyntax->GetId() == ":") {
                        auto& funcData = *static_cast<DslData::FunctionData*>(pSyntax);
                        if (funcData.GetParamNum() == 2) {
                            const std::string& fname = funcData.GetParamId(0);
                            const std::string& typeId = funcData.GetParamId(1);
                            int paramSyntaxType = funcData.GetParam(0)->GetSyntaxType();
                            if (paramSyntaxType == DslData::ISyntaxComponent::TYPE_FUNCTION && (typeId == "chararray" || typeId == "chararrayptr")) {
                                auto* paramFuncData = static_cast<DslData::FunctionData*>(funcData.GetParam(1));
                                int size = std::stoi(paramFuncData->GetParamId(0), nullptr, 0);
                                FieldInfo fi{};
                                fi.Name = fname;
                                fi.IsPtr = typeId == "chararrayptr";
                                fi.Type.Type = Brace::BRACE_DATA_TYPE_STRING;
                                fi.Type.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                                fi.Offset = fieldTable.Size;
                                fi.Size = size;
                                fieldTable.Size += (fi.IsPtr ? static_cast<int>(sizeof(void*)) : fi.Size);
                                fieldTable.Fields.push_back(std::move(fi));
                            }
                            else {
                                auto tinfo = ParseParamTypeInfo(*funcData.GetParam(1));
                                FieldInfo fi{};
                                fi.Name = fname;
                                fi.IsPtr = tinfo.IsRef;
                                fi.Type.Type = tinfo.Type;
                                fi.Type.ObjectTypeId = tinfo.ObjectTypeId;
                                fi.Offset = fieldTable.Size;
                                if (!tinfo.IsRef && tinfo.Type == Brace::BRACE_DATA_TYPE_OBJECT) {
                                    auto* pFieldTypeInfo = g_ObjectInfoMgr.GetBraceObjectInfo(tinfo.ObjectTypeId);
                                    fi.BraceObjInfo = pFieldTypeInfo;
                                    if (nullptr != pFieldTypeInfo && pFieldTypeInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STRUCT) {
                                        fi.Size = pFieldTypeInfo->FieldTable.Size;
                                    }
                                    else {
                                        ret = false;
                                    }
                                }
                                else {
                                    fi.Size = (tinfo.IsRef ? static_cast<int>(sizeof(void*)) : Brace::GetDataTypeSize(tinfo.Type));
                                }
                                fieldTable.Size += fi.Size;
                                fieldTable.Fields.push_back(std::move(fi));
                            }
                        }
                    }
                }
                executor = nullptr;
                return ret;
            }
            std::stringstream ss;
            ss << "Illegal struct syntax, line: " << data.GetLine();
            LogError(ss.str());
            executor = nullptr;
            return false;
        }
    };
    class NewStructExp final : public Brace::AbstractBraceApi
    {
    public:
        NewStructExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter), m_pObjectInfo(nullptr)
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& funcData, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            if (funcData.GetParamNum() == 1) {
                const std::string& id = funcData.GetParamId(0);
                int objTypeId = g_ObjectInfoMgr.GetObjectTypeId(id);
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
                if (nullptr != pInfo && pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STRUCT) {
                    m_pObjectInfo = pInfo;
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                    resultInfo.ObjectTypeId = objTypeId;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;

                    executor.attach(this, &NewStructExp::Execute);
                    return true;
                }
            }
            //error
            std::stringstream ss;
            ss << "BraceScript error, " << funcData.GetId() << " line " << funcData.GetLine();
            LogError(ss.str());
            return false;
        }
    private:
        int Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            auto* pObj = new StructObj();
            pObj->AllocMemory(m_pObjectInfo);
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(pObj));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    private:
        const BraceObjectInfo* m_pObjectInfo;
        Brace::OperandRuntimeInfo m_ResultInfo;
    };
    class ReInterpretAsExp final : public Brace::AbstractBraceApi
    {
    public:
        ReInterpretAsExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter), m_pObjectInfo(nullptr)
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& funcData, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            if (funcData.GetParamNum() == 2) {
                const std::string& id = funcData.GetParamId(1);
                int objTypeId = g_ObjectInfoMgr.GetObjectTypeId(id);
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
                if (nullptr != pInfo && pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STRUCT) {
                    Brace::OperandLoadtimeInfo argInfo;
                    m_Arg = LoadHelper(*funcData.GetParam(0), argInfo);
                    m_ArgInfo = argInfo;
                    if (Brace::IsSignedType(argInfo.Type) || Brace::IsUnsignedType(argInfo.Type)) {
                        m_pObjectInfo = pInfo;
                        resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                        resultInfo.ObjectTypeId = objTypeId;
                        resultInfo.Name = GenTempVarName();
                        resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                        m_ResultInfo = resultInfo;

                        executor.attach(this, &ReInterpretAsExp::Execute);
                        return true;
                    }
                }
            }
            //error
            std::stringstream ss;
            ss << "BraceScript error, " << funcData.GetId() << " line " << funcData.GetLine();
            LogError(ss.str());
            return false;
        }
    private:
        int Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Arg.isNull())
                m_Arg(gvars, lvars);
            uint64_t v = Brace::VarGetU64((m_ArgInfo.IsGlobal ? gvars : lvars), m_ArgInfo.Type, m_ArgInfo.VarIndex);
            auto* pObj = new StructObj();
            pObj->SetMemory(m_pObjectInfo, reinterpret_cast<void*>(v));
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(pObj));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    private:
        Brace::OperandRuntimeInfo m_ArgInfo;
        Brace::BraceApiExecutor m_Arg;
        const BraceObjectInfo* m_pObjectInfo;
        Brace::OperandRuntimeInfo m_ResultInfo;
    };

    /// Internally fixed collection objects use switch-case rather than virtual-function-dispatch, simply because switch-case may require less code.
    /// and virtual-function-dispatch must define one class for each API, and dozens may be required.
    class ArrayHashtableMemberCallProvider final : public AbstractMemberCallApiProvider
    {
        friend class MemberCallExp;
    protected:
        virtual bool LoadMemberCall(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& objInfo, Brace::BraceApiExecutor&& obj, std::string&& member, std::vector<Brace::OperandLoadtimeInfo>&& argInfos, std::vector<Brace::BraceApiExecutor>&& args, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            int num = data.GetParamNum();
            if (num < 2)
                return false;
            std::swap(m_Obj, obj);
            m_ObjInfo = objInfo;
            std::swap(m_Member, member);
            std::swap(m_Args, args);
            Brace::OperandLoadtimeInfo firstArgInfo;
            bool first = true;
            for (auto& argInfo : argInfos) {
                m_ArgInfos.push_back(argInfo);
                if (first) {
                    first = false;
                    firstArgInfo = std::move(argInfo);
                }
            }
            bool isArray = false;
            bool isHashtable = false;
            bool isIntKey = true;
            int dataType = Brace::BRACE_DATA_TYPE_OBJECT;
            int objTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            switch (objInfo.ObjectTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY:
                isArray = true;
                dataType = Brace::BRACE_DATA_TYPE_STRING;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY:
                isArray = true;
                dataType = Brace::BRACE_DATA_TYPE_INT64;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY:
                isArray = true;
                dataType = Brace::BRACE_DATA_TYPE_DOUBLE;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY:
                isArray = true;
                dataType = Brace::BRACE_DATA_TYPE_BOOL;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE:
                isHashtable = true;
                isIntKey = false;
                dataType = Brace::BRACE_DATA_TYPE_STRING;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE:
                isHashtable = true;
                isIntKey = false;
                dataType = Brace::BRACE_DATA_TYPE_INT64;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE:
                isHashtable = true;
                isIntKey = false;
                dataType = Brace::BRACE_DATA_TYPE_DOUBLE;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE:
                isHashtable = true;
                isIntKey = false;
                dataType = Brace::BRACE_DATA_TYPE_BOOL;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE:
                isHashtable = true;
                dataType = Brace::BRACE_DATA_TYPE_STRING;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE:
                isHashtable = true;
                dataType = Brace::BRACE_DATA_TYPE_INT64;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE:
                isHashtable = true;
                dataType = Brace::BRACE_DATA_TYPE_DOUBLE;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE:
                isHashtable = true;
                dataType = Brace::BRACE_DATA_TYPE_BOOL;
                break;
            }
            switch (braceObjInfo.ObjectCategory) {
            case BRACE_OBJECT_CATEGORY_OBJ_ARRAY:
                isArray = true;
                objTypeId = braceObjInfo.GetTypeParamObjTypeId(0);
                break;
            case BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE:
                isHashtable = true;
                objTypeId = braceObjInfo.GetTypeParamObjTypeId(1);
                break;
            case BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE:
                isHashtable = true;
                isIntKey = false;
                objTypeId = braceObjInfo.GetTypeParamObjTypeId(1);
                break;
            }
            if (isArray) {
                if (m_Member == "resize") {
                    bool good = false;
                    if (m_ArgInfos.size() == 1) {
                        auto& argInfo = m_ArgInfos[0];
                        if (argInfo.Type >= Brace::BRACE_DATA_TYPE_INT8 && argInfo.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                            good = true;
                        }
                    }
                    if (good) {
                        resultInfo = Brace::OperandLoadtimeInfo();
                        m_ResultInfo = resultInfo;
                        executor.attach(this, &ArrayHashtableMemberCallProvider::ExecuteArrayResize);
                        return true;
                    }
                    else {
                        std::stringstream ss;
                        ss << "Array.resize's param dismatch, line: " << data.GetLine();
                        LogError(ss.str());
                        executor = nullptr;
                        return false;
                    }
                }
                else if (m_Member == "push") {
                    bool good = false;
                    if (m_ArgInfos.size() == 1) {
                        if (CanAssign(dataType, objTypeId, firstArgInfo.Type, firstArgInfo.ObjectTypeId)) {
                            good = true;
                        }
                    }
                    if (good) {
                        resultInfo = Brace::OperandLoadtimeInfo();
                        m_ResultInfo = resultInfo;
                        executor.attach(this, &ArrayHashtableMemberCallProvider::ExecuteArrayPush);
                        return true;
                    }
                    else {
                        std::stringstream ss;
                        ss << "Array.push's param dismatch, line: " << data.GetLine();
                        LogError(ss.str());
                        executor = nullptr;
                        return false;
                    }
                }
                else if (m_Member == "pop") {
                    resultInfo.Type = dataType;
                    resultInfo.ObjectTypeId = objTypeId;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &ArrayHashtableMemberCallProvider::ExecuteArrayPop);
                    return true;
                }
                else if (m_Member == "insert") {
                    bool good = false;
                    if (m_ArgInfos.size() == 2) {
                        auto& keyArgInfo = m_ArgInfos[0];
                        auto& valArgInfo = m_ArgInfos[1];
                        if (keyArgInfo.Type >= Brace::BRACE_DATA_TYPE_INT8 && keyArgInfo.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                            if (CanAssign(dataType, objTypeId, valArgInfo.Type, valArgInfo.ObjectTypeId)) {
                                good = true;
                            }
                        }
                    }
                    if (good) {
                        resultInfo = Brace::OperandLoadtimeInfo();
                        m_ResultInfo = resultInfo;
                        executor.attach(this, &ArrayHashtableMemberCallProvider::ExecuteArrayInsert);
                        return true;
                    }
                    else {
                        std::stringstream ss;
                        ss << "Array.insert's param dismatch, line: " << data.GetLine();
                        LogError(ss.str());
                        executor = nullptr;
                        return false;
                    }
                }
                else if (m_Member == "remove") {
                    bool good = false;
                    if (m_ArgInfos.size() == 1) {
                        auto& argInfo = m_ArgInfos[0];
                        if (argInfo.Type >= Brace::BRACE_DATA_TYPE_INT8 && argInfo.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                            good = true;
                        }
                    }
                    if (good) {
                        resultInfo = Brace::OperandLoadtimeInfo();
                        m_ResultInfo = resultInfo;
                        executor.attach(this, &ArrayHashtableMemberCallProvider::ExecuteArrayRemove);
                        return true;
                    }
                    else {
                        std::stringstream ss;
                        ss << "Array.remove's param dismatch, line: " << data.GetLine();
                        LogError(ss.str());
                        executor = nullptr;
                        return false;
                    }
                }
                else if (m_Member == "clear") {
                    resultInfo = Brace::OperandLoadtimeInfo();
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &ArrayHashtableMemberCallProvider::ExecuteArrayClear);
                    return true;
                }
            }
            else if (isHashtable) {
                if (m_Member == "contains") {
                    bool good = false;
                    if (m_ArgInfos.size() == 1) {
                        auto& argInfo = m_ArgInfos[0];
                        if ((isIntKey && argInfo.Type >= Brace::BRACE_DATA_TYPE_INT8 && argInfo.Type <= Brace::BRACE_DATA_TYPE_UINT64) ||
                            (!isIntKey && argInfo.Type == Brace::BRACE_DATA_TYPE_STRING)) {
                            good = true;
                        }
                    }
                    if (good) {
                        resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
                        resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                        resultInfo.Name = GenTempVarName();
                        resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                        m_ResultInfo = resultInfo;
                        executor.attach(this, &ArrayHashtableMemberCallProvider::ExecuteHashtableContains);
                        return true;
                    }
                    else {
                        std::stringstream ss;
                        ss << "Hashtable.contains's param dismatch, line: " << data.GetLine();
                        LogError(ss.str());
                        executor = nullptr;
                        return false;
                    }
                }
                else if (m_Member == "add") {
                    bool good = false;
                    if (m_ArgInfos.size() == 2) {
                        auto& keyArgInfo = m_ArgInfos[0];
                        auto& valArgInfo = m_ArgInfos[1];
                        if ((isIntKey && keyArgInfo.Type >= Brace::BRACE_DATA_TYPE_INT8 && keyArgInfo.Type <= Brace::BRACE_DATA_TYPE_UINT64) ||
                            (!isIntKey && keyArgInfo.Type == Brace::BRACE_DATA_TYPE_STRING)) {
                            if (CanAssign(dataType, objTypeId, valArgInfo.Type, valArgInfo.ObjectTypeId)) {
                                good = true;
                            }
                        }
                    }
                    if (good) {
                        resultInfo = Brace::OperandLoadtimeInfo();
                        m_ResultInfo = resultInfo;
                        executor.attach(this, &ArrayHashtableMemberCallProvider::ExecuteHashtableAdd);
                        return true;
                    }
                    else {
                        std::stringstream ss;
                        ss << "Hashtable.add's param dismatch, line: " << data.GetLine();
                        LogError(ss.str());
                        executor = nullptr;
                        return false;
                    }
                }
                else if (m_Member == "remove") {
                    bool good = false;
                    if (m_ArgInfos.size() == 1) {
                        auto& argInfo = m_ArgInfos[0];
                        if ((isIntKey && argInfo.Type >= Brace::BRACE_DATA_TYPE_INT8 && argInfo.Type <= Brace::BRACE_DATA_TYPE_UINT64) ||
                            (!isIntKey && argInfo.Type == Brace::BRACE_DATA_TYPE_STRING)) {
                            good = true;
                        }
                    }
                    if (good) {
                        resultInfo = Brace::OperandLoadtimeInfo();
                        m_ResultInfo = resultInfo;
                        executor.attach(this, &ArrayHashtableMemberCallProvider::ExecuteHashtableRemove);
                        return true;
                    }
                    else {
                        std::stringstream ss;
                        ss << "Hashtable.remove's param dismatch, line: " << data.GetLine();
                        LogError(ss.str());
                        executor = nullptr;
                        return false;
                    }
                }
                else if (m_Member == "clear") {
                    resultInfo = Brace::OperandLoadtimeInfo();
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &ArrayHashtableMemberCallProvider::ExecuteHashtableClear);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "Unknown member " << m_Member << " line: " << data.GetLine();
            LogError(ss.str());
            executor = nullptr;
            return false;
        }
    private:
        int ExecuteArrayResize(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            for (auto&& _arg : m_Args) {
                if (!_arg.isNull())
                    _arg(gvars, lvars);
            }
            auto& arr = m_ObjInfo;
            auto& arg = m_ArgInfos[0];
            const std::shared_ptr<void>& p = Brace::VarGetObject((arr.IsGlobal ? gvars : lvars), arr.VarIndex);
            switch (arr.ObjectTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<bool>*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    pArr->resize(varg);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<int64_t>*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    pArr->resize(varg);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<double>*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    pArr->resize(varg);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<std::string>*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    pArr->resize(varg);
                }
            }break;
            default: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ObjectArray*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    pArr->resize(varg);
                }
            }break;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteArrayPush(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            for (auto&& _arg : m_Args) {
                if (!_arg.isNull())
                    _arg(gvars, lvars);
            }
            auto& arr = m_ObjInfo;
            auto& arg = m_ArgInfos[0];
            const std::shared_ptr<void>& p = Brace::VarGetObject((arr.IsGlobal ? gvars : lvars), arr.VarIndex);
            switch (arr.ObjectTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<bool>*>(ptr);
                    bool varg = Brace::VarGetBoolean((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    pArr->push_back(varg);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<int64_t>*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    pArr->push_back(varg);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<double>*>(ptr);
                    double varg = Brace::VarGetF64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    pArr->push_back(varg);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<std::string>*>(ptr);
                    std::string varg = Brace::VarGetStr((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    pArr->push_back(std::move(varg));
                }
            }break;
            default: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ObjectArray*>(ptr);
                    auto varg = Brace::VarGetObject((arg.IsGlobal ? gvars : lvars), arg.VarIndex);
                    pArr->push_back(varg);
                }
            }break;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteArrayPop(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            auto& arr = m_ObjInfo;
            const std::shared_ptr<void>& p = Brace::VarGetObject((arr.IsGlobal ? gvars : lvars), arr.VarIndex);
            switch (arr.ObjectTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<bool>*>(ptr);
                    bool v = pArr->back();
                    Brace::VarSetBool((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                    pArr->pop_back();
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<int64_t>*>(ptr);
                    int64_t v = pArr->back();
                    Brace::VarSetInt64((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                    pArr->pop_back();
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<double>*>(ptr);
                    double v = pArr->back();
                    Brace::VarSetDouble((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                    pArr->pop_back();
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<std::string>*>(ptr);
                    const std::string& v = pArr->back();
                    Brace::VarSetString((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                    pArr->pop_back();
                }
            }break;
            default: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ObjectArray*>(ptr);
                    auto v = pArr->back();
                    Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                    pArr->pop_back();
                }
            }break;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteArrayInsert(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            for (auto&& _arg : m_Args) {
                if (!_arg.isNull())
                    _arg(gvars, lvars);
            }
            auto& arr = m_ObjInfo;
            auto& arg = m_ArgInfos[0];
            auto& val = m_ArgInfos[1];
            const std::shared_ptr<void>& p = Brace::VarGetObject((arr.IsGlobal ? gvars : lvars), arr.VarIndex);
            switch (arr.ObjectTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<bool>*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    bool v = Brace::VarGetBoolean((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
                    VectorInsert((*pArr), varg, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<int64_t>*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    int64_t v = Brace::VarGetI64((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
                    VectorInsert((*pArr), varg, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<double>*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    double v = Brace::VarGetF64((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
                    VectorInsert((*pArr), varg, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<std::string>*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    std::string v = Brace::VarGetStr((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
                    VectorInsert((*pArr), varg, std::move(v));
                }
            }break;
            default: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ObjectArray*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    auto v = Brace::VarGetObject((val.IsGlobal ? gvars : lvars), val.VarIndex);
                    VectorInsert((*pArr), varg, v);
                }
            }break;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteArrayRemove(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            for (auto&& _arg : m_Args) {
                if (!_arg.isNull())
                    _arg(gvars, lvars);
            }
            auto& arr = m_ObjInfo;
            auto& arg = m_ArgInfos[0];
            const std::shared_ptr<void>& p = Brace::VarGetObject((arr.IsGlobal ? gvars : lvars), arr.VarIndex);
            switch (arr.ObjectTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<bool>*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    VectorErase((*pArr), varg);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<int64_t>*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    VectorErase((*pArr), varg);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<double>*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    VectorErase((*pArr), varg);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<std::string>*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    VectorErase((*pArr), varg);
                }
            }break;
            default: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ObjectArray*>(ptr);
                    int64_t varg = Brace::VarGetI64((arg.IsGlobal ? gvars : lvars), arg.Type, arg.VarIndex);
                    VectorErase((*pArr), varg);
                }
            }break;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteArrayClear(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            auto& arr = m_ObjInfo;
            const std::shared_ptr<void>& p = Brace::VarGetObject((arr.IsGlobal ? gvars : lvars), arr.VarIndex);
            switch (arr.ObjectTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<bool>*>(ptr);
                    pArr->clear();
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<int64_t>*>(ptr);
                    pArr->clear();
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<double>*>(ptr);
                    pArr->clear();
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<std::string>*>(ptr);
                    pArr->clear();
                }
            }break;
            default: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ObjectArray*>(ptr);
                    pArr->clear();
                }
            }break;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteHashtableContains(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            for (auto&& _arg : m_Args) {
                if (!_arg.isNull())
                    _arg(gvars, lvars);
            }
            auto& hash = m_ObjInfo;
            auto& ix = m_ArgInfos[0];
            const std::shared_ptr<void>& p = Brace::VarGetObject((hash.IsGlobal ? gvars : lvars), hash.VarIndex);
            int objTypeId = hash.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, std::string>*>(ptr);
                    bool v = HashtableContains((*pHashtable), vix);
                    Brace::VarSetBool((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, int64_t>*>(ptr);
                    bool v = HashtableContains((*pHashtable), vix);
                    Brace::VarSetBool((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, double>*>(ptr);
                    bool v = HashtableContains((*pHashtable), vix);
                    Brace::VarSetBool((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, bool>*>(ptr);
                    bool v = HashtableContains((*pHashtable), vix);
                    Brace::VarSetBool((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, std::string>*>(ptr);
                    bool v = HashtableContains((*pHashtable), vix);
                    Brace::VarSetBool((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, int64_t>*>(ptr);
                    bool v = HashtableContains((*pHashtable), vix);
                    Brace::VarSetBool((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, double>*>(ptr);
                    bool v = HashtableContains((*pHashtable), vix);
                    Brace::VarSetBool((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, bool>*>(ptr);
                    bool v = HashtableContains((*pHashtable), vix);
                    Brace::VarSetBool((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                }
            }break;
            default: {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
                if (nullptr != pInfo) {
                    if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE) {
                        auto* ptr = p.get();
                        if (nullptr != ptr) {
                            int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                            auto* pHashtable = static_cast<IntObjHashtable*>(ptr);
                            bool v = HashtableContains((*pHashtable), vix);
                            Brace::VarSetBool((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                        }
                    }
                    else if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE) {
                        auto* ptr = p.get();
                        if (nullptr != ptr) {
                            std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                            auto* pHashtable = static_cast<StrObjHashtable*>(ptr);
                            bool v = HashtableContains((*pHashtable), vix);
                            Brace::VarSetBool((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, v);
                        }
                    }
                }
            }break;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteHashtableAdd(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            for (auto&& _arg : m_Args) {
                if (!_arg.isNull())
                    _arg(gvars, lvars);
            }
            auto& hash = m_ObjInfo;
            auto& ix = m_ArgInfos[0];
            auto& val = m_ArgInfos[1];
            const std::shared_ptr<void>& p = Brace::VarGetObject((hash.IsGlobal ? gvars : lvars), hash.VarIndex);
            int objTypeId = hash.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE: {
                const std::string& v = Brace::VarGetString((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, std::string>*>(ptr);
                    (*pHashtable)[vix] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                int64_t v = Brace::VarGetInt64((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, int64_t>*>(ptr);
                    (*pHashtable)[vix] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                double v = Brace::VarGetDouble((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, double>*>(ptr);
                    (*pHashtable)[vix] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE: {
                bool v = Brace::VarGetBool((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, bool>*>(ptr);
                    (*pHashtable)[vix] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE: {
                const std::string& v = Brace::VarGetString((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, std::string>*>(ptr);
                    (*pHashtable)[static_cast<size_t>(vix)] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                int64_t v = Brace::VarGetInt64((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, int64_t>*>(ptr);
                    (*pHashtable)[static_cast<size_t>(vix)] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                double v = Brace::VarGetDouble((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, double>*>(ptr);
                    (*pHashtable)[static_cast<size_t>(vix)] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE: {
                bool v = Brace::VarGetBool((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, bool>*>(ptr);
                    (*pHashtable)[static_cast<size_t>(vix)] = v;
                }
            }break;
            default: {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
                if (nullptr != pInfo) {
                    if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE) {
                        auto& v = Brace::VarGetObject((val.IsGlobal ? gvars : lvars), val.VarIndex);
                        auto* ptr = p.get();
                        if (nullptr != ptr) {
                            int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                            auto* pHashtable = static_cast<IntObjHashtable*>(ptr);
                            (*pHashtable)[static_cast<size_t>(vix)] = v;
                        }
                    }
                    else if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE) {
                        auto& v = Brace::VarGetObject((val.IsGlobal ? gvars : lvars), val.VarIndex);
                        auto* ptr = p.get();
                        if (nullptr != ptr) {
                            std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                            auto* pHashtable = static_cast<StrObjHashtable*>(ptr);
                            (*pHashtable)[vix] = v;
                        }
                    }
                }
            }break;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteHashtableRemove(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            for (auto&& _arg : m_Args) {
                if (!_arg.isNull())
                    _arg(gvars, lvars);
            }
            auto& hash = m_ObjInfo;
            auto& ix = m_ArgInfos[0];
            const std::shared_ptr<void>& p = Brace::VarGetObject((hash.IsGlobal ? gvars : lvars), hash.VarIndex);
            int objTypeId = hash.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, std::string>*>(ptr);
                    pHashtable->erase(vix);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, int64_t>*>(ptr);
                    pHashtable->erase(vix);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, double>*>(ptr);
                    pHashtable->erase(vix);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, bool>*>(ptr);
                    pHashtable->erase(vix);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, std::string>*>(ptr);
                    pHashtable->erase(vix);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, int64_t>*>(ptr);
                    pHashtable->erase(vix);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, double>*>(ptr);
                    pHashtable->erase(vix);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, bool>*>(ptr);
                    pHashtable->erase(vix);
                }
            }break;
            default: {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
                if (nullptr != pInfo) {
                    if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE) {
                        auto* ptr = p.get();
                        if (nullptr != ptr) {
                            int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                            auto* pHashtable = static_cast<IntObjHashtable*>(ptr);
                            pHashtable->erase(vix);
                        }
                    }
                    else if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE) {
                        auto* ptr = p.get();
                        if (nullptr != ptr) {
                            std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                            auto* pHashtable = static_cast<StrObjHashtable*>(ptr);
                            pHashtable->erase(vix);
                        }
                    }
                }
            }break;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteHashtableClear(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            auto& hash = m_ObjInfo;
            const std::shared_ptr<void>& p = Brace::VarGetObject((hash.IsGlobal ? gvars : lvars), hash.VarIndex);
            int objTypeId = hash.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<std::string, std::string>*>(ptr);
                    pHashtable->clear();
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<std::string, int64_t>*>(ptr);
                    pHashtable->clear();
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<std::string, double>*>(ptr);
                    pHashtable->clear();
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<std::string, bool>*>(ptr);
                    pHashtable->clear();
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<int64_t, std::string>*>(ptr);
                    pHashtable->clear();
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<int64_t, int64_t>*>(ptr);
                    pHashtable->clear();
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<int64_t, double>*>(ptr);
                    pHashtable->clear();
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<int64_t, bool>*>(ptr);
                    pHashtable->clear();
                }
            }break;
            default: {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
                if (nullptr != pInfo) {
                    if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE) {
                        auto* ptr = p.get();
                        if (nullptr != ptr) {
                            auto* pHashtable = static_cast<IntObjHashtable*>(ptr);
                            pHashtable->clear();
                        }
                    }
                    else if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE) {
                        auto* ptr = p.get();
                        if (nullptr != ptr) {
                            auto* pHashtable = static_cast<StrObjHashtable*>(ptr);
                            pHashtable->clear();
                        }
                    }
                }
            }break;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    private:
        ArrayHashtableMemberCallProvider(Brace::BraceScript& interpreter) :AbstractMemberCallApiProvider(interpreter), m_Obj(), m_ObjInfo(), m_Member(), m_Args(), m_ArgInfos(), m_ResultInfo()
        {
        }
    private:
        Brace::BraceApiExecutor m_Obj;
        Brace::OperandRuntimeInfo m_ObjInfo;
        std::string m_Member;
        std::vector<Brace::BraceApiExecutor> m_Args;
        std::vector<Brace::OperandRuntimeInfo> m_ArgInfos;
        Brace::OperandRuntimeInfo m_ResultInfo;
    private:
        template<typename VectorT, typename ValT>
        static inline void VectorInsert(VectorT& vr, int64_t pos, const ValT& val)
        {
            auto it = vr.begin();
            std::advance(it, pos);
            if (it != vr.end())
                vr.insert(it, val);
            else
                vr.push_back(val);
        }
        template<typename VectorT, typename ValT>
        static inline void VectorInsert(VectorT& vr, int64_t pos, ValT&& val)
        {
            auto it = vr.begin();
            std::advance(it, pos);
            if (it != vr.end())
                vr.insert(it, std::move(val));
            else
                vr.push_back(std::move(val));
        }
        template<typename VectorT>
        static inline void VectorErase(VectorT& vr, int64_t pos)
        {
            auto it = vr.begin();
            std::advance(it, pos);
            if (it != vr.end())
                vr.erase(it);
        }
        template<typename HashtableT, typename ValT>
        static inline bool HashtableContains(const HashtableT& hash, const ValT& val)
        {
            auto it = hash.find(val);
            return it != hash.end();
        }
    };
    class ArrayHashtableMemberSetProvider final : public AbstractMemberSetApiProvider
    {
        friend class MemberSetExp;
    protected:
        virtual bool LoadMemberSet(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& objInfo, Brace::BraceApiExecutor&& obj, std::string&& member, Brace::OperandLoadtimeInfo&& argInfo, Brace::BraceApiExecutor&& arg, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor)override
        {
            return false;
        }
    private:
        ArrayHashtableMemberSetProvider(Brace::BraceScript& interpreter) :AbstractMemberSetApiProvider(interpreter)
        {
        }
    };
    class ArrayHashtableMemberGetProvider final : public AbstractMemberGetApiProvider
    {
        friend class MemberGetExp;
    protected:
        virtual bool LoadMemberGet(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& objInfo, Brace::BraceApiExecutor&& obj, std::string&& member, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            if (data.GetParamNum() != 2)
                return false;
            std::swap(m_Obj, obj);
            m_ObjInfo = objInfo;
            std::swap(m_Member, member);
            bool isArray = false;
            bool isHashtable = false;
            switch (objInfo.ObjectTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY:
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY:
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY:
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY:
                isArray = true;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE:
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE:
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE:
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE:
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE:
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE:
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE:
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE:
                isHashtable = true;
                break;
            }
            switch (braceObjInfo.ObjectCategory) {
            case BRACE_OBJECT_CATEGORY_OBJ_ARRAY:
                isArray = true;
                break;
            case BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE:
                isHashtable = true;
                break;
            case BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE:
                isHashtable = true;
                break;
            }
            if (isArray) {
                if (m_Member == "length") {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &ArrayHashtableMemberGetProvider::ExecuteArrayLength);
                    return true;
                }
            }
            else if (isHashtable) {
                if (m_Member == "count") {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &ArrayHashtableMemberGetProvider::ExecuteHashtableCount);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "Unknown member " << m_Member << " line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
    private:
        int ExecuteArrayLength(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            const std::shared_ptr<void>& p = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            int objTypeId = m_ObjInfo.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<bool>*>(ptr);
                    size_t v = pArr->size();
                    Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, static_cast<int32_t>(v));
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<int>*>(ptr);
                    size_t v = pArr->size();
                    Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, static_cast<int32_t>(v));
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<double>*>(ptr);
                    size_t v = pArr->size();
                    Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, static_cast<int32_t>(v));
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<std::string>*>(ptr);
                    size_t v = pArr->size();
                    Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, static_cast<int32_t>(v));
                }
            }break;
            default: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ObjectArray*>(ptr);
                    size_t v = pArr->size();
                    Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, static_cast<int32_t>(v));
                }
            }break;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteHashtableCount(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            const std::shared_ptr<void>& p = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            int objTypeId = m_ObjInfo.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<int64_t, bool>*>(ptr);
                    size_t v = pHashtable->size();
                    Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, static_cast<int32_t>(v));
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<int64_t, int64_t>*>(ptr);
                    size_t v = pHashtable->size();
                    Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, static_cast<int32_t>(v));
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<int64_t, double>*>(ptr);
                    size_t v = pHashtable->size();
                    Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, static_cast<int32_t>(v));
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<int64_t, std::string>*>(ptr);
                    size_t v = pHashtable->size();
                    Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, static_cast<int32_t>(v));
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<std::string, bool>*>(ptr);
                    size_t v = pHashtable->size();
                    Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, static_cast<int32_t>(v));
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<std::string, int64_t>*>(ptr);
                    size_t v = pHashtable->size();
                    Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, static_cast<int32_t>(v));
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<std::string, double>*>(ptr);
                    size_t v = pHashtable->size();
                    Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, static_cast<int32_t>(v));
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<std::string, std::string>*>(ptr);
                    size_t v = pHashtable->size();
                    Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, static_cast<int32_t>(v));
                }
            }break;
            default: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
                    if (nullptr != pInfo) {
                        if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE) {
                            auto* pHashtable = static_cast<IntObjHashtable*>(ptr);
                            size_t v = pHashtable->size();
                            Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, static_cast<int32_t>(v));
                        }
                        else if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE) {
                            auto* pHashtable = static_cast<StrObjHashtable*>(ptr);
                            size_t v = pHashtable->size();
                            Brace::VarSetInt32((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, static_cast<int32_t>(v));
                        }
                    }
                }
            }break;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    private:
        ArrayHashtableMemberGetProvider(Brace::BraceScript& interpreter) :AbstractMemberGetApiProvider(interpreter), m_Obj(), m_ObjInfo(), m_Member(), m_ResultInfo()
        {
        }
    private:
        Brace::BraceApiExecutor m_Obj;
        Brace::OperandRuntimeInfo m_ObjInfo;
        std::string m_Member;
        Brace::OperandRuntimeInfo m_ResultInfo;
    };
    class ArrayHashtableCollectionCallProvider final : public AbstractCollectionCallApiProvider
    {
        friend class CollectionCallExp;
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) const override
        {
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo) const override
        {
        }
    private:
        ArrayHashtableCollectionCallProvider(Brace::BraceScript& interpreter) :AbstractCollectionCallApiProvider(interpreter)
        {}
    };
    class ArrayHashtableCollectionSetProvider final : public AbstractCollectionSetApiProvider
    {
        friend class CollectionSetExp;
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, const Brace::OperandLoadtimeInfo& arr, const Brace::OperandLoadtimeInfo& ix, const Brace::OperandLoadtimeInfo& val, Brace::OperandLoadtimeInfo& resultInfo) const override
        {
            bool isArray = false;
            bool isHashtable = false;
            bool isIntKey = true;
            int dataType = Brace::BRACE_DATA_TYPE_OBJECT;
            int objTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            switch (braceObjInfo.ObjectTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY:
                isArray = true;
                dataType = Brace::BRACE_DATA_TYPE_STRING;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY:
                isArray = true;
                dataType = Brace::BRACE_DATA_TYPE_INT64;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY:
                isArray = true;
                dataType = Brace::BRACE_DATA_TYPE_DOUBLE;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY:
                isArray = true;
                dataType = Brace::BRACE_DATA_TYPE_BOOL;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE:
                isHashtable = true;
                isIntKey = false;
                dataType = Brace::BRACE_DATA_TYPE_STRING;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE:
                isHashtable = true;
                isIntKey = false;
                dataType = Brace::BRACE_DATA_TYPE_INT64;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE:
                isHashtable = true;
                isIntKey = false;
                dataType = Brace::BRACE_DATA_TYPE_DOUBLE;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE:
                isHashtable = true;
                isIntKey = false;
                dataType = Brace::BRACE_DATA_TYPE_BOOL;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE:
                isHashtable = true;
                dataType = Brace::BRACE_DATA_TYPE_STRING;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE:
                isHashtable = true;
                dataType = Brace::BRACE_DATA_TYPE_INT64;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE:
                isHashtable = true;
                dataType = Brace::BRACE_DATA_TYPE_DOUBLE;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE:
                isHashtable = true;
                dataType = Brace::BRACE_DATA_TYPE_BOOL;
                break;
            }
            switch (braceObjInfo.ObjectCategory) {
            case BRACE_OBJECT_CATEGORY_OBJ_ARRAY:
                isArray = true;
                objTypeId = braceObjInfo.GetTypeParamObjTypeId(0);
                break;
            case BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE:
                isHashtable = true;
                objTypeId = braceObjInfo.GetTypeParamObjTypeId(1);
                break;
            case BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE:
                isHashtable = true;
                isIntKey = false;
                objTypeId = braceObjInfo.GetTypeParamObjTypeId(1);
                break;
            }
            if (isArray) {
                if (!(ix.Type >= Brace::BRACE_DATA_TYPE_INT8 && ix.Type <= Brace::BRACE_DATA_TYPE_UINT64)) {
                    std::stringstream ss;
                    ss << "Array's index must be integer ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
                if (!CanAssign(dataType, objTypeId, val.Type, val.ObjectTypeId)) {
                    std::stringstream ss;
                    ss << "Array element's type and val type dismatch ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
                resultInfo = val;
                return true;
            }
            else if (isHashtable) {
                if (isIntKey) {
                    if (!(ix.Type >= Brace::BRACE_DATA_TYPE_INT8 && ix.Type <= Brace::BRACE_DATA_TYPE_UINT64)) {
                        std::stringstream ss;
                        ss << "key must be integer ! line: " << data.GetLine();
                        LogError(ss.str());
                        return false;
                    }
                }
                else {
                    if (ix.Type != Brace::BRACE_DATA_TYPE_STRING) {
                        std::stringstream ss;
                        ss << "key must be string ! line: " << data.GetLine();
                        LogError(ss.str());
                        return false;
                    }
                }
                if (!CanAssign(dataType, objTypeId, val.Type, val.ObjectTypeId)) {
                    std::stringstream ss;
                    ss << "Hashtable type and val type dismatch ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
                resultInfo = val;
                return true;
            }
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& arr, const Brace::OperandRuntimeInfo& ix, const Brace::OperandRuntimeInfo& val, const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            const std::shared_ptr<void>& p = Brace::VarGetObject((arr.IsGlobal ? gvars : lvars), arr.VarIndex);
            int objTypeId = arr.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY: {
                bool v = Brace::VarGetBoolean((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pArr = static_cast<ArrayT<bool>*>(ptr);
                    (*pArr)[static_cast<size_t>(vix)] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                int64_t v = Brace::VarGetI64((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pArr = static_cast<ArrayT<int64_t>*>(ptr);
                    (*pArr)[static_cast<size_t>(vix)] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                double v = Brace::VarGetF64((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pArr = static_cast<ArrayT<double>*>(ptr);
                    (*pArr)[static_cast<size_t>(vix)] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY: {
                const std::string& v = Brace::VarGetString((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pArr = static_cast<ArrayT<std::string>*>(ptr);
                    (*pArr)[static_cast<size_t>(vix)] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE: {
                const std::string& v = Brace::VarGetString((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, std::string>*>(ptr);
                    (*pHashtable)[vix] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                int64_t v = Brace::VarGetInt64((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, int64_t>*>(ptr);
                    (*pHashtable)[vix] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                double v = Brace::VarGetDouble((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, double>*>(ptr);
                    (*pHashtable)[vix] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE: {
                bool v = Brace::VarGetBool((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, bool>*>(ptr);
                    (*pHashtable)[vix] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE: {
                const std::string& v = Brace::VarGetString((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, std::string>*>(ptr);
                    (*pHashtable)[static_cast<size_t>(vix)] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                int64_t v = Brace::VarGetInt64((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, int64_t>*>(ptr);
                    (*pHashtable)[static_cast<size_t>(vix)] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                double v = Brace::VarGetDouble((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, double>*>(ptr);
                    (*pHashtable)[static_cast<size_t>(vix)] = v;
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE: {
                bool v = Brace::VarGetBool((val.IsGlobal ? gvars : lvars), val.VarIndex);
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, bool>*>(ptr);
                    (*pHashtable)[static_cast<size_t>(vix)] = v;
                }
            }break;
            default: {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
                if (nullptr != pInfo) {
                    if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_OBJ_ARRAY) {
                        auto& v = Brace::VarGetObject((val.IsGlobal ? gvars : lvars), val.VarIndex);
                        auto* ptr = p.get();
                        if (nullptr != ptr) {
                            int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                            auto* pArr = static_cast<ObjectArray*>(ptr);
                            (*pArr)[static_cast<size_t>(vix)] = v;
                        }
                    }
                    else if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE) {
                        auto& v = Brace::VarGetObject((val.IsGlobal ? gvars : lvars), val.VarIndex);
                        auto* ptr = p.get();
                        if (nullptr != ptr) {
                            int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                            auto* pHashtable = static_cast<IntObjHashtable*>(ptr);
                            (*pHashtable)[static_cast<size_t>(vix)] = v;
                        }
                    }
                    else if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE) {
                        auto& v = Brace::VarGetObject((val.IsGlobal ? gvars : lvars), val.VarIndex);
                        auto* ptr = p.get();
                        if (nullptr != ptr) {
                            std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                            auto* pHashtable = static_cast<StrObjHashtable*>(ptr);
                            (*pHashtable)[vix] = v;
                        }
                    }
                }
            }break;
            }
        }
    private:
        ArrayHashtableCollectionSetProvider(Brace::BraceScript& interpreter) :AbstractCollectionSetApiProvider(interpreter)
        {}
    };
    class ArrayHashtableCollectionGetProvider final : public AbstractCollectionGetApiProvider
    {
        friend class CollectionGetExp;
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, const Brace::OperandLoadtimeInfo& arr, const Brace::OperandLoadtimeInfo& ix, Brace::OperandLoadtimeInfo& resultInfo) const override
        {
            bool isArray = false;
            bool isHashtable = false;
            bool isIntKey = true;
            int dataType = Brace::BRACE_DATA_TYPE_OBJECT;
            int objTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            switch (braceObjInfo.ObjectTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY:
                isArray = true;
                dataType = Brace::BRACE_DATA_TYPE_STRING;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY:
                isArray = true;
                dataType = Brace::BRACE_DATA_TYPE_INT64;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY:
                isArray = true;
                dataType = Brace::BRACE_DATA_TYPE_DOUBLE;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY:
                isArray = true;
                dataType = Brace::BRACE_DATA_TYPE_BOOL;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE:
                isHashtable = true;
                isIntKey = false;
                dataType = Brace::BRACE_DATA_TYPE_STRING;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE:
                isHashtable = true;
                isIntKey = false;
                dataType = Brace::BRACE_DATA_TYPE_INT64;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE:
                isHashtable = true;
                isIntKey = false;
                dataType = Brace::BRACE_DATA_TYPE_DOUBLE;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE:
                isHashtable = true;
                isIntKey = false;
                dataType = Brace::BRACE_DATA_TYPE_BOOL;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE:
                isHashtable = true;
                dataType = Brace::BRACE_DATA_TYPE_STRING;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE:
                isHashtable = true;
                dataType = Brace::BRACE_DATA_TYPE_INT64;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE:
                isHashtable = true;
                dataType = Brace::BRACE_DATA_TYPE_DOUBLE;
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE:
                isHashtable = true;
                dataType = Brace::BRACE_DATA_TYPE_BOOL;
                break;
            }
            switch (braceObjInfo.ObjectCategory) {
            case BRACE_OBJECT_CATEGORY_OBJ_ARRAY:
                isArray = true;
                objTypeId = braceObjInfo.GetTypeParamObjTypeId(0);
                break;
            case BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE:
                isHashtable = true;
                objTypeId = braceObjInfo.GetTypeParamObjTypeId(1);
                break;
            case BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE:
                isHashtable = true;
                isIntKey = false;
                objTypeId = braceObjInfo.GetTypeParamObjTypeId(1);
                break;
            }
            if (isArray) {
                if (!(ix.Type >= Brace::BRACE_DATA_TYPE_INT8 && ix.Type <= Brace::BRACE_DATA_TYPE_UINT64)) {
                    std::stringstream ss;
                    ss << "Array's index must be integer ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
                resultInfo.Type = dataType;
                resultInfo.ObjectTypeId = objTypeId;
                resultInfo.Name = GenTempVarName();
                resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                return true;
            }
            else if (isHashtable) {
                if (isIntKey) {
                    if (!(ix.Type >= Brace::BRACE_DATA_TYPE_INT8 && ix.Type <= Brace::BRACE_DATA_TYPE_UINT64)) {
                        std::stringstream ss;
                        ss << "key must be integer ! line: " << data.GetLine();
                        LogError(ss.str());
                        return false;
                    }
                }
                else {
                    if (ix.Type != Brace::BRACE_DATA_TYPE_STRING) {
                        std::stringstream ss;
                        ss << "key must be string ! line: " << data.GetLine();
                        LogError(ss.str());
                        return false;
                    }
                }
                resultInfo.Type = dataType;
                resultInfo.ObjectTypeId = objTypeId;
                resultInfo.Name = GenTempVarName();
                resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                return true;
            }
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& arr, const Brace::OperandRuntimeInfo& ix, const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            const std::shared_ptr<void>& p = Brace::VarGetObject((arr.IsGlobal ? gvars : lvars), arr.VarIndex);
            int objTypeId = arr.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pArr = static_cast<ArrayT<bool>*>(ptr);
                    bool v = (*pArr)[static_cast<size_t>(vix)];
                    Brace::VarSetBool((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pArr = static_cast<ArrayT<int64_t>*>(ptr);
                    int64_t v = (*pArr)[static_cast<size_t>(vix)];
                    Brace::VarSetInt64((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pArr = static_cast<ArrayT<double>*>(ptr);
                    double v = (*pArr)[static_cast<size_t>(vix)];
                    Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pArr = static_cast<ArrayT<std::string>*>(ptr);
                    const std::string& v = (*pArr)[static_cast<size_t>(vix)];
                    Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, bool>*>(ptr);
                    bool v = (*pHashtable)[static_cast<size_t>(vix)];
                    Brace::VarSetBool((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, int64_t>*>(ptr);
                    int64_t v = (*pHashtable)[static_cast<size_t>(vix)];
                    Brace::VarSetInt64((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, double>*>(ptr);
                    double v = (*pHashtable)[static_cast<size_t>(vix)];
                    Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<int64_t, std::string>*>(ptr);
                    const std::string& v = (*pHashtable)[static_cast<size_t>(vix)];
                    Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, bool>*>(ptr);
                    bool v = (*pHashtable)[vix];
                    Brace::VarSetBool((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, int64_t>*>(ptr);
                    int64_t v = (*pHashtable)[vix];
                    Brace::VarSetInt64((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, double>*>(ptr);
                    double v = (*pHashtable)[vix];
                    Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
                }
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                    auto* pHashtable = static_cast<HashtableT<std::string, std::string>*>(ptr);
                    const std::string& v = (*pHashtable)[vix];
                    Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
                }
            }break;
            default: {
                auto* ptr = p.get();
                if (nullptr != ptr) {
                    auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
                    if (nullptr != pInfo) {
                        switch (pInfo->ObjectCategory) {
                        case BRACE_OBJECT_CATEGORY_OBJ_ARRAY: {
                            int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                            auto* pArr = static_cast<ObjectArray*>(ptr);
                            auto& v = (*pArr)[static_cast<size_t>(vix)];
                            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
                        }break;
                        case BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE: {
                            int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                            auto* pHashtable = static_cast<IntObjHashtable*>(ptr);
                            auto& v = (*pHashtable)[static_cast<size_t>(vix)];
                            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
                        }break;
                        case BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE: {
                            std::string vix = Brace::VarGetStr((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
                            auto* pHashtable = static_cast<StrObjHashtable*>(ptr);
                            auto& v = (*pHashtable)[vix];
                            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
                        }break;
                        }
                    }
                }
            }break;
            }
        }
    private:
        ArrayHashtableCollectionGetProvider(Brace::BraceScript& interpreter) :AbstractCollectionGetApiProvider(interpreter)
        {}
    };
    class ArrayHashtableLoopListProvider final : public AbstractLoopListApiProvider
    {
        friend class LoopListExp;
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::ISyntaxComponent& data, const BraceObjectInfo& braceObjInfo, const Brace::OperandLoadtimeInfo& listInfo, Brace::BraceApiExecutor& executor) override
        {
            return TypeInference(listInfo, executor);
        }
        virtual void StoreRuntimeInfo(Brace::OperandRuntimeInfo&& listInfo, Brace::BraceApiExecutor&& list, std::vector<Brace::BraceApiExecutor>&& statements, const std::vector<int>& objVars) override
        {
            std::swap(m_ListInfo, listInfo);
            std::swap(m_List, list);
            std::swap(m_Statements, statements);
            m_ObjVars = objVars;
        }
    private:
        ArrayHashtableLoopListProvider(Brace::BraceScript& interpreter) :AbstractLoopListApiProvider(interpreter), m_IteratorIndex(INVALID_INDEX), m_IteratorIndexV(INVALID_INDEX), m_List(), m_ListInfo(), m_Statements(), m_ObjVars()
        {}
    private:
        bool TypeInference(const Brace::OperandLoadtimeInfo& listInfo, Brace::BraceApiExecutor& executor)
        {
            if (listInfo.Type == Brace::BRACE_DATA_TYPE_OBJECT) {
                if (listInfo.ObjectTypeId == CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY) {
                    m_IteratorIndex = AllocVariable("$$", Brace::BRACE_DATA_TYPE_BOOL, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    executor.attach(this, &ArrayHashtableLoopListProvider::ExecuteBoolArray);
                    return true;
                }
                else if (listInfo.ObjectTypeId == CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY) {
                    m_IteratorIndex = AllocVariable("$$", Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    executor.attach(this, &ArrayHashtableLoopListProvider::ExecuteIntArray);
                    return true;
                }
                else if (listInfo.ObjectTypeId == CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY) {
                    m_IteratorIndex = AllocVariable("$$", Brace::BRACE_DATA_TYPE_DOUBLE, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    executor.attach(this, &ArrayHashtableLoopListProvider::ExecuteFloatArray);
                    return true;
                }
                else if (listInfo.ObjectTypeId == CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY) {
                    m_IteratorIndex = AllocVariable("$$", Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    executor.attach(this, &ArrayHashtableLoopListProvider::ExecuteStringArray);
                    return true;
                }
                else if (listInfo.ObjectTypeId == CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE) {
                    m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    m_IteratorIndexV = AllocVariable("$$v", Brace::BRACE_DATA_TYPE_BOOL, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    executor.attach(this, &ArrayHashtableLoopListProvider::ExecuteIntBoolHashtable);
                    return true;
                }
                else if (listInfo.ObjectTypeId == CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE) {
                    m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    m_IteratorIndexV = AllocVariable("$$v", Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    executor.attach(this, &ArrayHashtableLoopListProvider::ExecuteIntIntHashtable);
                    return true;
                }
                else if (listInfo.ObjectTypeId == CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE) {
                    m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    m_IteratorIndexV = AllocVariable("$$v", Brace::BRACE_DATA_TYPE_DOUBLE, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    executor.attach(this, &ArrayHashtableLoopListProvider::ExecuteIntFloatHashtable);
                    return true;
                }
                else if (listInfo.ObjectTypeId == CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE) {
                    m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    m_IteratorIndexV = AllocVariable("$$v", Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    executor.attach(this, &ArrayHashtableLoopListProvider::ExecuteIntStrHashtable);
                    return true;
                }
                else if (listInfo.ObjectTypeId == CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE) {
                    m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    m_IteratorIndexV = AllocVariable("$$v", Brace::BRACE_DATA_TYPE_BOOL, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    executor.attach(this, &ArrayHashtableLoopListProvider::ExecuteStrBoolHashtable);
                    return true;
                }
                else if (listInfo.ObjectTypeId == CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE) {
                    m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    m_IteratorIndexV = AllocVariable("$$v", Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    executor.attach(this, &ArrayHashtableLoopListProvider::ExecuteStrIntHashtable);
                    return true;
                }
                else if (listInfo.ObjectTypeId == CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE) {
                    m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    m_IteratorIndexV = AllocVariable("$$v", Brace::BRACE_DATA_TYPE_DOUBLE, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    executor.attach(this, &ArrayHashtableLoopListProvider::ExecuteStrFloatHashtable);
                    return true;
                }
                else if (listInfo.ObjectTypeId == CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE) {
                    m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    m_IteratorIndexV = AllocVariable("$$v", Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                    executor.attach(this, &ArrayHashtableLoopListProvider::ExecuteStrStrHashtable);
                    return true;
                }
                else {
                    auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(listInfo.ObjectTypeId);
                    if (nullptr != pInfo) {
                        if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_OBJ_ARRAY) {
                            m_IteratorIndex = AllocVariable("$$", Brace::BRACE_DATA_TYPE_OBJECT, pInfo->GetTypeParamObjTypeId(0));
                            executor.attach(this, &ArrayHashtableLoopListProvider::ExecuteObjectArray);
                            return true;
                        }
                        else if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE) {
                            m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                            m_IteratorIndexV = AllocVariable("$$v", Brace::BRACE_DATA_TYPE_OBJECT, pInfo->GetTypeParamObjTypeId(1));
                            executor.attach(this, &ArrayHashtableLoopListProvider::ExecuteIntObjHashtable);
                            return true;
                        }
                        else if (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE) {
                            m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                            m_IteratorIndexV = AllocVariable("$$v", Brace::BRACE_DATA_TYPE_OBJECT, pInfo->GetTypeParamObjTypeId(1));
                            executor.attach(this, &ArrayHashtableLoopListProvider::ExecuteStrObjHashtable);
                            return true;
                        }
                    }
                }
            }
            return false;
        }
    private:
        int ExecuteBoolArray(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<bool>*>(ptr);
                    for (auto val : (*pArr)) {
                        Brace::VarSetBool(lvars, m_IteratorIndex, val);
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteIntArray(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<int64_t>*>(ptr);
                    for (auto val : (*pArr)) {
                        Brace::VarSetInt64(lvars, m_IteratorIndex, val);
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteFloatArray(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<double>*>(ptr);
                    for (auto val : (*pArr)) {
                        Brace::VarSetDouble(lvars, m_IteratorIndex, val);
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteStringArray(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ArrayT<std::string>*>(ptr);
                    for (auto&& val : (*pArr)) {
                        Brace::VarSetString(lvars, m_IteratorIndex, val);
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteObjectArray(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pArr = static_cast<ObjectArray*>(ptr);
                    for (auto&& val : (*pArr)) {
                        Brace::VarSetObject(lvars, m_IteratorIndex, val);
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteIntBoolHashtable(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<int64_t, bool>*>(ptr);
                    for (auto&& val : (*pHashtable)) {
                        Brace::VarSetInt64(lvars, m_IteratorIndex, val.first);
                        Brace::VarSetBool(lvars, m_IteratorIndexV, val.second);
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteIntIntHashtable(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<int64_t, int64_t>*>(ptr);
                    for (auto&& val : (*pHashtable)) {
                        Brace::VarSetInt64(lvars, m_IteratorIndex, val.first);
                        Brace::VarSetInt64(lvars, m_IteratorIndexV, val.second);
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteIntFloatHashtable(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<int64_t, double>*>(ptr);
                    for (auto&& val : (*pHashtable)) {
                        Brace::VarSetInt64(lvars, m_IteratorIndex, val.first);
                        Brace::VarSetDouble(lvars, m_IteratorIndexV, val.second);
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteIntStrHashtable(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<int64_t, std::string>*>(ptr);
                    for (auto&& val : (*pHashtable)) {
                        Brace::VarSetInt64(lvars, m_IteratorIndex, val.first);
                        Brace::VarSetString(lvars, m_IteratorIndexV, val.second);
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteIntObjHashtable(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<IntObjHashtable*>(ptr);
                    for (auto&& val : (*pHashtable)) {
                        Brace::VarSetInt64(lvars, m_IteratorIndex, val.first);
                        Brace::VarSetObject(lvars, m_IteratorIndexV, val.second);
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteStrBoolHashtable(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<std::string, bool>*>(ptr);
                    for (auto&& val : (*pHashtable)) {
                        Brace::VarSetString(lvars, m_IteratorIndex, val.first);
                        Brace::VarSetBool(lvars, m_IteratorIndexV, val.second);
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteStrIntHashtable(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<std::string, int64_t>*>(ptr);
                    for (auto&& val : (*pHashtable)) {
                        Brace::VarSetString(lvars, m_IteratorIndex, val.first);
                        Brace::VarSetInt64(lvars, m_IteratorIndexV, val.second);
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteStrFloatHashtable(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<std::string, double>*>(ptr);
                    for (auto&& val : (*pHashtable)) {
                        Brace::VarSetString(lvars, m_IteratorIndex, val.first);
                        Brace::VarSetDouble(lvars, m_IteratorIndexV, val.second);
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteStrStrHashtable(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<HashtableT<std::string, std::string>*>(ptr);
                    for (auto&& val : (*pHashtable)) {
                        Brace::VarSetString(lvars, m_IteratorIndex, val.first);
                        Brace::VarSetString(lvars, m_IteratorIndexV, val.second);
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteStrObjHashtable(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pHashtable = static_cast<StrObjHashtable*>(ptr);
                    for (auto&& val : (*pHashtable)) {
                        Brace::VarSetString(lvars, m_IteratorIndex, val.first);
                        Brace::VarSetObject(lvars, m_IteratorIndexV, val.second);
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    private:
        int m_IteratorIndex;
        int m_IteratorIndexV;
        Brace::BraceApiExecutor m_List;
        Brace::OperandRuntimeInfo m_ListInfo;
        std::vector<Brace::BraceApiExecutor> m_Statements;
        std::vector<int> m_ObjVars;
    };
    class ArrayHashtableLinqProvider final : public AbstractLinqApiProvider
    {
        friend class LinqExp;
        enum OperationType
        {
            OPERATION_UNKNOWN = -1,
            OPERATION_ORDERBY = 0,
            OPERATION_ORDERBYDESC,
            OPERATION_TOP,
            OPERATION_WHERE,
            OPERATION_NUM
        };
        struct CmpVal
        {
            double NumVal;
            std::string StrVal;
        };
    protected:
        virtual bool LoadLinqCall(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, int iteratorIndex, Brace::OperandLoadtimeInfo&& listInfo, Brace::BraceApiExecutor&& list, std::string&& member, std::vector<Brace::OperandLoadtimeInfo>&& argInfos, std::vector<Brace::BraceApiExecutor>&& args, std::vector<int>&& objVars, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            if (member == "orderby") {
                m_Operation = OPERATION_ORDERBY;
            }
            else if (member == "orderbydesc") {
                m_Operation = OPERATION_ORDERBYDESC;
            }
            else if (member == "top") {
                m_Operation = OPERATION_TOP;
            }
            else if (member == "where") {
                m_Operation = OPERATION_WHERE;
            }

            m_IteratorIndex = iteratorIndex;
            m_ListInfo = listInfo;
            std::swap(m_List, list);
            for (auto&& argInfo : argInfos) {
                m_ArgInfos.push_back(argInfo);
            }
            std::swap(m_Args, args);
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.ObjectTypeId = listInfo.ObjectTypeId;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            m_ResultInfo = resultInfo;
            std::swap(m_ObjVars, objVars);
            executor.attach(this, &ArrayHashtableLinqProvider::ExecuteObjectArray);
            return true;
        }
    private:
        ArrayHashtableLinqProvider(Brace::BraceScript& interpreter) :AbstractLinqApiProvider(interpreter), m_Operation(OPERATION_UNKNOWN), m_IteratorIndex(INVALID_INDEX), m_List(), m_ListInfo(), m_ArgInfos(), m_Args(), m_ResultInfo(), m_ObjVars()
        {}
    private:
        int ExecuteObjectArray(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            if (nullptr != obj) {
                auto* ptr = obj.get();
                if (nullptr != ptr) {
                    auto* pNewArr = new ObjectArray();
                    std::shared_ptr<void> newArr(pNewArr);
                    Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, newArr);
                    auto* pArr = static_cast<ObjectArray*>(ptr);
                    switch (m_Operation) {
                    case OPERATION_ORDERBY:
                    case OPERATION_ORDERBYDESC:
                        ExecuteOrderBy(gvars, lvars, pArr, pNewArr, m_Operation == OPERATION_ORDERBY);
                        break;
                    case OPERATION_TOP:
                        ExecuteTop(gvars, lvars, pArr, pNewArr);
                        break;
                    case OPERATION_WHERE:
                        ExecuteWhere(gvars, lvars, pArr, pNewArr);
                        break;
                    default:
                        break;
                    }
                }
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        void ExecuteOrderBy(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const ObjectArray* pArr, ObjectArray* pNewArr, bool asc)const
        {
            for (auto&& val : (*pArr)) {
                pNewArr->push_back(val);
            }
            std::vector<CmpVal> sortVals{};
            std::sort(pNewArr->begin(), pNewArr->end(), [&](auto& e1, auto& e2) {
                Brace::VarSetObject(lvars, m_IteratorIndex, e1);
                for (auto&& arg : m_Args) {
                    if (!arg.isNull())
                        arg(gvars, lvars);
                }
                sortVals.clear();
                for (auto&& argInfo : m_ArgInfos) {
                    CmpVal cv;
                    if (Brace::IsStringType(argInfo.Type))
                        cv.StrVal = Brace::VarGetString((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
                    else
                        cv.NumVal = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                    sortVals.push_back(cv);
                }
                Brace::VarSetObject(lvars, m_IteratorIndex, e2);
                for (auto&& arg : m_Args) {
                    if (!arg.isNull())
                        arg(gvars, lvars);
                }
                int ix = 0;
                for (auto&& argInfo : m_ArgInfos) {
                    const CmpVal& cv = sortVals[ix++];
                    if (Brace::IsStringType(argInfo.Type)) {
                        const std::string& v2 = Brace::VarGetString((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
                        if (asc) {
                            if (cv.StrVal < v2)
                                return true;
                            else if (cv.StrVal > v2)
                                return false;
                        }
                        else {
                            if (cv.StrVal > v2)
                                return true;
                            else if (cv.StrVal < v2)
                                return false;
                        }
                    }
                    else {
                        double v2 = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                        if (asc) {
                            if (cv.NumVal < v2)
                                return true;
                            else if (cv.NumVal > v2)
                                return false;
                        }
                        else {
                            if (cv.NumVal > v2)
                                return true;
                            else if (cv.NumVal < v2)
                                return false;
                        }
                    }
                }
                return false;
            });
            FreeObjVars(lvars, m_ObjVars);
        }
        void ExecuteTop(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const ObjectArray* pArr, ObjectArray* pNewArr)const
        {
            for (auto&& arg : m_Args) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            auto& argInfo = m_ArgInfos[0];
            int64_t n = Brace::VarGetI64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            int64_t ct = 0;
            for (auto&& val : (*pArr)) {
                pNewArr->push_back(val);
                ++ct;
                if (ct >= n)
                    break;
            }
            FreeObjVars(lvars, m_ObjVars);
        }
        void ExecuteWhere(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const ObjectArray* pArr, ObjectArray* pNewArr)const
        {
            for (auto&& val : (*pArr)) {
                Brace::VarSetObject(lvars, m_IteratorIndex, val);
                for (auto&& arg : m_Args) {
                    if (!arg.isNull())
                        arg(gvars, lvars);
                }
                auto& argInfo = m_ArgInfos[0];
                bool v = Brace::VarGetBoolean((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                if (v)
                    pNewArr->push_back(val);
            }
            FreeObjVars(lvars, m_ObjVars);
        }
    private:
        OperationType m_Operation;
        int m_IteratorIndex;
        Brace::BraceApiExecutor m_List;
        Brace::OperandRuntimeInfo m_ListInfo;
        std::vector<Brace::OperandRuntimeInfo> m_ArgInfos;
        std::vector<Brace::BraceApiExecutor> m_Args;
        Brace::OperandRuntimeInfo m_ResultInfo;
        std::vector<int> m_ObjVars;
    };
    class ArrayHashtableSelectProvider final : public AbstractSelectApiProvider
    {
        friend class SelectExp;
        struct CmpVal
        {
            bool IsStr;
            double NumVal;
            std::string StrVal;
            double NewNumVal;
            std::string NewStrVal;
        };
    protected:
        virtual bool LoadSelect(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, int iteratorIndex, std::vector<Brace::OperandLoadtimeInfo>&& argInfos, std::vector<Brace::BraceApiExecutor>&& args, std::vector<int>&& statMethods, std::vector<int>&& objVars) override
        {
            m_SelectIteratorIndex = iteratorIndex;
            for (auto&& argInfo : argInfos) {
                m_SelectArgInfos.push_back(argInfo);
            }
            std::swap(m_SelectArgs, args);
            std::swap(m_SelectStats, statMethods);
            std::swap(m_SelectObjVars, objVars);
            return true;
        }
        virtual bool LoadTop(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo&& argInfo, Brace::BraceApiExecutor&& arg, std::vector<int>&& objVars) override
        {
            m_TopArgInfo = argInfo;
            std::swap(m_TopArg, arg);
            std::swap(m_TopObjVars, objVars);
            return true;
        }
        virtual bool LoadFromList(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& argInfo, Brace::BraceApiExecutor&& arg) override
        {
            m_ListInfo = argInfo;
            std::swap(m_List, arg);
            return true;
        }
        virtual bool LoadFromType(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, const std::string& type) override
        {
            m_Type = type;
            return true;
        }
        virtual bool LoadWhere(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, int iteratorIndex, Brace::OperandLoadtimeInfo&& argInfo, Brace::BraceApiExecutor&& arg, std::vector<int>&& objVars) override
        {
            m_WhereIteratorIndex = iteratorIndex;
            m_WhereArgInfo = argInfo;
            std::swap(m_WhereArg, arg);
            std::swap(m_WhereObjVars, objVars);
            return true;
        }
        virtual bool LoadOrderBy(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, int iteratorIndex, std::vector<Brace::OperandLoadtimeInfo>&& argInfos, std::vector<Brace::BraceApiExecutor>&& args, std::vector<bool>&& ascOrDescs, std::vector<int>&& objVars) override
        {
            m_OrderIteratorIndex = iteratorIndex;
            for (auto&& argInfo : argInfos) {
                m_OrderArgInfos.push_back(argInfo);
            }
            std::swap(m_OrderArgs, args);
            std::swap(m_OrderAscs, ascOrDescs);
            std::swap(m_OrderObjVars, objVars);
            return true;
        }
        virtual bool LoadGroupBy(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, int iteratorIndex, std::vector<Brace::OperandLoadtimeInfo>&& argInfos, std::vector<Brace::BraceApiExecutor>&& args, std::vector<int>&& objVars) override
        {
            m_GroupIteratorIndex = iteratorIndex;
            for (auto&& argInfo : argInfos) {
                m_GroupArgInfos.push_back(argInfo);
            }
            std::swap(m_GroupArgs, args);
            std::swap(m_GroupObjVars, objVars);
            return true;
        }
        virtual bool LoadHaving(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo&& argInfo, Brace::BraceApiExecutor&& arg) override
        {
            m_HavingArgInfo = argInfo;
            std::swap(m_HavingArg, arg);
            return true;
        }
        virtual bool LoadStatements(const Brace::FuncInfo& func, const DslData::FunctionData& data, std::vector<Brace::BraceApiExecutor>&& statements, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            std::swap(m_Statements, statements);
            executor.attach(this, &ArrayHashtableSelectProvider::ExecuteObjectArray);
            return true;
        }
        virtual void LoadResultIterator(std::vector<Brace::OperandLoadtimeInfo>&& iterators, std::vector<int>&& objVars) override
        {
            for (auto&& itInfo : iterators) {
                m_Iterators.push_back(itInfo);
                m_IteratorAssigns.push_back(Brace::GetVarAssignPtr(itInfo.Type, false, itInfo.Type, false));
            }
            std::swap(m_ObjVars, objVars);
        }
    private:
        ArrayHashtableSelectProvider(Brace::BraceScript& interpreter) :AbstractSelectApiProvider(interpreter)
            , m_Type()
            , m_List()
            , m_ListInfo()
            , m_SelectIteratorIndex(INVALID_INDEX)
            , m_SelectArgInfos()
            , m_SelectArgs()
            , m_SelectStats()
            , m_SelectObjVars()
            , m_TopArgInfo()
            , m_TopArg()
            , m_TopObjVars()
            , m_WhereIteratorIndex(INVALID_INDEX)
            , m_WhereArgInfo()
            , m_WhereArg()
            , m_WhereObjVars()
            , m_OrderIteratorIndex(INVALID_INDEX)
            , m_OrderArgInfos()
            , m_OrderArgs()
            , m_OrderAscs()
            , m_OrderObjVars()
            , m_GroupIteratorIndex(INVALID_INDEX)
            , m_GroupArgInfos()
            , m_GroupArgs()
            , m_GroupObjVars()
            , m_Iterators()
            , m_IteratorAssigns()
            , m_HavingArgInfo()
            , m_HavingArg()
            , m_Statements()
            , m_ObjVars()
        {}
    private:
        int ExecuteObjectArray(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            ObjectArray* pArr = nullptr;
            std::shared_ptr<void> arrFromType;
            if (!m_Type.empty()) {
                pArr = BuildObjectArray(m_Type);
                arrFromType.reset(pArr);
            }
            else {
                auto& obj = Brace::VarGetObject((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
                if (nullptr != obj) {
                    auto* ptr = obj.get();
                    if (nullptr != ptr) {
                        pArr = static_cast<ObjectArray*>(ptr);
                    }
                }
            }
            if (nullptr == pArr)
                return Brace::BRACE_FLOW_CONTROL_NORMAL;
            auto* pNewArr = new ObjectArray();
            std::shared_ptr<void> newArr(pNewArr);
            // where
            ExecuteWhere(gvars, lvars, pArr, pNewArr);
            // orderby
            ExecuteOrderBy(gvars, lvars, pNewArr);
            // groupby
            if (m_GroupIteratorIndex != INVALID_INDEX) {
                std::vector<ObjectArray> groups{};
                ExecuteGroupBy(gvars, lvars, pNewArr, groups);
                return ExecuteGroupSelect(gvars, lvars, groups);
            }
            else {
                return ExecuteSelect(gvars, lvars, pNewArr);
            }
        }
        ObjectArray* BuildObjectArray(const std::string& className)const
        {
            if (className == "MemoryModifyInfo") {
                auto* p = new ObjectArray();
                auto&& system = g_pApiProvider->GetSystem();
                auto&& memorySniffer = system.MemorySniffer();
                auto&& results = memorySniffer.GetResultMemoryModifyInfo();

                for (auto&& pair : results) {
                    p->push_back(pair.second);
                }
                return p;
            }
            if (className == "LastMemoryModifyInfo") {
                auto* p = new ObjectArray();
                auto&& system = g_pApiProvider->GetSystem();
                auto&& memorySniffer = system.MemorySniffer();
                auto&& results = memorySniffer.GetLastHistoryMemoryModifyInfo();

                for (auto&& pair : results) {
                    p->push_back(pair.second);
                }
                return p;
            }
            return nullptr;
        }
        void ExecuteWhere(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const ObjectArray* pArr, ObjectArray* pNewArr)const
        {
            if (m_WhereIteratorIndex != INVALID_INDEX) {
                for (auto&& val : (*pArr)) {
                    Brace::VarSetObject(lvars, m_WhereIteratorIndex, val);
                    if (!m_WhereArg.isNull())
                        m_WhereArg(gvars, lvars);
                    bool v = Brace::VarGetBoolean((m_WhereArgInfo.IsGlobal ? gvars : lvars), m_WhereArgInfo.Type, m_WhereArgInfo.VarIndex);
                    if (v)
                        pNewArr->push_back(val);
                }
                FreeObjVars(lvars, m_WhereObjVars);
            }
            else {
                for (auto&& val : (*pArr)) {
                    pNewArr->push_back(val);
                }
            }
        }
        void ExecuteOrderBy(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, ObjectArray* pNewArr)const
        {
            if (m_OrderIteratorIndex != INVALID_INDEX) {
                std::vector<CmpVal> sortVals{};
                std::sort(pNewArr->begin(), pNewArr->end(), [&](auto& e1, auto& e2) {
                    Brace::VarSetObject(lvars, m_OrderIteratorIndex, e1);
                    for (auto&& arg : m_OrderArgs) {
                        if (!arg.isNull())
                            arg(gvars, lvars);
                    }
                    sortVals.clear();
                    for (auto&& argInfo : m_OrderArgInfos) {
                        CmpVal cv;
                        if (Brace::IsStringType(argInfo.Type))
                            cv.StrVal = Brace::VarGetString((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
                        else
                            cv.NumVal = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                        sortVals.push_back(cv);
                    }
                    Brace::VarSetObject(lvars, m_OrderIteratorIndex, e2);
                    for (auto&& arg : m_OrderArgs) {
                        if (!arg.isNull())
                            arg(gvars, lvars);
                    }
                    int ix = 0;
                    for (auto&& argInfo : m_OrderArgInfos) {
                        bool asc = m_OrderAscs[ix];
                        const CmpVal& cv = sortVals[ix++];
                        if (Brace::IsStringType(argInfo.Type)) {
                            const std::string& v2 = Brace::VarGetString((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
                            if (asc) {
                                if (cv.StrVal < v2)
                                    return true;
                                else if (cv.StrVal > v2)
                                    return false;
                            }
                            else {
                                if (cv.StrVal > v2)
                                    return true;
                                else if (cv.StrVal < v2)
                                    return false;
                            }
                        }
                        else {
                            double v2 = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                            if (asc) {
                                if (cv.NumVal < v2)
                                    return true;
                                else if (cv.NumVal > v2)
                                    return false;
                            }
                            else {
                                if (cv.NumVal > v2)
                                    return true;
                                else if (cv.NumVal < v2)
                                    return false;
                            }
                        }
                    }
                    return false;
                });
                FreeObjVars(lvars, m_OrderObjVars);
            }
        }
        void ExecuteGroupBy(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, ObjectArray* pNewArr, std::vector<ObjectArray>& groups)const
        {
            std::vector<CmpVal> groupVals{};
            auto& arr = *pNewArr;
            for (auto&& optr : arr) {
                Brace::VarSetObject(lvars, m_GroupIteratorIndex, optr);
                for (auto&& arg : m_GroupArgs) {
                    if (!arg.isNull())
                        arg(gvars, lvars);
                }
                bool newGroup = false;
                if (groupVals.empty()) {
                    newGroup = true;
                    for (auto&& argInfo : m_GroupArgInfos) {
                        CmpVal cv;
                        if (Brace::IsStringType(argInfo.Type))
                            cv.StrVal = Brace::VarGetString((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
                        else
                            cv.NumVal = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                        groupVals.push_back(cv);
                    }
                }
                else {
                    int ix = 0;
                    for (auto&& argInfo : m_GroupArgInfos) {
                        CmpVal& cv = groupVals[ix++];
                        if (Brace::IsStringType(argInfo.Type)) {
                            const std::string& v2 = Brace::VarGetString((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
                            if (cv.StrVal != v2) {
                                newGroup = true;
                                cv.StrVal = v2;
                            }
                        }
                        else {
                            double v2 = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                            if (cv.NumVal < v2 - DBL_EPSILON || cv.NumVal > v2 + DBL_EPSILON) {
                                newGroup = true;
                                cv.NumVal = v2;
                            }
                        }
                    }
                }
                if (newGroup) {
                    ObjectArray ng;
                    ng.push_back(optr);
                    groups.push_back(std::move(ng));
                }
                else {
                    groups.back().push_back(optr);
                }
            }
            FreeObjVars(lvars, m_GroupObjVars);
        }
        int ExecuteGroupSelect(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<ObjectArray>& groups)const
        {
            // top
            int64_t topn = -1;
            if (m_TopArgInfo.VarIndex != INVALID_INDEX) {
                if (!m_TopArg.isNull())
                    m_TopArg(gvars, lvars);
                topn = Brace::VarGetI64((m_TopArgInfo.IsGlobal ? gvars : lvars), m_TopArgInfo.Type, m_TopArgInfo.VarIndex);
                FreeObjVars(lvars, m_TopObjVars);
            }
            // select and having
            if (m_SelectIteratorIndex != INVALID_INDEX) {
                if (m_Statements.size() > 0) {
                    std::vector<CmpVal> selectVals{};
                    int64_t resultCount = 0;
                    for (auto&& group : groups) {
                        if (!ExecuteGroupStatAndHaving(gvars, lvars, group, selectVals)) {
                            continue;
                        }
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                        FreeObjVars(lvars, m_SelectObjVars);
                        FreeObjVars(lvars, m_ObjVars);
                        ++resultCount;
                        if (topn > 0 && resultCount >= topn)
                            break;
                    }
                }
                else {
                    //same as csvecho
                    std::vector<CmpVal> selectVals{};
                    std::stringstream ss;
                    int64_t resultCount = 0;
                    for (auto&& group : groups) {
                        if (!ExecuteGroupStatAndHaving(gvars, lvars, group, selectVals)) {
                            continue;
                        }
                        ss.str(std::string());
                        bool first = true;
                        for (auto&& cv : selectVals) {
                            bool needQuote = false;
                            if (cv.IsStr && cv.StrVal.length() > 0 && cv.StrVal[0] != '"' && cv.StrVal[0] != '\'') {
                                for (auto c : cv.StrVal) {
                                    if (c == ' ' || c == '\t') {
                                        needQuote = true;
                                        break;
                                    }
                                }
                            }
                            if (first) {
                                first = false;
                            }
                            else {
                                ss << ", ";
                            }
                            if (needQuote)
                                ss << '"' << cv.StrVal << '"';
                            else if (cv.IsStr)
                                ss << cv.StrVal;
                            else
                                ss << std::fixed << std::setprecision(3) << cv.NumVal;
                        }
                        LogInfo(ss.str());
                        FreeObjVars(lvars, m_SelectObjVars);
                        if (m_HavingArgInfo.VarIndex != INVALID_INDEX) {
                            FreeObjVars(lvars, m_ObjVars);
                        }
                        ++resultCount;
                        if (topn > 0 && resultCount >= topn)
                            break;
                    }
                }
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        bool ExecuteGroupStatAndHaving(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const ObjectArray& group, std::vector<CmpVal>& selectVals)const
        {
            selectVals.clear();
            bool first = true;
            for (auto&& optr : group) {
                Brace::VarSetObject(lvars, m_SelectIteratorIndex, optr);
                for (auto&& arg : m_SelectArgs) {
                    if (!arg.isNull())
                        arg(gvars, lvars);
                }
                bool update = false;
                int six = 0;
                for (auto&& argInfo : m_SelectArgInfos) {
                    int stat = m_SelectStats[six];
                    bool isStr = false;
                    std::string str{};
                    double val{};
                    if (Brace::IsObjectType(argInfo.Type)) {
                        auto& ptr = Brace::VarGetObject((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
                        switch (argInfo.ObjectTypeId) {
                        case CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO: {
                            auto* p = static_cast<Core::Memory::MemoryModifyInfo*>(ptr.get());
                            if (p) {
                                str = std::to_string(p->addr.GetValue());
                            }
                        }break;
                        }
                        isStr = true;
                    }
                    else if (Brace::IsStringType(argInfo.Type)) {
                        str = Brace::VarGetString((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
                        isStr = true;
                    }
                    else {
                        val = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                    }
                    if (first) {
                        CmpVal cv;
                        cv.IsStr = isStr;
                        if (isStr) {
                            switch (stat) {
                            case STAT_METHOD_NONE:
                                cv.StrVal = str;
                                break;
                            case STAT_METHOD_MAX:
                                cv.StrVal = str;
                                break;
                            case STAT_METHOD_MIN:
                                cv.StrVal = str;
                                break;
                            case STAT_METHOD_AVG:
                                break;
                            case STAT_METHOD_SUM:
                                break;
                            case STAT_METHOD_COUNT:
                                cv.IsStr = false;
                                cv.NumVal = 1;
                                break;
                            }
                        }
                        else {
                            switch (stat) {
                            case STAT_METHOD_NONE:
                                cv.NumVal = val;
                                break;
                            case STAT_METHOD_MAX:
                                cv.NumVal = val;
                                break;
                            case STAT_METHOD_MIN:
                                cv.NumVal = val;
                                break;
                            case STAT_METHOD_AVG:
                                cv.NumVal = val;
                                break;
                            case STAT_METHOD_SUM:
                                cv.NumVal = val;
                                break;
                            case STAT_METHOD_COUNT:
                                cv.NumVal = 1;
                                break;
                            }
                        }
                        selectVals.push_back(std::move(cv));
                    }
                    else {
                        CmpVal& cv = selectVals[six];
                        if (isStr) {
                            switch (stat) {
                            case STAT_METHOD_NONE:
                                cv.NewStrVal = str;
                                break;
                            case STAT_METHOD_MAX:
                                if (cv.StrVal < str) {
                                    cv.StrVal = str;
                                    update = true;
                                }
                                break;
                            case STAT_METHOD_MIN:
                                if (cv.StrVal > str) {
                                    cv.StrVal = str;
                                    update = true;
                                }
                                break;
                            case STAT_METHOD_AVG:
                                break;
                            case STAT_METHOD_SUM:
                                break;
                            case STAT_METHOD_COUNT:
                                cv.NumVal = cv.NumVal + 1;
                                break;
                            }
                        }
                        else {
                            switch (stat) {
                            case STAT_METHOD_NONE:
                                cv.NewNumVal = val;
                                break;
                            case STAT_METHOD_MAX:
                                if (cv.NumVal < val) {
                                    cv.NumVal = val;
                                    update = true;
                                }
                                break;
                            case STAT_METHOD_MIN:
                                if (cv.NumVal > val) {
                                    cv.NumVal = val;
                                    update = true;
                                }
                                break;
                            case STAT_METHOD_AVG:
                                cv.NumVal = cv.NumVal + val;
                                break;
                            case STAT_METHOD_SUM:
                                cv.NumVal = cv.NumVal + val;
                                break;
                            case STAT_METHOD_COUNT:
                                cv.NumVal = cv.NumVal + 1;
                                break;
                            }
                        }
                    }
                    ++six;
                }
                if (update) {
                    six = 0;
                    for (auto&& cv : selectVals) {
                        int stat = m_SelectStats[six++];
                        if (stat == STAT_METHOD_NONE) {
                            if (cv.IsStr)
                                cv.StrVal = cv.NewStrVal;
                            else
                                cv.NumVal = cv.NewNumVal;
                        }
                    }
                }
                first = false;
            }
            int vix = 0;
            for (auto&& cv : selectVals) {
                int stat = m_SelectStats[vix];
                auto& itInfo = m_Iterators[vix++];
                if (stat == STAT_METHOD_AVG) {
                    cv.NumVal /= group.size();
                }
                if (Brace::IsStringType(itInfo.Type))
                    Brace::VarSetString(lvars, itInfo.VarIndex, cv.StrVal);
                else
                    Brace::VarSetF64(lvars, itInfo.Type, itInfo.VarIndex, cv.NumVal);
            }
            if (m_HavingArgInfo.VarIndex != INVALID_INDEX) {
                if (!m_HavingArg.isNull())
                    m_HavingArg(gvars, lvars);
                bool v = Brace::VarGetBoolean((m_HavingArgInfo.IsGlobal ? gvars : lvars), m_HavingArgInfo.Type, m_HavingArgInfo.VarIndex);
                if (!v) {
                    FreeObjVars(lvars, m_SelectObjVars);
                    return false;
                }
            }
            return true;
        }
        int ExecuteSelect(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, ObjectArray* pNewArr)const
        {
            // top
            if (m_TopArgInfo.VarIndex != INVALID_INDEX) {
                if (!m_TopArg.isNull())
                    m_TopArg(gvars, lvars);
                int64_t n = Brace::VarGetI64((m_TopArgInfo.IsGlobal ? gvars : lvars), m_TopArgInfo.Type, m_TopArgInfo.VarIndex);
                auto& arr = *pNewArr;
                if (static_cast<int64_t>(arr.size()) > n) {
                    auto it = arr.begin();
                    std::advance(it, n);
                    arr.erase(it, arr.end());
                }
                FreeObjVars(lvars, m_TopObjVars);
            }
            // select
            if (m_SelectIteratorIndex != INVALID_INDEX) {
                if (m_Statements.size() > 0) {
                    auto& arr = *pNewArr;
                    for (auto&& optr : arr) {
                        Brace::VarSetObject(lvars, m_SelectIteratorIndex, optr);
                        for (auto&& arg : m_SelectArgs) {
                            if (!arg.isNull())
                                arg(gvars, lvars);
                        }
                        int itIndex = 0;
                        for (auto&& argInfo : m_SelectArgInfos) {
                            int vix = m_Iterators[itIndex].VarIndex;
                            auto* fptr = m_IteratorAssigns[itIndex++];
                            (*fptr)(lvars, vix, lvars, argInfo.VarIndex);
                        }
                        for (auto&& statement : m_Statements) {
                            int v = statement(gvars, lvars);
                            if (IsForceQuit()) {
                                FreeObjVars(lvars, m_SelectObjVars);
                                FreeObjVars(lvars, m_ObjVars);
                                return v;
                            }
                            if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                                break;
                            }
                            else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                                FreeObjVars(lvars, m_SelectObjVars);
                                FreeObjVars(lvars, m_ObjVars);
                                if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                                    return Brace::BRACE_FLOW_CONTROL_NORMAL;
                                return v;
                            }
                        }
                        FreeObjVars(lvars, m_SelectObjVars);
                        FreeObjVars(lvars, m_ObjVars);
                    }
                }
                else {
                    //same as csvecho
                    auto& arr = *pNewArr;
                    for (auto&& optr : arr) {
                        Brace::VarSetObject(lvars, m_SelectIteratorIndex, optr);
                        for (auto&& arg : m_SelectArgs) {
                            if (!arg.isNull())
                                arg(gvars, lvars);
                        }
                        std::stringstream ss;
                        bool first = true;
                        for (auto&& argInfo : m_SelectArgInfos) {
                            std::string str;
                            if (Brace::IsObjectType(argInfo.Type)) {
                                auto& ptr = Brace::VarGetObject((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
                                switch (argInfo.ObjectTypeId) {
                                case CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO: {
                                    auto* p = static_cast<Core::Memory::MemoryModifyInfo*>(ptr.get());
                                    if (p) {
                                        str = std::to_string(p->addr.GetValue());
                                    }
                                }break;
                                }
                            }
                            else if (Brace::IsFloatType(argInfo.Type)) {
                                double dv;
                                if (argInfo.IsGlobal)
                                    dv = Brace::VarGetF64(gvars, argInfo.Type, argInfo.VarIndex);
                                else
                                    dv = Brace::VarGetF64(lvars, argInfo.Type, argInfo.VarIndex);
                                std::stringstream tss;
                                tss << std::fixed << std::setprecision(3) << dv;
                                str = tss.str();
                            }
                            else {
                                if (argInfo.IsGlobal)
                                    str = Brace::VarGetStr(gvars, argInfo.Type, argInfo.VarIndex);
                                else
                                    str = Brace::VarGetStr(lvars, argInfo.Type, argInfo.VarIndex);
                            }
                            bool needQuote = false;
                            if (str.length() > 0 && str[0] != '"' && str[0] != '\'') {
                                for (auto c : str) {
                                    if (c == ' ' || c == '\t') {
                                        needQuote = true;
                                        break;
                                    }
                                }
                            }
                            if (first) {
                                first = false;
                            }
                            else {
                                ss << ", ";
                            }
                            if (needQuote)
                                ss << '"' << str << '"';
                            else
                                ss << str;
                        }
                        LogInfo(ss.str());
                        FreeObjVars(lvars, m_SelectObjVars);
                    }
                }
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    private:
        std::string m_Type;
        Brace::BraceApiExecutor m_List;
        Brace::OperandRuntimeInfo m_ListInfo;

        int m_SelectIteratorIndex;
        std::vector<Brace::OperandRuntimeInfo> m_SelectArgInfos;
        std::vector<Brace::BraceApiExecutor> m_SelectArgs;
        std::vector<int> m_SelectStats;
        std::vector<int> m_SelectObjVars;

        Brace::OperandRuntimeInfo m_TopArgInfo;
        Brace::BraceApiExecutor m_TopArg;
        std::vector<int> m_TopObjVars;

        int m_WhereIteratorIndex;
        Brace::OperandRuntimeInfo m_WhereArgInfo;
        Brace::BraceApiExecutor m_WhereArg;
        std::vector<int> m_WhereObjVars;

        int m_OrderIteratorIndex;
        std::vector<Brace::OperandRuntimeInfo> m_OrderArgInfos;
        std::vector<Brace::BraceApiExecutor> m_OrderArgs;
        std::vector<bool> m_OrderAscs;
        std::vector<int> m_OrderObjVars;

        int m_GroupIteratorIndex;
        std::vector<Brace::OperandRuntimeInfo> m_GroupArgInfos;
        std::vector<Brace::BraceApiExecutor> m_GroupArgs;
        std::vector<int> m_GroupObjVars;

        std::vector<Brace::OperandRuntimeInfo> m_Iterators;
        std::vector<Brace::VarAssignPtr> m_IteratorAssigns;

        Brace::OperandRuntimeInfo m_HavingArgInfo;
        Brace::BraceApiExecutor m_HavingArg;

        std::vector<Brace::BraceApiExecutor> m_Statements;
        std::vector<int> m_ObjVars;
    };

    class StringMemberCallProvider final : public AbstractMemberCallApiProvider
    {
        friend class MemberCallExp;
    protected:
        virtual bool LoadMemberCall(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& objInfo, Brace::BraceApiExecutor&& obj, std::string&& member, std::vector<Brace::OperandLoadtimeInfo>&& argInfos, std::vector<Brace::BraceApiExecutor>&& args, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor)override
        {
            std::swap(m_Obj, obj);
            m_ObjInfo = objInfo;
            std::swap(m_Member, member);
            std::swap(m_Args, args);
            for (auto&& argInfo : argInfos) {
                m_ArgInfos.push_back(argInfo);
            }
            auto& m = m_Member;
            if (m == "replace_all") {
                if (argInfos.size() == 2) {
                    auto& argInfo = argInfos[0];
                    auto& argInfo2 = argInfos[1];
                    if (Brace::IsStringType(argInfo.Type) && Brace::IsStringType(argInfo2.Type)) {
                        resultInfo = objInfo;
                        executor.attach(this, &StringMemberCallProvider::ExecuteReplaceAll);
                        return true;
                    }
                }
                std::stringstream ss;
                ss << "expected String.replace_all(string, string) ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return false;
        }
    private:
        int ExecuteReplaceAll(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            for (auto&& _arg : m_Args) {
                if (!_arg.isNull())
                    _arg(gvars, lvars);
            }
            auto& strInfo = m_ObjInfo;
            auto& strInfo1 = m_ArgInfos[0];
            auto& strInfo2 = m_ArgInfos[1];
            std::string& str = Brace::VarGetString((strInfo.IsGlobal ? gvars : lvars), strInfo.VarIndex);
            const std::string& what = Brace::VarGetString((strInfo1.IsGlobal ? gvars : lvars), strInfo1.VarIndex);
            const std::string& with = Brace::VarGetString((strInfo2.IsGlobal ? gvars : lvars), strInfo2.VarIndex);
            for (std::string::size_type pos{}; str.npos != (pos = str.find(what.data(), pos, what.length())); pos += with.length()) {
                str.replace(pos, what.length(), with.data(), with.length());
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    private:
        StringMemberCallProvider(Brace::BraceScript& interpreter) :AbstractMemberCallApiProvider(interpreter), m_Obj(), m_ObjInfo(), m_Member(), m_Args(), m_ArgInfos(), m_ResultInfo()
        {
        }
    private:
        Brace::BraceApiExecutor m_Obj;
        Brace::OperandRuntimeInfo m_ObjInfo;
        std::string m_Member;
        std::vector<Brace::BraceApiExecutor> m_Args;
        std::vector<Brace::OperandRuntimeInfo> m_ArgInfos;
        Brace::OperandRuntimeInfo m_ResultInfo;
    };
    class StringMemberSetProvider final : public AbstractMemberSetApiProvider
    {
        friend class MemberSetExp;
    protected:
        virtual bool LoadMemberSet(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& objInfo, Brace::BraceApiExecutor&& obj, std::string&& member, Brace::OperandLoadtimeInfo&& argInfo, Brace::BraceApiExecutor&& arg, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor)override
        {
            return false;
        }
    private:
        StringMemberSetProvider(Brace::BraceScript& interpreter) :AbstractMemberSetApiProvider(interpreter)
        {}
    };
    class StringMemberGetProvider final : public AbstractMemberGetApiProvider
    {
        friend class MemberGetExp;
    protected:
        virtual bool LoadMemberGet(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& objInfo, Brace::BraceApiExecutor&& obj, std::string&& member, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor)override
        {
            return false;
        }
    private:
        StringMemberGetProvider(Brace::BraceScript& interpreter) :AbstractMemberGetApiProvider(interpreter)
        {}
    };
    class StringCollectionCallProvider final : public AbstractCollectionCallApiProvider
    {
        friend class CollectionCallExp;
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) const override
        {
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo) const override
        {
        }
    private:
        StringCollectionCallProvider(Brace::BraceScript& interpreter) :AbstractCollectionCallApiProvider(interpreter)
        {}
    };
    class StringCollectionSetProvider final : public AbstractCollectionSetApiProvider
    {
        friend class CollectionSetExp;
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, const Brace::OperandLoadtimeInfo& arr, const Brace::OperandLoadtimeInfo& ix, const Brace::OperandLoadtimeInfo& val, Brace::OperandLoadtimeInfo& resultInfo) const override
        {
            if (!(ix.Type >= Brace::BRACE_DATA_TYPE_INT8 && ix.Type <= Brace::BRACE_DATA_TYPE_UINT64)) {
                std::stringstream ss;
                ss << "String's index must be integer ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            if (!CanAssign(Brace::BRACE_DATA_TYPE_UINT8, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ, val.Type, val.ObjectTypeId)) {
                std::stringstream ss;
                ss << "String element's type and val type dismatch ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo = val;
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& arr, const Brace::OperandRuntimeInfo& ix, const Brace::OperandRuntimeInfo& val, const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            std::string& str = Brace::VarGetString((arr.IsGlobal ? gvars : lvars), arr.VarIndex);
            int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
            uint64_t v = Brace::VarGetU64((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
            if (vix >= 0 && vix < static_cast<int64_t>(str.length())) {
                str.replace(static_cast<size_t>(vix), 1, 1, static_cast<char>(static_cast<uint8_t>(v)));
            }
        }
    private:
        StringCollectionSetProvider(Brace::BraceScript& interpreter) :AbstractCollectionSetApiProvider(interpreter)
        {}
    };
    class StringCollectionGetProvider final : public AbstractCollectionGetApiProvider
    {
        friend class CollectionGetExp;
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, const Brace::OperandLoadtimeInfo& arr, const Brace::OperandLoadtimeInfo& ix, Brace::OperandLoadtimeInfo& resultInfo) const override
        {
            if (!(ix.Type >= Brace::BRACE_DATA_TYPE_INT8 && ix.Type <= Brace::BRACE_DATA_TYPE_UINT64)) {
                std::stringstream ss;
                ss << "String's index must be integer ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT8;
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& arr, const Brace::OperandRuntimeInfo& ix, const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            const std::string& str = Brace::VarGetString((arr.IsGlobal ? gvars : lvars), arr.VarIndex);
            int64_t vix = Brace::VarGetI64((ix.IsGlobal ? gvars : lvars), ix.Type, ix.VarIndex);
            char v = 0;
            if (vix >= 0 && vix < static_cast<int64_t>(str.length())) {
                v = str[vix];
            }
            Brace::VarSetUInt8((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, static_cast<uint8_t>(v));
        }
    private:
        StringCollectionGetProvider(Brace::BraceScript& interpreter) :AbstractCollectionGetApiProvider(interpreter)
        {}
    };
    class StringLoopListProvider final : public AbstractLoopListApiProvider
    {
        friend class LoopListExp;
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::ISyntaxComponent& data, const BraceObjectInfo& braceObjInfo, const Brace::OperandLoadtimeInfo& listInfo, Brace::BraceApiExecutor& executor) override
        {
            return TypeInference(listInfo, executor);
        }
        virtual void StoreRuntimeInfo(Brace::OperandRuntimeInfo&& listInfo, Brace::BraceApiExecutor&& list, std::vector<Brace::BraceApiExecutor>&& statements, const std::vector<int>& objVars) override
        {
            std::swap(m_ListInfo, listInfo);
            std::swap(m_List, list);
            std::swap(m_Statements, statements);
            m_ObjVars = objVars;
        }
    private:
        StringLoopListProvider(Brace::BraceScript& interpreter) :AbstractLoopListApiProvider(interpreter), m_IteratorIndex(INVALID_INDEX), m_List(), m_ListInfo(), m_Statements(), m_ObjVars()
        {}
    private:
        bool TypeInference(const Brace::OperandLoadtimeInfo& listInfo, Brace::BraceApiExecutor& executor)
        {
            if (listInfo.Type == Brace::BRACE_DATA_TYPE_STRING) {
                m_IteratorIndex = AllocVariable("$$", Brace::BRACE_DATA_TYPE_UINT8, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                executor.attach(this, &StringLoopListProvider::Execute);
                return true;
            }
            return false;
        }
        int Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_List.isNull())
                m_List(gvars, lvars);
            auto& str = Brace::VarGetString((m_ListInfo.IsGlobal ? gvars : lvars), m_ListInfo.VarIndex);
            for (char val : str) {
                Brace::VarSetUInt8(lvars, m_IteratorIndex, static_cast<uint8_t>(val));
                for (auto&& statement : m_Statements) {
                    int v = statement(gvars, lvars);
                    if (IsForceQuit()) {
                        FreeObjVars(lvars, m_ObjVars);
                        return v;
                    }
                    if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                        break;
                    }
                    else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                        FreeObjVars(lvars, m_ObjVars);
                        if (v == Brace::BRACE_FLOW_CONTROL_BREAK)
                            return Brace::BRACE_FLOW_CONTROL_NORMAL;
                        return v;
                    }
                }
            }
            FreeObjVars(lvars, m_ObjVars);
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    private:
        int m_IteratorIndex;
        Brace::BraceApiExecutor m_List;
        Brace::OperandRuntimeInfo m_ListInfo;
        std::vector<Brace::BraceApiExecutor> m_Statements;
        std::vector<int> m_ObjVars;
    };

    class MemberCallExp final : public Brace::AbstractBraceApi
    {
    public:
        MemberCallExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter), m_ApiProvider()
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            int num = data.GetParamNum();
            if (num < 2)
                return false;
            Brace::OperandLoadtimeInfo loadInfo;
            auto obj = LoadHelper(*data.GetParam(0), loadInfo);
            auto& objInfo = loadInfo;
            auto& m = data.GetParamId(1);
            auto member = m;
            std::vector<Brace::OperandLoadtimeInfo> argInfos;
            std::vector<Brace::BraceApiExecutor> args;
            Brace::OperandLoadtimeInfo firstArgInfo;
            for (int ix = 2; ix < num; ++ix) {
                auto* param = data.GetParam(ix);
                Brace::OperandLoadtimeInfo argLoadInfo;
                auto p = LoadHelper(*param, argLoadInfo);
                args.push_back(std::move(p));
                argInfos.push_back(argLoadInfo);
                if (ix == 2)
                    firstArgInfo = std::move(argLoadInfo);
            }
            if (objInfo.Type == Brace::BRACE_DATA_TYPE_OBJECT) {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(loadInfo.ObjectTypeId);
                if (nullptr != pInfo) {
                    AbstractMemberCallApiProvider* pProvider = nullptr;
                    switch (pInfo->ObjectCategory) {
                    case BRACE_OBJECT_CATEGORY_SPECIAL:
                        pProvider = new CppObjectMemberCallProvider(GetInterpreter());
                        break;
                    case BRACE_OBJECT_CATEGORY_STRUCT:
                        pProvider = new StructMemberCallProvider(GetInterpreter());
                        break;
                    default:
                        pProvider = new ArrayHashtableMemberCallProvider(GetInterpreter());
                        break;
                    }
                    if (pProvider) {
                        m_ApiProvider.reset(pProvider);
                        return pProvider->LoadMemberCall(func, data, *pInfo, std::move(objInfo), std::move(obj), std::move(member), std::move(argInfos), std::move(args), resultInfo, executor);
                    }
                }
            }
            else if (objInfo.Type == Brace::BRACE_DATA_TYPE_STRING) {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_STRING);
                if (nullptr != pInfo) {
                    AbstractMemberCallApiProvider* pProvider = new StringMemberCallProvider(GetInterpreter());
                    if (pProvider) {
                        m_ApiProvider.reset(pProvider);
                        return pProvider->LoadMemberCall(func, data, *pInfo, std::move(objInfo), std::move(obj), std::move(member), std::move(argInfos), std::move(args), resultInfo, executor);
                    }
                }
            }
            std::stringstream ss;
            ss << "Unknown member " << m << " line: " << data.GetLine();
            LogError(ss.str());
            executor = nullptr;
            return false;
        }
    private:
        std::unique_ptr<AbstractMemberCallApiProvider> m_ApiProvider;
    };
    class MemberSetExp final : public Brace::AbstractBraceApi
    {
    public:
        MemberSetExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter), m_ApiProvider()
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            if (data.GetParamNum() != 3)
                return false;
            Brace::OperandLoadtimeInfo loadInfo;
            auto obj = LoadHelper(*data.GetParam(0), loadInfo);
            auto& objInfo = loadInfo;
            auto& m = data.GetParamId(1);
            auto member = m;
            auto* param = data.GetParam(2);
            Brace::OperandLoadtimeInfo argLoadInfo;
            auto p = LoadHelper(*param, argLoadInfo);
            if (objInfo.Type == Brace::BRACE_DATA_TYPE_OBJECT) {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(loadInfo.ObjectTypeId);
                if (nullptr != pInfo) {
                    AbstractMemberSetApiProvider* pProvider = nullptr;
                    switch (pInfo->ObjectCategory) {
                    case BRACE_OBJECT_CATEGORY_SPECIAL:
                        pProvider = new CppObjectMemberSetProvider(GetInterpreter());
                        break;
                    case BRACE_OBJECT_CATEGORY_STRUCT:
                        pProvider = new StructMemberSetProvider(GetInterpreter());
                        break;
                    default:
                        pProvider = new ArrayHashtableMemberSetProvider(GetInterpreter());
                        break;
                    }
                    if (pProvider) {
                        m_ApiProvider.reset(pProvider);
                        return pProvider->LoadMemberSet(func, data, *pInfo, std::move(objInfo), std::move(obj), std::move(member), std::move(argLoadInfo), std::move(p), resultInfo, executor);
                    }
                }
            }
            else if (objInfo.Type == Brace::BRACE_DATA_TYPE_STRING) {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_STRING);
                if (nullptr != pInfo) {
                    AbstractMemberSetApiProvider* pProvider = new StringMemberSetProvider(GetInterpreter());
                    if (pProvider) {
                        m_ApiProvider.reset(pProvider);
                        return pProvider->LoadMemberSet(func, data, *pInfo, std::move(objInfo), std::move(obj), std::move(member), std::move(argLoadInfo), std::move(p), resultInfo, executor);
                    }
                }
            }
            executor = nullptr;
            return true;
        }
    private:
        std::unique_ptr<AbstractMemberSetApiProvider> m_ApiProvider;
    };
    class MemberGetExp final : public Brace::AbstractBraceApi
    {
    public:
        MemberGetExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter), m_ApiProvider()
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            if (data.GetParamNum() != 2)
                return false;
            Brace::OperandLoadtimeInfo loadInfo;
            auto obj = LoadHelper(*data.GetParam(0), loadInfo);
            auto& objInfo = loadInfo;
            auto& m = data.GetParamId(1);
            auto member = m;
            if (objInfo.Type == Brace::BRACE_DATA_TYPE_OBJECT) {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(loadInfo.ObjectTypeId);
                if (nullptr != pInfo) {
                    AbstractMemberGetApiProvider* pProvider = nullptr;
                    switch (pInfo->ObjectCategory) {
                    case BRACE_OBJECT_CATEGORY_SPECIAL:
                        pProvider = new CppObjectMemberGetProvider(GetInterpreter());
                        break;
                    case BRACE_OBJECT_CATEGORY_STRUCT:
                        pProvider = new StructMemberGetProvider(GetInterpreter());
                        break;
                    default:
                        pProvider = new ArrayHashtableMemberGetProvider(GetInterpreter());
                        break;
                    }
                    if (pProvider) {
                        m_ApiProvider.reset(pProvider);
                        return pProvider->LoadMemberGet(func, data, *pInfo, std::move(objInfo), std::move(obj), std::move(member), resultInfo, executor);
                    }
                }
            }
            else if (objInfo.Type == Brace::BRACE_DATA_TYPE_STRING) {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_STRING);
                if (nullptr != pInfo) {
                    AbstractMemberGetApiProvider* pProvider = new StringMemberGetProvider(GetInterpreter());
                    if (pProvider) {
                        m_ApiProvider.reset(pProvider);
                        return pProvider->LoadMemberGet(func, data, *pInfo, std::move(objInfo), std::move(obj), std::move(member), resultInfo, executor);
                    }
                }
            }
            std::stringstream ss;
            ss << "Unknown member " << m << " line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
    private:
        std::unique_ptr<AbstractMemberGetApiProvider> m_ApiProvider;
    };
    class CollectionCallExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CollectionCallExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter), m_ApiProvider()
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            /// how to interpret this, f = obj[m] and f(args) or obj[m](obj, args) ?
            if (argInfos.size() < 2)
                return false;
            auto& arr = argInfos[0];
            if (arr.Type == Brace::BRACE_DATA_TYPE_OBJECT) {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(arr.ObjectTypeId);
                if (nullptr != pInfo) {
                    AbstractCollectionCallApiProvider* pProvider = nullptr;
                    switch (pInfo->ObjectCategory) {
                    case BRACE_OBJECT_CATEGORY_SPECIAL:
                    default:
                        pProvider = new ArrayHashtableCollectionCallProvider(GetInterpreter());
                        break;
                    }
                    if (pProvider) {
                        m_ApiProvider.reset(pProvider);
                        return pProvider->TypeInference(func, data, *pInfo, argInfos, resultInfo);
                    }
                }
            }
            else if (arr.Type == Brace::BRACE_DATA_TYPE_STRING) {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_STRING);
                if (nullptr != pInfo) {
                    AbstractCollectionCallApiProvider* pProvider = new StringCollectionCallProvider(GetInterpreter());
                    if (pProvider) {
                        m_ApiProvider.reset(pProvider);
                        return pProvider->TypeInference(func, data, *pInfo, argInfos, resultInfo);
                    }
                }
            }
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            /// how to interpret this, f = obj[m] and f(args) or obj[m](obj, args) ?
            m_ApiProvider->Execute(gvars, lvars, argInfos, resultInfo);
        }
    private:
        std::unique_ptr<AbstractCollectionCallApiProvider> m_ApiProvider;
    };
    class CollectionSetExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CollectionSetExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter), m_ApiProvider()
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 3)
                return false;
            auto& arr = argInfos[0];
            auto& ix = argInfos[1];
            auto& val = argInfos[2];
            if (arr.Type == Brace::BRACE_DATA_TYPE_OBJECT) {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(arr.ObjectTypeId);
                if (nullptr != pInfo) {
                    AbstractCollectionSetApiProvider* pProvider = nullptr;
                    switch (pInfo->ObjectCategory) {
                    case BRACE_OBJECT_CATEGORY_SPECIAL:
                    default:
                        pProvider = new ArrayHashtableCollectionSetProvider(GetInterpreter());
                        break;
                    }
                    if (pProvider) {
                        m_ApiProvider.reset(pProvider);
                        return pProvider->TypeInference(func, data, *pInfo, arr, ix, val, resultInfo);
                    }
                }
            }
            else if (arr.Type == Brace::BRACE_DATA_TYPE_STRING) {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_STRING);
                if (nullptr != pInfo) {
                    AbstractCollectionSetApiProvider* pProvider = new StringCollectionSetProvider(GetInterpreter());
                    if (pProvider) {
                        m_ApiProvider.reset(pProvider);
                        return pProvider->TypeInference(func, data, *pInfo, arr, ix, val, resultInfo);
                    }
                }
            }
            std::stringstream ss;
            ss << "Unknown collection type ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& arr = argInfos[0];
            auto& ix = argInfos[1];
            auto& val = argInfos[2];
            m_ApiProvider->Execute(gvars, lvars, arr, ix, val, resultInfo);
        }
    private:
        std::unique_ptr<AbstractCollectionSetApiProvider> m_ApiProvider;
    };
    class CollectionGetExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CollectionGetExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter), m_ApiProvider()
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 2)
                return false;
            auto& arr = argInfos[0];
            auto& ix = argInfos[1];
            if (arr.Type == Brace::BRACE_DATA_TYPE_OBJECT) {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(arr.ObjectTypeId);
                if (nullptr != pInfo) {
                    AbstractCollectionGetApiProvider* pProvider = nullptr;
                    switch (pInfo->ObjectCategory) {
                    case BRACE_OBJECT_CATEGORY_SPECIAL:
                    default:
                        pProvider = new ArrayHashtableCollectionGetProvider(GetInterpreter());
                        break;
                    }
                    if (pProvider) {
                        m_ApiProvider.reset(pProvider);
                        return pProvider->TypeInference(func, data, *pInfo, arr, ix, resultInfo);
                    }
                }
            }
            else if (arr.Type == Brace::BRACE_DATA_TYPE_STRING) {
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_STRING);
                if (nullptr != pInfo) {
                    AbstractCollectionGetApiProvider* pProvider = new StringCollectionGetProvider(GetInterpreter());
                    if (pProvider) {
                        m_ApiProvider.reset(pProvider);
                        return pProvider->TypeInference(func, data, *pInfo, arr, ix, resultInfo);
                    }
                }
            }
            std::stringstream ss;
            ss << "Unknown collection type ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& arr = argInfos[0];
            auto& ix = argInfos[1];
            m_ApiProvider->Execute(gvars, lvars, arr, ix, resultInfo);
        }
    private:
        std::unique_ptr<AbstractCollectionGetApiProvider> m_ApiProvider;
    };
    class LambdaExp final : public Brace::AbstractBraceApi
    {
    public:
        LambdaExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter)
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //(args) => {...}; or (args)int => {...}; or [...](args) => {...}; or [...](args)int => {...};
            bool hasError = true;
            if (hasError) {
                std::stringstream ss;
                ss << "lambda syntax error, line " << data.GetLine();
                LogError(ss.str());
            }
            return false;
        }
    };
    class LinqExp final : public Brace::AbstractBraceApi
    {
    public:
        LinqExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter)
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //objs.where(condition) => linq(objs, "where", condition)
            //objs.orderby(fields) => linq(objs, "orderby", fields)
            //objs.orderbydesc(fields) => linq(objs, "orderbydesc", fields)
            //objs.top(count) => linq(objs, "top", count)
            int pnum = data.GetParamNum();
            if (pnum > 2) {
                Brace::OperandLoadtimeInfo listInfo;
                Brace::BraceApiExecutor list = LoadHelper(*data.GetParam(0), listInfo);
                std::string mid = data.GetParamId(1);
                BraceObjectInfo* pInfo = nullptr;
                AbstractLinqApiProvider* pProvider = nullptr;
                if (listInfo.Type == Brace::BRACE_DATA_TYPE_OBJECT) {
                    pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(listInfo.ObjectTypeId);
                    if (nullptr != pInfo && pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_OBJ_ARRAY) {
                        pProvider = new ArrayHashtableLinqProvider(GetInterpreter());
                    }
                }
                if (nullptr != pInfo && nullptr != pProvider) {
                    PushBlock();
                    int iteratorIndex = INVALID_INDEX;
                    if (mid != "top") {
                        iteratorIndex = AllocVariable("$$", Brace::BRACE_DATA_TYPE_OBJECT, pInfo->GetTypeParamObjTypeId(0));
                    }
                    std::vector<Brace::OperandLoadtimeInfo> argInfos;
                    std::vector<Brace::BraceApiExecutor> args;
                    for (int ix = 2; ix < pnum; ++ix) {
                        auto* param = data.GetParam(ix);
                        Brace::OperandLoadtimeInfo argLoadInfo;
                        auto p = LoadHelper(*param, argLoadInfo);
                        argInfos.push_back(std::move(argLoadInfo));
                        args.push_back(std::move(p));
                    }
                    auto objVars = CurBlockObjVars();
                    PopBlock();
                    if (nullptr != pProvider) {
                        bool ret = pProvider->LoadLinqCall(func, data, *pInfo, iteratorIndex, std::move(listInfo), std::move(list), std::move(mid), std::move(argInfos), std::move(args), std::move(objVars), resultInfo, executor);
                        return ret;
                    }
                }
            }
            std::stringstream ss;
            ss << "linq syntax error, line " << data.GetLine();
            LogError(ss.str());
            return false;
        }
    private:
        std::unique_ptr<AbstractLinqApiProvider> m_ApiProvider;
    };
    class SelectExp final : public Brace::AbstractBraceApi
    {
    public:
        SelectExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter)
        {
        }
    protected:
        virtual bool LoadStatement(const Brace::FuncInfo& func, const DslData::StatementData& statementData, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //select(fields)top(10)from(objlist)where(exp)oderby(exps)groupby(exps)having(exp){statements;};
            std::string uobjArrKey = "array<:uobject:>";
            int uobjArrTypeId = g_ObjectInfoMgr.GetObjectTypeId(uobjArrKey);

            int fnum = statementData.GetFunctionNum();
            BraceObjectInfo* pInfo = nullptr;
            AbstractSelectApiProvider* pProvider = new ArrayHashtableSelectProvider(GetInterpreter());
            for (int ix = 0; ix < fnum; ++ix) {
                DslData::FunctionData* pFuncData = statementData.GetFunction(ix)->AsFunction();
                if (nullptr != pFuncData) {
                    const std::string& fid = pFuncData->GetId();
                    DslData::FunctionData* pCallData = pFuncData;
                    if (pFuncData->IsHighOrder()) {
                        pCallData = &pFuncData->GetLowerOrderFunction();
                        if (ix != fnum - 1)
                            return false;
                    }
                    if (fid == "from") {
                        DslData::ISyntaxComponent* pSyntax = pCallData->GetParam(0);
                        if (pSyntax->GetSyntaxType() == DslData::ISyntaxComponent::TYPE_VALUE) {
                            pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(uobjArrTypeId);
                            pProvider->LoadFromType(func, *pFuncData, *pInfo, pSyntax->GetId());
                        }
                        else {
                            Brace::OperandLoadtimeInfo listInfo;
                            Brace::BraceApiExecutor list;
                            list = LoadHelper(*pCallData->GetParam(0), listInfo);
                            if (listInfo.Type == Brace::BRACE_DATA_TYPE_OBJECT) {
                                pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(listInfo.ObjectTypeId);
                            }
                            else {
                                pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(uobjArrTypeId);
                            }
                            pProvider->LoadFromList(func, *pCallData, *pInfo, std::move(listInfo), std::move(list));
                        }
                        break;
                    }
                }
            }
            DslData::FunctionData* pHavingCallData = nullptr;
            std::vector<Brace::DataTypeInfo> selectItTypes;
            for (int fix = 0; fix < fnum; ++fix) {
                DslData::FunctionData* pFuncData = statementData.GetFunction(fix)->AsFunction();
                if (nullptr != pFuncData) {
                    const std::string& fid = pFuncData->GetId();
                    DslData::FunctionData* pCallData = pFuncData;
                    if (pFuncData->IsHighOrder()) {
                        pCallData = &pFuncData->GetLowerOrderFunction();
                        if (fix != fnum - 1)
                            return false;
                    }
                    if (fid == "select") {
                        PushBlock();
                        int iteratorIndex = AllocVariable("$$", Brace::BRACE_DATA_TYPE_OBJECT, nullptr != pInfo ? pInfo->GetTypeParamObjTypeId(0) : CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO);
                        std::vector<Brace::OperandLoadtimeInfo> argInfos;
                        std::vector<Brace::BraceApiExecutor> args;
                        std::vector<int> stats;
                        int pnum = pCallData->GetParamNum();
                        for (int pix = 0; pix < pnum; ++pix) {
                            auto* param = pCallData->GetParam(pix);
                            int stat = AbstractSelectApiProvider::STAT_METHOD_NONE;
                            if (param->GetSyntaxType() == DslData::ISyntaxComponent::TYPE_FUNCTION) {
                                auto* pParamFunc = static_cast<DslData::FunctionData*>(param);
                                const std::string& pid = param->GetId();
                                if (pid == "max") {
                                    stat = AbstractSelectApiProvider::STAT_METHOD_MAX;
                                    param = pParamFunc->GetParam(0);
                                }
                                else if (pid == "min") {
                                    stat = AbstractSelectApiProvider::STAT_METHOD_MIN;
                                    param = pParamFunc->GetParam(0);
                                }
                                else if (pid == "sum") {
                                    stat = AbstractSelectApiProvider::STAT_METHOD_SUM;
                                    param = pParamFunc->GetParam(0);
                                }
                                else if (pid == "avg") {
                                    stat = AbstractSelectApiProvider::STAT_METHOD_AVG;
                                    param = pParamFunc->GetParam(0);
                                }
                                else if (pid == "count") {
                                    stat = AbstractSelectApiProvider::STAT_METHOD_COUNT;
                                    param = pParamFunc->GetParam(0);
                                }
                            }
                            Brace::OperandLoadtimeInfo argLoadInfo;
                            auto p = LoadHelper(*param, argLoadInfo);
                            switch (stat) {
                            case AbstractSelectApiProvider::STAT_METHOD_COUNT:
                                selectItTypes.push_back(Brace::DataTypeInfo(Brace::BRACE_DATA_TYPE_INT32, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ));
                                break;
                            case AbstractSelectApiProvider::STAT_METHOD_NONE:
                            default:
                                selectItTypes.push_back(argLoadInfo);
                                break;
                            }
                            argInfos.push_back(std::move(argLoadInfo));
                            args.push_back(std::move(p));
                            stats.push_back(stat);
                        }
                        auto objVars = CurBlockObjVars();
                        PopBlock();
                        bool ret = pProvider->LoadSelect(func, *pCallData, *pInfo, iteratorIndex, std::move(argInfos), std::move(args), std::move(stats), std::move(objVars));
                        if (!ret)
                            return false;
                    }
                    else if (fid == "top") {
                        PushBlock();
                        Brace::OperandLoadtimeInfo argInfo;
                        Brace::BraceApiExecutor arg;
                        //int pnum = pCallData->GetParamNum();
                        auto* param = pCallData->GetParam(0);
                        arg = LoadHelper(*param, argInfo);
                        auto objVars = CurBlockObjVars();
                        PopBlock();
                        bool ret = pProvider->LoadTop(func, *pCallData, std::move(argInfo), std::move(arg), std::move(objVars));
                        if (!ret)
                            return false;
                    }
                    else if (fid == "from") {

                    }
                    else if (fid == "where") {
                        PushBlock();
                        int iteratorIndex = AllocVariable("$$", Brace::BRACE_DATA_TYPE_OBJECT, nullptr != pInfo ? pInfo->GetTypeParamObjTypeId(0) : CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO);
                        Brace::OperandLoadtimeInfo argInfo;
                        Brace::BraceApiExecutor arg;
                        //int pnum = pCallData->GetParamNum();
                        auto* param = pCallData->GetParam(0);
                        arg = LoadHelper(*param, argInfo);
                        auto objVars = CurBlockObjVars();
                        PopBlock();
                        bool ret = pProvider->LoadWhere(func, *pCallData, *pInfo, iteratorIndex, std::move(argInfo), std::move(arg), std::move(objVars));
                        if (!ret)
                            return false;
                    }
                    else if (fid == "orderby") {
                        PushBlock();
                        int iteratorIndex = AllocVariable("$$", Brace::BRACE_DATA_TYPE_OBJECT, nullptr != pInfo ? pInfo->GetTypeParamObjTypeId(0) : CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO);
                        std::vector<Brace::OperandLoadtimeInfo> argInfos;
                        std::vector<Brace::BraceApiExecutor> args;
                        std::vector<bool> ascOrDescs;
                        int pnum = pCallData->GetParamNum();
                        for (int pix = 0; pix < pnum; ++pix) {
                            auto* param = pCallData->GetParam(pix);
                            bool asc = true;
                            if (param->GetSyntaxType() == DslData::ISyntaxComponent::TYPE_FUNCTION && param->GetId() == ":") {
                                auto* pParamFunc = static_cast<DslData::FunctionData*>(param);
                                param = pParamFunc->GetParam(0);
                                asc = pParamFunc->GetParamId(1) != "desc";
                            }
                            Brace::OperandLoadtimeInfo argLoadInfo;
                            auto p = LoadHelper(*param, argLoadInfo);
                            argInfos.push_back(std::move(argLoadInfo));
                            args.push_back(std::move(p));
                            ascOrDescs.push_back(asc);
                        }
                        auto objVars = CurBlockObjVars();
                        PopBlock();
                        bool ret = pProvider->LoadOrderBy(func, *pCallData, *pInfo, iteratorIndex, std::move(argInfos), std::move(args), std::move(ascOrDescs), std::move(objVars));
                        if (!ret)
                            return false;
                    }
                    else if (fid == "groupby") {
                        PushBlock();
                        int iteratorIndex = AllocVariable("$$", Brace::BRACE_DATA_TYPE_OBJECT, nullptr != pInfo ? pInfo->GetTypeParamObjTypeId(0) : CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO);
                        std::vector<Brace::OperandLoadtimeInfo> argInfos;
                        std::vector<Brace::BraceApiExecutor> args;
                        int pnum = pCallData->GetParamNum();
                        for (int pix = 0; pix < pnum; ++pix) {
                            auto* param = pCallData->GetParam(pix);
                            Brace::OperandLoadtimeInfo argLoadInfo;
                            auto p = LoadHelper(*param, argLoadInfo);
                            argInfos.push_back(std::move(argLoadInfo));
                            args.push_back(std::move(p));
                        }
                        auto objVars = CurBlockObjVars();
                        PopBlock();
                        bool ret = pProvider->LoadGroupBy(func, *pCallData, *pInfo, iteratorIndex, std::move(argInfos), std::move(args), std::move(objVars));
                        if (!ret)
                            return false;
                    }
                    else if (fid == "having") {
                        pHavingCallData = pCallData;
                    }
                    else {
                        return false;
                    }
                    if (fix == fnum - 1) {
                        std::vector<Brace::OperandLoadtimeInfo> iterators;
                        std::vector<Brace::BraceApiExecutor> statements;
                        PushBlock();
                        for (int i = 0; i < static_cast<int>(selectItTypes.size()); ++i) {
                            auto& dt = selectItTypes[i];
                            Brace::OperandLoadtimeInfo itInfo;
                            itInfo.Type = dt.Type;
                            itInfo.ObjectTypeId = dt.ObjectTypeId;
                            itInfo.Name = std::string("$") + std::to_string(i);
                            itInfo.VarIndex = AllocVariable(itInfo.Name, itInfo.Type, itInfo.ObjectTypeId);
                            iterators.push_back(std::move(itInfo));
                        }
                        bool ret = true;
                        if (nullptr != pHavingCallData) {
                            Brace::OperandLoadtimeInfo argInfo;
                            Brace::BraceApiExecutor arg;
                            auto* param = pHavingCallData->GetParam(0);
                            arg = LoadHelper(*param, argInfo);
                            ret = pProvider->LoadHaving(func, *pHavingCallData, std::move(argInfo), std::move(arg));
                        }
                        if (pFuncData->IsHighOrder()) {
                            std::vector<Brace::OperandLoadtimeInfo> argInfos;
                            std::vector<Brace::BraceApiExecutor> args;
                            int pnum = pFuncData->GetParamNum();
                            for (int pix = 0; pix < pnum; ++pix) {
                                auto* param = pFuncData->GetParam(pix);
                                Brace::OperandLoadtimeInfo argLoadInfo;
                                auto p = LoadHelper(*param, argLoadInfo);
                                if (!p.isNull())
                                    statements.push_back(std::move(p));
                            }
                        }
                        auto objVars = CurBlockObjVars();
                        PopBlock();
                        ret = pProvider->LoadStatements(func, *pFuncData, std::move(statements), resultInfo, executor) && ret;
                        pProvider->LoadResultIterator(std::move(iterators), std::move(objVars));
                        if (!ret)
                            return false;
                    }
                }
            }
            return true;
        }
    private:
        std::unique_ptr<AbstractSelectApiProvider> m_ApiProvider;
    };
    class ArrayExp final : public Brace::AbstractBraceApi
    {
        enum ArrayCategory
        {
            ARRAY_UNKNOWN = -1,
            ARRAY_BOOL = 0,
            ARRAY_INT,
            ARRAY_FLOAT,
            ARRAY_STRING,
            ARRAY_OBJ
        };
    public:
        ArrayExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter), m_ObjectTypeId(Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ), m_Args(), m_ArgInfos(), m_ResultInfo()
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& curFunc, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor)
        {
            std::vector<Brace::BraceApiExecutor> args;
            std::vector<Brace::OperandLoadtimeInfo> argLoadInfos;
            int num = data.GetParamNum();
            for (int ix = 0; ix < num; ++ix) {
                auto* param = data.GetParam(ix);
                Brace::OperandLoadtimeInfo argLoadInfo;
                auto p = LoadHelper(*param, argLoadInfo);
                args.push_back(std::move(p));
                argLoadInfos.push_back(std::move(argLoadInfo));
            }
            int arrayCategory = ARRAY_UNKNOWN;
            int dataType = Brace::BRACE_DATA_TYPE_UNKNOWN;
            int objTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            if (data.IsHighOrder()) {
                auto& lowerFunc = data.GetLowerOrderFunction();
                if (lowerFunc.GetParamNum() == 1 && lowerFunc.GetParamClassUnmasked() == DslData::FunctionData::PARAM_CLASS_ANGLE_BRACKET_COLON) {
                    auto* tp = lowerFunc.GetParam(0);
                    auto ti = ParseParamTypeInfo(*tp);
                    dataType = ti.Type;
                    objTypeId = ti.ObjectTypeId;
                }
            }
            else if (!argLoadInfos.empty()) {
                auto& firstInfo = argLoadInfos.front();
                dataType = firstInfo.Type;
                objTypeId = firstInfo.ObjectTypeId;
            }
            switch (dataType) {
            case Brace::BRACE_DATA_TYPE_BOOL:
                arrayCategory = ARRAY_BOOL;
                break;
            case Brace::BRACE_DATA_TYPE_INT8:
            case Brace::BRACE_DATA_TYPE_UINT8:
            case Brace::BRACE_DATA_TYPE_INT16:
            case Brace::BRACE_DATA_TYPE_UINT16:
            case Brace::BRACE_DATA_TYPE_INT32:
            case Brace::BRACE_DATA_TYPE_UINT32:
            case Brace::BRACE_DATA_TYPE_INT64:
            case Brace::BRACE_DATA_TYPE_UINT64:
                arrayCategory = ARRAY_INT;
                break;
            case Brace::BRACE_DATA_TYPE_FLOAT:
            case Brace::BRACE_DATA_TYPE_DOUBLE:
                arrayCategory = ARRAY_FLOAT;
                break;
            case Brace::BRACE_DATA_TYPE_STRING:
                arrayCategory = ARRAY_STRING;
                break;
            case Brace::BRACE_DATA_TYPE_OBJECT:
                arrayCategory = ARRAY_OBJ;
                break;
            }
            switch (arrayCategory) {
            case ARRAY_BOOL: {
                bool good = true;
                for (auto&& ai : argLoadInfos) {
                    if (ai.Type >= Brace::BRACE_DATA_TYPE_BOOL && ai.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    std::swap(m_Args, args);
                    SetArgInfos(argLoadInfos);
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                    resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &ArrayExp::ExecuteBool);
                    return true;
                }
            }break;
            case ARRAY_INT: {
                bool good = true;
                for (auto&& ai : argLoadInfos) {
                    if (ai.Type >= Brace::BRACE_DATA_TYPE_INT8 && ai.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    std::swap(m_Args, args);
                    SetArgInfos(argLoadInfos);
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                    resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &ArrayExp::ExecuteInt);
                    return true;
                }
            }break;
            case ARRAY_FLOAT: {
                bool good = true;
                for (auto&& ai : argLoadInfos) {
                    if (ai.Type >= Brace::BRACE_DATA_TYPE_INT8 && ai.Type <= Brace::BRACE_DATA_TYPE_DOUBLE) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    std::swap(m_Args, args);
                    SetArgInfos(argLoadInfos);
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                    resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &ArrayExp::ExecuteFloat);
                    return true;
                }
            }break;
            case ARRAY_STRING: {
                bool good = true;
                for (auto&& ai : argLoadInfos) {
                    if (ai.Type == Brace::BRACE_DATA_TYPE_STRING) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    std::swap(m_Args, args);
                    SetArgInfos(argLoadInfos);
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                    resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &ArrayExp::ExecuteString);
                    return true;
                }
            }break;
            case ARRAY_OBJ: {
                bool good = true;
                for (auto&& ai : argLoadInfos) {
                    if (ai.Type == Brace::BRACE_DATA_TYPE_OBJECT && ai.ObjectTypeId == objTypeId) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    std::string typeKey = std::string("array<:") + GetObjectTypeName(objTypeId) + ":>";
                    m_ObjectTypeId = g_ObjectInfoMgr.GetObjectTypeId(typeKey);
                    if (m_ObjectTypeId == Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN) {
                        m_ObjectTypeId = g_ObjectInfoMgr.AddNewObjectTypeId(typeKey);
                    }
                    auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(m_ObjectTypeId);
                    if (nullptr == pInfo) {
                        pInfo = g_ObjectInfoMgr.AddBraceObjectInfo(m_ObjectTypeId, BRACE_OBJECT_CATEGORY_OBJ_ARRAY, std::move(typeKey));
                        g_ObjectInfoMgr.SetBraceObjectTypeParams(m_ObjectTypeId, dataType, objTypeId);
                    }
                    if (nullptr != pInfo) {
                        std::swap(m_Args, args);
                        SetArgInfos(argLoadInfos);
                        resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                        resultInfo.ObjectTypeId = m_ObjectTypeId;
                        resultInfo.Name = GenTempVarName();
                        resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                        m_ResultInfo = resultInfo;
                        executor.attach(this, &ArrayExp::ExecuteObject);
                        return true;
                    }
                }
            }break;
            }
            std::stringstream ss;
            ss << "Array syntax error ! array<: bool|int32|float|string|obj :>(v1, v2, ...) line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
    private:
        void SetArgInfos(std::vector<Brace::OperandLoadtimeInfo> argLoadInfos)
        {
            for (auto&& info : argLoadInfos) {
                m_ArgInfos.push_back(info);
            }
        }
    private:
        int ExecuteBool(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& arg : m_Args) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            using BoolArrayObj = ArrayT<bool>;
            auto* p = new BoolArrayObj();
            for (auto&& ai : m_ArgInfos) {
                bool v = Brace::VarGetBoolean((ai.IsGlobal ? gvars : lvars), ai.Type, ai.VarIndex);
                p->push_back(v);
            }
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(p));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteInt(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& arg : m_Args) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            using IntArrayObj = ArrayT<int64_t>;
            auto* p = new IntArrayObj();
            for (auto&& ai : m_ArgInfos) {
                int64_t v = Brace::VarGetI64((ai.IsGlobal ? gvars : lvars), ai.Type, ai.VarIndex);
                p->push_back(v);
            }
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(p));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteFloat(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& arg : m_Args) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            using FloatArrayObj = ArrayT<double>;
            auto* p = new FloatArrayObj();
            for (auto&& ai : m_ArgInfos) {
                double v = Brace::VarGetF64((ai.IsGlobal ? gvars : lvars), ai.Type, ai.VarIndex);
                p->push_back(v);
            }
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(p));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteString(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& arg : m_Args) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            using StrArrayObj = ArrayT<std::string>;
            auto* p = new StrArrayObj();
            for (auto&& ai : m_ArgInfos) {
                const std::string& v = Brace::VarGetString((ai.IsGlobal ? gvars : lvars), ai.VarIndex);
                p->push_back(v);
            }
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(p));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteObject(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& arg : m_Args) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            auto* p = new ObjectArray();
            for (auto&& ai : m_ArgInfos) {
                auto& v = Brace::VarGetObject((ai.IsGlobal ? gvars : lvars), ai.VarIndex);
                p->push_back(v);
            }
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(p));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    private:
        int m_ObjectTypeId;
        std::vector<Brace::BraceApiExecutor> m_Args;
        std::vector<Brace::OperandRuntimeInfo> m_ArgInfos;
        Brace::OperandRuntimeInfo m_ResultInfo;
    };
    class HashtableExp final : public Brace::AbstractBraceApi
    {
        enum HashtableCategory
        {
            HASHTABLE_UNKNOWN = -1,
            HASHTABLE_STR_STR = 0,
            HASHTABLE_STR_INT,
            HASHTABLE_STR_FLOAT,
            HASHTABLE_STR_BOOL,
            HASHTABLE_STR_OBJ,
            HASHTABLE_INT_STR,
            HASHTABLE_INT_INT,
            HASHTABLE_INT_FLOAT,
            HASHTABLE_INT_BOOL,
            HASHTABLE_INT_OBJ
        };
    public:
        HashtableExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter)
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            std::vector<Brace::BraceApiExecutor> argKeys;
            std::vector<Brace::BraceApiExecutor> argVals;
            std::vector<Brace::OperandLoadtimeInfo> argKeyLoadInfos;
            std::vector<Brace::OperandLoadtimeInfo> argValLoadInfos;
            int num = data.GetParamNum();
            for (int ix = 0; ix < num; ++ix) {
                auto* param = data.GetParam(ix);
                if (param->GetSyntaxType() != DslData::ISyntaxComponent::TYPE_FUNCTION || (param->GetId() != "=>" && param->GetId() != ":")) {
                    std::stringstream ss;
                    ss << "Hashtable syntax error ! param must be pairs (k1 => v1, k2 => v2, ...) line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
                auto* funcData = static_cast<DslData::FunctionData*>(param);
                if (funcData->GetParamNum() != 2) {
                    std::stringstream ss;
                    ss << "Hashtable syntax error ! param must be pairs (k1 => v1, k2 => v2, ...) line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
                Brace::OperandLoadtimeInfo argKeyLoadInfo;
                Brace::OperandLoadtimeInfo argValLoadInfo;
                auto p_key = LoadHelper(*funcData->GetParam(0), argKeyLoadInfo);
                auto p_val = LoadHelper(*funcData->GetParam(1), argValLoadInfo);
                argKeys.push_back(std::move(p_key));
                argKeyLoadInfos.push_back(std::move(argKeyLoadInfo));
                argVals.push_back(std::move(p_val));
                argValLoadInfos.push_back(std::move(argValLoadInfo));
            }
            int hashtableCategory = HASHTABLE_UNKNOWN;
            int keyDataType = Brace::BRACE_DATA_TYPE_UNKNOWN;
            int valDataType = Brace::BRACE_DATA_TYPE_UNKNOWN;
            int keyObjTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            int valObjTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            if (data.IsHighOrder()) {
                auto& lowerFunc = data.GetLowerOrderFunction();
                if (lowerFunc.GetParamNum() == 2 && lowerFunc.GetParamClassUnmasked() == DslData::FunctionData::PARAM_CLASS_ANGLE_BRACKET_COLON) {
                    const std::string& key = lowerFunc.GetParamId(0);
                    auto* val = lowerFunc.GetParam(1);
                    keyDataType = Brace::GetDataType(key);
                    auto ti = ParseParamTypeInfo(*val);
                    valDataType = ti.Type;
                    valObjTypeId = ti.ObjectTypeId;
                }
            }
            else if (!argKeyLoadInfos.empty() && !argValLoadInfos.empty()) {
                auto& firstKeyInfo = argKeyLoadInfos.front();
                auto& firstValInfo = argValLoadInfos.front();
                keyDataType = firstKeyInfo.Type;
                keyObjTypeId = firstKeyInfo.ObjectTypeId;
                valDataType = firstValInfo.Type;
                valObjTypeId = firstValInfo.ObjectTypeId;
            }
            switch (keyDataType) {
            case Brace::BRACE_DATA_TYPE_STRING:
                switch (valDataType) {
                case Brace::BRACE_DATA_TYPE_BOOL:
                    hashtableCategory = HASHTABLE_STR_BOOL;
                    break;
                case Brace::BRACE_DATA_TYPE_INT8:
                case Brace::BRACE_DATA_TYPE_UINT8:
                case Brace::BRACE_DATA_TYPE_INT16:
                case Brace::BRACE_DATA_TYPE_UINT16:
                case Brace::BRACE_DATA_TYPE_INT32:
                case Brace::BRACE_DATA_TYPE_UINT32:
                case Brace::BRACE_DATA_TYPE_INT64:
                case Brace::BRACE_DATA_TYPE_UINT64:
                    hashtableCategory = HASHTABLE_STR_INT;
                    break;
                case Brace::BRACE_DATA_TYPE_FLOAT:
                case Brace::BRACE_DATA_TYPE_DOUBLE:
                    hashtableCategory = HASHTABLE_STR_FLOAT;
                    break;
                case Brace::BRACE_DATA_TYPE_STRING:
                    hashtableCategory = HASHTABLE_STR_STR;
                    break;
                case Brace::BRACE_DATA_TYPE_OBJECT:
                    hashtableCategory = HASHTABLE_STR_OBJ;
                    break;
                }
                break;
            case Brace::BRACE_DATA_TYPE_INT8:
            case Brace::BRACE_DATA_TYPE_UINT8:
            case Brace::BRACE_DATA_TYPE_INT16:
            case Brace::BRACE_DATA_TYPE_UINT16:
            case Brace::BRACE_DATA_TYPE_INT32:
            case Brace::BRACE_DATA_TYPE_UINT32:
            case Brace::BRACE_DATA_TYPE_INT64:
            case Brace::BRACE_DATA_TYPE_UINT64:
                switch (valDataType) {
                case Brace::BRACE_DATA_TYPE_BOOL:
                    hashtableCategory = HASHTABLE_INT_BOOL;
                    break;
                case Brace::BRACE_DATA_TYPE_INT8:
                case Brace::BRACE_DATA_TYPE_UINT8:
                case Brace::BRACE_DATA_TYPE_INT16:
                case Brace::BRACE_DATA_TYPE_UINT16:
                case Brace::BRACE_DATA_TYPE_INT32:
                case Brace::BRACE_DATA_TYPE_UINT32:
                case Brace::BRACE_DATA_TYPE_INT64:
                case Brace::BRACE_DATA_TYPE_UINT64:
                    hashtableCategory = HASHTABLE_INT_INT;
                    break;
                case Brace::BRACE_DATA_TYPE_FLOAT:
                case Brace::BRACE_DATA_TYPE_DOUBLE:
                    hashtableCategory = HASHTABLE_INT_FLOAT;
                    break;
                case Brace::BRACE_DATA_TYPE_STRING:
                    hashtableCategory = HASHTABLE_INT_STR;
                    break;
                case Brace::BRACE_DATA_TYPE_OBJECT:
                    hashtableCategory = HASHTABLE_INT_OBJ;
                    break;
                }
                break;
            }
            switch (hashtableCategory) {
            case HASHTABLE_INT_BOOL: {
                bool good = true;
                for (auto&& ai : argKeyLoadInfos) {
                    if (ai.Type >= Brace::BRACE_DATA_TYPE_INT8 && ai.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                for (auto&& ai : argValLoadInfos) {
                    if (ai.Type >= Brace::BRACE_DATA_TYPE_BOOL && ai.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    std::swap(m_ArgKeys, argKeys);
                    SetArgKeyInfos(argKeyLoadInfos);
                    std::swap(m_ArgVals, argVals);
                    SetArgValInfos(argValLoadInfos);
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                    resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &HashtableExp::ExecuteIntBool);
                    return true;
                }
            }break;
            case HASHTABLE_INT_INT: {
                bool good = true;
                for (auto&& ai : argKeyLoadInfos) {
                    if (ai.Type >= Brace::BRACE_DATA_TYPE_INT8 && ai.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                for (auto&& ai : argValLoadInfos) {
                    if (ai.Type >= Brace::BRACE_DATA_TYPE_INT8 && ai.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    std::swap(m_ArgKeys, argKeys);
                    SetArgKeyInfos(argKeyLoadInfos);
                    std::swap(m_ArgVals, argVals);
                    SetArgValInfos(argValLoadInfos);
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                    resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &HashtableExp::ExecuteIntInt);
                    return true;
                }
            }break;
            case HASHTABLE_INT_FLOAT: {
                bool good = true;
                for (auto&& ai : argKeyLoadInfos) {
                    if (ai.Type >= Brace::BRACE_DATA_TYPE_INT8 && ai.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                for (auto&& ai : argValLoadInfos) {
                    if (ai.Type >= Brace::BRACE_DATA_TYPE_INT8 && ai.Type <= Brace::BRACE_DATA_TYPE_DOUBLE) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    std::swap(m_ArgKeys, argKeys);
                    SetArgKeyInfos(argKeyLoadInfos);
                    std::swap(m_ArgVals, argVals);
                    SetArgValInfos(argValLoadInfos);
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                    resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &HashtableExp::ExecuteIntFloat);
                    return true;
                }
            }break;
            case HASHTABLE_INT_STR: {
                bool good = true;
                for (auto&& ai : argKeyLoadInfos) {
                    if (ai.Type >= Brace::BRACE_DATA_TYPE_INT8 && ai.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                for (auto&& ai : argValLoadInfos) {
                    if (ai.Type == Brace::BRACE_DATA_TYPE_STRING) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    std::swap(m_ArgKeys, argKeys);
                    SetArgKeyInfos(argKeyLoadInfos);
                    std::swap(m_ArgVals, argVals);
                    SetArgValInfos(argValLoadInfos);
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                    resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &HashtableExp::ExecuteIntStr);
                    return true;
                }
            }break;
            case HASHTABLE_INT_OBJ: {
                bool good = true;
                for (auto&& ai : argKeyLoadInfos) {
                    if (ai.Type >= Brace::BRACE_DATA_TYPE_INT8 && ai.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                for (auto&& ai : argValLoadInfos) {
                    if (ai.Type == Brace::BRACE_DATA_TYPE_OBJECT && ai.ObjectTypeId == valObjTypeId) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    std::string tname = GetObjectTypeName(valObjTypeId);
                    std::string typeKey = std::string("hashtable<:int64,") + tname + ":>";
                    int objectTypeId = g_ObjectInfoMgr.GetObjectTypeId(typeKey);
                    if (objectTypeId == Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN) {
                        objectTypeId = g_ObjectInfoMgr.AddNewObjectTypeId(typeKey);
                        g_ObjectInfoMgr.AddBraceObjectAlias(objectTypeId, "hashtable<:int8," + tname + ":>");
                        g_ObjectInfoMgr.AddBraceObjectAlias(objectTypeId, "hashtable<:int16," + tname + ":>");
                        g_ObjectInfoMgr.AddBraceObjectAlias(objectTypeId, "hashtable<:int32," + tname + ":>");
                        g_ObjectInfoMgr.AddBraceObjectAlias(objectTypeId, "hashtable<:uint8," + tname + ":>");
                        g_ObjectInfoMgr.AddBraceObjectAlias(objectTypeId, "hashtable<:uint16," + tname + ":>");
                        g_ObjectInfoMgr.AddBraceObjectAlias(objectTypeId, "hashtable<:uint32," + tname + ":>");
                        g_ObjectInfoMgr.AddBraceObjectAlias(objectTypeId, "hashtable<:uint64," + tname + ":>");
                    }
                    auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objectTypeId);
                    if (nullptr == pInfo) {
                        pInfo = g_ObjectInfoMgr.AddBraceObjectInfo(objectTypeId, BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE, std::move(typeKey));
                        g_ObjectInfoMgr.SetBraceObjectTypeParams(objectTypeId, keyDataType, keyObjTypeId, valDataType, valObjTypeId);
                    }
                    if (nullptr != pInfo) {
                        std::swap(m_ArgKeys, argKeys);
                        SetArgKeyInfos(argKeyLoadInfos);
                        std::swap(m_ArgVals, argVals);
                        SetArgValInfos(argValLoadInfos);
                        resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                        resultInfo.ObjectTypeId = objectTypeId;
                        resultInfo.Name = GenTempVarName();
                        resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                        m_ResultInfo = resultInfo;
                        executor.attach(this, &HashtableExp::ExecuteIntObj);
                        return true;
                    }
                }
            }break;
            case HASHTABLE_STR_BOOL: {
                bool good = true;
                for (auto&& ai : argKeyLoadInfos) {
                    if (ai.Type == Brace::BRACE_DATA_TYPE_STRING) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                for (auto&& ai : argValLoadInfos) {
                    if (ai.Type >= Brace::BRACE_DATA_TYPE_BOOL && ai.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    std::swap(m_ArgKeys, argKeys);
                    SetArgKeyInfos(argKeyLoadInfos);
                    std::swap(m_ArgVals, argVals);
                    SetArgValInfos(argValLoadInfos);
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                    resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &HashtableExp::ExecuteStrBool);
                    return true;
                }
            }break;
            case HASHTABLE_STR_INT: {
                bool good = true;
                for (auto&& ai : argKeyLoadInfos) {
                    if (ai.Type == Brace::BRACE_DATA_TYPE_STRING) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                for (auto&& ai : argValLoadInfos) {
                    if (ai.Type >= Brace::BRACE_DATA_TYPE_INT8 && ai.Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    std::swap(m_ArgKeys, argKeys);
                    SetArgKeyInfos(argKeyLoadInfos);
                    std::swap(m_ArgVals, argVals);
                    SetArgValInfos(argValLoadInfos);
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                    resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &HashtableExp::ExecuteStrInt);
                    return true;
                }
            }break;
            case HASHTABLE_STR_FLOAT: {
                bool good = true;
                for (auto&& ai : argKeyLoadInfos) {
                    if (ai.Type == Brace::BRACE_DATA_TYPE_STRING) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                for (auto&& ai : argValLoadInfos) {
                    if (ai.Type >= Brace::BRACE_DATA_TYPE_INT8 && ai.Type <= Brace::BRACE_DATA_TYPE_DOUBLE) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    std::swap(m_ArgKeys, argKeys);
                    SetArgKeyInfos(argKeyLoadInfos);
                    std::swap(m_ArgVals, argVals);
                    SetArgValInfos(argValLoadInfos);
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                    resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &HashtableExp::ExecuteStrFloat);
                    return true;
                }
            }break;
            case HASHTABLE_STR_STR: {
                bool good = true;
                for (auto&& ai : argKeyLoadInfos) {
                    if (ai.Type == Brace::BRACE_DATA_TYPE_STRING) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                for (auto&& ai : argValLoadInfos) {
                    if (ai.Type == Brace::BRACE_DATA_TYPE_STRING) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    std::swap(m_ArgKeys, argKeys);
                    SetArgKeyInfos(argKeyLoadInfos);
                    std::swap(m_ArgVals, argVals);
                    SetArgValInfos(argValLoadInfos);
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                    resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    m_ResultInfo = resultInfo;
                    executor.attach(this, &HashtableExp::ExecuteStrStr);
                    return true;
                }
            }break;
            case HASHTABLE_STR_OBJ: {
                bool good = true;
                for (auto&& ai : argKeyLoadInfos) {
                    if (ai.Type == Brace::BRACE_DATA_TYPE_STRING) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                for (auto&& ai : argValLoadInfos) {
                    if (ai.Type == Brace::BRACE_DATA_TYPE_OBJECT && ai.ObjectTypeId == valObjTypeId) {
                    }
                    else {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    std::string typeKey = std::string("hashtable<:string,") + GetObjectTypeName(valObjTypeId) + ":>";
                    int objectTypeId = g_ObjectInfoMgr.GetObjectTypeId(typeKey);
                    if (objectTypeId == Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN) {
                        objectTypeId = g_ObjectInfoMgr.AddNewObjectTypeId(typeKey);
                    }
                    auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objectTypeId);
                    if (nullptr == pInfo) {
                        pInfo = g_ObjectInfoMgr.AddBraceObjectInfo(objectTypeId, BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE, std::move(typeKey));
                        g_ObjectInfoMgr.SetBraceObjectTypeParams(objectTypeId, keyDataType, keyObjTypeId, valDataType, valObjTypeId);
                    }
                    if (nullptr != pInfo) {
                        std::swap(m_ArgKeys, argKeys);
                        SetArgKeyInfos(argKeyLoadInfos);
                        std::swap(m_ArgVals, argVals);
                        SetArgValInfos(argValLoadInfos);
                        resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                        resultInfo.ObjectTypeId = objectTypeId;
                        resultInfo.Name = GenTempVarName();
                        resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                        m_ResultInfo = resultInfo;
                        executor.attach(this, &HashtableExp::ExecuteStrObj);
                        return true;
                    }
                }
            }break;
            }
            std::stringstream ss;
            ss << "Hashtable syntax error ! hashtable<: int32|string, bool|int32|float|string|obj :>(k1 => v1, k2 => v2, ...) line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
    private:
        void SetArgKeyInfos(std::vector<Brace::OperandLoadtimeInfo> argKeyLoadInfos)
        {
            for (auto&& info : argKeyLoadInfos) {
                m_ArgKeyInfos.push_back(info);
            }
        }
        void SetArgValInfos(std::vector<Brace::OperandLoadtimeInfo> argValLoadInfos)
        {
            for (auto&& info : argValLoadInfos) {
                m_ArgValInfos.push_back(info);
            }
        }
    private:
        int ExecuteIntStr(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& arg : m_ArgKeys) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            for (auto&& arg : m_ArgVals) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            using HashtableObj = HashtableT<int64_t, std::string>;
            auto* p = new HashtableObj();
            for (int ix = 0; ix < static_cast<int>(m_ArgKeyInfos.size()) && ix < static_cast<int>(m_ArgValInfos.size()); ++ix) {
                auto& key = m_ArgKeyInfos[ix];
                auto& val = m_ArgValInfos[ix];
                int64_t k = Brace::VarGetI64((key.IsGlobal ? gvars : lvars), key.Type, key.VarIndex);
                std::string v = Brace::VarGetStr((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
                p->insert(std::make_pair(k, std::move(v)));
            }
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(p));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteIntInt(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& arg : m_ArgKeys) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            for (auto&& arg : m_ArgVals) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            using HashtableObj = HashtableT<int64_t, int64_t>;
            auto* p = new HashtableObj();
            for (int ix = 0; ix < static_cast<int>(m_ArgKeyInfos.size()) && ix < static_cast<int>(m_ArgValInfos.size()); ++ix) {
                auto& key = m_ArgKeyInfos[ix];
                auto& val = m_ArgValInfos[ix];
                int64_t k = Brace::VarGetI64((key.IsGlobal ? gvars : lvars), key.Type, key.VarIndex);
                int64_t v = Brace::VarGetI64((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
                p->insert(std::make_pair(k, v));
            }
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(p));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteIntFloat(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& arg : m_ArgKeys) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            for (auto&& arg : m_ArgVals) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            using HashtableObj = HashtableT<int64_t, double>;
            auto* p = new HashtableObj();
            for (int ix = 0; ix < static_cast<int>(m_ArgKeyInfos.size()) && ix < static_cast<int>(m_ArgValInfos.size()); ++ix) {
                auto& key = m_ArgKeyInfos[ix];
                auto& val = m_ArgValInfos[ix];
                int64_t k = Brace::VarGetI64((key.IsGlobal ? gvars : lvars), key.Type, key.VarIndex);
                double v = Brace::VarGetF64((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
                p->insert(std::make_pair(k, v));
            }
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(p));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteIntBool(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& arg : m_ArgKeys) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            for (auto&& arg : m_ArgVals) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            using HashtableObj = HashtableT<int64_t, bool>;
            auto* p = new HashtableObj();
            for (int ix = 0; ix < static_cast<int>(m_ArgKeyInfos.size()) && ix < static_cast<int>(m_ArgValInfos.size()); ++ix) {
                auto& key = m_ArgKeyInfos[ix];
                auto& val = m_ArgValInfos[ix];
                int64_t k = Brace::VarGetI64((key.IsGlobal ? gvars : lvars), key.Type, key.VarIndex);
                bool v = Brace::VarGetBoolean((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
                p->insert(std::make_pair(k, v));
            }
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(p));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteIntObj(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& arg : m_ArgKeys) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            for (auto&& arg : m_ArgVals) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            auto* p = new IntObjHashtable();
            for (int ix = 0; ix < static_cast<int>(m_ArgKeyInfos.size()) && ix < static_cast<int>(m_ArgValInfos.size()); ++ix) {
                auto& key = m_ArgKeyInfos[ix];
                auto& val = m_ArgValInfos[ix];
                int64_t k = Brace::VarGetI64((key.IsGlobal ? gvars : lvars), key.Type, key.VarIndex);
                auto v = Brace::VarGetObject((val.IsGlobal ? gvars : lvars), val.VarIndex);
                p->insert(std::make_pair(k, std::move(v)));
            }
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(p));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteStrStr(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& arg : m_ArgKeys) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            for (auto&& arg : m_ArgVals) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            using HashtableObj = HashtableT<std::string, std::string>;
            auto* p = new HashtableObj();
            for (int ix = 0; ix < static_cast<int>(m_ArgKeyInfos.size()) && ix < static_cast<int>(m_ArgValInfos.size()); ++ix) {
                auto& key = m_ArgKeyInfos[ix];
                auto& val = m_ArgValInfos[ix];
                std::string k = Brace::VarGetStr((key.IsGlobal ? gvars : lvars), key.Type, key.VarIndex);
                std::string v = Brace::VarGetStr((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
                p->insert(std::make_pair(std::move(k), std::move(v)));
            }
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(p));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteStrInt(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& arg : m_ArgKeys) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            for (auto&& arg : m_ArgVals) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            using HashtableObj = HashtableT<std::string, int64_t>;
            auto* p = new HashtableObj();
            for (int ix = 0; ix < static_cast<int>(m_ArgKeyInfos.size()) && ix < static_cast<int>(m_ArgValInfos.size()); ++ix) {
                auto& key = m_ArgKeyInfos[ix];
                auto& val = m_ArgValInfos[ix];
                std::string k = Brace::VarGetStr((key.IsGlobal ? gvars : lvars), key.Type, key.VarIndex);
                int64_t v = Brace::VarGetI64((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
                p->insert(std::make_pair(std::move(k), v));
            }
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(p));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteStrFloat(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& arg : m_ArgKeys) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            for (auto&& arg : m_ArgVals) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            using HashtableObj = HashtableT<std::string, double>;
            auto* p = new HashtableObj();
            for (int ix = 0; ix < static_cast<int>(m_ArgKeyInfos.size()) && ix < static_cast<int>(m_ArgValInfos.size()); ++ix) {
                auto& key = m_ArgKeyInfos[ix];
                auto& val = m_ArgValInfos[ix];
                std::string k = Brace::VarGetStr((key.IsGlobal ? gvars : lvars), key.Type, key.VarIndex);
                double v = Brace::VarGetF64((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
                p->insert(std::make_pair(std::move(k), v));
            }
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(p));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteStrBool(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& arg : m_ArgKeys) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            for (auto&& arg : m_ArgVals) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            using HashtableObj = HashtableT<std::string, bool>;
            auto* p = new HashtableObj();
            for (int ix = 0; ix < static_cast<int>(m_ArgKeyInfos.size()) && ix < static_cast<int>(m_ArgValInfos.size()); ++ix) {
                auto& key = m_ArgKeyInfos[ix];
                auto& val = m_ArgValInfos[ix];
                std::string k = Brace::VarGetStr((key.IsGlobal ? gvars : lvars), key.Type, key.VarIndex);
                bool v = Brace::VarGetBoolean((val.IsGlobal ? gvars : lvars), val.Type, val.VarIndex);
                p->insert(std::make_pair(std::move(k), v));
            }
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(p));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
        int ExecuteStrObj(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& arg : m_ArgKeys) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            for (auto&& arg : m_ArgVals) {
                if (!arg.isNull())
                    arg(gvars, lvars);
            }
            auto* p = new StrObjHashtable();
            for (int ix = 0; ix < static_cast<int>(m_ArgKeyInfos.size()) && ix < static_cast<int>(m_ArgValInfos.size()); ++ix) {
                auto& key = m_ArgKeyInfos[ix];
                auto& val = m_ArgValInfos[ix];
                std::string k = Brace::VarGetStr((key.IsGlobal ? gvars : lvars), key.Type, key.VarIndex);
                auto v = Brace::VarGetObject((val.IsGlobal ? gvars : lvars), val.VarIndex);
                p->insert(std::make_pair(std::move(k), std::move(v)));
            }
            Brace::VarSetObject((m_ResultInfo.IsGlobal ? gvars : lvars), m_ResultInfo.VarIndex, std::shared_ptr<void>(p));
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    private:
        std::vector<Brace::BraceApiExecutor> m_ArgKeys;
        std::vector<Brace::OperandRuntimeInfo> m_ArgKeyInfos;
        std::vector<Brace::BraceApiExecutor> m_ArgVals;
        std::vector<Brace::OperandRuntimeInfo> m_ArgValInfos;
        Brace::OperandRuntimeInfo m_ResultInfo;
    };
    class LoopListExp final : public Brace::AbstractBraceApi
    {
    public:
        LoopListExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter), m_ApiProvider()
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& funcData, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            if (funcData.IsHighOrder()) {
                bool ret = false;
                auto* p = funcData.GetLowerOrderFunction().GetParam(0);
                Brace::OperandLoadtimeInfo loadInfo;
                auto list = LoadHelper(*p, loadInfo);
                auto& listInfo = loadInfo;
                PushBlock();
                BraceObjectInfo* pInfo = nullptr;
                AbstractLoopListApiProvider* pProvider = nullptr;
                if (listInfo.Type == Brace::BRACE_DATA_TYPE_OBJECT) {
                    pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(listInfo.ObjectTypeId);
                    if (nullptr != pInfo) {
                        switch (pInfo->ObjectCategory) {
                        case BRACE_OBJECT_CATEGORY_SPECIAL:
                        default:
                            pProvider = new ArrayHashtableLoopListProvider(GetInterpreter());
                            break;
                        }
                    }
                }
                else if (listInfo.Type == Brace::BRACE_DATA_TYPE_STRING) {
                    pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_STRING);
                    if (nullptr != pInfo) {
                        pProvider = new StringLoopListProvider(GetInterpreter());
                    }
                }
                if (pProvider) {
                    m_ApiProvider.reset(pProvider);
                    if (pProvider->TypeInference(func, funcData, *pInfo, listInfo, executor)) {
                        std::vector<Brace::BraceApiExecutor> statements{};
                        for (int ix = 0; ix < funcData.GetParamNum(); ++ix) {
                            Brace::OperandLoadtimeInfo argLoadInfo;
                            auto statement = LoadHelper(*funcData.GetParam(ix), argLoadInfo);
                            if (!statement.isNull())
                                statements.push_back(std::move(statement));
                        }
                        auto& objVars = CurBlockObjVars();
                        pProvider->StoreRuntimeInfo(std::move(listInfo), std::move(list), std::move(statements), objVars);
                        ret = true;
                    }
                }
                PopBlock();
                if (ret)
                    return true;
            }
            //error
            std::stringstream ss;
            ss << "BraceScript error, " << funcData.GetId() << " line " << funcData.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual bool LoadStatement(const Brace::FuncInfo& func, const DslData::StatementData& statementData, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //looplist(exp) func(args);
            if (statementData.GetFunctionNum() == 2) {
                auto* first = statementData.GetFirst()->AsFunction();
                //const std::string& firstId = first->GetId();
                if (!first->HaveStatement() && !first->HaveExternScript()) {
                    auto* second = statementData.GetSecond();
                    auto* secondVal = second->AsValue();
                    auto* secondFunc = second->AsFunction();
                    if (nullptr != secondVal || (nullptr != secondFunc && secondFunc->HaveId() && !secondFunc->HaveStatement() && !secondFunc->HaveExternScript())) {
                        if (first->GetParamNum() > 0) {
                            bool ret = false;
                            auto* exp = first->GetParam(0);
                            Brace::OperandLoadtimeInfo loadInfo;
                            auto list = LoadHelper(*exp, loadInfo);
                            auto& listInfo = loadInfo;
                            PushBlock();
                            if (listInfo.Type == Brace::BRACE_DATA_TYPE_OBJECT) {
                                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(listInfo.ObjectTypeId);
                                if (nullptr != pInfo) {
                                    AbstractLoopListApiProvider* pProvider = nullptr;
                                    switch (pInfo->ObjectCategory) {
                                    case BRACE_OBJECT_CATEGORY_SPECIAL:
                                    default:
                                        pProvider = new ArrayHashtableLoopListProvider(GetInterpreter());
                                        break;
                                    }
                                    if (pProvider) {
                                        m_ApiProvider.reset(pProvider);
                                        if (pProvider->TypeInference(func, statementData, *pInfo, listInfo, executor)) {
                                            Brace::OperandLoadtimeInfo argLoadInfo;
                                            std::vector<Brace::BraceApiExecutor> statements{};
                                            auto statement = LoadHelper(*second, argLoadInfo);
                                            if (!statement.isNull())
                                                statements.push_back(std::move(statement));
                                            auto& objVars = CurBlockObjVars();
                                            pProvider->StoreRuntimeInfo(std::move(listInfo), std::move(list), std::move(statements), objVars);
                                            ret = true;
                                        }
                                    }
                                }
                            }
                            PopBlock();
                            if (ret)
                                return true;
                        }
                    }
                }
            }
            //error
            std::stringstream ss;
            ss << "BraceScript error, " << statementData.GetId() << " line " << statementData.GetLine();
            LogError(ss.str());
            return false;
        }
    private:
        std::unique_ptr<AbstractLoopListApiProvider> m_ApiProvider;
    };
}