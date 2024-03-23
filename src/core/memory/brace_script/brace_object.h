#pragma once

#include <ios>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <vector>
#include <queue>
#include <unordered_map>
#include <memory>
#include <utility>
#include <functional>
#include <variant>
#include <type_traits>
#include <typeinfo>
#include <ranges>
#include <string_view>
#include <chrono>
#include <algorithm>
#include "BraceScript.h"
#include "core/core.h"
#include "core/memory.h"
#include "core/memory/memory_sniffer.h"
#include "common/fs/fs.h"

namespace BraceScriptInterpreter
{
    enum BraceObjectCategoryEnum
    {
        /// <summary>
        /// Internal objects, no inheritance, handled specifically in the MemberCall/MemberSet/MemberGet/CollectionCall/CollectionSet/CollectionGet/LoopList APIs.
        /// </summary>
        BRACE_OBJECT_CATEGORY_INTERNAL_FIXED_OBJECT = 0,
        /// <summary>
        /// Internal special objects, handled specifically in the MemberCall/MemberSet/MemberGet/CollectionCall/CollectionSet/CollectionGet/LoopList APIs.
        /// </summary>
        BRACE_OBJECT_CATEGORY_OBJ_ARRAY,
        BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE,
        BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE,
        /// <summary>
        /// Custom structs, we will use a class to handle member/collection operations for this category.
        /// </summary>
        BRACE_OBJECT_CATEGORY_STRUCT,
        /// <summary>
        /// Custom objects, has inheritance, we will use a class to handle member/collection operations for this category.
        /// </summary>
        BRACE_OBJECT_CATEGORY_CUSTOM,
        /// <summary>
        /// Special Memory objects, we will use a class to handle member/collection operations for this category.
        /// </summary>
        BRACE_OBJECT_CATEGORY_SPECIAL,
        BRACE_OBJECT_CATEGORY_NUM
    };
    enum CustomBraceObjectTypeIdEnum
    {
        CUSTOM_BRACE_OBJECT_TYPE_STRING = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NUM,
        CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY,
        CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY,
        CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY,
        CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY,
        CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE,
        CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE,
        CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE,
        CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE,
        CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE,
        CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE,
        CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE,
        CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE,
        CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO,
        BRACE_INNER_FIXED_OBJECT_TYPE_NUM
    };

    struct MethodInfo final
    {
        Brace::DataTypeInfo ReturnType;
        std::vector<Brace::ParamTypeInfo> ParamTypes;
        std::string Name;
    };
    struct BraceObjectInfo;
    struct FieldInfo final
    {
        Brace::DataTypeInfo Type;
        int Offset;
        int Size;
        bool IsPtr;
        std::string Name;
        const BraceObjectInfo* BraceObjInfo;
    };
    struct MethodTableInfo final
    {
        std::vector<MethodInfo> Methods;
    };
    struct FieldTableInfo final
    {
        int Size;
        std::vector<FieldInfo> Fields;
    };
    struct BraceObjectInfo final
    {
        int GetTypeParamCount()const
        {
            auto& ps = TypeParams;
            return static_cast<int>(ps.size()) / 2;
        }
        int GetTypeParamType(int ix)const
        {
            auto& ps = TypeParams;
            int index = ix * 2;
            if (index >= 0 && index < static_cast<int>(ps.size())) {
                return ps[index];
            }
            return Brace::BRACE_DATA_TYPE_UNKNOWN;
        }
        int GetTypeParamObjTypeId(int ix)const
        {
            auto& ps = TypeParams;
            int index = ix * 2 + 1;
            if (index >= 0 && index < static_cast<int>(ps.size())) {
                return ps[index];
            }
            return Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
        }
        int FirstTypeParamType()const
        {
            return GetTypeParamType(0);
        }
        int FirstTypeParamObjTypeId()const
        {
            return GetTypeParamObjTypeId(0);
        }
        int SecondTypeParamType()const
        {
            return GetTypeParamType(1);
        }
        int SecondTypeParamObjTypeId()const
        {
            return GetTypeParamObjTypeId(1);
        }
        int LastTypeParamType()const
        {
            int ct = GetTypeParamCount();
            return GetTypeParamType(ct - 1);
        }
        int LastTypeParamObjTypeId()const
        {
            int ct = GetTypeParamCount();
            return GetTypeParamObjTypeId(ct - 1);
        }

        std::string TypeName;
        int ObjectTypeId;
        int ObjectCategory;
        std::vector<int> TypeParams;
        MethodTableInfo MethodTable;
        FieldTableInfo FieldTable;
    };

    class MemberCallExp;
    class MemberSetExp;
    class MemberGetExp;
    class CollectionCallExp;
    class CollectionSetExp;
    class CollectionGetExp;
    class LoopListExp;
    class LinqExp;
    class SelectExp;
    class AbstractMemberCallApiProvider : public Brace::BraceApiImplHelper
    {
        friend class MemberCallExp;
    protected:
        virtual bool LoadMemberCall(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& objInfo, Brace::BraceApiExecutor&& obj, std::string&& member, std::vector<Brace::OperandLoadtimeInfo>&& argInfos, std::vector<Brace::BraceApiExecutor>&& args, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) = 0;
    protected:
        AbstractMemberCallApiProvider(Brace::BraceScript& interpreter) :Brace::BraceApiImplHelper(interpreter)
        {}
    };
    class AbstractMemberSetApiProvider : public Brace::BraceApiImplHelper
    {
        friend class MemberSetExp;
    protected:
        virtual bool LoadMemberSet(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& objInfo, Brace::BraceApiExecutor&& obj, std::string&& member, Brace::OperandLoadtimeInfo&& argInfo, Brace::BraceApiExecutor&& arg, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) = 0;
    protected:
        AbstractMemberSetApiProvider(Brace::BraceScript& interpreter) :Brace::BraceApiImplHelper(interpreter)
        {}
    };
    class AbstractMemberGetApiProvider : public Brace::BraceApiImplHelper
    {
        friend class MemberGetExp;
    protected:
        virtual bool LoadMemberGet(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& objInfo, Brace::BraceApiExecutor&& obj, std::string&& member, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) = 0;
    protected:
        AbstractMemberGetApiProvider(Brace::BraceScript& interpreter) :Brace::BraceApiImplHelper(interpreter)
        {}
    };
    class AbstractCollectionCallApiProvider : public Brace::BraceApiImplHelper
    {
        friend class CollectionCallExp;
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) const = 0;
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo) const = 0;
    protected:
        AbstractCollectionCallApiProvider(Brace::BraceScript& interpreter) :Brace::BraceApiImplHelper(interpreter)
        {}
    };
    class AbstractCollectionSetApiProvider : public Brace::BraceApiImplHelper
    {
        friend class CollectionSetExp;
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, const Brace::OperandLoadtimeInfo& arr, const Brace::OperandLoadtimeInfo& ix, const Brace::OperandLoadtimeInfo& val, Brace::OperandLoadtimeInfo& resultInfo) const = 0;
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& arr, const Brace::OperandRuntimeInfo& ix, const Brace::OperandRuntimeInfo& val, const Brace::OperandRuntimeInfo& resultInfo) const = 0;
    protected:
        AbstractCollectionSetApiProvider(Brace::BraceScript& interpreter) :Brace::BraceApiImplHelper(interpreter)
        {}
    };
    class AbstractCollectionGetApiProvider : public Brace::BraceApiImplHelper
    {
        friend class CollectionGetExp;
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, const Brace::OperandLoadtimeInfo& arr, const Brace::OperandLoadtimeInfo& ix, Brace::OperandLoadtimeInfo& resultInfo) const = 0;
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& arr, const Brace::OperandRuntimeInfo& ix, const Brace::OperandRuntimeInfo& resultInfo) const = 0;
    protected:
        AbstractCollectionGetApiProvider(Brace::BraceScript& interpreter) :Brace::BraceApiImplHelper(interpreter)
        {}
    };
    class AbstractLoopListApiProvider : public Brace::BraceApiImplHelper
    {
        friend class LoopListExp;
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::ISyntaxComponent& data, const BraceObjectInfo& braceObjInfo, const Brace::OperandLoadtimeInfo& listInfo, Brace::BraceApiExecutor& executor) = 0;
        virtual void StoreRuntimeInfo(Brace::OperandRuntimeInfo&& listInfo, Brace::BraceApiExecutor&& list, std::vector<Brace::BraceApiExecutor>&& statements, const std::vector<int>& objVars) = 0;
    protected:
        AbstractLoopListApiProvider(Brace::BraceScript& interpreter) :Brace::BraceApiImplHelper(interpreter)
        {}
    };
    class AbstractLinqApiProvider : public Brace::BraceApiImplHelper
    {
        friend class LinqExp;
    protected:
        virtual bool LoadLinqCall(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, int iteratorIndex, Brace::OperandLoadtimeInfo&& listInfo, Brace::BraceApiExecutor&& list, std::string&& member, std::vector<Brace::OperandLoadtimeInfo>&& argInfos, std::vector<Brace::BraceApiExecutor>&& args, std::vector<int>&& objVars, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) = 0;
    protected:
        AbstractLinqApiProvider(Brace::BraceScript& interpreter) :Brace::BraceApiImplHelper(interpreter)
        {}
    };
    class AbstractSelectApiProvider : public Brace::BraceApiImplHelper
    {
        friend class SelectExp;
    protected:
        enum StatMethodEnum
        {
            STAT_METHOD_NONE = -1,
            STAT_METHOD_MAX = 0,
            STAT_METHOD_MIN,
            STAT_METHOD_SUM,
            STAT_METHOD_AVG,
            STAT_METHOD_COUNT,
            MAX_STAT_METHOD_NUM
        };
    protected:
        virtual bool LoadSelect(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, int iteratorIndex, std::vector<Brace::OperandLoadtimeInfo>&& argInfos, std::vector<Brace::BraceApiExecutor>&& args, std::vector<int>&& statMethods, std::vector<int>&& objVars) = 0;
        virtual bool LoadTop(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo&& argInfo, Brace::BraceApiExecutor&& arg, std::vector<int>&& objVars) = 0;
        virtual bool LoadFromList(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, Brace::OperandLoadtimeInfo&& argInfo, Brace::BraceApiExecutor&& arg) = 0;
        virtual bool LoadFromType(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, const std::string& type) = 0;
        virtual bool LoadWhere(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, int iteratorIndex, Brace::OperandLoadtimeInfo&& argInfo, Brace::BraceApiExecutor&& arg, std::vector<int>&& objVars) = 0;
        virtual bool LoadOrderBy(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, int iteratorIndex, std::vector<Brace::OperandLoadtimeInfo>&& argInfos, std::vector<Brace::BraceApiExecutor>&& args, std::vector<bool>&& ascOrDescs, std::vector<int>&& objVars) = 0;
        virtual bool LoadGroupBy(const Brace::FuncInfo& func, const DslData::FunctionData& data, const BraceObjectInfo& braceObjInfo, int iteratorIndex, std::vector<Brace::OperandLoadtimeInfo>&& argInfos, std::vector<Brace::BraceApiExecutor>&& args, std::vector<int>&& objVars) = 0;
        virtual bool LoadHaving(const Brace::FuncInfo& func, const DslData::FunctionData& data, Brace::OperandLoadtimeInfo&& argInfo, Brace::BraceApiExecutor&& arg) = 0;
        virtual bool LoadStatements(const Brace::FuncInfo& func, const DslData::FunctionData& data, std::vector<Brace::BraceApiExecutor>&& statements, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) = 0;
        virtual void LoadResultIterator(std::vector<Brace::OperandLoadtimeInfo>&& iterators, std::vector<int>&& objVars) = 0;
    protected:
        AbstractSelectApiProvider(Brace::BraceScript& interpreter) :Brace::BraceApiImplHelper(interpreter)
        {}
    };
    /// <summary>
    /// We make a objtypeid -> BraceObjectInfo map, which allows object category info to process class by category,
    /// Such as DispatchObject, CustomObject, UObject etc.
    /// </summary>
    class BraceObjectInfoManager final
    {
    public:
        int GetObjectTypeId(const std::string& key)const
        {
            auto it = m_ObjTypeIdMap.find(key);
            if (it != m_ObjTypeIdMap.end())
                return it->second;
            return Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
        }
        int AddNewObjectTypeId(const std::string& key)
        {
            int newId = GenNextObjectTypeId();
            m_ObjTypeIdMap.insert(std::make_pair(key, newId));
            return newId;
        }
        BraceObjectInfo* AddBraceObjectInfo(int objTypeId, int objCategory, const std::string& typeName)
        {
            std::string name = typeName;
            return AddBraceObjectInfo(objTypeId, objCategory, std::move(name));
        }
        BraceObjectInfo* AddBraceObjectInfo(int objTypeId, int objCategory, std::string&& typeName)
        {
            auto it = m_ObjTypeIdMap.find(typeName);
            if (it == m_ObjTypeIdMap.end()) {
                m_ObjTypeIdMap.insert(std::make_pair(typeName, objTypeId));
            }
            BraceObjectInfo info{};
            info.TypeName = std::move(typeName);
            info.ObjectCategory = objCategory;
            info.ObjectTypeId = objTypeId;
            auto pair = m_ObjInfoMap.insert(std::make_pair(objTypeId, std::move(info)));
            return pair.second ? &(pair.first->second) : nullptr;
        }
        void AddBraceObjectAlias(int objTypeId, std::string&& typeNameAlias)
        {
            auto it = m_ObjTypeIdMap.find(typeNameAlias);
            if (it == m_ObjTypeIdMap.end()) {
                m_ObjTypeIdMap.insert(std::make_pair(typeNameAlias, objTypeId));
            }
        }
        void SetBraceObjectTypeParams(int objTypeId, int param1Type, int param1ObjTypeId)
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end()) {
                auto& ps = it->second.TypeParams;
                ps.clear();
                ps.push_back(param1Type);
                ps.push_back(param1ObjTypeId);
            }
        }
        void SetBraceObjectTypeParams(int objTypeId, int param1Type, int param1ObjTypeId, int param2Type, int param2ObjTypeId)
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end()) {
                auto& ps = it->second.TypeParams;
                ps.clear();
                ps.push_back(param1Type);
                ps.push_back(param1ObjTypeId);
                ps.push_back(param2Type);
                ps.push_back(param2ObjTypeId);
            }
        }
        void ClearBraceObjectTypeParams(int objTypeId)
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end()) {
                it->second.TypeParams.clear();
            }
        }
        void AddBraceObjectTypeParam(int objTypeId, int paramType, int paramObjTypeId)
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end()) {
                auto& ps = it->second.TypeParams;
                ps.push_back(paramType);
                ps.push_back(paramObjTypeId);
            }
        }
        const std::string& GetBraceObjectTypeName(int objTypeId)const
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end()) {
                return it->second.TypeName;
            }
            return EmptyString();
        }
        int GetBraceObjectCategory(int objTypeId)const
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end()) {
                return it->second.ObjectCategory;
            }
            return INVALID_ID;
        }
        int GetBraceObjectTypeParamCount(int objTypeId)const
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end()) {
                return it->second.GetTypeParamCount();
            }
            return 0;
        }
        int GetBraceObjectTypeParamType(int objTypeId, int ix)const
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end()) {
                return it->second.GetTypeParamType(ix);
            }
            return Brace::BRACE_DATA_TYPE_UNKNOWN;
        }
        int GetBraceObjectTypeParamObjTypeId(int objTypeId, int ix)const
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end()) {
                return it->second.GetTypeParamObjTypeId(ix);
            }
            return Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
        }
        int BraceObjectFirstTypeParamType(int objTypeId)const
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end()) {
                return it->second.FirstTypeParamType();
            }
            return Brace::BRACE_DATA_TYPE_UNKNOWN;
        }
        int BraceObjectFirstTypeParamObjTypeId(int objTypeId)const
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end()) {
                return it->second.FirstTypeParamType();
            }
            return Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
        }
        int BraceObjectSecondTypeParamType(int objTypeId)const
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end()) {
                return it->second.SecondTypeParamType();
            }
            return Brace::BRACE_DATA_TYPE_UNKNOWN;
        }
        int BraceObjectSecondTypeParamObjTypeId(int objTypeId)const
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end()) {
                return it->second.SecondTypeParamType();
            }
            return Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
        }
        int BraceObjectLastTypeParamType(int objTypeId)const
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end()) {
                return it->second.LastTypeParamType();
            }
            return Brace::BRACE_DATA_TYPE_UNKNOWN;
        }
        int BraceObjectLastTypeParamObjTypeId(int objTypeId)const
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end()) {
                return it->second.LastTypeParamType();
            }
            return Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
        }
        const BraceObjectInfo* GetBraceObjectInfo(int objTypeId)const
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end())
                return &(it->second);
            return nullptr;
        }
        BraceObjectInfo* GetBraceObjectInfo(int objTypeId)
        {
            auto it = m_ObjInfoMap.find(objTypeId);
            if (it != m_ObjInfoMap.end())
                return &(it->second);
            return nullptr;
        }
    public:
        bool TryGetOrAddBraceObjectInfo(const DslData::ISyntaxComponent& syntax, const Brace::LoadTypeInfoDelegation& doLoadTypeInfo, int& objTypeId)
        {
            bool ret = false;
            std::string key = CalcObjTypeKey(syntax, doLoadTypeInfo);
            objTypeId = GetObjectTypeId(key);
            if (objTypeId == Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN) {
                if (syntax.GetSyntaxType() == DslData::ISyntaxComponent::TYPE_FUNCTION) {
                    auto& funcData = static_cast<const DslData::FunctionData&>(syntax);
                    const std::string& id = syntax.GetId();
                    if (id == "decltype") {
                        auto& p = *funcData.GetParam(0);
                        Brace::OperandLoadtimeInfo loadInfo;
                        if (doLoadTypeInfo(p, loadInfo)) {
                            objTypeId = loadInfo.ObjectTypeId;
                            ret = true;
                        }
                    }
                    else if (id == "array") {
                        auto* p0 = funcData.GetParam(0);
                        int ot;
                        if (TryGetOrAddBraceObjectInfo(*p0, doLoadTypeInfo, ot)) {
                            objTypeId = AddNewObjectTypeId(key);
                            AddBraceObjectInfo(objTypeId, BRACE_OBJECT_CATEGORY_OBJ_ARRAY, std::move(key));
                            SetBraceObjectTypeParams(objTypeId, Brace::BRACE_DATA_TYPE_OBJECT, ot);
                            ret = true;
                        }
                    }
                    else if (id == "hashtable") {
                        const std::string& keyType = funcData.GetParamId(0);
                        int kt = Brace::GetDataType(keyType);
                        auto* p1 = funcData.GetParam(1);
                        int ot;
                        if (TryGetOrAddBraceObjectInfo(*p1, doLoadTypeInfo, ot)) {
                            objTypeId = AddNewObjectTypeId(key);
                            if (kt == Brace::BRACE_DATA_TYPE_STRING) {
                                AddBraceObjectInfo(objTypeId, BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE, std::move(key));
                                SetBraceObjectTypeParams(objTypeId, Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ, Brace::BRACE_DATA_TYPE_OBJECT, ot);
                                ret = true;
                            }
                            else if (kt >= Brace::BRACE_DATA_TYPE_INT8 && kt <= Brace::BRACE_DATA_TYPE_UINT64) {
                                AddBraceObjectInfo(objTypeId, BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE, std::move(key));
                                SetBraceObjectTypeParams(objTypeId, Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ, Brace::BRACE_DATA_TYPE_OBJECT, ot);
                                ret = true;
                            }
                        }
                    }
                }
            }
            else {
                ret = true;
            }
            return ret;
        }
    public:
        BraceObjectInfoManager() :m_ObjTypeIdMap(), m_ObjInfoMap(), m_NextObjectTypeId(BRACE_INNER_FIXED_OBJECT_TYPE_NUM)
        {}
    private:
        void CalcObjTypeKey(const DslData::ISyntaxComponent& syntax, const Brace::LoadTypeInfoDelegation& doLoadTypeInfo, std::stringstream& ss)
        {
            if (syntax.GetSyntaxType() == DslData::ISyntaxComponent::TYPE_FUNCTION) {
                auto& funcData = static_cast<const DslData::FunctionData&>(syntax);
                if (syntax.GetId() == "decltype") {
                    auto& p = *funcData.GetParam(0);
                    Brace::OperandLoadtimeInfo loadInfo;
                    if (doLoadTypeInfo(p, loadInfo)) {
                        CalcTypeInfoKey(loadInfo.Type, loadInfo.ObjectTypeId, ss);
                    }
                }
                else {
                    ss << syntax.GetId();
                    ss << "<:";
                    for (int ix = 0; ix < funcData.GetParamNum(); ++ix) {
                        if (ix > 0)
                            ss << ",";
                        auto& p = *funcData.GetParam(ix);
                        CalcObjTypeKey(p, doLoadTypeInfo, ss);
                    }
                    ss << ":>";
                }
            }
            else if (syntax.GetSyntaxType() == DslData::ISyntaxComponent::TYPE_VALUE) {
                ss << syntax.GetId();
            }
        }
        void CalcTypeInfoKey(int type, int objTypeId, std::stringstream& ss)
        {
            if (type == Brace::BRACE_DATA_TYPE_OBJECT) {
                ss << Brace::GetDataTypeName(type);
            }
            else {
                auto* pInfo = GetBraceObjectInfo(objTypeId);
                if (nullptr != pInfo) {
                    ss << pInfo->TypeName;
                    int size = static_cast<int>(pInfo->TypeParams.size());
                    if (size > 0) {
                        ss << "<:";
                        for (int i = 0; i + 1 < size; i += 2) {
                            int ty = pInfo->TypeParams[i];
                            int ot = pInfo->TypeParams[i + 1];
                            CalcTypeInfoKey(ty, ot, ss);
                        }
                        ss << ":>";
                    }
                }
                else {
                    ss << "[error]";
                }
            }
        }
        std::string CalcObjTypeKey(const DslData::ISyntaxComponent& syntax, const Brace::LoadTypeInfoDelegation& doLoadTypeInfo)
        {
            std::stringstream ss;
            CalcObjTypeKey(syntax, doLoadTypeInfo, ss);
            return ss.str();
        }
    private:
        int GenNextObjectTypeId()
        {
            return m_NextObjectTypeId++;
        }
    private:
        std::unordered_map<std::string, int> m_ObjTypeIdMap;
        std::unordered_map<int, BraceObjectInfo> m_ObjInfoMap;
        int m_NextObjectTypeId;
    public:
        static const std::string& EmptyString()
        {
            static std::string s_Empty{};
            return s_Empty;
        }
    };

    class StructObj final
    {
    public:
        void AllocMemory(const BraceObjectInfo* pInfo)
        {
            m_pObjectInfo = pInfo;
            m_pMemory = new char[m_pObjectInfo->FieldTable.Size];
        }
        void SetMemory(const BraceObjectInfo* pInfo, void* pMemory)
        {
            m_pObjectInfo = pInfo;
            m_pMemory = reinterpret_cast<char*>(pMemory);
        }
        void CacheObjField(int offset, const std::shared_ptr<void>& ptr)
        {
            PrepareObjFields();
            (*m_pObjFields)[offset] = ptr;
        }
        std::shared_ptr<void>* GetCachedObjField(int offset)
        {
            PrepareObjFields();
            auto it = m_pObjFields->find(offset);
            if (it != m_pObjFields->end())
                return &(it->second);
            return nullptr;
        }
        void CacheStrField(int offset, const std::string& txt)
        {
            PrepareStrFields();
            (*m_pStrFields)[offset] = txt;
        }
        std::string* GetCachedStrField(int offset)
        {
            PrepareStrFields();
            auto it = m_pStrFields->find(offset);
            if (it != m_pStrFields->end())
                return &(it->second);
            return nullptr;
        }
        const BraceObjectInfo* GetObjectInfo()const { return m_pObjectInfo; }
        void* GetMemory()const { return m_pMemory; }
    public:
        StructObj() :m_pObjectInfo(nullptr), m_pMemory(nullptr), m_pObjFields(nullptr), m_pStrFields(nullptr)
        {}
        virtual ~StructObj()
        {
            if (nullptr != m_pMemory) {
                delete[] m_pMemory;
                m_pMemory = nullptr;
            }
        }
    private:
        void PrepareObjFields()
        {
            if (nullptr == m_pObjFields)
                m_pObjFields = new std::unordered_map<int, std::shared_ptr<void>>();
        }
        void PrepareStrFields()
        {
            if (nullptr == m_pStrFields)
                m_pStrFields = new std::unordered_map<int, std::string>();
        }
    private:
        const BraceObjectInfo* m_pObjectInfo;
        char* m_pMemory;
        std::unordered_map<int, std::shared_ptr<void>>* m_pObjFields;
        std::unordered_map<int, std::string>* m_pStrFields;
    };

    template<typename T>
    using ArrayT = std::vector<T>;
    using ObjectArray = std::vector<std::shared_ptr<void>>;

    template<typename KeyT, typename ValT>
    using HashtableT = std::unordered_map<KeyT, ValT>;
    using StrObjHashtable = std::unordered_map<std::string, std::shared_ptr<void>>;
    using IntObjHashtable = std::unordered_map<int64_t, std::shared_ptr<void>>;

    template<typename ValueTypeT>
    struct Str2Type
    {
        static inline ValueTypeT Do(const std::string& src)
        {
            return ValueTypeT{};
        }
    };
    template<>
    struct Str2Type<std::string>
    {
        static inline std::string Do(const std::string& src)
        {
            return src;
        }
    };
    template<>
    struct Str2Type<int64_t>
    {
        static inline int64_t Do(const std::string& src)
        {
            return std::stol(src);
        }
    };
    template<>
    struct Str2Type<double>
    {
        static inline double Do(const std::string& src)
        {
            return std::stod(src);
        }
    };
    template<>
    struct Str2Type<bool>
    {
        static inline bool Do(const std::string& src)
        {
            return src == "true";
        }
    };

    template<typename ValueTypeT>
    struct Type2Str
    {
        static inline std::string Do(const ValueTypeT& src)
        {
            return std::string{};
        }
    };
    template<>
    struct Type2Str<std::string>
    {
        static inline std::string Do(const std::string& src)
        {
            return src;
        }
    };
    template<>
    struct Type2Str<int64_t>
    {
        static inline std::string Do(int64_t src)
        {
            return std::to_string(src);
        }
    };
    template<>
    struct Type2Str<double>
    {
        static inline std::string Do(double src)
        {
            return std::to_string(src);
        }
    };
    template<>
    struct Type2Str<bool>
    {
        static inline std::string Do(bool src)
        {
            return src ? "true" : "false";
        }
    };

    extern thread_local BraceObjectInfoManager g_ObjectInfoMgr;
}