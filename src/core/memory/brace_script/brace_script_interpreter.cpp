// Fill out your copyright notice in the Description page of Project Settings.

#include "brace_script_interpreter.h"

#define BRACE_SUPPORTED_PLATFORM

#ifdef BRACE_SUPPORTED_PLATFORM
#include "BraceScript.h"
#include "BraceCoroutine.h"
#include "BraceAny.h"
#include "brace_object.h"
#include "math_api.h"
#include <boost/dll/runtime_symbol_info.hpp>
#include <thread>

namespace BraceScriptInterpreter
{
    static std::chrono::time_point<std::chrono::high_resolution_clock> g_start_time_point;

    class BraceScriptManager;
    using DslBufferForCommand = DslParser::DslStringAndObjectBufferT<8192,1024,256>;
    thread_local static DslBufferForCommand* g_pDslBufferForCommand = nullptr;
    thread_local static IBraceScriptApiProvider* g_pApiProvider = nullptr;
    thread_local static BraceScriptManager* g_pBraceScriptManager = nullptr;
    thread_local BraceObjectInfoManager g_ObjectInfoMgr;

    struct DmntData
    {
        std::stringstream ss;
        uint64_t main_base;
        uint64_t main_size;
    };
    thread_local static DmntData g_DmntData;

    std::string get_exe_path()
    {
        auto&& exe_path = boost::dll::program_location();
        return exe_path.remove_filename().string();
    }
    std::string get_absolutely_path(const std::string& path)
    {
        std::filesystem::path p(path);
        if (p.is_relative()) {
            std::filesystem::path r(get_exe_path());
            return (r / p).string();
        }
        else {
            return path;
        }
    }
    std::string read_file(const std::string& filename) {
        std::ifstream file(get_absolutely_path(filename));
        if (!file.is_open()) {
            return "";
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
    bool write_file(const std::string& filename, const std::string& content) {
        std::ofstream file(get_absolutely_path(filename));
        if (!file.is_open()) {
            return false;
        }
        file << content;
        return true;
    }
    bool write_file(const std::string& filename, std::string&& content) {
        std::ofstream file(get_absolutely_path(filename));
        if (!file.is_open()) {
            return false;
        }
        file << content;
        return true;
    }
    std::vector<std::string> read_file_lines(const std::string& filename) {
        std::ifstream file(get_absolutely_path(filename));
        std::vector<std::string> lines;
        if (!file.is_open()) {
            return lines;
        }

        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
        }
        return lines;
    }
    bool write_file_lines(const std::string& filename, const std::vector<std::string>& lines) {
        std::ofstream file(get_absolutely_path(filename));
        if (!file.is_open()) {
            return false;
        }

        for (const auto& line : lines) {
            file << line << '\n';
        }
        return true;
    }
    std::string trim_string(const std::string& str)
    {
        auto is_space = [](unsigned char ch) { return std::isspace(ch); };

        auto start = std::find_if_not(str.begin(), str.end(), is_space);
        auto end = std::find_if_not(str.rbegin(), str.rend(), is_space).base();

        return (start < end) ? std::string(start, end) : "";
    }
    std::size_t replace_all(std::string& inout, const std::string& what, const std::string& with)
    {
        std::size_t count{};
        for (std::string::size_type pos{};
            inout.npos != (pos = inout.find(what.data(), pos, what.length()));
            pos += with.length(), ++count) {
            inout.replace(pos, what.length(), with.data(), with.length());
        }
        return count;
    }
    std::vector<std::string> split_string(const std::string& s, const std::string& delimiters)
    {
        std::vector<std::string> tokens{};
        std::stringstream ss;
        for (auto&& c : s) {
            if (std::string::npos == delimiters.find(c)) {
                ss << c;
            }
            else {
                if (ss.tellp() > 0)
                    tokens.push_back(ss.str());
                ss.str(std::string());
            }
        }
        if (ss.tellp() > 0)
            tokens.push_back(ss.str());
        return tokens;
    }
    static inline std::string get_first_unquoted_arg(const std::string& str, size_t& pos)
    {
        size_t len = str.length();
        if (len >= 2 && (str[0] == '"' || str[0] == '\'')) {
            std::stringstream ss;
            for (size_t ix = 1; ix < len; ++ix) {
                char c = str[ix];
                if (c == '\\') {
                    ++ix;
                }
                else if (c == str[0]) {
                    if (ix < len - 1 && c == str[ix + 1]) {
                        ++ix;
                    }
                    else {
                        pos = ix + 1;
                        break;
                    }
                }
                else {
                    ss << c;
                }
                if (ix == len - 1)
                    pos = len;
            }
            for (size_t ix = pos; ix < len; ++ix) {
                char c = str[ix];
                if (c == ' ' || c == '\t') {
                    pos = ix;
                    break;
                }
                else {
                    ss << c;
                }
                if (ix == len - 1)
                    pos = len;
            }
            return ss.str();
        }
        else {
            std::stringstream ss;
            for (size_t ix = 0; ix < len; ++ix) {
                char c = str[ix];
                if (c == ' ' || c == '\t') {
                    pos = ix;
                    break;
                }
                else {
                    ss << c;
                }
                if (ix == len - 1)
                    pos = len;
            }
            return ss.str();
        }
    }

    class BraceScriptCoroutine final : public CoroutineWithBoostContext::Coroutine
    {
    public:
        using RoutineType = std::function<void()>;

        RoutineType OnRoutine;

        Brace::RuntimeStack* GetRuntimeStack()
        {
            return &m_RuntimeStack;
        }
        void SetMsgId(const std::string& msgId)
        {
            m_MsgId = msgId;
        }
        const std::string& GetMsgId()const
        {
            return m_MsgId;
        }
    public:
        BraceScriptCoroutine() : CoroutineWithBoostContext::Coroutine(4 * 1024 * 1024), OnRoutine(nullptr), m_RuntimeStack(), m_MsgId()
        {
        }
        virtual ~BraceScriptCoroutine() override
        {
        }
    protected:
        virtual void Routine() override
        {
            if (OnRoutine)
                OnRoutine();
        }
    private:
        Brace::RuntimeStack m_RuntimeStack;
        std::string m_MsgId;
    };
    class BraceScriptManager final
    {
    public:
        using MessageQueue = std::queue<MessageArgs>;
    public:
        virtual ~BraceScriptManager()
        {
            if (nullptr != m_pBraceScript) {
                delete m_pBraceScript;
                m_pBraceScript = nullptr;
            }
            for (auto* p : m_DslFiles) {
                delete p;
            }
            m_DslFiles.clear();

            if (nullptr != m_pCallbackBraceScript) {
                delete m_pCallbackBraceScript;
                m_pCallbackBraceScript = nullptr;
            }
            for (auto* p : m_CallbackDslFiles) {
                p->Reset();
            }
            m_CallbackDslFiles.clear();

            if (nullptr != m_pBuffer) {
                delete m_pBuffer;
                m_pBuffer = nullptr;
            }
            m_Imports.clear();
        }
    private:
        Brace::RuntimeStack* GetRuntimeStack()
        {
            auto* p = CoroutineWithBoostContext::CurrentCoroutine();
            if (p == &m_ScriptCoroutine) {
                return m_ScriptCoroutine.GetRuntimeStack();
            }
            else {
                for (auto&& co : m_MessageHandlerCoroutines) {
                    if (p == co.get()) {
                        return co->GetRuntimeStack();
                    }
                }
            }
            return nullptr;
        }
        void AddImport(std::string&& scpTxt)
        {
            m_Imports.push_back(std::move(scpTxt));
        }
        void ClearImports()
        {
            m_Imports.clear();
        }
        void ResetScriptImpl()
        {
            if (nullptr != m_pBraceScript) {
                m_pBraceScript->Reset();
            }
            for (auto* p : m_DslFiles) {
                p->Reset();
            }
            m_DslFiles.clear();

            bool first = true;
            for (auto&& scp : m_Imports) {
                auto* pDslFile = LoadDslFile(scp.c_str(), first, false);
                if (pDslFile) {
                    int nextPos = m_pBraceScript->GetGlobalCodeNum();
                    m_pBraceScript->LoadScript(*pDslFile);
                    m_pBraceScript->Run(nextPos);
                }
                first = false;
            }
        }
        void RunScript()
        {
            if (m_ScriptTxt.empty() && !m_ScriptQueue.empty()) {
                std::swap(m_ScriptTxt, m_ScriptQueue.front());
                m_ScriptQueue.pop();
            }
            if (!m_ScriptTxt.empty()) {
                ClearMessagesImpl();

                auto* pDslFile = LoadDslFile(m_ScriptTxt.c_str(), true, false);
                if (pDslFile) {
                    int nextPos = m_pBraceScript->GetGlobalCodeNum();
                    m_pBraceScript->LoadScript(*pDslFile);
                    m_pBraceScript->Run(nextPos);
                }

                m_ScriptTxt.clear();
            }
            m_IsQuitting = false;
        }
        DslData::DslFile* LoadDslFile(const std::string& scp, bool resetParserBuffer, bool isCallback)
        {
            if (nullptr == m_pBuffer)
                m_pBuffer = new DslParser::DslStringAndObjectBufferT<>();
            else if (resetParserBuffer)
                m_pBuffer->Reset();

            DslParser::DslFile parsedFile(*m_pBuffer);
            parsedFile.Parse(scp.c_str());
            if (parsedFile.HasError()) {
                for (int i = 0; i < parsedFile.GetErrorNum(); ++i) {
                    g_pApiProvider->LogToView(std::string("[Syntax]: ") + parsedFile.GetErrorInfo(i));
                }
                return nullptr;
            }
            else {
                auto* pDslFile = new DslData::DslFile();
                if (isCallback)
                    m_CallbackDslFiles.push_back(pDslFile);
                else
                    m_DslFiles.push_back(pDslFile);
                Dsl::Transform(parsedFile, *pDslFile);
                return pDslFile;
            }
        }
        void WaitScriptRun()
        {
            m_ScriptCoroutine.TryStart();
            CoroutineWithBoostContext::TryYield();
        }
        void ScheduleMessageHandler()
        {
            auto* pCurrent = CoroutineWithBoostContext::CurrentCoroutine();
            if (pCurrent == &m_ScriptCoroutine) {
                for (auto&& coro : m_MessageHandlerCoroutines) {
                    auto&& it = m_MessageQueues.find(coro->GetMsgId());
                    if (it != m_MessageQueues.end() && (it->second.size() > 0 || !coro->IsTerminated())) {
                        coro->TryStart();
                    }
                }
            }
            CoroutineWithBoostContext::TryYield();
        }
        void HandleMessage(const std::string& msgId, const std::shared_ptr<Brace::FunctionExecutor>& exer)
        {
            auto&& it = m_MessageQueues.find(msgId);
            if (it != m_MessageQueues.end() && it->second.size() > 0) {
                auto&& msgArgs = it->second.front();
                int argCt = exer->GetArgCount();
                int argIx = 0;
                for (auto&& arg : msgArgs) {
                    if (argIx >= argCt)
                        break;
                    auto* pArgInfo = exer->ArgInfo(argIx);
                    if (const bool* pBoolVal = std::get_if<bool>(&arg)) {
                        Brace::VarSetBoolean(*m_pBraceScript->GlobalVariables(), pArgInfo->Type, pArgInfo->VarIndex, *pBoolVal);
                    }
                    else if (const int64_t* pI64Val = std::get_if<int64_t>(&arg)) {
                        Brace::VarSetI64(*m_pBraceScript->GlobalVariables(), pArgInfo->Type, pArgInfo->VarIndex, *pI64Val);
                    }
                    else if (const uint64_t* pU64Val = std::get_if<uint64_t>(&arg)) {
                        Brace::VarSetU64(*m_pBraceScript->GlobalVariables(), pArgInfo->Type, pArgInfo->VarIndex, *pU64Val);
                    }
                    else if (const double* pDoubleVal = std::get_if<double>(&arg)) {
                        Brace::VarSetF64(*m_pBraceScript->GlobalVariables(), pArgInfo->Type, pArgInfo->VarIndex, *pDoubleVal);
                    }
                    else if (const std::string* pStrVal = std::get_if<std::string>(&arg)) {
                        Brace::VarSetStr(*m_pBraceScript->GlobalVariables(), pArgInfo->Type, pArgInfo->VarIndex, *pStrVal);
                    }
                    else if (pArgInfo->Type == Brace::BRACE_DATA_TYPE_OBJECT && std::holds_alternative<std::shared_ptr<void>>(arg)) {
                        auto&& objVal = std::get<std::shared_ptr<void>>(arg);
                        Brace::VarSetObject(*m_pBraceScript->GlobalVariables(), pArgInfo->VarIndex, objVal);
                    }
                    else {
                        //unexpected
                    }
                    ++argIx;
                }
                it->second.pop();

                exer->Run(*m_pBraceScript->GlobalVariables(), *m_pBraceScript->GlobalVariables());
            }
        }
    private:
        void ResetCallbackImpl()
        {
            if (nullptr != m_pCallbackBraceScript) {
                m_pCallbackBraceScript->Reset();
            }
            for (auto* p : m_CallbackDslFiles) {
                p->Reset();
            }
            m_CallbackDslFiles.clear();
        }
        void LoadCallbackImpl(const std::string& scp)
        {
            auto* pDslFile = LoadDslFile(scp.c_str(), true, true);
            if (pDslFile) {
                int nextPos = m_pCallbackBraceScript->GetGlobalCodeNum();
                m_pCallbackBraceScript->LoadScript(*pDslFile);
                m_pCallbackBraceScript->Run(nextPos);
            }
        }
        void AddCallbackHandlerImpl(const std::string& id)
        {
            auto exer = std::make_shared<Brace::FunctionExecutor>(*m_pCallbackBraceScript);
            exer->Build(id);
            m_CallbackExers.insert(std::make_pair(id, std::move(exer)));
        }
        bool RunCallbackImpl(std::string&& msg, MessageArgs&& args)
        {
            bool ret = false;
            auto&& it = m_CallbackExers.find(msg);
            if (it != m_CallbackExers.end()) {
                auto&& exer = it->second;
                int argCt = exer->GetArgCount();
                int argIx = 0;
                for (auto&& arg : args) {
                    if (argIx >= argCt)
                        break;
                    auto* pArgInfo = exer->ArgInfo(argIx);
                    if (const bool* pBoolVal = std::get_if<bool>(&arg)) {
                        Brace::VarSetBoolean(*m_pCallbackBraceScript->GlobalVariables(), pArgInfo->Type, pArgInfo->VarIndex, *pBoolVal);
                    }
                    else if (const int64_t* pI64Val = std::get_if<int64_t>(&arg)) {
                        Brace::VarSetI64(*m_pCallbackBraceScript->GlobalVariables(), pArgInfo->Type, pArgInfo->VarIndex, *pI64Val);
                    }
                    else if (const uint64_t* pU64Val = std::get_if<uint64_t>(&arg)) {
                        Brace::VarSetU64(*m_pCallbackBraceScript->GlobalVariables(), pArgInfo->Type, pArgInfo->VarIndex, *pU64Val);
                    }
                    else if (const double* pDoubleVal = std::get_if<double>(&arg)) {
                        Brace::VarSetF64(*m_pCallbackBraceScript->GlobalVariables(), pArgInfo->Type, pArgInfo->VarIndex, *pDoubleVal);
                    }
                    else if (const std::string* pStrVal = std::get_if<std::string>(&arg)) {
                        Brace::VarSetStr(*m_pCallbackBraceScript->GlobalVariables(), pArgInfo->Type, pArgInfo->VarIndex, *pStrVal);
                    }
                    else if (pArgInfo->Type == Brace::BRACE_DATA_TYPE_OBJECT && std::holds_alternative<std::shared_ptr<void>>(arg)) {
                        auto&& objVal = std::get<std::shared_ptr<void>>(arg);
                        Brace::VarSetObject(*m_pCallbackBraceScript->GlobalVariables(), pArgInfo->VarIndex, objVal);
                    }
                    else {
                        //unexpected
                    }
                    ++argIx;
                }

                exer->Run(*m_pCallbackBraceScript->GlobalVariables(), *m_pCallbackBraceScript->GlobalVariables());
                ret = true;
            }
            return ret;
        }
    private:
        BraceScriptManager() : m_ScriptTxt(), m_ScriptQueue(), m_MessageQueues(), m_CommandQueue(), m_Imports(), m_IsQuitting(false),
            m_pBuffer(nullptr), m_DslFiles(), m_pBraceScript(nullptr), m_ScriptCoroutine(), m_MessageHandlerCoroutines(), m_CoroutineExers(),
            m_CallbackDslFiles(), m_pCallbackBraceScript(nullptr), m_CallbackExers()
        {
            m_ScriptCoroutine.OnRoutine = std::bind(&BraceScriptManager::RunScript, this);

            InitGlobalBraceObjectInfo();

            if (nullptr == m_pBraceScript) {
                InitBraceScript(m_pBraceScript, false);
            }
            if (nullptr == m_pCallbackBraceScript) {
                InitBraceScript(m_pCallbackBraceScript, true);
            }
        }
        void InitGlobalBraceObjectInfo();
        void InitBraceScript(Brace::BraceScript*& pBraceScript, bool isCallback);
        const std::map<std::string, std::string>& GetApiDocsImpl() const
        {
            assert(m_pBraceScript);
            return m_pBraceScript->GetApiDocs();
        }
        bool NeedRun()const
        {
            return !m_ScriptTxt.empty() || !m_ScriptQueue.empty();
        }
        void SetScriptImpl(std::string&& fstr)
        {
            m_ScriptTxt = fstr;
        }
        void AddMessageHandlerImpl(const std::string& id, int pool_num)
        {
            auto&& it = m_MessageQueues.find(id);
            if (it == m_MessageQueues.end()) {
                m_MessageQueues.insert(std::make_pair(id, MessageQueue()));
            }
            for (int i = 0; i < pool_num; ++i) {
                auto exer = std::make_shared<Brace::FunctionExecutor>(*m_pBraceScript);
                exer->Build(id);
                auto coro = std::make_shared<BraceScriptCoroutine>();
                coro->SetMsgId(id);
                coro->OnRoutine = [exer = exer, msgId = id, this]() {
                    HandleMessage(msgId, exer);
                };
                m_MessageHandlerCoroutines.push_back(coro);
                m_CoroutineExers.push_back(exer);
            }
        }
        bool SendMessageImpl(std::string&& msg)
        {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4455)
#endif
            using std::operator""sv;
            constexpr auto delim{ " "sv };
#ifdef _MSC_VER
#pragma warning(pop)
#endif
            std::string msgId, msgStr;
            SplitCmd(msg, msgId, msgStr);

            bool ret = false;
            auto&& it = m_MessageQueues.find(msgId);
            if (it != m_MessageQueues.end()) {
                MessageArgs args;
                std::string_view strView{msgStr};
                auto&& words = std::views::split(strView, delim);
                for (auto&& word : words) {
                    args.push_back(std::string(word.begin(), word.end()));
                }
                it->second.push(std::move(args));
                ret = true;
            }
            return ret;
        }
        bool SendMessageImpl(std::string&& msgId, MessageArgs&& args)
        {
            bool ret = false;
            auto&& it = m_MessageQueues.find(msgId);
            if (it != m_MessageQueues.end()) {
                it->second.push(std::move(args));
                ret = true;
            }
            return ret;
        }
        void ClearMessagesImpl()
        {
            for (auto&& pair : m_MessageQueues) {
                auto&& q = pair.second;
                q = MessageQueue();
            }
        }
        bool IsQuittingImpl()const { return m_IsQuitting; }
        void SetQuittingImpl(bool val)
        {
            m_IsQuitting = val;
            if (nullptr != m_pBraceScript) {
                m_pBraceScript->SetForceQuit(true);
            }
        }
        const std::queue<std::string>& GetScriptQueue()const
        {
            return m_ScriptQueue;
        }
        std::queue<std::string>& GetScriptQueue()
        {
            return m_ScriptQueue;
        }
        const std::queue<std::string>& GetCommandQueue()const
        {
            return m_CommandQueue;
        }
        std::queue<std::string>& GetCommandQueue()
        {
            return m_CommandQueue;
        }
    private:
        std::string m_ScriptTxt;
        std::queue<std::string> m_ScriptQueue;
        std::map<std::string, MessageQueue> m_MessageQueues;
        std::queue<std::string> m_CommandQueue;

        std::vector<std::string> m_Imports;

        bool m_IsQuitting;
        DslParser::IDslStringAndObjectBuffer* m_pBuffer;
        std::vector<DslData::DslFile*> m_DslFiles;
        Brace::BraceScript* m_pBraceScript;
        BraceScriptCoroutine m_ScriptCoroutine;
        std::vector<std::shared_ptr<BraceScriptCoroutine>> m_MessageHandlerCoroutines;
        std::vector< std::shared_ptr<Brace::FunctionExecutor>> m_CoroutineExers;

        std::vector<DslData::DslFile*> m_CallbackDslFiles;
        Brace::BraceScript* m_pCallbackBraceScript;
        std::unordered_map<std::string, std::shared_ptr<Brace::FunctionExecutor>> m_CallbackExers;
    public:
        static void PushScript(const std::string& scp)
        {
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->GetScriptQueue().push(scp);
            }
        }
        static void PushScript(std::string&& scp)
        {
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->GetScriptQueue().push(std::move(scp));
            }
        }
        static void AddImportScript(const std::string& scp)
        {
            if (nullptr != g_pBraceScriptManager) {
                std::string scpTxt = scp;
                g_pBraceScriptManager->AddImport(std::move(scpTxt));
            }
        }
        static void AddImportScript(std::string&& scp)
        {
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->AddImport(std::move(scp));
            }
        }
        static void ClearImportScripts()
        {
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->ClearImports();
            }
        }
        static void ResetScript()
        {
            SetQuitting(true);
            WaitQuitting();
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->ResetScriptImpl();
            }
        }
        static void SetScript(std::string&& scp)
        {
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->SetScriptImpl(std::move(scp));
            }
        }
        static void AddMessageHandler(const std::string& id, int pool_num)
        {
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->AddMessageHandlerImpl(id, pool_num);
            }
        }
    public:
        static void ResetCallback()
        {
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->ResetCallbackImpl();
            }
        }
        static void LoadCallback(std::string&& scp)
        {
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->LoadCallbackImpl(std::move(scp));
            }
        }
        static void AddCallbackHandler(const std::string& id)
        {
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->AddCallbackHandlerImpl(id);
            }
        }
        static bool RunCallback(std::string&& msg, MessageArgs&& args)
        {
            bool ret = false;
            if (nullptr != g_pBraceScriptManager) {
                ret = g_pBraceScriptManager->RunCallbackImpl(std::move(msg), std::move(args));
            }
            return ret;
        }
    public:
        static bool SendMessage(std::string&& msg)
        {
            bool ret = false;
            if (nullptr != g_pBraceScriptManager) {
                ret = g_pBraceScriptManager->SendMessageImpl(std::move(msg));
            }
            return ret;
        }
        static bool SendMessage(std::string&& msgId, MessageArgs&& args)
        {
            bool ret = false;
            if (nullptr != g_pBraceScriptManager) {
                ret = g_pBraceScriptManager->SendMessageImpl(std::move(msgId), std::move(args));
            }
            return ret;
        }
        static void ClearMessages()
        {
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->ClearMessages();
            }
        }
    public:
        static void Schedule()
        {
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->ScheduleMessageHandler();
            }
        }
        static void PushCommand(const std::string& cmd)
        {
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->GetCommandQueue().push(cmd);
            }
        }
        static void PushCommand(std::string&& cmd)
        {
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->GetCommandQueue().push(std::move(cmd));
            }
        }
        static bool ExistsCommands()
        {
            if (nullptr != g_pBraceScriptManager) {
                auto& q = g_pBraceScriptManager->GetCommandQueue();
                if (!q.empty()) {
                    return true;
                }
            }
            return false;
        }
        static bool TryPopCommand(std::string& cmd)
        {
            if (nullptr != g_pBraceScriptManager) {
                auto& q = g_pBraceScriptManager->GetCommandQueue();
                if (!q.empty()) {
                    std::swap(cmd, q.front());
                    q.pop();
                    return true;
                }
            }
            return false;
        }
    public:
        static void InitScript()
        {
            if (nullptr == g_pBraceScriptManager) {
                CoroutineWithBoostContext::TryInit();
                g_pBraceScriptManager = new BraceScriptManager();
            }
        }
        static const std::map<std::string, std::string>& GetApiDocs()
        {
            assert(g_pBraceScriptManager);
            return g_pBraceScriptManager->GetApiDocsImpl();
        }
        static void Go()
        {
            if (nullptr != g_pBraceScriptManager && g_pBraceScriptManager->NeedRun()) {
                g_pBraceScriptManager->WaitScriptRun();
            }
        }
        static void FreeScript()
        {
            if (nullptr != g_pBraceScriptManager) {
                delete g_pBraceScriptManager;
                CoroutineWithBoostContext::TryRelease();
                //std::size_t count = 0;
                //std::size_t alloced_size = 0;
                //std::size_t pooled_size = CoroutineWithBoostContext::StatMemory(count, alloced_size);
                //printf("[Brace Memory]: count:%llu pooled size:%llu, alloced size:%llu", count, pooled_size, alloced_size);
                CoroutineWithBoostContext::CleanupPool();
                g_pBraceScriptManager = nullptr;
            }
        }
        static bool IsQuitting()
        {
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->IsQuittingImpl();
            }
            return false;
        }
        static void SetQuitting(bool val)
        {
            if (nullptr != g_pBraceScriptManager) {
                g_pBraceScriptManager->SetQuittingImpl(val);
            }
        }
        static void WaitQuitting()
        {
            while (nullptr != g_pBraceScriptManager && g_pBraceScriptManager->IsQuittingImpl()) {
                if (g_pBraceScriptManager->NeedRun()) {
                    g_pBraceScriptManager->WaitScriptRun();
                }
                else {
                    g_pBraceScriptManager->SetQuittingImpl(false);
                }
            }
        }
    };
}

#define BRACE_SCRIPT_INTERPRETER_INC
#include "brace_object.inl"

namespace BraceScriptInterpreter
{

    class CallbackHandlerExp final : public Brace::AbstractBraceApi
    {
    public:
        CallbackHandlerExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter)
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& outerFunc, const DslData::FunctionData& funcData, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            if (funcData.IsHighOrder()) {
                auto&& callData = funcData.GetLowerOrderFunction();
                const std::string& func = callData.GetParamId(0);
                auto* curFunc = PushFuncInfo(func);
                int num = funcData.GetParamNum();
                for (int ix = 0; ix < num; ++ix) {
                    auto* exp = funcData.GetParam(ix);
                    Brace::OperandLoadtimeInfo expLoadInfo;
                    auto statement = LoadHelper(*exp, expLoadInfo);
                    if (!statement.isNull())
                        curFunc->Codes.push_back(std::move(statement));
                }
                resultInfo = Brace::OperandLoadtimeInfo();
                executor = nullptr;
                PopFuncInfo();
                BraceScriptManager::AddCallbackHandler(func);
                return true;
            }
            else {
                //error
                std::stringstream ss;
                ss << "expected oncallback(msg){...};" << funcData.GetId() << " line " << funcData.GetLine();
                LogError(ss.str());
                return false;
            }
        }
        virtual bool LoadStatement(const Brace::FuncInfo& outerFunc, const DslData::StatementData& funcData, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            bool hasError = false;
            if (funcData.GetFunctionNum() == 2) {
                auto* f1 = funcData.GetFirst()->AsFunction();
                auto* f2 = funcData.GetSecond()->AsFunction();
                if (f1 && !f1->IsHighOrder() && f1->HaveParam() && f2 && f2->IsHighOrder() && f2->HaveStatement()) {
                    const std::string& func = f1->GetParamId(0);
                    auto* newFunc = PushFuncInfo(func);
                    auto& callData = f2->GetLowerOrderFunction();
                    for (int ix = 0; ix < callData.GetParamNum(); ++ix) {
                        auto* p = callData.GetParam(ix);
                        if (p->GetSyntaxType() == DslData::ISyntaxComponent::TYPE_FUNCTION) {
                            auto* pf = static_cast<const DslData::FunctionData*>(p);
                            if (pf->IsOperatorParamClass() && pf->GetId() == ":") {
                                auto& name = pf->GetParamId(0);
                                auto* typeParam = pf->GetParam(1);
                                auto pti = ParseParamTypeInfo(*typeParam);
                                if (pti.IsRef) {
                                    int varIndex = AllocVariable(name, Brace::BRACE_DATA_TYPE_REF, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                                    newFunc->VarInitInfo.ReferenceVars[varIndex] = Brace::ReferenceInfo(pti.Type, pti.ObjectTypeId, INVALID_INDEX, nullptr);
                                    newFunc->Params.push_back(Brace::ParamRetInfo(name, pti.Type, pti.ObjectTypeId, varIndex, true));
                                }
                                else {
                                    int varIndex = AllocVariable(name, pti.Type, pti.ObjectTypeId);
                                    newFunc->Params.push_back(Brace::ParamRetInfo(name, pti.Type, pti.ObjectTypeId, varIndex, false));
                                }
                            }
                        }
                        else {
                            hasError = true;
                        }
                    }
                    int num = f2->GetParamNum();
                    for (int ix = 0; ix < num; ++ix) {
                        auto* exp = f2->GetParam(ix);
                        Brace::OperandLoadtimeInfo expLoadInfo;
                        auto statement = LoadHelper(*exp, expLoadInfo);
                        if (!statement.isNull())
                            newFunc->Codes.push_back(std::move(statement));
                    }
                    resultInfo = Brace::OperandLoadtimeInfo();
                    executor = nullptr;
                    PopFuncInfo();
                    BraceScriptManager::AddCallbackHandler(func);
                    return true;
                }
                else {
                    hasError = true;
                }
            }
            if (hasError) {
                //error
                std::stringstream ss;
                ss << "expected oncallback(msg)args($a:int,$b:int,...){...};" << funcData.GetId() << " line " << funcData.GetLine();
                LogError(ss.str());
            }
            return false;
        }
    };
    class MessageHandlerExp final : public Brace::AbstractBraceApi
    {
        static const int c_def_pool_num = 8;
    public:
        MessageHandlerExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter)
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& outerFunc, const DslData::FunctionData& funcData, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            if (funcData.IsHighOrder()) {
                auto&& callData = funcData.GetLowerOrderFunction();
                const std::string& func = callData.GetParamId(0);
                int pool_num = c_def_pool_num;
                if (callData.GetParamNum() > 1) {
                    pool_num = std::stoi(callData.GetParamId(1), nullptr, 0);
                }
                auto* curFunc = PushFuncInfo(func);
                int num = funcData.GetParamNum();
                for (int ix = 0; ix < num; ++ix) {
                    auto* exp = funcData.GetParam(ix);
                    Brace::OperandLoadtimeInfo expLoadInfo;
                    auto statement = LoadHelper(*exp, expLoadInfo);
                    if (!statement.isNull())
                        curFunc->Codes.push_back(std::move(statement));
                }
                resultInfo = Brace::OperandLoadtimeInfo();
                executor = nullptr;
                PopFuncInfo();
                BraceScriptManager::AddMessageHandler(func, pool_num);
                return true;
            }
            else {
                //error
                std::stringstream ss;
                ss << "expected onmessage(msg[, pool_num]){...};" << funcData.GetId() << " line " << funcData.GetLine();
                LogError(ss.str());
                return false;
            }
        }
        virtual bool LoadStatement(const Brace::FuncInfo& outerFunc, const DslData::StatementData& funcData, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            bool hasError = false;
            if (funcData.GetFunctionNum() == 2) {
                auto* f1 = funcData.GetFirst()->AsFunction();
                auto* f2 = funcData.GetSecond()->AsFunction();
                if (f1 && !f1->IsHighOrder() && f1->HaveParam() && f2 && f2->IsHighOrder() && f2->HaveStatement()) {
                    const std::string& func = f1->GetParamId(0);
                    int pool_num = c_def_pool_num;
                    if (f1->GetParamNum() > 1) {
                        pool_num = std::stoi(f1->GetParamId(1), nullptr, 0);
                    }
                    auto* newFunc = PushFuncInfo(func);
                    auto& callData = f2->GetLowerOrderFunction();
                    for (int ix = 0; ix < callData.GetParamNum(); ++ix) {
                        auto* p = callData.GetParam(ix);
                        if (p->GetSyntaxType() == DslData::ISyntaxComponent::TYPE_FUNCTION) {
                            auto* pf = static_cast<const DslData::FunctionData*>(p);
                            if (pf->IsOperatorParamClass() && pf->GetId() == ":") {
                                auto& name = pf->GetParamId(0);
                                auto* typeParam = pf->GetParam(1);
                                auto pti = ParseParamTypeInfo(*typeParam);
                                if (pti.IsRef) {
                                    int varIndex = AllocVariable(name, Brace::BRACE_DATA_TYPE_REF, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                                    newFunc->VarInitInfo.ReferenceVars[varIndex] = Brace::ReferenceInfo(pti.Type, pti.ObjectTypeId, INVALID_INDEX, nullptr);
                                    newFunc->Params.push_back(Brace::ParamRetInfo(name, pti.Type, pti.ObjectTypeId, varIndex, true));
                                }
                                else {
                                    int varIndex = AllocVariable(name, pti.Type, pti.ObjectTypeId);
                                    newFunc->Params.push_back(Brace::ParamRetInfo(name, pti.Type, pti.ObjectTypeId, varIndex, false));
                                }
                            }
                        }
                        else {
                            hasError = true;
                        }
                    }
                    int num = f2->GetParamNum();
                    for (int ix = 0; ix < num; ++ix) {
                        auto* exp = f2->GetParam(ix);
                        Brace::OperandLoadtimeInfo expLoadInfo;
                        auto statement = LoadHelper(*exp, expLoadInfo);
                        if (!statement.isNull())
                            newFunc->Codes.push_back(std::move(statement));
                    }
                    resultInfo = Brace::OperandLoadtimeInfo();
                    executor = nullptr;
                    PopFuncInfo();
                    BraceScriptManager::AddMessageHandler(func, pool_num);
                    return true;
                }
                else {
                    hasError = true;
                }
            }
            if (hasError) {
                //error
                std::stringstream ss;
                ss << "expected onmessage(msg[, pool_num])args($a:int,$b:int,...){...};" << funcData.GetId() << " line " << funcData.GetLine();
                LogError(ss.str());
            }
            return false;
        }
    };
    class ClearMessagesExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ClearMessagesExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo = Brace::OperandLoadtimeInfo();
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            BraceScriptManager::ClearMessages();
        }
    };
    class QCmdExp final : public Brace::SimpleBraceApiBase
    {
    public:
        QCmdExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            for (auto&& ali : argInfos) {
                if (!Brace::IsStringType(ali.Type)) {
                    std::stringstream ss;
                    ss << "cmd's param must be string ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            resultInfo = Brace::OperandLoadtimeInfo();
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            for (auto&& argInfo : argInfos) {
                const std::string& str = (argInfo.IsGlobal ? gvars : lvars).StringVars[argInfo.VarIndex];
                BraceScriptManager::PushCommand(str);
            }
        }
    };
    class CmdExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CmdExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            for (auto&& ali : argInfos) {
                if (!Brace::IsStringType(ali.Type)) {
                    std::stringstream ss;
                    ss << "cmd's param must be string ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            resultInfo = Brace::OperandLoadtimeInfo();
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            for (auto&& argInfo : argInfos) {
                const std::string& str = (argInfo.IsGlobal ? gvars : lvars).StringVars[argInfo.VarIndex];
                std::string cmd, arg;
                SplitCmd(str, cmd, arg);
                g_pApiProvider->ExecCommand(std::move(cmd), std::move(arg));
            }
        }
    };
    class WaitExp final : public Brace::SimpleBraceApiBase
    {
    public:
        WaitExp(Brace::BraceScript& interpreter, bool forCallback) :Brace::SimpleBraceApiBase(interpreter), m_ForCallback(forCallback)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            for (auto&& ali : argInfos) {
                if (ali.Type != Brace::BRACE_DATA_TYPE_INT32) {
                    std::stringstream ss;
                    ss << "wait's param must be int32 ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            resultInfo = Brace::OperandLoadtimeInfo();
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            using namespace std::chrono_literals;

            auto lcv = std::chrono::system_clock::now();

            for (auto&& argInfo : argInfos) {
                if (argInfo.Type == Brace::BRACE_DATA_TYPE_INT32) {
                    int v = (argInfo.IsGlobal ? gvars : lvars).NumericVars[argInfo.VarIndex].Int32Val;
                    if (v <= 60000) {
                        auto ccv = lcv;
                        while (std::chrono::duration_cast<std::chrono::milliseconds>(ccv - lcv).count() < static_cast<long long>(v)) {
                            if (IsForceQuit())
                                break;

                            if (m_ForCallback)
                                std::this_thread::sleep_for(10ms);
                            else
                                BraceScriptManager::Schedule();

                            ccv = std::chrono::system_clock::now();
                        }
                        printf("wait finish.");
                    }
                }
            }
        }
    private:
        bool m_ForCallback;
    };
    class WaitUntilQuitExp final : public Brace::SimpleBraceApiBase
    {
    public:
        WaitUntilQuitExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo = Brace::OperandLoadtimeInfo();
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            while (!IsForceQuit()) {
                BraceScriptManager::Schedule();
            }
            printf("wait for quit.");
        }
    };
    class TimeExp final : public Brace::SimpleBraceApiBase
    {
    public:
        TimeExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto tv = GetTimeUs();
            Brace::VarSetUInt64((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, tv);
        }
    };
    class Int2CharExp final : public Brace::SimpleBraceApiBase
    {
    public:
        Int2CharExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (Brace::IsSignedType(argInfo.Type) || Brace::IsUnsignedType(argInfo.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected int2char(integer) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo = argInfos[0];
            uint64_t v = Brace::VarGetU64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            char c = static_cast<char>(static_cast<uint8_t>(v));
            Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::string(1, c));
        }
    };
    class Char2IntExp final : public Brace::SimpleBraceApiBase
    {
    public:
        Char2IntExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (Brace::IsStringType(argInfo.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT8;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected char2int(string) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo = argInfos[0];
            const std::string& v = Brace::VarGetString((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            uint8_t r = 0;
            if (v.length() > 0) {
                r = static_cast<uint8_t>(v[0]);
            }
            Brace::VarSetUInt8((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, r);
        }
    };
    class Int2HexExp final : public Brace::SimpleBraceApiBase
    {
    public:
        Int2HexExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (Brace::IsSignedType(argInfo.Type) || Brace::IsUnsignedType(argInfo.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected int2hex(integer) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo = argInfos[0];
            uint64_t v = Brace::VarGetU64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            std::stringstream stream;
            stream << std::hex << v;
            Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, stream.str());
        }
    };
    class Hex2IntExp final : public Brace::SimpleBraceApiBase
    {
    public:
        Hex2IntExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (Brace::IsStringType(argInfo.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected hex2int(string) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo = argInfos[0];
            const std::string& v = Brace::VarGetString((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            if (v.length() > 2 && v[0] == '0' && v[1] == 'x')
                Brace::VarSetUInt64((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::stoull(v.substr(2, v.length() - 2), nullptr, 16));
            else
                Brace::VarSetUInt64((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::stoull(v, nullptr, 16));
        }
    };
    class Int2StrExp final : public Brace::SimpleBraceApiBase
    {
    public:
        Int2StrExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (Brace::IsSignedType(argInfo.Type) || Brace::IsUnsignedType(argInfo.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected int2str(integer) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo = argInfos[0];
            uint64_t v = Brace::VarGetU64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::to_string(v));
        }
    };
    class Str2IntExp final : public Brace::SimpleBraceApiBase
    {
    public:
        Str2IntExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (Brace::IsStringType(argInfo.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected str2int(string) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo = argInfos[0];
            const std::string& v = Brace::VarGetString((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            Brace::VarSetUInt64((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::stoull(v));
        }
    };
    class Float2StrExp final : public Brace::SimpleBraceApiBase
    {
    public:
        Float2StrExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1 || argInfos.size() == 2) {
                auto& argInfo = argInfos[0];
                if (argInfo.Type < Brace::BRACE_DATA_TYPE_STRING) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected float2str(number) or float2str(number, precise) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo = argInfos[0];
            double v = Brace::VarGetF64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
            std::stringstream ss;
            if (argInfos.size() == 2) {
                auto& argInfo2 = argInfos[1];
                int precise = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
                ss << std::fixed << std::setprecision(precise) << v;
            }
            else {
                ss << v;
            }
            Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, ss.str());
        }
    };
    class Str2FloatExp final : public Brace::SimpleBraceApiBase
    {
    public:
        Str2FloatExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (Brace::IsStringType(argInfo.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_DOUBLE;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected str2float(string) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo = argInfos[0];
            const std::string& v = Brace::VarGetString((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            Brace::VarSetDouble((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::stod(v));
        }
    };
    class StrConcatExp final : public Brace::SimpleBraceApiBase
    {
    public:
        StrConcatExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            std::stringstream ss;
            for (auto&& argInfo : argInfos) {
                std::string v = Brace::VarGetStr((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                ss << v;
            }
            Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, ss.str());
        }
    };
    class StrContainsOneExp final : public Brace::SimpleBraceApiBase
    {
    public:
        StrContainsOneExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() >= 1) {
                auto& argInfo = argInfos[0];
                if (Brace::IsStringType(argInfo.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected strcontainsone(string, string, ...) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& strInfo = argInfos[0];
            const std::string& str = Brace::VarGetString((strInfo.IsGlobal ? gvars : lvars), strInfo.VarIndex);
            bool ret = false;
            for (int ix = 1; ix < static_cast<int>(argInfos.size()); ++ix) {
                auto& argInfo = argInfos[ix];
                std::string v = Brace::VarGetStr((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                if (std::string::npos != str.find(v)) {
                    ret = true;
                    break;
                }
            }
            //L_EXIT:
            Brace::VarSetBool((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, ret);
        }
    };
    class StrContainsAllExp final : public Brace::SimpleBraceApiBase
    {
    public:
        StrContainsAllExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() >= 1) {
                auto& argInfo = argInfos[0];
                if (Brace::IsStringType(argInfo.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected strcontainsall(string, string, ...) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& strInfo = argInfos[0];
            const std::string& str = Brace::VarGetString((strInfo.IsGlobal ? gvars : lvars), strInfo.VarIndex);
            bool ret = true;
            for (int ix = 1; ix < static_cast<int>(argInfos.size()); ++ix) {
                auto& argInfo = argInfos[ix];
                std::string v = Brace::VarGetStr((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex);
                if (std::string::npos == str.find(v)) {
                    ret = false;
                    break;
                }
            }
            //L_EXIT:
            Brace::VarSetBool((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, ret);
        }
    };
    class StrIndexOfExp final : public Brace::SimpleBraceApiBase
    {
    public:
        StrIndexOfExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() >= 2) {
                auto& argInfo = argInfos[0];
                auto& argInfo2 = argInfos[1];
                if (Brace::IsStringType(argInfo.Type) && Brace::IsStringType(argInfo2.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected strindexof(string, string, int) or strindexof(string, string) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& strInfo = argInfos[0];
            auto& strInfo2 = argInfos[1];
            size_t pos = 0;
            if (argInfos.size() == 3) {
                auto& posInfo = argInfos[2];
                pos = static_cast<size_t>(Brace::VarGetU64((posInfo.IsGlobal ? gvars : lvars), posInfo.Type, posInfo.VarIndex));
            }
            const std::string& str = Brace::VarGetString((strInfo.IsGlobal ? gvars : lvars), strInfo.VarIndex);
            const std::string& str2 = Brace::VarGetString((strInfo2.IsGlobal ? gvars : lvars), strInfo2.VarIndex);
            size_t r = str.find(str2, pos);
            int rv = static_cast<int>(r);
            if (r == std::string::npos)
                rv = -1;
            Brace::VarSetInt32((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, rv);
        }
    };
    class StrLastIndexOfExp final : public Brace::SimpleBraceApiBase
    {
    public:
        StrLastIndexOfExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() >= 2) {
                auto& argInfo = argInfos[0];
                auto& argInfo2 = argInfos[1];
                if (Brace::IsStringType(argInfo.Type) && Brace::IsStringType(argInfo2.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected strlastindexof(string, string, int) or strlastindexof(string, string) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& strInfo = argInfos[0];
            auto& strInfo2 = argInfos[1];
            size_t pos = std::string::npos;
            if (argInfos.size() == 3) {
                auto& posInfo = argInfos[2];
                pos = static_cast<size_t>(Brace::VarGetU64((posInfo.IsGlobal ? gvars : lvars), posInfo.Type, posInfo.VarIndex));
            }
            const std::string& str = Brace::VarGetString((strInfo.IsGlobal ? gvars : lvars), strInfo.VarIndex);
            const std::string& str2 = Brace::VarGetString((strInfo2.IsGlobal ? gvars : lvars), strInfo2.VarIndex);
            size_t r = str.rfind(str2, pos);
            int rv = static_cast<int>(r);
            if (r == std::string::npos)
                rv = -1;
            Brace::VarSetInt32((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, rv);
        }
    };
    class StrLenExp final : public Brace::SimpleBraceApiBase
    {
    public:
        StrLenExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& argInfo = argInfos[0];
                if (Brace::IsStringType(argInfo.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected strlen(string) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo = argInfos[0];
            const std::string& v = Brace::VarGetString((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            Brace::VarSetInt32((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, static_cast<int>(v.length()));
        }
    };
    class SubStrExp final : public Brace::SimpleBraceApiBase
    {
    public:
        SubStrExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() >= 1) {
                auto& argInfo = argInfos[0];
                if (Brace::IsStringType(argInfo.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected substr(string, pos, count) or substr(string, pos) or substr(string) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& strInfo = argInfos[0];
            size_t pos = 0;
            size_t count = std::string::npos;
            if (argInfos.size() >= 2) {
                auto& posInfo = argInfos[1];
                pos = static_cast<size_t>(Brace::VarGetU64((posInfo.IsGlobal ? gvars : lvars), posInfo.Type, posInfo.VarIndex));
            }
            if (argInfos.size() == 3) {
                auto& countInfo = argInfos[2];
                count = static_cast<size_t>(Brace::VarGetU64((countInfo.IsGlobal ? gvars : lvars), countInfo.Type, countInfo.VarIndex));
            }
            const std::string& str = Brace::VarGetString((strInfo.IsGlobal ? gvars : lvars), strInfo.VarIndex);
            std::string r = str.substr(pos, count);
            Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::move(r));
        }
    };
    class StrReplaceExp final : public Brace::SimpleBraceApiBase
    {
    public:
        StrReplaceExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 3) {
                auto& argInfo = argInfos[0];
                auto& argInfo2 = argInfos[1];
                auto& argInfo3 = argInfos[2];
                if (Brace::IsStringType(argInfo.Type) && Brace::IsStringType(argInfo2.Type) && Brace::IsStringType(argInfo3.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected strreplace(string, string, string) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& strInfo = argInfos[0];
            auto& strInfo2 = argInfos[1];
            auto& strInfo3 = argInfos[2];
            const std::string& str = Brace::VarGetString((strInfo.IsGlobal ? gvars : lvars), strInfo.VarIndex);
            const std::string& str2 = Brace::VarGetString((strInfo2.IsGlobal ? gvars : lvars), strInfo2.VarIndex);
            const std::string& str3 = Brace::VarGetString((strInfo3.IsGlobal ? gvars : lvars), strInfo3.VarIndex);
            std::string r = str;
            replace_all(r, str2, str3);
            Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::move(r));
        }
    };
    class StrSplitExp final : public Brace::SimpleBraceApiBase
    {
    public:
        StrSplitExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 2) {
                auto& argInfo = argInfos[0];
                auto& argInfo2 = argInfos[1];
                if (Brace::IsStringType(argInfo.Type) && Brace::IsStringType(argInfo2.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
                    resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected strsplit(string, string) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& strInfo = argInfos[0];
            auto& strInfo2 = argInfos[1];
            const std::string& str = Brace::VarGetString((strInfo.IsGlobal ? gvars : lvars), strInfo.VarIndex);
            const std::string& str2 = Brace::VarGetString((strInfo2.IsGlobal ? gvars : lvars), strInfo2.VarIndex);
            auto arr = split_string(str, str2);
            using StrArray = ArrayT<std::string>;
            StrArray* pArr = new StrArray();
            std::swap(*pArr, arr);
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(pArr));
        }
    };
    class StrJoinExp final : public Brace::SimpleBraceApiBase
    {
    public:
        StrJoinExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 2) {
                auto& argInfo = argInfos[0];
                auto& argInfo2 = argInfos[1];
                if (Brace::IsObjectType(argInfo.Type) && argInfo.ObjectTypeId == CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY && Brace::IsStringType(argInfo2.Type)) {
                    resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
                    resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                    resultInfo.Name = GenTempVarName();
                    resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
                    return true;
                }
            }
            std::stringstream ss;
            ss << "expected strjoin(array<:string:>, string) ! line: " << data.GetLine();
            LogError(ss.str());
            return false;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& strInfo = argInfos[0];
            auto& strInfo2 = argInfos[1];
            auto& ptr = Brace::VarGetObject((strInfo.IsGlobal ? gvars : lvars), strInfo.VarIndex);
            const std::string& delim = Brace::VarGetString((strInfo2.IsGlobal ? gvars : lvars), strInfo2.VarIndex);
            using StrArray = ArrayT<std::string>;
            StrArray* pArr = static_cast<StrArray*>(ptr.get());
            if (nullptr != pArr) {
                std::stringstream ss;
                bool first = true;
                for (auto&& str : *pArr) {
                    if (!first)
                        ss << delim;
                    ss << str;
                    first = false;
                }
                Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, ss.str());
            }
            else {
                Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::string());
            }
        }
    };
    class CsvEchoExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CsvEchoExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            std::stringstream ss;
            bool first = true;
            for (auto&& info : argInfos) {
                std::string str;
                if (Brace::IsFloatType(info.Type)) {
                    double dv;
                    if (info.IsGlobal)
                        dv = Brace::VarGetF64(gvars, info.Type, info.VarIndex);
                    else
                        dv = Brace::VarGetF64(lvars, info.Type, info.VarIndex);
                    std::stringstream tss;
                    tss << std::fixed << std::setprecision(3) << dv;
                    str = tss.str();
                }
                else {
                    if (info.IsGlobal)
                        str = Brace::VarGetStr(gvars, info.Type, info.VarIndex);
                    else
                        str = Brace::VarGetStr(lvars, info.Type, info.VarIndex);
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
        }
    };
    class CsvConcatExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CsvConcatExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            std::stringstream ss;
            bool first = true;
            for (auto&& info : argInfos) {
                std::string str;
                if (Brace::IsFloatType(info.Type)) {
                    double dv;
                    if (info.IsGlobal)
                        dv = Brace::VarGetF64(gvars, info.Type, info.VarIndex);
                    else
                        dv = Brace::VarGetF64(lvars, info.Type, info.VarIndex);
                    std::stringstream tss;
                    tss << std::fixed << std::setprecision(3) << dv;
                    str = tss.str();
                }
                else {
                    if (info.IsGlobal)
                        str = Brace::VarGetStr(gvars, info.Type, info.VarIndex);
                    else
                        str = Brace::VarGetStr(lvars, info.Type, info.VarIndex);
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
            Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, ss.str());
        }
    };
    class CsvDebugExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CsvDebugExp(Brace::BraceScript& interpreter) : Brace::SimpleBraceApiBase(interpreter)
        {
        }

    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func,
            const DslData::FunctionData& data,
            const std::vector<Brace::OperandLoadtimeInfo>& argInfos,
            Brace::OperandLoadtimeInfo& resultInfo) override
        {
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars,
            Brace::VariableInfo& lvars,
            const std::vector<Brace::OperandRuntimeInfo>& argInfos,
            const Brace::OperandRuntimeInfo& resultInfo) const override
        {
            std::stringstream ss;
            bool first = true;
            for (auto&& info : argInfos) {
                std::string str;
                if (Brace::IsFloatType(info.Type)) {
                    double dv;
                    if (info.IsGlobal)
                        dv = Brace::VarGetF64(gvars, info.Type, info.VarIndex);
                    else
                        dv = Brace::VarGetF64(lvars, info.Type, info.VarIndex);
                    std::stringstream tss;
                    tss << std::fixed << std::setprecision(3) << dv;
                    str = tss.str();
                }
                else {
                    if (info.IsGlobal)
                        str = Brace::VarGetStr(gvars, info.Type, info.VarIndex);
                    else
                        str = Brace::VarGetStr(lvars, info.Type, info.VarIndex);
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
        }
    };

    class FileExistsExp final : public Brace::SimpleBraceApiBase
    {
    public:
        FileExistsExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& ai1 = argInfos[0];
                if (!Brace::IsStringType(ai1.Type)) {
                    std::stringstream ss;
                    ss << "expected fileexists(file_path) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "fileexists must have a string argument ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            const std::string& str = (argInfo0.IsGlobal ? gvars : lvars).StringVars[argInfo0.VarIndex];

            if (std::filesystem::exists(get_absolutely_path(str))) {
                Brace::VarSetBool((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, true);
            }
            else {
                Brace::VarSetBool((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, false);
            }
        }
    };
    class LoadFileExp final : public Brace::SimpleBraceApiBase
    {
    public:
        LoadFileExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& ai1 = argInfos[0];
                if (!Brace::IsStringType(ai1.Type)) {
                    std::stringstream ss;
                    ss << "expected loadfile(file_path) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "loadfile must have a string argument ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            const std::string& str = (argInfo0.IsGlobal ? gvars : lvars).StringVars[argInfo0.VarIndex];

            auto&& txt = read_file(str);
            if (txt.length() > 0) {
                Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, txt);
            }
            else {
                Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::string());
            }
        }
    };
    class SaveFileExp final : public Brace::SimpleBraceApiBase
    {
    public:
        SaveFileExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                if (!Brace::IsStringType(ai1.Type) || !Brace::IsStringType(ai2.Type)) {
                    std::stringstream ss;
                    ss << "expected savefile(string, file_path) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "savefile must have two args ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            auto& argInfo1 = argInfos[1];
            const std::string& txt = (argInfo0.IsGlobal ? gvars : lvars).StringVars[argInfo0.VarIndex];
            const std::string& str = (argInfo1.IsGlobal ? gvars : lvars).StringVars[argInfo1.VarIndex];

            if (write_file(str, txt)) {
                Brace::VarSetBool((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, true);
            }
            else {
                Brace::VarSetBool((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, false);
            }
        }
    };
    class LoadFileToArrayExp final : public Brace::SimpleBraceApiBase
    {
    public:
        LoadFileToArrayExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            int objTypeId = CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY;
            if (argInfos.size() == 1) {
                auto& ai1 = argInfos[0];
                if (!Brace::IsStringType(ai1.Type)) {
                    std::stringstream ss;
                    ss << "expected loadfiletoarray(file_path) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                objTypeId = ai2.ObjectTypeId;
                bool isArray = false;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY) {
                    isArray = true;
                }
                if (!Brace::IsStringType(ai1.Type) || !isArray) {
                    std::stringstream ss;
                    ss << "expected loadfiletoarray(file_path, typetag(array_type)) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "loadfiletoarray must have a string argument ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.ObjectTypeId = objTypeId;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            const std::string& str = (argInfo0.IsGlobal ? gvars : lvars).StringVars[argInfo0.VarIndex];
            int objTypeId = resultInfo.ObjectTypeId;

            std::vector<std::string> result = read_file_lines(str);
            if (result.size() > 0) {
                switch (objTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY:
                    DoLoad<std::string>(gvars, lvars, resultInfo, result);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY:
                    DoLoad<int64_t>(gvars, lvars, resultInfo, result);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY:
                    DoLoad<double>(gvars, lvars, resultInfo, result);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY:
                    DoLoad<bool>(gvars, lvars, resultInfo, result);
                    break;
                }
            }
            else {
                switch (objTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY:
                    DoLoadEmpty<std::string>(gvars, lvars, resultInfo);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY:
                    DoLoadEmpty<int64_t>(gvars, lvars, resultInfo);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY:
                    DoLoadEmpty<double>(gvars, lvars, resultInfo);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY:
                    DoLoadEmpty<bool>(gvars, lvars, resultInfo);
                    break;
                }
            }
        }
    private:
        template<typename TypeT>
        inline void DoLoad(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& resultInfo, const std::vector<std::string>& result)const
        {
            using ArrayType = ArrayT<TypeT>;
            auto* arrayObj = new ArrayType();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(arrayObj));
            auto& arr = *arrayObj;
            for (auto&& line : result) {
                auto val = Str2Type<TypeT>::Do(line);
                arr.push_back(std::move(val));
            }
        }
        template<typename TypeT>
        inline void DoLoadEmpty(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            using ArrayType = ArrayT<TypeT>;
            auto* arrayObj = new ArrayType();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(arrayObj));
        }
    };
    class SaveArrayToFileExp final : public Brace::SimpleBraceApiBase
    {
    public:
        SaveArrayToFileExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                int objTypeId = ai1.ObjectTypeId;
                bool isArray = false;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY) {
                    isArray = true;
                }
                if (!isArray || !Brace::IsStringType(ai2.Type)) {
                    std::stringstream ss;
                    ss << "expected savearraytofile(xxx_array, file_path) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "savearraytofile must have two args ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            auto& argInfo1 = argInfos[1];
            auto& arrayWrap = (argInfo0.IsGlobal ? gvars : lvars).ObjectVars[argInfo0.VarIndex];
            const std::string& str = (argInfo1.IsGlobal ? gvars : lvars).StringVars[argInfo1.VarIndex];

            std::vector<std::string> list{};
            switch (argInfo0.ObjectTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY:
                ArrayToStrArray<std::string>(gvars, lvars, arrayWrap, list);
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY:
                ArrayToStrArray<int64_t>(gvars, lvars, arrayWrap, list);
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY:
                ArrayToStrArray<double>(gvars, lvars, arrayWrap, list);
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY:
                ArrayToStrArray<bool>(gvars, lvars, arrayWrap, list);
                break;
            }
            if (write_file_lines(str, list)) {
                Brace::VarSetBool((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, true);
            }
            else {
                Brace::VarSetBool((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, false);
            }
        }
    private:
        template<typename TypeT>
        inline void ArrayToStrArray(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& arrayWrap, std::vector<std::string>& list)const
        {
            using ArrayType = ArrayT<TypeT>;
            auto* arrayObj = static_cast<ArrayType*>(arrayWrap.get());
            if (arrayObj) {
                for (auto&& line : *arrayObj) {
                    list.push_back(Type2Str<TypeT>::Do(line));
                }
            }
        }
    };

    class LoadHashtableExp final : public Brace::SimpleBraceApiBase
    {
    public:
        LoadHashtableExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            int objTypeId = CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE;
            if (argInfos.size() == 1) {
                auto& ai1 = argInfos[0];
                if (!Brace::IsStringType(ai1.Type)) {
                    std::stringstream ss;
                    ss << "expected loadhashtable(file_path) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                objTypeId = ai2.ObjectTypeId;
                bool isHash = false;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE) {
                    isHash = true;
                }
                if (!Brace::IsStringType(ai1.Type) || !isHash) {
                    std::stringstream ss;
                    ss << "expected loadhashtable(file_path, typetag(hash_type)) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "loadhashtable must have a string argument ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.ObjectTypeId = objTypeId;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            const std::string& str = (argInfo0.IsGlobal ? gvars : lvars).StringVars[argInfo0.VarIndex];
            int objTypeId = resultInfo.ObjectTypeId;

            std::vector<std::string> result = read_file_lines(str);
            if (result.size() > 0) {
                switch (objTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE:
                    DoLoad<std::string, std::string>(gvars, lvars, resultInfo, result);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE:
                    DoLoad<std::string, int64_t>(gvars, lvars, resultInfo, result);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE:
                    DoLoad<std::string, double>(gvars, lvars, resultInfo, result);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE:
                    DoLoad<std::string, bool>(gvars, lvars, resultInfo, result);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE:
                    DoLoad<int64_t, std::string>(gvars, lvars, resultInfo, result);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE:
                    DoLoad<int64_t, int64_t>(gvars, lvars, resultInfo, result);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE:
                    DoLoad<int64_t, double>(gvars, lvars, resultInfo, result);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE:
                    DoLoad<int64_t, bool>(gvars, lvars, resultInfo, result);
                    break;
                }
            }
            else {
                switch (objTypeId) {
                case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE:
                    DoLoadEmpty<std::string, std::string>(gvars, lvars, resultInfo);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE:
                    DoLoadEmpty<std::string, int64_t>(gvars, lvars, resultInfo);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE:
                    DoLoadEmpty<std::string, double>(gvars, lvars, resultInfo);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE:
                    DoLoadEmpty<std::string, bool>(gvars, lvars, resultInfo);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE:
                    DoLoadEmpty<int64_t, std::string>(gvars, lvars, resultInfo);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE:
                    DoLoadEmpty<int64_t, int64_t>(gvars, lvars, resultInfo);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE:
                    DoLoadEmpty<int64_t, double>(gvars, lvars, resultInfo);
                    break;
                case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE:
                    DoLoadEmpty<int64_t, bool>(gvars, lvars, resultInfo);
                    break;
                }
            }
        }
    private:
        template<typename KeyTypeT, typename ValueTypeT>
        inline void DoLoad(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& resultInfo, const std::vector<std::string>& result)const
        {
            using HashType = HashtableT<KeyTypeT, ValueTypeT>;
            auto* hashObj = new HashType();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(hashObj));
            auto& hash = *hashObj;
            for (auto&& line : result) {
                size_t si = line.find('\t');
                if (std::string::npos != si && si > 0) {
                    auto key = Str2Type<KeyTypeT>::Do(line.substr(0, si));
                    auto val = Str2Type<ValueTypeT>::Do(line.substr(si + 1));
                    if (hash.find(key) == hash.end()) {
                        hash.insert(std::make_pair(std::move(key), std::move(val)));
                    }
                }
            }
        }
        template<typename KeyTypeT, typename ValueTypeT>
        inline void DoLoadEmpty(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            using HashType = HashtableT<KeyTypeT, ValueTypeT>;
            auto* hashObj = new HashType();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(hashObj));
        }
    };
    class SaveHashtableExp final : public Brace::SimpleBraceApiBase
    {
    public:
        SaveHashtableExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                int objTypeId = ai1.ObjectTypeId;
                bool isHash = false;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE) {
                    isHash = true;
                }
                if (!isHash || !Brace::IsStringType(ai2.Type)) {
                    std::stringstream ss;
                    ss << "expected savehashtable(int_xxx_hash, file_path) or savememinfo(str_xxx_hash, file_path) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "savehashtable must have two args ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            auto& argInfo1 = argInfos[1];
            auto& hashWrap = (argInfo0.IsGlobal ? gvars : lvars).ObjectVars[argInfo0.VarIndex];
            const std::string& str = (argInfo1.IsGlobal ? gvars : lvars).StringVars[argInfo1.VarIndex];

            std::vector<std::string> list{};
            switch (argInfo0.ObjectTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE:
                HashtableToFStrArray<std::string, std::string>(gvars, lvars, hashWrap, list);
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE:
                HashtableToFStrArray<std::string, int64_t>(gvars, lvars, hashWrap, list);
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE:
                HashtableToFStrArray<std::string, double>(gvars, lvars, hashWrap, list);
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE:
                HashtableToFStrArray<std::string, bool>(gvars, lvars, hashWrap, list);
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE:
                HashtableToFStrArray<int64_t, std::string>(gvars, lvars, hashWrap, list);
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE:
                HashtableToFStrArray<int64_t, int64_t>(gvars, lvars, hashWrap, list);
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE:
                HashtableToFStrArray<int64_t, double>(gvars, lvars, hashWrap, list);
                break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE:
                HashtableToFStrArray<int64_t, bool>(gvars, lvars, hashWrap, list);
                break;
            }
            if (write_file_lines(str, list)) {
                Brace::VarSetBool((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, true);
            }
            else {
                Brace::VarSetBool((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, false);
            }
        }
    private:
        template<typename KeyTypeT, typename ValueTypeT>
        inline void HashtableToFStrArray(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& hashWrap, std::vector<std::string>& list)const
        {
            using HashType = HashtableT<KeyTypeT, ValueTypeT>;
            auto* hashObj = static_cast<HashType*>(hashWrap.get());
            if (nullptr != hashObj) {
                for (auto&& pair : *hashObj) {
                    std::string key = Type2Str<KeyTypeT>::Do(pair.first);
                    std::string val = Type2Str<ValueTypeT>::Do(pair.second);
                    list.push_back(key + "\t" + val);
                }
            }
        }
    };
    class CalcNewItemsExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CalcNewItemsExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter), m_ObjectCategory(INVALID_ID)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            int objTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
            if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                objTypeId = ai1.ObjectTypeId;
                bool isHash = false;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE) {
                    isHash = true;
                }
                else {
                    auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
                    if (nullptr != pInfo && (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE || pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE)) {
                        m_ObjectCategory = pInfo->ObjectCategory;
                        isHash = true;
                    }
                }
                if (!Brace::IsObjectType(ai1.Type) || !Brace::IsObjectType(ai2.Type) || ai1.ObjectTypeId != ai2.ObjectTypeId || !isHash) {
                    std::stringstream ss;
                    ss << "expected calcnewitems(int_xxx_hash, int_xxx_hash) or calcnewitems(str_xxx_hash, str_xxx_hash) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "calcnewitems must have two args ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.ObjectTypeId = objTypeId;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            auto& argInfo1 = argInfos[1];
            auto& hashWrap0 = (argInfo0.IsGlobal ? gvars : lvars).ObjectVars[argInfo0.VarIndex];
            auto& hashWrap1 = (argInfo1.IsGlobal ? gvars : lvars).ObjectVars[argInfo1.VarIndex];
            int objTypeId = argInfo0.ObjectTypeId;

            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE: {
                using StrStrHash = HashtableT<std::string, std::string>;
                DoCalc<StrStrHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                using StrIntHash = HashtableT<std::string, int64_t>;
                DoCalc<StrIntHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                using StrFloatHash = HashtableT<std::string, double>;
                DoCalc<StrFloatHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE: {
                using StrBoolHash = HashtableT<std::string, bool>;
                DoCalc<StrBoolHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE: {
                using IntStrHash = HashtableT<int64_t, std::string>;
                DoCalc<IntStrHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                using IntIntHash = HashtableT<int64_t, int64_t>;
                DoCalc<IntIntHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                using IntFloatHash = HashtableT<int64_t, double>;
                DoCalc<IntFloatHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE: {
                using IntBoolHash = HashtableT<int64_t, bool>;
                DoCalc<IntBoolHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            default: {
                if (m_ObjectCategory == BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE) {
                    DoCalc<IntObjHashtable>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
                }
                else if (m_ObjectCategory == BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE) {
                    DoCalc<StrObjHashtable>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
                }
            }break;
            }
        }
    private:
        template<typename HashObjT>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& hashWrap0, const std::shared_ptr<void>& hashWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* hashObj = new HashObjT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(hashObj));
            auto& hash = *hashObj;

            auto* hashObj0 = static_cast<HashObjT*>(hashWrap0.get());
            auto* hashObj1 = static_cast<HashObjT*>(hashWrap1.get());
            if (nullptr != hashObj0 && nullptr != hashObj1) {
                auto& hash0 = *hashObj0;
                auto& hash1 = *hashObj1;
                for (auto&& pair1 : hash1) {
                    if (hash0.find(pair1.first) == hash0.end()) {
                        if (hash.find(pair1.first) == hash.end()) {
                            hash.insert(pair1);
                        }
                    }
                }
            }
        }
    private:
        int m_ObjectCategory;
    };
    class CalcSameItemsExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CalcSameItemsExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter), m_ObjectCategory(INVALID_ID)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            int objTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
            if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                objTypeId = ai1.ObjectTypeId;
                bool isHash = false;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE) {
                    isHash = true;
                }
                else {
                    auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
                    if (nullptr != pInfo && (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE || pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE)) {
                        m_ObjectCategory = pInfo->ObjectCategory;
                        isHash = true;
                    }
                }
                if (!Brace::IsObjectType(ai1.Type) || !Brace::IsObjectType(ai2.Type) || ai1.ObjectTypeId != ai2.ObjectTypeId || !isHash) {
                    std::stringstream ss;
                    ss << "expected calcsameitems(int_xxx_hash, int_xxx_hash) or calcnewitems(str_xxx_hash, str_xxx_hash) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "calcsameitems must have two args ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.ObjectTypeId = objTypeId;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            auto& argInfo1 = argInfos[1];
            auto& hashWrap0 = (argInfo0.IsGlobal ? gvars : lvars).ObjectVars[argInfo0.VarIndex];
            auto& hashWrap1 = (argInfo1.IsGlobal ? gvars : lvars).ObjectVars[argInfo1.VarIndex];
            int objTypeId = argInfo0.ObjectTypeId;

            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE: {
                using StrStrHash = HashtableT<std::string, std::string>;
                DoCalc<StrStrHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                using StrIntHash = HashtableT<std::string, int64_t>;
                DoCalc<StrIntHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                using StrFloatHash = HashtableT<std::string, double>;
                DoCalc<StrFloatHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE: {
                using StrBoolHash = HashtableT<std::string, bool>;
                DoCalc<StrBoolHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE: {
                using IntStrHash = HashtableT<int64_t, std::string>;
                DoCalc<IntStrHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                using IntIntHash = HashtableT<int64_t, int64_t>;
                DoCalc<IntIntHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                using IntFloatHash = HashtableT<int64_t, double>;
                DoCalc<IntFloatHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE: {
                using IntBoolHash = HashtableT<int64_t, bool>;
                DoCalc<IntBoolHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            default: {
                if (m_ObjectCategory == BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE) {
                    DoCalc<IntObjHashtable>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
                }
                else if (m_ObjectCategory == BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE) {
                    DoCalc<StrObjHashtable>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
                }
            }break;
            }
        }
    private:
        template<typename HashObjT>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& hashWrap0, const std::shared_ptr<void>& hashWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* hashObj = new HashObjT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(hashObj));
            auto& hash = *hashObj;

            auto* hashObj0 = static_cast<HashObjT*>(hashWrap0.get());
            auto* hashObj1 = static_cast<HashObjT*>(hashWrap1.get());
            if (nullptr != hashObj0 && nullptr != hashObj1) {
                auto& hash0 = *hashObj0;
                auto& hash1 = *hashObj1;
                for (auto&& pair1 : hash1) {
                    if (hash0.find(pair1.first) != hash0.end()) {
                        if (hash.find(pair1.first) == hash.end()) {
                            hash.insert(pair1);
                        }
                    }
                }
            }
        }
    private:
        int m_ObjectCategory;
    };
    class CalcItemsUnionExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CalcItemsUnionExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter), m_ObjectCategory(INVALID_ID)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            int objTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
            if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                objTypeId = ai1.ObjectTypeId;
                bool isHash = false;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE) {
                    isHash = true;
                }
                else {
                    auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
                    if (nullptr != pInfo && (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE || pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE)) {
                        m_ObjectCategory = pInfo->ObjectCategory;
                        isHash = true;
                    }
                }
                if (!Brace::IsObjectType(ai1.Type) || !Brace::IsObjectType(ai2.Type) || ai1.ObjectTypeId != ai2.ObjectTypeId || !isHash) {
                    std::stringstream ss;
                    ss << "expected calcitemsunion(int_xxx_hash, int_xxx_hash) or calcsumitems(str_xxx_hash, str_xxx_hash) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "calcitemsunion must have two args ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.ObjectTypeId = objTypeId;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            auto& argInfo1 = argInfos[1];
            auto& hashWrap0 = (argInfo0.IsGlobal ? gvars : lvars).ObjectVars[argInfo0.VarIndex];
            auto& hashWrap1 = (argInfo1.IsGlobal ? gvars : lvars).ObjectVars[argInfo1.VarIndex];
            int objTypeId = argInfo0.ObjectTypeId;

            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE: {
                using StrStrHash = HashtableT<std::string, std::string>;
                DoCalc<StrStrHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                using StrIntHash = HashtableT<std::string, int64_t>;
                DoCalc<StrIntHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                using StrFloatHash = HashtableT<std::string, double>;
                DoCalc<StrFloatHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE: {
                using StrBoolHash = HashtableT<std::string, bool>;
                DoCalc<StrBoolHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE: {
                using IntStrHash = HashtableT<int64_t, std::string>;
                DoCalc<IntStrHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                using IntIntHash = HashtableT<int64_t, int64_t>;
                DoCalc<IntIntHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                using IntFloatHash = HashtableT<int64_t, double>;
                DoCalc<IntFloatHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE: {
                using IntBoolHash = HashtableT<int64_t, bool>;
                DoCalc<IntBoolHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            default: {
                if (m_ObjectCategory == BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE) {
                    DoCalc<IntObjHashtable>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
                }
                else if (m_ObjectCategory == BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE) {
                    DoCalc<StrObjHashtable>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
                }
            }break;
            }
        }
    private:
        template<typename HashObjT>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& hashWrap0, const std::shared_ptr<void>& hashWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* hashObj = new HashObjT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(hashObj));
            auto& hash = *hashObj;

            auto* hashObj0 = static_cast<HashObjT*>(hashWrap0.get());
            auto* hashObj1 = static_cast<HashObjT*>(hashWrap1.get());
            if (nullptr != hashObj0 && nullptr != hashObj1) {
                auto& hash0 = *hashObj0;
                auto& hash1 = *hashObj1;

                for (auto&& pair : hash1) {
                    if (hash.find(pair.first) == hash.end()) {
                        hash.insert(pair);
                    }
                }
                for (auto&& pair : hash0) {
                    if (hash.find(pair.first) == hash.end()) {
                        hash.insert(pair);
                    }
                }
            }
        }
    private:
        int m_ObjectCategory;
    };
    class ItemsAddExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ItemsAddExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            int objTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
            if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                objTypeId = ai1.ObjectTypeId;
                bool isHash = false;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE) {
                    isHash = true;
                }
                if (!Brace::IsObjectType(ai1.Type) || !Brace::IsObjectType(ai2.Type) || ai1.ObjectTypeId != ai2.ObjectTypeId || !isHash) {
                    std::stringstream ss;
                    ss << "expected itemsadd(int_xxx_hash, int_xxx_hash) or itemsadd(str_xxx_hash, str_xxx_hash) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "itemsadd must have two args ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.ObjectTypeId = objTypeId;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            auto& argInfo1 = argInfos[1];
            auto& hashWrap0 = (argInfo0.IsGlobal ? gvars : lvars).ObjectVars[argInfo0.VarIndex];
            auto& hashWrap1 = (argInfo1.IsGlobal ? gvars : lvars).ObjectVars[argInfo1.VarIndex];
            int objTypeId = argInfo0.ObjectTypeId;

            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE: {
                using StrStrHash = HashtableT<std::string, std::string>;
                DoCalc<StrStrHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                using StrIntHash = HashtableT<std::string, int64_t>;
                DoCalc<StrIntHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                using StrFloatHash = HashtableT<std::string, double>;
                DoCalc<StrFloatHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE: {
                using StrBoolHash = HashtableT<std::string, bool>;
                DoBoolCalc<StrBoolHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE: {
                using IntStrHash = HashtableT<int64_t, std::string>;
                DoCalc<IntStrHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                using IntIntHash = HashtableT<int64_t, int64_t>;
                DoCalc<IntIntHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                using IntFloatHash = HashtableT<int64_t, double>;
                DoCalc<IntFloatHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE: {
                using IntBoolHash = HashtableT<int64_t, bool>;
                DoBoolCalc<IntBoolHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            }
        }
    private:
        template<typename HashObjT>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& hashWrap0, const std::shared_ptr<void>& hashWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* hashObj = new HashObjT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(hashObj));
            auto& hash = *hashObj;

            auto* hashObj0 = static_cast<HashObjT*>(hashWrap0.get());
            auto* hashObj1 = static_cast<HashObjT*>(hashWrap1.get());
            if (nullptr != hashObj0 && nullptr != hashObj1) {
                auto& hash0 = *hashObj0;
                auto& hash1 = *hashObj1;
                for (auto&& pair : hash1) {
                    auto it = hash0.find(pair.first);
                    if (it != hash0.end()) {
                        if (hash.find(pair.first) == hash.end()) {
                            hash.insert(std::make_pair(pair.first, pair.second + it->second));
                        }
                    }
                }
            }
        }
        template<typename HashObjT>
        inline void DoBoolCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& hashWrap0, const std::shared_ptr<void>& hashWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* hashObj = new HashObjT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(hashObj));
            auto& hash = *hashObj;

            auto* hashObj0 = static_cast<HashObjT*>(hashWrap0.get());
            auto* hashObj1 = static_cast<HashObjT*>(hashWrap1.get());
            if (nullptr != hashObj0 && nullptr != hashObj1) {
                auto& hash0 = *hashObj0;
                auto& hash1 = *hashObj1;
                for (auto&& pair : hash1) {
                    auto it = hash0.find(pair.first);
                    if (it != hash0.end()) {
                        if (hash.find(pair.first) == hash.end()) {
                            hash.insert(std::make_pair(pair.first, pair.second || it->second));
                        }
                    }
                }
            }
        }
    };
    class ItemsSubExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ItemsSubExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            int objTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
            if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                objTypeId = ai1.ObjectTypeId;
                bool isHash = false;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE) {
                    isHash = true;
                }
                if (!Brace::IsObjectType(ai1.Type) || !Brace::IsObjectType(ai2.Type) || ai1.ObjectTypeId != ai2.ObjectTypeId || !isHash) {
                    std::stringstream ss;
                    ss << "expected itemssub(int_xxx_hash, int_xxx_hash) or itemssub(str_xxx_hash, str_xxx_hash) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "itemssub must have two args ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.ObjectTypeId = objTypeId;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            auto& argInfo1 = argInfos[1];
            auto& hashWrap0 = (argInfo0.IsGlobal ? gvars : lvars).ObjectVars[argInfo0.VarIndex];
            auto& hashWrap1 = (argInfo1.IsGlobal ? gvars : lvars).ObjectVars[argInfo1.VarIndex];
            int objTypeId = argInfo0.ObjectTypeId;

            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                using StrIntHash = HashtableT<std::string, int64_t>;
                DoCalc<StrIntHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                using StrFloatHash = HashtableT<std::string, double>;
                DoCalc<StrFloatHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE: {
                using StrBoolHash = HashtableT<std::string, bool>;
                DoBoolCalc<StrBoolHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                using IntIntHash = HashtableT<int64_t, int64_t>;
                DoCalc<IntIntHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                using IntFloatHash = HashtableT<int64_t, double>;
                DoCalc<IntFloatHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE: {
                using IntBoolHash = HashtableT<int64_t, bool>;
                DoBoolCalc<IntBoolHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            }
        }
    private:
        template<typename HashObjT>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& hashWrap0, const std::shared_ptr<void>& hashWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* hashObj = new HashObjT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(hashObj));
            auto& hash = *hashObj;

            auto* hashObj0 = static_cast<HashObjT*>(hashWrap0.get());
            auto* hashObj1 = static_cast<HashObjT*>(hashWrap1.get());
            if (nullptr != hashObj0 && nullptr != hashObj1) {
                auto& hash0 = *hashObj0;
                auto& hash1 = *hashObj1;
                for (auto&& pair : hash1) {
                    auto it = hash0.find(pair.first);
                    if (it != hash0.end()) {
                        if (hash.find(pair.first) == hash.end()) {
                            hash.insert(std::make_pair(pair.first, pair.second - it->second));
                        }
                    }
                }
            }
        }
        template<typename HashObjT>
        inline void DoBoolCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& hashWrap0, const std::shared_ptr<void>& hashWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* hashObj = new HashObjT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(hashObj));
            auto& hash = *hashObj;

            auto* hashObj0 = static_cast<HashObjT*>(hashWrap0.get());
            auto* hashObj1 = static_cast<HashObjT*>(hashWrap1.get());
            if (nullptr != hashObj0 && nullptr != hashObj1) {
                auto& hash0 = *hashObj0;
                auto& hash1 = *hashObj1;
                for (auto&& pair : hash1) {
                    auto it = hash0.find(pair.first);
                    if (it != hash0.end()) {
                        if (hash.find(pair.first) == hash.end()) {
                            hash.insert(std::make_pair(pair.first, pair.second == it->second ? false : true));
                        }
                    }
                }
            }
        }
    };
    class ItemsMulExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ItemsMulExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            int objTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
            if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                objTypeId = ai1.ObjectTypeId;
                bool isHash = false;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE) {
                    isHash = true;
                }
                if (!Brace::IsObjectType(ai1.Type) || !Brace::IsObjectType(ai2.Type) || ai1.ObjectTypeId != ai2.ObjectTypeId || !isHash) {
                    std::stringstream ss;
                    ss << "expected itemsmul(int_xxx_hash, int_xxx_hash) or itemsmul(str_xxx_hash, str_xxx_hash) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "itemsmul must have two args ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.ObjectTypeId = objTypeId;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            auto& argInfo1 = argInfos[1];
            auto& hashWrap0 = (argInfo0.IsGlobal ? gvars : lvars).ObjectVars[argInfo0.VarIndex];
            auto& hashWrap1 = (argInfo1.IsGlobal ? gvars : lvars).ObjectVars[argInfo1.VarIndex];
            int objTypeId = argInfo0.ObjectTypeId;

            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                using StrIntHash = HashtableT<std::string, int64_t>;
                DoCalc<StrIntHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                using StrFloatHash = HashtableT<std::string, double>;
                DoCalc<StrFloatHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE: {
                using StrBoolHash = HashtableT<std::string, bool>;
                DoBoolCalc<StrBoolHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                using IntIntHash = HashtableT<int64_t, int64_t>;
                DoCalc<IntIntHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                using IntFloatHash = HashtableT<int64_t, double>;
                DoCalc<IntFloatHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE: {
                using IntBoolHash = HashtableT<int64_t, bool>;
                DoBoolCalc<IntBoolHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            }
        }
    private:
        template<typename HashObjT>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& hashWrap0, const std::shared_ptr<void>& hashWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* hashObj = new HashObjT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(hashObj));
            auto& hash = *hashObj;

            auto* hashObj0 = static_cast<HashObjT*>(hashWrap0.get());
            auto* hashObj1 = static_cast<HashObjT*>(hashWrap1.get());
            if (nullptr != hashObj0 && nullptr != hashObj1) {
                auto& hash0 = *hashObj0;
                auto& hash1 = *hashObj1;
                for (auto&& pair : hash1) {
                    auto it = hash0.find(pair.first);
                    if (it != hash0.end()) {
                        if (hash.find(pair.first) == hash.end()) {
                            hash.insert(std::make_pair(pair.first, pair.second * it->second));
                        }
                    }
                }
            }
        }
        template<typename HashObjT>
        inline void DoBoolCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& hashWrap0, const std::shared_ptr<void>& hashWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* hashObj = new HashObjT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(hashObj));
            auto& hash = *hashObj;

            auto* hashObj0 = static_cast<HashObjT*>(hashWrap0.get());
            auto* hashObj1 = static_cast<HashObjT*>(hashWrap1.get());
            if (nullptr != hashObj0 && nullptr != hashObj1) {
                auto& hash0 = *hashObj0;
                auto& hash1 = *hashObj1;
                for (auto&& pair : hash1) {
                    auto it = hash0.find(pair.first);
                    if (it != hash0.end()) {
                        if (hash.find(pair.first) == hash.end()) {
                            hash.insert(std::make_pair(pair.first, pair.second && it->second));
                        }
                    }
                }
            }
        }
    };
    class ItemsDivExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ItemsDivExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            int objTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
            if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                objTypeId = ai1.ObjectTypeId;
                bool isHash = false;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE) {
                    isHash = true;
                }
                if (!Brace::IsObjectType(ai1.Type) || !Brace::IsObjectType(ai2.Type) || ai1.ObjectTypeId != ai2.ObjectTypeId || !isHash) {
                    std::stringstream ss;
                    ss << "expected itemsdiv(int_xxx_hash, int_xxx_hash) or itemsdiv(str_xxx_hash, str_xxx_hash) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "itemsdiv must have two args ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.ObjectTypeId = objTypeId;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            auto& argInfo1 = argInfos[1];
            auto& hashWrap0 = (argInfo0.IsGlobal ? gvars : lvars).ObjectVars[argInfo0.VarIndex];
            auto& hashWrap1 = (argInfo1.IsGlobal ? gvars : lvars).ObjectVars[argInfo1.VarIndex];
            int objTypeId = argInfo0.ObjectTypeId;

            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                using StrIntHash = HashtableT<std::string, int64_t>;
                DoCalc<StrIntHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                using StrFloatHash = HashtableT<std::string, double>;
                DoCalc<StrFloatHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                using IntIntHash = HashtableT<int64_t, int64_t>;
                DoCalc<IntIntHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                using IntFloatHash = HashtableT<int64_t, double>;
                DoCalc<IntFloatHash>(gvars, lvars, hashWrap0, hashWrap1, resultInfo);
            }break;
            }
        }
    private:
        template<typename HashObjT>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& hashWrap0, const std::shared_ptr<void>& hashWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* hashObj = new HashObjT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(hashObj));
            auto& hash = *hashObj;

            auto* hashObj0 = static_cast<HashObjT*>(hashWrap0.get());
            auto* hashObj1 = static_cast<HashObjT*>(hashWrap1.get());
            if (nullptr != hashObj0 && nullptr != hashObj1) {
                auto& hash0 = *hashObj0;
                auto& hash1 = *hashObj1;
                for (auto&& pair : hash1) {
                    auto it = hash0.find(pair.first);
                    if (it != hash0.end()) {
                        if (hash.find(pair.first) == hash.end()) {
                            hash.insert(std::make_pair(pair.first, pair.second / it->second));
                        }
                    }
                }
            }
        }
    };

    class ArrayAddExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ArrayAddExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            int objTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
            if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                objTypeId = ai1.ObjectTypeId;
                bool isArray = false;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY) {
                    isArray = true;
                }
                if (!Brace::IsObjectType(ai1.Type) || !Brace::IsObjectType(ai2.Type) || ai1.ObjectTypeId != ai2.ObjectTypeId || !isArray) {
                    std::stringstream ss;
                    ss << "expected arrayadd(int_array, int_array) or arrayadd(float_array, float_array) or arrayadd(bool_array, bool_array) or arrayadd(str_array, str_array) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "arrayadd must have two args ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.ObjectTypeId = objTypeId;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            auto& argInfo1 = argInfos[1];
            auto& arrayWrap0 = (argInfo0.IsGlobal ? gvars : lvars).ObjectVars[argInfo0.VarIndex];
            auto& arrayWrap1 = (argInfo1.IsGlobal ? gvars : lvars).ObjectVars[argInfo1.VarIndex];
            int objTypeId = argInfo0.ObjectTypeId;

            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY: {
                using StrArray = ArrayT<std::string>;
                DoCalc<StrArray>(gvars, lvars, arrayWrap0, arrayWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                using IntArray = ArrayT<int64_t>;
                DoCalc<IntArray>(gvars, lvars, arrayWrap0, arrayWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                using FloatArray = ArrayT<double>;
                DoCalc<FloatArray>(gvars, lvars, arrayWrap0, arrayWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY: {
                using BoolHash = ArrayT<bool>;
                DoBoolCalc<BoolHash>(gvars, lvars, arrayWrap0, arrayWrap1, resultInfo);
            }break;
            }
        }
    private:
        template<typename ArrayT>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& arrayWrap0, const std::shared_ptr<void>& arrayWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* arrayObj = new ArrayT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(arrayObj));
            auto& arr = *arrayObj;

            auto* arrayObj0 = static_cast<ArrayT*>(arrayWrap0.get());
            auto* arrayObj1 = static_cast<ArrayT*>(arrayWrap1.get());
            if (nullptr != arrayObj0 && nullptr != arrayObj1) {
                auto& array0 = *arrayObj0;
                auto& array1 = *arrayObj1;
                int ct0 = static_cast<int>(array0.size());
                int ct1 = static_cast<int>(array1.size());
                for (int i = 0; i < ct0 && i < ct1; ++i) {
                    arr.push_back(array1[i] + array0[i]);
                }
            }
        }
        template<typename ArrayT>
        inline void DoBoolCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& arrayWrap0, const std::shared_ptr<void>& arrayWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* arrayObj = new ArrayT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(arrayObj));
            auto& arr = *arrayObj;

            auto* arrayObj0 = static_cast<ArrayT*>(arrayWrap0.get());
            auto* arrayObj1 = static_cast<ArrayT*>(arrayWrap1.get());
            if (nullptr != arrayObj0 && nullptr != arrayObj1) {
                auto& array0 = *arrayObj0;
                auto& array1 = *arrayObj1;
                int ct0 = static_cast<int>(array0.size());
                int ct1 = static_cast<int>(array1.size());
                for (int i = 0; i < ct0 && i < ct1; ++i) {
                    arr.push_back(array1[i] || array0[i]);
                }
            }
        }
    };
    class ArraySubExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ArraySubExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            int objTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
            if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                objTypeId = ai1.ObjectTypeId;
                bool isArray = false;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY) {
                    isArray = true;
                }
                if (!Brace::IsObjectType(ai1.Type) || !Brace::IsObjectType(ai2.Type) || ai1.ObjectTypeId != ai2.ObjectTypeId || !isArray) {
                    std::stringstream ss;
                    ss << "expected arraysub(int_array, int_array) or arraysub(float_array, float_array) or arraysub(bool_array, bool_array) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "arraysub must have two args ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.ObjectTypeId = objTypeId;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            auto& argInfo1 = argInfos[1];
            auto& arrayWrap0 = (argInfo0.IsGlobal ? gvars : lvars).ObjectVars[argInfo0.VarIndex];
            auto& arrayWrap1 = (argInfo1.IsGlobal ? gvars : lvars).ObjectVars[argInfo1.VarIndex];
            int objTypeId = argInfo0.ObjectTypeId;

            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                using IntArray = ArrayT<int64_t>;
                DoCalc<IntArray>(gvars, lvars, arrayWrap0, arrayWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                using FloatArray = ArrayT<double>;
                DoCalc<FloatArray>(gvars, lvars, arrayWrap0, arrayWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY: {
                using BoolHash = ArrayT<bool>;
                DoBoolCalc<BoolHash>(gvars, lvars, arrayWrap0, arrayWrap1, resultInfo);
            }break;
            }
        }
    private:
        template<typename ArrayT>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& arrayWrap0, const std::shared_ptr<void>& arrayWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* arrayObj = new ArrayT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(arrayObj));
            auto& arr = *arrayObj;

            auto* arrayObj0 = static_cast<ArrayT*>(arrayWrap0.get());
            auto* arrayObj1 = static_cast<ArrayT*>(arrayWrap1.get());
            if (nullptr != arrayObj0 && nullptr != arrayObj1) {
                auto& array0 = *arrayObj0;
                auto& array1 = *arrayObj1;
                int ct0 = static_cast<int>(array0.size());
                int ct1 = static_cast<int>(array1.size());
                for (int i = 0; i < ct0 && i < ct1; ++i) {
                    arr.push_back(array1[i] - array0[i]);
                }
            }
        }
        template<typename ArrayT>
        inline void DoBoolCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& arrayWrap0, const std::shared_ptr<void>& arrayWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* arrayObj = new ArrayT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(arrayObj));
            auto& arr = *arrayObj;

            auto* arrayObj0 = static_cast<ArrayT*>(arrayWrap0.get());
            auto* arrayObj1 = static_cast<ArrayT*>(arrayWrap1.get());
            if (nullptr != arrayObj0 && nullptr != arrayObj1) {
                auto& array0 = *arrayObj0;
                auto& array1 = *arrayObj1;
                int ct0 = static_cast<int>(array0.size());
                int ct1 = static_cast<int>(array1.size());
                for (int i = 0; i < ct0 && i < ct1; ++i) {
                    arr.push_back(array1[i] == array0[i] ? false : true);
                }
            }
        }
    };
    class ArrayMulExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ArrayMulExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            int objTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
            if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                objTypeId = ai1.ObjectTypeId;
                bool isArray = false;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY) {
                    isArray = true;
                }
                if (!Brace::IsObjectType(ai1.Type) || !Brace::IsObjectType(ai2.Type) || ai1.ObjectTypeId != ai2.ObjectTypeId || !isArray) {
                    std::stringstream ss;
                    ss << "expected arraymul(int_array, int_array) or arraymul(float_array, float_array) or arraymul(bool_array, bool_array) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "arraymul must have two args ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.ObjectTypeId = objTypeId;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            auto& argInfo1 = argInfos[1];
            auto& arrayWrap0 = (argInfo0.IsGlobal ? gvars : lvars).ObjectVars[argInfo0.VarIndex];
            auto& arrayWrap1 = (argInfo1.IsGlobal ? gvars : lvars).ObjectVars[argInfo1.VarIndex];
            int objTypeId = argInfo0.ObjectTypeId;

            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                using IntArray = ArrayT<int64_t>;
                DoCalc<IntArray>(gvars, lvars, arrayWrap0, arrayWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                using FloatArray = ArrayT<double>;
                DoCalc<FloatArray>(gvars, lvars, arrayWrap0, arrayWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY: {
                using BoolHash = ArrayT<bool>;
                DoBoolCalc<BoolHash>(gvars, lvars, arrayWrap0, arrayWrap1, resultInfo);
            }break;
            }
        }
    private:
        template<typename ArrayT>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& arrayWrap0, const std::shared_ptr<void>& arrayWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* arrayObj = new ArrayT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(arrayObj));
            auto& arr = *arrayObj;

            auto* arrayObj0 = static_cast<ArrayT*>(arrayWrap0.get());
            auto* arrayObj1 = static_cast<ArrayT*>(arrayWrap1.get());
            if (nullptr != arrayObj0 && nullptr != arrayObj1) {
                auto& array0 = *arrayObj0;
                auto& array1 = *arrayObj1;
                int ct0 = static_cast<int>(array0.size());
                int ct1 = static_cast<int>(array1.size());
                for (int i = 0; i < ct0 && i < ct1; ++i) {
                    arr.push_back(array1[i] * array0[i]);
                }
            }
        }
        template<typename ArrayT>
        inline void DoBoolCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& arrayWrap0, const std::shared_ptr<void>& arrayWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* arrayObj = new ArrayT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(arrayObj));
            auto& arr = *arrayObj;

            auto* arrayObj0 = static_cast<ArrayT*>(arrayWrap0.get());
            auto* arrayObj1 = static_cast<ArrayT*>(arrayWrap1.get());
            if (nullptr != arrayObj0 && nullptr != arrayObj1) {
                auto& array0 = *arrayObj0;
                auto& array1 = *arrayObj1;
                int ct0 = static_cast<int>(array0.size());
                int ct1 = static_cast<int>(array1.size());
                for (int i = 0; i < ct0 && i < ct1; ++i) {
                    arr.push_back(array1[i] && array0[i]);
                }
            }
        }
    };
    class ArrayDivExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ArrayDivExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            int objTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN;
            if (argInfos.size() == 2) {
                auto& ai1 = argInfos[0];
                auto& ai2 = argInfos[1];
                objTypeId = ai1.ObjectTypeId;
                bool isArray = false;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY) {
                    isArray = true;
                }
                if (!Brace::IsObjectType(ai1.Type) || !Brace::IsObjectType(ai2.Type) || ai1.ObjectTypeId != ai2.ObjectTypeId || !isArray) {
                    std::stringstream ss;
                    ss << "expected arraydiv(int_array, int_array) or arraydiv(float_array, float_array) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "arraydiv must have two args ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.ObjectTypeId = objTypeId;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            auto& argInfo1 = argInfos[1];
            auto& arrayWrap0 = (argInfo0.IsGlobal ? gvars : lvars).ObjectVars[argInfo0.VarIndex];
            auto& arrayWrap1 = (argInfo1.IsGlobal ? gvars : lvars).ObjectVars[argInfo1.VarIndex];
            int objTypeId = argInfo0.ObjectTypeId;

            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                using IntArray = ArrayT<int64_t>;
                DoCalc<IntArray>(gvars, lvars, arrayWrap0, arrayWrap1, resultInfo);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                using FloatArray = ArrayT<double>;
                DoCalc<FloatArray>(gvars, lvars, arrayWrap0, arrayWrap1, resultInfo);
            }break;
            }
        }
    private:
        template<typename ArrayT>
        inline void DoCalc(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& arrayWrap0, const std::shared_ptr<void>& arrayWrap1, const Brace::OperandRuntimeInfo& resultInfo)const
        {
            auto* arrayObj = new ArrayT();
            Brace::VarSetObject((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::shared_ptr<void>(arrayObj));
            auto& arr = *arrayObj;

            auto* arrayObj0 = static_cast<ArrayT*>(arrayWrap0.get());
            auto* arrayObj1 = static_cast<ArrayT*>(arrayWrap1.get());
            if (nullptr != arrayObj0 && nullptr != arrayObj1) {
                auto& array0 = *arrayObj0;
                auto& array1 = *arrayObj1;
                int ct0 = static_cast<int>(array0.size());
                int ct1 = static_cast<int>(array1.size());
                for (int i = 0; i < ct0 && i < ct1; ++i) {
                    arr.push_back(array1[i] / array0[i]);
                }
            }
        }
    };

    class ArrayModifyExp final : public Brace::AbstractBraceApi
    {
    public:
        ArrayModifyExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter), m_IteratorIndex(INVALID_INDEX), m_ObjInfo(), m_Obj(), m_ExpInfo(), m_Exp(), m_ObjVars()
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& funcData, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //arraymodify(array, modify_exp);
            int num = funcData.GetParamNum();
            if (num == 2) {
                Brace::OperandLoadtimeInfo objInfo;
                m_Obj = LoadHelper(*funcData.GetParam(0), objInfo);
                m_ObjInfo = objInfo;

                bool success = true;
                PushBlock();
                int objTypeId = objInfo.ObjectTypeId;
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
                int elemType = Brace::BRACE_DATA_TYPE_UNKNOWN;
                int elemObjTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY) {
                    switch (objTypeId) {
                    case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY:
                        elemType = Brace::BRACE_DATA_TYPE_STRING;
                        break;
                    case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY:
                        elemType = Brace::BRACE_DATA_TYPE_INT64;
                        break;
                    case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY:
                        elemType = Brace::BRACE_DATA_TYPE_DOUBLE;
                        break;
                    case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY:
                        elemType = Brace::BRACE_DATA_TYPE_BOOL;
                        break;
                    }
                    m_IteratorIndex = AllocVariable("$$", elemType, elemObjTypeId);
                }
                else if (nullptr != pInfo && pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_OBJ_ARRAY) {
                    elemType = Brace::BRACE_DATA_TYPE_OBJECT;
                    elemObjTypeId = pInfo->GetTypeParamObjTypeId(0);
                    m_IteratorIndex = AllocVariable("$$", elemType, elemObjTypeId);
                }
                else {
                    success = false;
                }
                Brace::OperandLoadtimeInfo argInfo;
                m_Exp = LoadHelper(*funcData.GetParam(1), argInfo);
                m_ExpInfo = argInfo;
                if (!CanAssign(elemType, elemObjTypeId, m_ExpInfo.Type, m_ExpInfo.ObjectTypeId)) {
                    std::stringstream ss0;
                    ss0 << "expression type dismatch the array element, " << funcData.GetId() << " line " << funcData.GetLine();
                    LogError(ss0.str());
                    success = false;
                }
                m_ObjVars = CurBlockObjVars();
                PopBlock();
                executor.attach(this, &ArrayModifyExp::Execute);
                if (success)
                    return true;
            }
            //error
            std::stringstream ss;
            ss << "expected arraymodify(array, modify_exp), " << funcData.GetId() << " line " << funcData.GetLine();
            LogError(ss.str());
            return false;
        }
    private:
        int Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            auto& objPtr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            int objTypeId = m_ObjInfo.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY: {
                using ArrayType = ArrayT<std::string>;
                DoModify<ArrayType>(gvars, lvars, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY: {
                using ArrayType = ArrayT<int64_t>;
                DoModify<ArrayType>(gvars, lvars, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY: {
                using ArrayType = ArrayT<double>;
                DoModify<ArrayType>(gvars, lvars, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY: {
                using ArrayType = ArrayT<bool>;
                DoModify<ArrayType>(gvars, lvars, objPtr);
            }break;
            default: {
                using ArrayType = ObjectArray;
                DoModify<ArrayType>(gvars, lvars, objPtr);
            }break;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    private:
        template<typename ArrayType>
        inline void DoModify(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& arrayWrap)const
        {
            using value_type = typename ArrayType::value_type;
            auto* arrayObj = static_cast<ArrayType*>(arrayWrap.get());
            if (nullptr != arrayObj) {
                auto& arr = *arrayObj;
                for (int ix = 0; ix < static_cast<int>(arr.size()); ++ix) {
                    DoSetIter(lvars, m_IteratorIndex, arr[ix]);
                    if (!m_Exp.isNull())
                        m_Exp(gvars, lvars);
                    arr[ix] = DoGetVal<value_type>(gvars, lvars);
                    FreeObjVars(lvars, m_ObjVars);
                }
            }
        }
        inline void DoSetIter(Brace::VariableInfo& lvars, int index, const std::string& val)const
        {
            Brace::VarSetString(lvars, index, val);
        }
        inline void DoSetIter(Brace::VariableInfo& lvars, int index, int64_t val)const
        {
            Brace::VarSetInt64(lvars, index, val);
        }
        inline void DoSetIter(Brace::VariableInfo& lvars, int index, double val)const
        {
            Brace::VarSetDouble(lvars, index, val);
        }
        inline void DoSetIter(Brace::VariableInfo& lvars, int index, bool val)const
        {
            Brace::VarSetBool(lvars, index, val);
        }
        inline void DoSetIter(Brace::VariableInfo& lvars, int index, const std::shared_ptr<void>& val)const
        {
            Brace::VarSetObject(lvars, index, val);
        }
        template<typename RetT>
        inline RetT DoGetVal(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            return RetT{};
        }
        template<>
        inline std::string DoGetVal<std::string>(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            return Brace::VarGetStr((m_ExpInfo.IsGlobal ? gvars : lvars), m_ExpInfo.Type, m_ExpInfo.VarIndex);
        }
        template<>
        inline int64_t DoGetVal<int64_t>(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            return Brace::VarGetI64((m_ExpInfo.IsGlobal ? gvars : lvars), m_ExpInfo.Type, m_ExpInfo.VarIndex);
        }
        template<>
        inline double DoGetVal<double>(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            return Brace::VarGetF64((m_ExpInfo.IsGlobal ? gvars : lvars), m_ExpInfo.Type, m_ExpInfo.VarIndex);
        }
        template<>
        inline bool DoGetVal<bool>(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            return Brace::VarGetBoolean((m_ExpInfo.IsGlobal ? gvars : lvars), m_ExpInfo.Type, m_ExpInfo.VarIndex);
        }
        template<>
        inline std::shared_ptr<void> DoGetVal<std::shared_ptr<void>>(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            return Brace::VarGetObject((m_ExpInfo.IsGlobal ? gvars : lvars), m_ExpInfo.VarIndex);
        }
    private:
        int m_IteratorIndex;
        Brace::OperandRuntimeInfo m_ObjInfo;
        Brace::BraceApiExecutor m_Obj;
        Brace::OperandRuntimeInfo m_ExpInfo;
        Brace::BraceApiExecutor m_Exp;
        std::vector<int> m_ObjVars;
    };
    class HashtableModifyExp final : public Brace::AbstractBraceApi
    {
    public:
        HashtableModifyExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter), m_ObjectCategory(INVALID_ID), m_IteratorIndex(INVALID_INDEX), m_ValIteratorIndex(INVALID_INDEX), m_ObjInfo(), m_Obj(), m_ExpInfo(), m_Exp(), m_ObjVars()
        {
        }
    protected:
        virtual bool LoadFunction(const Brace::FuncInfo& func, const DslData::FunctionData& funcData, Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //hashtablemodify(hashtable, modify_exp);
            int num = funcData.GetParamNum();
            if (num == 2) {
                Brace::OperandLoadtimeInfo objInfo;
                m_Obj = LoadHelper(*funcData.GetParam(0), objInfo);
                m_ObjInfo = objInfo;

                bool success = true;
                PushBlock();
                int objTypeId = objInfo.ObjectTypeId;
                auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
                int elemType = Brace::BRACE_DATA_TYPE_UNKNOWN;
                int elemObjTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
                if (objTypeId >= CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE && objTypeId <= CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE) {
                    switch (objTypeId) {
                    case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE:
                        m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                        elemType = Brace::BRACE_DATA_TYPE_STRING;
                        break;
                    case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE:
                        m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                        elemType = Brace::BRACE_DATA_TYPE_INT64;
                        break;
                    case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE:
                        m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                        elemType = Brace::BRACE_DATA_TYPE_DOUBLE;
                        break;
                    case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE:
                        m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                        elemType = Brace::BRACE_DATA_TYPE_BOOL;
                        break;
                    case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE:
                        m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                        elemType = Brace::BRACE_DATA_TYPE_STRING;
                        break;
                    case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE:
                        m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                        elemType = Brace::BRACE_DATA_TYPE_INT64;
                        break;
                    case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE:
                        m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                        elemType = Brace::BRACE_DATA_TYPE_DOUBLE;
                        break;
                    case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE:
                        m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                        elemType = Brace::BRACE_DATA_TYPE_BOOL;
                        break;
                    }
                    m_IteratorIndex = AllocVariable("$$v", elemType, elemObjTypeId);
                }
                else if (nullptr != pInfo && (pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE || pInfo->ObjectCategory == BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE)) {
                    switch (pInfo->ObjectCategory) {
                    case BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE:
                        m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                        break;
                    case BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE:
                        m_IteratorIndex = AllocVariable("$$k", Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);
                        break;
                    }
                    elemType = Brace::BRACE_DATA_TYPE_OBJECT;
                    elemObjTypeId = pInfo->GetTypeParamObjTypeId(1);
                    m_IteratorIndex = AllocVariable("$$v", elemType, elemObjTypeId);
                    m_ObjectCategory = pInfo->ObjectCategory;
                }
                else {
                    success = false;
                }
                Brace::OperandLoadtimeInfo argInfo;
                m_Exp = LoadHelper(*funcData.GetParam(1), argInfo);
                m_ExpInfo = argInfo;
                if (!CanAssign(elemType, elemObjTypeId, m_ExpInfo.Type, m_ExpInfo.ObjectTypeId)) {
                    std::stringstream ss0;
                    ss0 << "expression type dismatch the hashtable value element, " << funcData.GetId() << " line " << funcData.GetLine();
                    LogError(ss0.str());
                    success = false;
                }
                m_ObjVars = CurBlockObjVars();
                PopBlock();
                executor.attach(this, &HashtableModifyExp::Execute);
                if (success)
                    return true;
            }
            //error
            std::stringstream ss;
            ss << "expected hashtablemodify(hashtable, modify_exp), " << funcData.GetId() << " line " << funcData.GetLine();
            LogError(ss.str());
            return false;
        }
    private:
        int Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            if (!m_Obj.isNull())
                m_Obj(gvars, lvars);
            auto& objPtr = Brace::VarGetObject((m_ObjInfo.IsGlobal ? gvars : lvars), m_ObjInfo.VarIndex);
            int objTypeId = m_ObjInfo.ObjectTypeId;
            switch (objTypeId) {
            case CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE: {
                using HashType = HashtableT<std::string, std::string>;
                DoModify<HashType>(gvars, lvars, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE: {
                using HashType = HashtableT<std::string, int64_t>;
                DoModify<HashType>(gvars, lvars, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE: {
                using HashType = HashtableT<std::string, double>;
                DoModify<HashType>(gvars, lvars, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE: {
                using HashType = HashtableT<std::string, bool>;
                DoModify<HashType>(gvars, lvars, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE: {
                using HashType = HashtableT<int64_t, std::string>;
                DoModify<HashType>(gvars, lvars, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE: {
                using HashType = HashtableT<int64_t, int64_t>;
                DoModify<HashType>(gvars, lvars, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE: {
                using HashType = HashtableT<int64_t, double>;
                DoModify<HashType>(gvars, lvars, objPtr);
            }break;
            case CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE: {
                using HashType = HashtableT<int64_t, bool>;
                DoModify<HashType>(gvars, lvars, objPtr);
            }break;
            default:
                switch (m_ObjectCategory) {
                case BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE: {
                    using HashType = IntObjHashtable;
                    DoModify<HashType>(gvars, lvars, objPtr);
                }break;
                case BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE: {
                    using HashType = StrObjHashtable;
                    DoModify<HashType>(gvars, lvars, objPtr);
                }break;
                }
                break;
            }
            return Brace::BRACE_FLOW_CONTROL_NORMAL;
        }
    private:
        template<typename HashType>
        inline void DoModify(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::shared_ptr<void>& hashWrap)const
        {
            using key_type = typename HashType::key_type;
            using val_type = typename HashType::mapped_type;
            //int objTypeId = m_ObjInfo.ObjectTypeId;
            auto* hashObj = static_cast<HashType*>(hashWrap.get());
            if (nullptr != hashObj) {
                auto& hash = *hashObj;
                for (auto it = hash.begin(); it != hash.end(); ++it) {
                    SetIter<key_type, val_type>(lvars, it);
                    SetValIter<key_type, val_type>(lvars, it);
                    if (!m_Exp.isNull())
                        m_Exp(gvars, lvars);
                    ChangeVal<key_type, val_type>(gvars, lvars, it);
                    FreeObjVars(lvars, m_ObjVars);
                }
            }
        }
        template<typename KeyT, typename ValT>
        inline void SetIter(Brace::VariableInfo& lvars, typename std::unordered_map<KeyT, ValT>::iterator it)const
        {
            DoSetIter(lvars, m_IteratorIndex, it->first);
        }
        template<typename KeyT, typename ValT>
        inline void SetValIter(Brace::VariableInfo& lvars, typename std::unordered_map<KeyT, ValT>::iterator it)const
        {
            DoSetIter(lvars, m_ValIteratorIndex, it->second);
        }
        template<typename KeyT, typename ValT>
        inline void ChangeVal(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, typename std::unordered_map<KeyT, ValT>::iterator it)const
        {
            it->second = DoGetVal<ValT>(gvars, lvars);
        }
        inline void DoSetIter(Brace::VariableInfo& lvars, int index, const std::string& val)const
        {
            Brace::VarSetString(lvars, index, val);
        }
        inline void DoSetIter(Brace::VariableInfo& lvars, int index, int64_t val)const
        {
            Brace::VarSetInt64(lvars, index, val);
        }
        inline void DoSetIter(Brace::VariableInfo& lvars, int index, double val)const
        {
            Brace::VarSetDouble(lvars, index, val);
        }
        inline void DoSetIter(Brace::VariableInfo& lvars, int index, bool val)const
        {
            Brace::VarSetBool(lvars, index, val);
        }
        inline void DoSetIter(Brace::VariableInfo& lvars, int index, const std::shared_ptr<void>& val)const
        {
            Brace::VarSetObject(lvars, index, val);
        }
        template<typename RetT>
        inline RetT DoGetVal(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            return RetT{};
        }
        template<>
        inline std::string DoGetVal<std::string>(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            return Brace::VarGetStr((m_ExpInfo.IsGlobal ? gvars : lvars), m_ExpInfo.Type, m_ExpInfo.VarIndex);
        }
        template<>
        inline int64_t DoGetVal<int64_t>(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            return Brace::VarGetI64((m_ExpInfo.IsGlobal ? gvars : lvars), m_ExpInfo.Type, m_ExpInfo.VarIndex);
        }
        template<>
        inline double DoGetVal<double>(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            return Brace::VarGetF64((m_ExpInfo.IsGlobal ? gvars : lvars), m_ExpInfo.Type, m_ExpInfo.VarIndex);
        }
        template<>
        inline bool DoGetVal<bool>(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            return Brace::VarGetBoolean((m_ExpInfo.IsGlobal ? gvars : lvars), m_ExpInfo.Type, m_ExpInfo.VarIndex);
        }
        template<>
        inline std::shared_ptr<void> DoGetVal<std::shared_ptr<void>>(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            return Brace::VarGetObject((m_ExpInfo.IsGlobal ? gvars : lvars), m_ExpInfo.VarIndex);
        }
    private:
        int m_ObjectCategory;
        int m_IteratorIndex;
        int m_ValIteratorIndex;
        Brace::OperandRuntimeInfo m_ObjInfo;
        Brace::BraceApiExecutor m_Obj;
        Brace::OperandRuntimeInfo m_ExpInfo;
        Brace::BraceApiExecutor m_Exp;
        std::vector<int> m_ObjVars;
    };

    class GetExePathExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetExePathExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            std::string str = get_exe_path();
            Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::move(str));
        }
    };
    class SetCurDirExp final : public Brace::SimpleBraceApiBase
    {
    public:
        SetCurDirExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() == 1) {
                auto& ai1 = argInfos[0];
                if (!Brace::IsStringType(ai1.Type)) {
                    std::stringstream ss;
                    ss << "expected cd(dir) ! line: " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            else {
                std::stringstream ss;
                ss << "cd must have a string argument ! line: " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto& argInfo0 = argInfos[0];
            const std::string& str = (argInfo0.IsGlobal ? gvars : lvars).StringVars[argInfo0.VarIndex];

            bool r = Common::FS::SetCurrentDir(std::filesystem::path(str));
            Brace::VarSetBool((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, r);
        }
    };
    class GetCurDirExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetCurDirExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& path = Common::FS::GetCurrentDir();
            std::string r = path.string();
            Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::move(r));
        }
    };
    class ShowUiExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ShowUiExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 2 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64 || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected setscriptbtncaption(index, bit_flags)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            int index = static_cast<int>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            int flags = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            g_pApiProvider->ShowUI(index, flags);
        }
    };
    class GetScriptInputExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetScriptInputExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            std::string str = g_pApiProvider->GetScriptInput();
            Brace::VarSetString((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, std::move(str));
        }
    };
    class SetScriptInputLabelExp final : public Brace::SimpleBraceApiBase
    {
    public:
        SetScriptInputLabelExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type != Brace::BRACE_DATA_TYPE_STRING) {
                //error
                std::stringstream ss;
                ss << "expected setscriptinputlabel(string)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo = argInfos[0];
            const std::string& label = Brace::VarGetString((resultInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            g_pApiProvider->SetScriptInputLabel(label);
        }
    };
    class SetScriptBtnCaptionExp final : public Brace::SimpleBraceApiBase
    {
    public:
        SetScriptBtnCaptionExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 2 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64 || argInfos[1].Type != Brace::BRACE_DATA_TYPE_STRING) {
                //error
                std::stringstream ss;
                ss << "expected setscriptbtncaption(index, string)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            int index = static_cast<int>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            const std::string& label = Brace::VarGetString((argInfo2.IsGlobal ? gvars : lvars), argInfo2.VarIndex);
            g_pApiProvider->SetScriptBtnCaption(index, label);
        }
    };
    class GetPixelExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetPixelExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 2 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64 || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected getpixel(x, y)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT32;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            int x = static_cast<int>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            int y = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            uint32_t pixel = g_pApiProvider->GetPixel(x, y);
            Brace::VarSetUInt32(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, pixel);
        }
    };
    class GetCursorXExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetCursorXExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            int x, y;
            g_pApiProvider->GetCursorPos(x, y);
            Brace::VarSetInt32(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, x);
        }
    };
    class GetCursorYExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetCursorYExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            int x, y;
            g_pApiProvider->GetCursorPos(x, y);
            Brace::VarSetInt32(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, y);
        }
    };
    class GetScreenWidthExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetScreenWidthExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            int x, y;
            g_pApiProvider->GetScreenSize(x, y);
            Brace::VarSetInt32(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, x);
        }
    };
    class GetScreenHeightExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetScreenHeightExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            int x, y;
            g_pApiProvider->GetScreenSize(x, y);
            Brace::VarSetInt32(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, y);
        }
    };
    class ReadButtonParamExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ReadButtonParamExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected readbuttonparam(index)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo = argInfos[0];
            int index = static_cast<int>(Brace::VarGetI64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex));
            std::string v = g_pApiProvider->ReadButtonParam(index);
            Brace::VarSetString(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, std::move(v));
        }
    };
    class ReadStickParamExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ReadStickParamExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected readstickparam(index)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo = argInfos[0];
            int index = static_cast<int>(Brace::VarGetI64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex));
            std::string v = g_pApiProvider->ReadStickParam(index);
            Brace::VarSetString(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, std::move(v));
        }
    };
    class ReadMotionParamExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ReadMotionParamExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected readmotionparam(index)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo = argInfos[0];
            int index = static_cast<int>(Brace::VarGetI64((argInfo.IsGlobal ? gvars : lvars), argInfo.Type, argInfo.VarIndex));
            std::string v = g_pApiProvider->ReadMotionParam(index);
            Brace::VarSetString(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, std::move(v));
        }
    };
    class ReadParamPackageExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ReadParamPackageExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type != Brace::BRACE_DATA_TYPE_STRING) {
                //error
                std::stringstream ss;
                ss << "expected readparampackage(str)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo = argInfos[0];
            const std::string& str = Brace::VarGetString((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            g_pApiProvider->ReadParamPackage(str);
        }
    };
    class HasParamExp final : public Brace::SimpleBraceApiBase
    {
    public:
        HasParamExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type != Brace::BRACE_DATA_TYPE_STRING) {
                //error
                std::stringstream ss;
                ss << "expected hasparam(key)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo = argInfos[0];
            const std::string& key = Brace::VarGetString((argInfo.IsGlobal ? gvars : lvars), argInfo.VarIndex);
            bool v = g_pApiProvider->HasParam(key);
            Brace::VarSetBool(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, v);
        }
    };
    class GetIntParamExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetIntParamExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 2 || argInfos[0].Type != Brace::BRACE_DATA_TYPE_STRING || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected getintparam(key, def)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            const std::string& key = Brace::VarGetString((argInfo1.IsGlobal ? gvars : lvars), argInfo1.VarIndex);
            int def = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            int v = g_pApiProvider->GetIntParam(key, def);
            Brace::VarSetInt32(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, v);
        }
    };
    class GetFloatParamExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetFloatParamExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 2 || argInfos[0].Type != Brace::BRACE_DATA_TYPE_STRING || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_DOUBLE) {
                //error
                std::stringstream ss;
                ss << "expected getfloatparam(key, def)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_FLOAT;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            const std::string& key = Brace::VarGetString((argInfo1.IsGlobal ? gvars : lvars), argInfo1.VarIndex);
            float def = static_cast<float>(Brace::VarGetF64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            float v = g_pApiProvider->GetFloatParam(key, def);
            Brace::VarSetFloat(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, v);
        }
    };
    class GetStrParamExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetStrParamExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 2 || argInfos[0].Type != Brace::BRACE_DATA_TYPE_STRING || argInfos[1].Type != Brace::BRACE_DATA_TYPE_STRING) {
                //error
                std::stringstream ss;
                ss << "expected getstrparam(key, def)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            const std::string& key = Brace::VarGetString((argInfo1.IsGlobal ? gvars : lvars), argInfo1.VarIndex);
            const std::string& def = Brace::VarGetString((argInfo2.IsGlobal ? gvars : lvars), argInfo2.VarIndex);
            std::string v = g_pApiProvider->GetStrParam(key, def);
            Brace::VarSetString(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, std::move(v));
        }
    };
    class KeyPressExp final : public Brace::SimpleBraceApiBase
    {
    public:
        KeyPressExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 2 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64 || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected keypress(modifier, key)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            int m = static_cast<int>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            int k = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            g_pApiProvider->KeyPress(m, k);
        }
    };
    class KeyReleaseExp final : public Brace::SimpleBraceApiBase
    {
    public:
        KeyReleaseExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 2 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64 || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected keyrelease(modifier, key)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            int m = static_cast<int>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            int k = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            g_pApiProvider->KeyRelease(m, k);
        }
    };
    class MousePressExp final : public Brace::SimpleBraceApiBase
    {
    public:
        MousePressExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 3 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64 ||
                argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64 ||
                argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected mousepress(x, y, button)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            int x = static_cast<int>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            int y = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            int btn = static_cast<int>(Brace::VarGetI64((argInfo3.IsGlobal ? gvars : lvars), argInfo3.Type, argInfo3.VarIndex));
            g_pApiProvider->MousePress(x, y, btn);
        }
    };
    class MouseReleaseExp final : public Brace::SimpleBraceApiBase
    {
    public:
        MouseReleaseExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected mouserelease(button)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            int btn = static_cast<int>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            g_pApiProvider->MouseRelease(btn);
        }
    };
    class MouseMoveExp final : public Brace::SimpleBraceApiBase
    {
    public:
        MouseMoveExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 2 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64 || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected mousemove(x, y)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            int x = static_cast<int>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            int y = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            g_pApiProvider->MouseMove(x, y);
        }
    };
    class MouseWheelChangeExp final : public Brace::SimpleBraceApiBase
    {
    public:
        MouseWheelChangeExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 2 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64 || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected mousewheelchange(x, y)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            int x = static_cast<int>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            int y = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            g_pApiProvider->MouseWheelChange(x, y);
        }
    };
    class TouchPressExp final : public Brace::SimpleBraceApiBase
    {
    public:
        TouchPressExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 3 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected touchpress(x, y, id)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            int x = static_cast<int>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            int y = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            int id = static_cast<int>(Brace::VarGetI64((argInfo3.IsGlobal ? gvars : lvars), argInfo3.Type, argInfo3.VarIndex));
            g_pApiProvider->TouchPress(x, y, id);
        }
    };
    class TouchUpdateBeginExp final : public Brace::SimpleBraceApiBase
    {
    public:
        TouchUpdateBeginExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            g_pApiProvider->TouchUpdateBegin();
        }
    };
    class TouchMoveExp final : public Brace::SimpleBraceApiBase
    {
    public:
        TouchMoveExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 3 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected touchmove(x, y, id)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            int x = static_cast<int>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            int y = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            int id = static_cast<int>(Brace::VarGetI64((argInfo3.IsGlobal ? gvars : lvars), argInfo3.Type, argInfo3.VarIndex));
            g_pApiProvider->TouchMove(x, y, id);
        }
    };
    class TouchUpdateEndExp final : public Brace::SimpleBraceApiBase
    {
    public:
        TouchUpdateEndExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            g_pApiProvider->TouchUpdateEnd();
        }
    };
    class TouchEndExp final : public Brace::SimpleBraceApiBase
    {
    public:
        TouchEndExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            g_pApiProvider->TouchEnd();
        }
    };

    class GetButtonStateExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetButtonStateExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected getbuttonstate(id)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            int id = static_cast<int>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            bool v = g_pApiProvider->GetButtonState(id);
            Brace::VarSetBool((resultInfo.IsGlobal ? gvars : lvars), resultInfo.VarIndex, v);
        }
    };
    class SetButtonStateExp final : public Brace::SimpleBraceApiBase
    {
    public:
        SetButtonStateExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 3 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_BOOL || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected setbuttonstate(uint_player_index, int_button_id, bool_value)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            std::size_t player_index = static_cast<std::size_t>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            int button_id = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            bool v = Brace::VarGetBoolean((argInfo3.IsGlobal ? gvars : lvars), argInfo3.Type, argInfo3.VarIndex);
            g_pApiProvider->SetButtonState(player_index, button_id, v);
        }
    };
    class SetStickPositionExp final : public Brace::SimpleBraceApiBase
    {
    public:
        SetStickPositionExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 4 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_DOUBLE
                || argInfos[3].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[3].Type > Brace::BRACE_DATA_TYPE_DOUBLE) {
                //error
                std::stringstream ss;
                ss << "expected setstickpos(uint_player_index, int_axis_id, float_x, float_y)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            auto&& argInfo4 = argInfos[3];
            std::size_t player_index = static_cast<std::size_t>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            int axis_id = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            float x = static_cast<float>(Brace::VarGetF64((argInfo3.IsGlobal ? gvars : lvars), argInfo3.Type, argInfo3.VarIndex));
            float y = static_cast<float>(Brace::VarGetF64((argInfo4.IsGlobal ? gvars : lvars), argInfo4.Type, argInfo4.VarIndex));
            g_pApiProvider->SetStickPosition(player_index, axis_id, x, y);
        }
    };
    class SetMotionStateExp final : public Brace::SimpleBraceApiBase
    {
    public:
        SetMotionStateExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 8 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_DOUBLE
                || argInfos[3].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[3].Type > Brace::BRACE_DATA_TYPE_DOUBLE
                || argInfos[4].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[4].Type > Brace::BRACE_DATA_TYPE_DOUBLE
                || argInfos[5].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[5].Type > Brace::BRACE_DATA_TYPE_DOUBLE
                || argInfos[6].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[6].Type > Brace::BRACE_DATA_TYPE_DOUBLE
                || argInfos[7].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[7].Type > Brace::BRACE_DATA_TYPE_DOUBLE) {
                //error
                std::stringstream ss;
                ss << "expected setmotionstate(uint_player_index, uint64_delta_time, float_gyro_x, float_gyro_y, float_gyro_z, float_accel_x, float_accel_y, float_accel_z)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            auto&& argInfo4 = argInfos[3];
            auto&& argInfo5 = argInfos[4];
            auto&& argInfo6 = argInfos[5];
            auto&& argInfo7 = argInfos[6];
            auto&& argInfo8 = argInfos[7];
            std::size_t player_index = static_cast<std::size_t>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            u64 delta_time = static_cast<u64>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            float gx = static_cast<float>(Brace::VarGetF64((argInfo3.IsGlobal ? gvars : lvars), argInfo3.Type, argInfo3.VarIndex));
            float gy = static_cast<float>(Brace::VarGetF64((argInfo4.IsGlobal ? gvars : lvars), argInfo4.Type, argInfo4.VarIndex));
            float gz = static_cast<float>(Brace::VarGetF64((argInfo5.IsGlobal ? gvars : lvars), argInfo5.Type, argInfo5.VarIndex));
            float ax = static_cast<float>(Brace::VarGetF64((argInfo6.IsGlobal ? gvars : lvars), argInfo6.Type, argInfo6.VarIndex));
            float ay = static_cast<float>(Brace::VarGetF64((argInfo7.IsGlobal ? gvars : lvars), argInfo7.Type, argInfo7.VarIndex));
            float az = static_cast<float>(Brace::VarGetF64((argInfo8.IsGlobal ? gvars : lvars), argInfo8.Type, argInfo8.VarIndex));
            g_pApiProvider->SetMotionState(player_index, delta_time, gx, gy, gz, ax, ay, az);
        }
    };

    static uint64_t ReadMemory(uint64_t addr, uint64_t val_size, bool& result)
    {
        static const size_t s_u8 = sizeof(uint8_t);
        static const size_t s_u16 = sizeof(uint16_t);
        static const size_t s_u32 = sizeof(uint32_t);
        static const size_t s_u64 = sizeof(uint64_t);

        uint64_t val = 0;
        if (nullptr != g_pApiProvider) {
            auto&& system = g_pApiProvider->GetSystem();
            auto&& sniffer = system.MemorySniffer();
            result = true;
            bool succ = true;
            switch (val_size) {
            case s_u8:
                val = sniffer.ReadMemory(addr, s_u8, succ);
                result = succ && result;
                break;
            case s_u16:
                val = sniffer.ReadMemory(addr, s_u16, succ);
                result = succ && result;
                break;
            case s_u32:
                val = sniffer.ReadMemory(addr, s_u32, succ);
                result = succ && result;
                break;
            case s_u64:
                val = sniffer.ReadMemory(addr, s_u64, succ);
                result = succ && result;
                break;
            default: {
                uint64_t left_val_size = val_size;
                uint64_t lshift = 0;
                uint64_t caddr = addr;
                val = 0;
                if (left_val_size > s_u32) {
                    val += (sniffer.ReadMemory(addr, s_u32, succ) << lshift);
                    caddr += s_u32;
                    lshift += s_u32 * 8;
                    left_val_size -= s_u32;
                    result = succ && result;
                }
                if (left_val_size >= s_u16) {
                    val = (sniffer.ReadMemory(addr, s_u16, succ) << lshift);
                    caddr += s_u16;
                    lshift += s_u16 * 8;
                    left_val_size -= s_u16;
                    result = succ && result;
                }
                if (left_val_size >= s_u8) {
                    val = (sniffer.ReadMemory(addr, s_u8, succ) << lshift);
                    caddr += s_u8;
                    lshift += s_u8 * 8;
                    left_val_size -= s_u8;
                    result = succ && result;
                }
            }
                   break;
            }
        }
        else {
            result = false;
        }
        return val;
    }
    static bool WriteMemory(uint64_t addr, uint64_t val_size, uint64_t val)
    {
        static const size_t s_u8 = sizeof(uint8_t);
        static const size_t s_u16 = sizeof(uint16_t);
        static const size_t s_u32 = sizeof(uint32_t);
        static const size_t s_u64 = sizeof(uint64_t);

        bool result = false;
        if (nullptr != g_pApiProvider) {
            auto&& system = g_pApiProvider->GetSystem();
            auto&& sniffer = system.MemorySniffer();
            result = true;
            bool succ = true;
            switch (val_size) {
            case s_u8:
                succ = sniffer.WriteMemory(addr, s_u8, val);
                result = succ && result;
                break;
            case s_u16:
                succ = sniffer.WriteMemory(addr, s_u16, val);
                result = succ && result;
                break;
            case s_u32:
                succ = sniffer.WriteMemory(addr, s_u32, val);
                result = succ && result;
                break;
            case s_u64:
                succ = sniffer.WriteMemory(addr, s_u64, val);
                result = succ && result;
                break;
            default: {
                uint64_t left_val_size = val_size;
                uint64_t rshift = 0;
                uint64_t caddr = addr;
                if (left_val_size > s_u32) {
                    succ = sniffer.WriteMemory(caddr, s_u32, val >> rshift);
                    caddr += s_u32;
                    rshift += s_u32 * 8;
                    left_val_size -= s_u32;
                    result = succ && result;
                }
                if (left_val_size >= s_u16) {
                    succ = sniffer.WriteMemory(caddr, s_u16, val >> rshift);
                    caddr += s_u16;
                    rshift += s_u16 * 8;
                    left_val_size -= s_u16;
                    result = succ && result;
                }
                if (left_val_size >= s_u8) {
                    succ = sniffer.WriteMemory(caddr, s_u8, val >> rshift);
                    caddr += s_u8;
                    rshift += s_u8 * 8;
                    left_val_size -= s_u8;
                    result = succ && result;
                }
            }
                   break;
            }
        }
        return result;
    }

    class GetResultInfoExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetResultInfoExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = g_ObjectInfoMgr.GetObjectTypeId("hashtable<:int64,MemoryModifyInfo:>");
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            auto&& results = memorySniffer.GetResultMemoryModifyInfo();

            auto ptr = std::make_shared<IntObjHashtable>();
            auto&& ht = *ptr;
            for (auto&& pair : results) {
                ht.insert(pair);
            }
            Brace::VarSetObject(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, std::move(ptr));
        }
    };
    class GetLastInfoExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetLastInfoExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = g_ObjectInfoMgr.GetObjectTypeId("hashtable<:int64,MemoryModifyInfo:>");
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            auto&& results = memorySniffer.GetLastHistoryMemoryModifyInfo();

            auto ptr = std::make_shared<IntObjHashtable>();
            auto&& ht = *ptr;
            for (auto&& pair : results) {
                ht.insert(pair);
            }
            Brace::VarSetObject(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, std::move(ptr));
        }
    };
    class GetHistoryInfoCountExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetHistoryInfoCountExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            int ct = memorySniffer.GetHistoryMemoryModifyInfoCount();

            Brace::VarSetInt32(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, ct);
        }
    };
    class GetHistoryInfoExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetHistoryInfoExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type<Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type>Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected gethistoryinfo(index)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = g_ObjectInfoMgr.GetObjectTypeId("hashtable<:int64,MemoryModifyInfo:>");
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            int ix = 0;
            if (argInfos.size() == 1) {
                auto&& argInfo = argInfos[0];
                ix = static_cast<int>(Brace::VarGetI64(argInfo.IsGlobal ? gvars : lvars, argInfo.Type, argInfo.VarIndex));
            }

            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            int ct = memorySniffer.GetHistoryMemoryModifyInfoCount();
            if (ix >= 0 && ix < ct) {
                auto&& results = memorySniffer.GetHistoryMemoryModifyInfo(ix);

                auto ptr = std::make_shared<IntObjHashtable>();
                auto&& ht = *ptr;
                for (auto&& pair : results) {
                    ht.insert(pair);
                }
                Brace::VarSetObject(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, std::move(ptr));
            }
            else {
                Brace::VarSetObject(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, nullptr);
            }
        }
    };
    class GetRollbackInfoCountExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetRollbackInfoCountExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            int ct = memorySniffer.GetRollbackMemoryModifyInfoCount();

            Brace::VarSetInt32(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, ct);
        }
    };
    class GetRollbackInfoExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetRollbackInfoExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type<Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type>Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected getrollbackinfo(index)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = g_ObjectInfoMgr.GetObjectTypeId("hashtable<:int64,MemoryModifyInfo:>");
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            int ix = 0;
            if (argInfos.size() == 1) {
                auto&& argInfo = argInfos[0];
                ix = static_cast<int>(Brace::VarGetI64(argInfo.IsGlobal ? gvars : lvars, argInfo.Type, argInfo.VarIndex));
            }

            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            int ct = memorySniffer.GetRollbackMemoryModifyInfoCount();
            if (ix >= 0 && ix < ct) {
                auto&& results = memorySniffer.GetRollbackMemoryModifyInfo(ix);

                auto ptr = std::make_shared<IntObjHashtable>();
                auto&& ht = *ptr;
                for (auto&& pair : results) {
                    ht.insert(pair);
                }
                Brace::VarSetObject(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, std::move(ptr));
            }
            else {
                Brace::VarSetObject(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, nullptr);
            }
        }
    };
    class SetResultInfoExp final : public Brace::SimpleBraceApiBase
    {
    public:
        SetResultInfoExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            int objTypeId = g_ObjectInfoMgr.GetObjectTypeId("hashtable<:int64,MemoryModifyInfo:>");
            if (argInfos.size() != 1 || argInfos[0].Type != Brace::BRACE_DATA_TYPE_OBJECT || argInfos[0].ObjectTypeId != objTypeId) {
                //error
                std::stringstream ss;
                ss << "expected setresultinfo(hashtable<:int64,MemoryModifyInfo:>)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo = argInfos[0];
            auto&& ptr = Brace::VarGetObject(argInfo.IsGlobal ? gvars : lvars, argInfo.VarIndex);
            IntObjHashtable* pHT = static_cast<IntObjHashtable*>(ptr.get());
            int ct = -1;
            if (nullptr != pHT) {
                ct = static_cast<int>(pHT->size());

                Core::Memory::MemoryModifyInfoMap newResult;
                for (auto&& pair : *pHT) {
                    uint64_t addr = static_cast<uint64_t>(pair.first);
                    auto&& p = std::static_pointer_cast<Core::Memory::MemoryModifyInfo>(pair.second);
                    newResult.insert(std::make_pair(addr, std::move(p)));
                }

                auto&& system = g_pApiProvider->GetSystem();
                auto&& memorySniffer = system.MemorySniffer();
                memorySniffer.SetResultMemoryModifyInfo(std::move(newResult));
            }
            Brace::VarSetInt32(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, ct);
        }
    };
    class NewMemoryModifyInfoExp final : public Brace::SimpleBraceApiBase
    {
    public:
        NewMemoryModifyInfoExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.ObjectTypeId = BraceScriptInterpreter::CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO;
            resultInfo.Name = GenTempVarName();
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto ptr = std::make_shared<Core::Memory::MemoryModifyInfo>();
            Brace::VarSetObject(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, std::move(ptr));
        }
    };
    class AddToResultExp final : public Brace::SimpleBraceApiBase
    {
    public:
        AddToResultExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 1 && argInfos.size() != 2) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos.size() == 2 && (argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64)) {
                //error
                std::stringstream ss;
                ss << "expected addtoresult(addr[, val_size]), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            static const size_t s_u8 = sizeof(uint8_t);
            static const size_t s_u16 = sizeof(uint16_t);
            static const size_t s_u32 = sizeof(uint32_t);
            static const size_t s_u64 = sizeof(uint64_t);

            auto&& argInfo1 = argInfos[0];
            uint64_t addr = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t val_size = sizeof(uint32_t);
            if (argInfos.size() == 2) {
                auto&& argInfo2 = argInfos[1];
                val_size = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            }
            if (val_size < sizeof(uint8_t) || val_size > sizeof(uint64_t))
                val_size = sizeof(uint32_t);

            bool ret = false;
            bool result = false;
            if (nullptr != g_pApiProvider) {
                uint64_t val = ReadMemory(addr, val_size, result);
                if (result) {
                    auto&& system = g_pApiProvider->GetSystem();
                    auto&& memorySniffer = system.MemorySniffer();
                    auto* pResults = memorySniffer.GetResultMemoryModifyInfoPtr();
                    if (pResults && !pResults->contains(val)) {
                        auto&& newPtr = std::make_shared<Core::Memory::MemoryModifyInfo>();
                        newPtr->addr = addr;
                        switch (val_size) {
                        case s_u8:
                            newPtr->type = Core::Memory::MemoryModifyInfo::type_u8;
                            newPtr->u8Val = static_cast<u8>(val);
                            newPtr->u8OldVal = 0;
                            break;
                        case s_u16:
                            newPtr->type = Core::Memory::MemoryModifyInfo::type_u16;
                            newPtr->u16Val = static_cast<u16>(val);
                            newPtr->u16OldVal = 0;
                            break;
                        case s_u32:
                            newPtr->type = Core::Memory::MemoryModifyInfo::type_u32;
                            newPtr->u32Val = static_cast<u32>(val);
                            newPtr->u32OldVal = 0;
                            break;
                        case s_u64:
                            newPtr->type = Core::Memory::MemoryModifyInfo::type_u64;
                            newPtr->u64Val = static_cast<u64>(val);
                            newPtr->u64OldVal = 0;
                            break;
                        }
                        newPtr->size = val_size;
                        pResults->insert(std::make_pair(addr, newPtr));
                        ret = true;
                    }
                }
                else {
                    std::stringstream ss;
                    ss << "read addr:" << std::hex << addr << " size:" << std::dec << val_size << " failed.";
                    g_pApiProvider->LogToView(ss.str());
                }
            }
            Brace::VarSetBool(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, ret);
        }
    };
    class AddToLastExp final : public Brace::SimpleBraceApiBase
    {
    public:
        AddToLastExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 1 && argInfos.size() != 2) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos.size() == 2 && (argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64)) {
                //error
                std::stringstream ss;
                ss << "expected addtolast(addr[, val_size]), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            static const size_t s_u8 = sizeof(uint8_t);
            static const size_t s_u16 = sizeof(uint16_t);
            static const size_t s_u32 = sizeof(uint32_t);
            static const size_t s_u64 = sizeof(uint64_t);

            auto&& argInfo1 = argInfos[0];
            uint64_t addr = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t val_size = sizeof(uint32_t);
            if (argInfos.size() == 2) {
                auto&& argInfo2 = argInfos[1];
                val_size = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            }
            if (val_size < sizeof(uint8_t) || val_size > sizeof(uint64_t))
                val_size = sizeof(uint32_t);

            bool ret = false;
            bool result = false;
            if (nullptr != g_pApiProvider) {
                uint64_t val = ReadMemory(addr, val_size, result);
                if (result) {
                    auto&& system = g_pApiProvider->GetSystem();
                    auto&& memorySniffer = system.MemorySniffer();
                    auto* pLastResults = memorySniffer.GetLastHistoryMemoryModifyInfoPtr();
                    if (pLastResults && !pLastResults->contains(val)) {
                        auto&& newPtr = std::make_shared<Core::Memory::MemoryModifyInfo>();
                        newPtr->addr = addr;
                        switch (val_size) {
                        case s_u8:
                            newPtr->type = Core::Memory::MemoryModifyInfo::type_u8;
                            newPtr->u8Val = static_cast<u8>(val);
                            newPtr->u8OldVal = 0;
                            break;
                        case s_u16:
                            newPtr->type = Core::Memory::MemoryModifyInfo::type_u16;
                            newPtr->u16Val = static_cast<u16>(val);
                            newPtr->u16OldVal = 0;
                            break;
                        case s_u32:
                            newPtr->type = Core::Memory::MemoryModifyInfo::type_u32;
                            newPtr->u32Val = static_cast<u32>(val);
                            newPtr->u32OldVal = 0;
                            break;
                        case s_u64:
                            newPtr->type = Core::Memory::MemoryModifyInfo::type_u64;
                            newPtr->u64Val = static_cast<u64>(val);
                            newPtr->u64OldVal = 0;
                            break;
                        }
                        newPtr->size = val_size;
                        pLastResults->insert(std::make_pair(addr, newPtr));
                        ret = true;
                    }
                }
                else {
                    std::stringstream ss;
                    ss << "read addr:" << std::hex << addr << " size:" << std::dec << val_size << " failed.";
                    g_pApiProvider->LogToView(ss.str());
                }
            }
            Brace::VarSetBool(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, ret);
        }
    };

    class GetTitleIdExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetTitleIdExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& system = g_pApiProvider->GetSystem();
            uint64_t title_id = system.GetApplicationProcessProgramID();

            std::stringstream ss;
            ss << std::setfill('0') << std::setw(sizeof(uint64_t) * 2) << std::hex << title_id;

            Brace::VarSetString(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, ss.str());
        }
    };
    class GetModuleCountExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetModuleCountExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_INT32;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            int ct = memorySniffer.GetModuleCount();

            Brace::VarSetInt32(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, ct);
        }
    };
    class GetModuleBaseExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetModuleBaseExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected getmodulebase(index)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo = argInfos[0];
            int ix = static_cast<int>(Brace::VarGetI64(argInfo.IsGlobal ? gvars : lvars, argInfo.Type, argInfo.VarIndex));

            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            uint64_t addr;
            uint64_t size;
            std::string build_id;
            std::string name;
            uint64_t base = memorySniffer.GetModuleBase(ix, addr, size, build_id, name);

            Brace::VarSetUInt64(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, base);
        }
    };
    class GetModuleAddrExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetModuleAddrExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected getmoduleaddr(index)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo = argInfos[0];
            int ix = static_cast<int>(Brace::VarGetI64(argInfo.IsGlobal ? gvars : lvars, argInfo.Type, argInfo.VarIndex));

            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            uint64_t addr;
            uint64_t size;
            std::string build_id;
            std::string name;
            [[maybe_unused]]uint64_t base = memorySniffer.GetModuleBase(ix, addr, size, build_id, name);

            Brace::VarSetUInt64(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, addr);
        }
    };
    class GetModuleSizeExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetModuleSizeExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected getmodulesize(index)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo = argInfos[0];
            int ix = static_cast<int>(Brace::VarGetI64(argInfo.IsGlobal ? gvars : lvars, argInfo.Type, argInfo.VarIndex));

            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            uint64_t addr;
            uint64_t size;
            std::string build_id;
            std::string name;
            [[maybe_unused]] uint64_t base = memorySniffer.GetModuleBase(ix, addr, size, build_id, name);

            Brace::VarSetUInt64(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, size);
        }
    };
    class GetModuleIdExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetModuleIdExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected getmoduleid(index)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo = argInfos[0];
            int ix = static_cast<int>(Brace::VarGetI64(argInfo.IsGlobal ? gvars : lvars, argInfo.Type, argInfo.VarIndex));

            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            uint64_t addr;
            uint64_t size;
            std::string build_id;
            std::string name;
            [[maybe_unused]] uint64_t base = memorySniffer.GetModuleBase(ix, addr, size, build_id, name);

            Brace::VarSetString(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, build_id);
        }
    };
    class GetModuleNameExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetModuleNameExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected getmodulename(index)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_STRING;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo = argInfos[0];
            int ix = static_cast<int>(Brace::VarGetI64(argInfo.IsGlobal ? gvars : lvars, argInfo.Type, argInfo.VarIndex));

            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            uint64_t addr;
            uint64_t size;
            std::string build_id;
            std::string name;
            [[maybe_unused]] uint64_t base = memorySniffer.GetModuleBase(ix, addr, size, build_id, name);

            Brace::VarSetString(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, name);
        }
    };
    class GetHeapBaseExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetHeapBaseExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            uint64_t size;
            uint64_t base = memorySniffer.GetHeapBase(size);

            Brace::VarSetUInt64(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, base);
        }
    };
    class GetHeapSizeExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetHeapSizeExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            uint64_t size;
            [[maybe_unused]] uint64_t base = memorySniffer.GetHeapBase(size);

            Brace::VarSetUInt64(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, size);
        }
    };
    class GetStackBaseExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetStackBaseExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            uint64_t size;
            uint64_t base = memorySniffer.GetStackBase(size);

            Brace::VarSetUInt64(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, base);
        }
    };
    class GetStackSizeExp final : public Brace::SimpleBraceApiBase
    {
    public:
        GetStackSizeExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& system = g_pApiProvider->GetSystem();
            auto&& memorySniffer = system.MemorySniffer();
            uint64_t size;
            [[maybe_unused]] uint64_t base = memorySniffer.GetStackBase(size);

            Brace::VarSetUInt64(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, size);
        }
    };
    class CmdMarkMemDebugExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CmdMarkMemDebugExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 2 && argInfos.size() != 3) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos.size() == 3 && (argInfos[2].Type < Brace::BRACE_DATA_TYPE_BOOL || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64)) {
                //error
                std::stringstream ss;
                ss << "expected markmemdebug(uint64 addr, uint64 size[, bool debug])," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo = Brace::OperandLoadtimeInfo();
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            if (nullptr == g_pApiProvider)
                return;
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            uint64_t addr = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t size = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            bool debug = true;
            if (argInfos.size() == 3) {
                auto&& argInfo3 = argInfos[2];
                debug = Brace::VarGetBoolean(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex);
            }

            auto&& system = g_pApiProvider->GetSystem();
            auto&& sniffer = system.MemorySniffer();
            sniffer.MarkMemoryDebug(addr, size, debug);
        }
    };
    class CmdAddSniffingExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CmdAddSniffingExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 2 && argInfos.size() != 3 && argInfos.size() != 4) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos.size() >= 3 && (argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64)
                || argInfos.size() == 4 && (argInfos[3].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[3].Type > Brace::BRACE_DATA_TYPE_UINT64)) {
                //error
                std::stringstream ss;
                ss << "expected addsniffing(uint64 addr, uint64 size[, uint64 step, uint64 val])," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo = Brace::OperandLoadtimeInfo();
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            if (nullptr == g_pApiProvider)
                return;
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            uint64_t addr = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t size = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            uint64_t step = sizeof(uint32_t);
            uint64_t val = 0;
            if (argInfos.size() >= 3) {
                auto&& argInfo3 = argInfos[2];
                step = static_cast<uint64_t>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));
                if (argInfos.size() == 4) {
                    auto&& argInfo4 = argInfos[3];
                    val = static_cast<uint64_t>(Brace::VarGetI64(argInfo4.IsGlobal ? gvars : lvars, argInfo4.Type, argInfo4.VarIndex));
                }
            }

            auto&& system = g_pApiProvider->GetSystem();
            auto&& sniffer = system.MemorySniffer();
            sniffer.AddSniffing(addr, size, step, val);
        }
    };
    class CmdAddSniffingFromSearchExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CmdAddSniffingFromSearchExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 1)
                || argInfos[0].Type != Brace::BRACE_DATA_TYPE_OBJECT || argInfos[0].ObjectTypeId != CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY) {
                //error
                std::stringstream ss;
                ss << "expected addsniffingfromsearch(find_vals)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo = Brace::OperandLoadtimeInfo();
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            if (nullptr == g_pApiProvider)
                return;
            auto&& argInfo1 = argInfos[0];
            auto&& objPtr = Brace::VarGetObject(argInfo1.IsGlobal ? gvars : lvars, argInfo1.VarIndex);
            using Array64 = ArrayT<uint64_t>;
            auto* pArr = static_cast<Array64*>(objPtr.get());
            uint64_t start;
            uint64_t end;
            uint64_t step;
            uint64_t range;
            uint64_t val_size = sizeof(uint32_t);
            uint64_t max_count;
            auto&& system = g_pApiProvider->GetSystem();
            auto&& sniffer = system.MemorySniffer();
            sniffer.GetMemorySearchInfo(start, end, step, val_size, range, max_count);
            if (val_size < sizeof(uint8_t) || val_size > sizeof(uint64_t))
                val_size = sizeof(uint32_t);

            std::set<uint64_t> findVals{};
            for (auto&& fval : *pArr) {
                if (findVals.find(fval) == findVals.end())
                    findVals.insert(fval);
            }

            using IntIntHash64 = HashtableT<int64_t, int64_t>;
            IntIntHash64 hash64{};
            using PrioQueue = std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>>;
            PrioQueue pqueue{};
            std::vector<uint64_t> tempAddrs{};

            uint64_t ct = 0;
            for (uint64_t addr = start; addr <= end - val_size; addr += step) {
                bool result = false;
                uint64_t val = ReadMemory(addr, val_size, result);
                if (findVals.find(val) != findVals.end()) {
                    auto&& it = hash64.find(val);
                    if (it == hash64.end()) {
                        pqueue.push(addr);

                        int64_t k = static_cast<int64_t>(val);
                        int64_t v = static_cast<int64_t>(addr);
                        hash64.insert(std::make_pair(k, v));
                    }
                    else {
                        uint64_t oldAddr = static_cast<uint64_t>(it->second);

                        tempAddrs.clear();
                        bool find = false;
                        for (size_t ix = pqueue.size(); ix > 0; --ix) {
                            uint64_t minAddr = pqueue.top();
                            pqueue.pop();
                            if (minAddr != oldAddr) {
                                tempAddrs.push_back(minAddr);
                            }
                            else {
                                find = true;
                                break;
                            }
                        }
                        if (!find) {
                            assert(false);
                        }
                        for (auto&& maddr : tempAddrs) {
                            pqueue.push(maddr);
                        }
                        pqueue.push(addr);

                        it->second = static_cast<int64_t>(addr);
                    }
                    if (pqueue.size() == findVals.size()) {
                        uint64_t stAddr = pqueue.top();
                        if (addr - stAddr <= range) {
                            sniffer.AddSniffing(stAddr, addr + val_size - stAddr, step, 0);

                            hash64.clear();
                            PrioQueue temp{};
                            pqueue.swap(temp);

                            ++ct;
                            if (ct >= max_count)
                                break;
                        }
                    }
                }
            }
        }
    };
    class CmdShowMemExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CmdShowMemExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 2 && argInfos.size() != 3) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos.size() == 3 && (argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64)) {
                //error
                std::stringstream ss;
                ss << "expected showmem(uint64 addr, uint64 size[, uint64 step])," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo = Brace::OperandLoadtimeInfo();
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            if (nullptr == g_pApiProvider)
                return;
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            uint64_t addr = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t size = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            uint64_t step = sizeof(uint32_t);
            if (argInfos.size() == 3) {
                auto&& argInfo3 = argInfos[2];
                step = static_cast<uint64_t>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));
            }

            g_pApiProvider->LogToView(std::string("===show memory==="));
            for (uint64_t maddr = addr; maddr <= addr + size - step; maddr += step) {
                bool mresult = false;
                uint64_t mval = ReadMemory(maddr, step, mresult);
                std::stringstream ss;
                ss << "addr: " << std::hex << maddr << " hex_val: " << mval << " dec_val: " << std::dec << mval;
                g_pApiProvider->LogToView(ss.str());
            }
        }
    };
    class CmdFindMemExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CmdFindMemExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 1)
                || argInfos[0].Type != Brace::BRACE_DATA_TYPE_OBJECT || argInfos[0].ObjectTypeId != CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY) {
                //error
                std::stringstream ss;
                ss << "expected findmem(find_vals)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo = Brace::OperandLoadtimeInfo();
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            if (nullptr == g_pApiProvider)
                return;
            auto&& argInfo1 = argInfos[0];
            auto&& objPtr = Brace::VarGetObject(argInfo1.IsGlobal ? gvars : lvars, argInfo1.VarIndex);
            using Array64 = ArrayT<uint64_t>;
            auto* pArr = static_cast<Array64*>(objPtr.get());
            uint64_t start;
            uint64_t end;
            uint64_t step;
            uint64_t range;
            uint64_t val_size = sizeof(uint32_t);
            uint64_t max_count;
            auto&& system = g_pApiProvider->GetSystem();
            auto&& sniffer = system.MemorySniffer();
            sniffer.GetMemorySearchInfo(start, end, step, val_size, range, max_count);
            if (val_size < sizeof(uint8_t) || val_size > sizeof(uint64_t))
                val_size = sizeof(uint32_t);

            std::set<uint64_t> findVals{};
            for (auto&& fval : *pArr) {
                if (findVals.find(fval) == findVals.end())
                    findVals.insert(fval);
            }

            using IntIntHash64 = HashtableT<int64_t, int64_t>;
            IntIntHash64 hash64{};
            using PrioQueue = std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>>;
            PrioQueue pqueue{};
            std::vector<uint64_t> tempAddrs{};

            for (uint64_t addr = start; addr <= end - val_size; addr += step) {
                bool result = false;
                uint64_t val = ReadMemory(addr, val_size, result);
                if (findVals.find(val) != findVals.cend()) {
                    auto&& it = hash64.find(val);
                    if (it == hash64.end()) {
                        pqueue.push(addr);

                        int64_t k = static_cast<int64_t>(val);
                        int64_t v = static_cast<int64_t>(addr);
                        hash64.insert(std::make_pair(k, v));
                    }
                    else {
                        uint64_t oldAddr = static_cast<uint64_t>(it->second);

                        tempAddrs.clear();
                        bool find = false;
                        for (size_t ix = pqueue.size(); ix > 0; --ix) {
                            uint64_t minAddr = pqueue.top();
                            pqueue.pop();
                            if (minAddr != oldAddr) {
                                tempAddrs.push_back(minAddr);
                            }
                            else {
                                find = true;
                                break;
                            }
                        }
                        if (!find) {
                            assert(false);
                        }
                        for (auto&& maddr : tempAddrs) {
                            pqueue.push(maddr);
                        }
                        pqueue.push(addr);

                        it->second = static_cast<int64_t>(addr);
                    }
                    if (pqueue.size() == findVals.size()) {
                        uint64_t stAddr = pqueue.top();
                        if (addr - stAddr <= range) {
                            g_pApiProvider->LogToView(std::string("===find result==="));
                            for (auto&& pair : hash64) {
                                std::stringstream ss;
                                ss << "addr: " << std::hex << pair.second << " hex_val: " << pair.first << " dec_val: " << std::dec << pair.first;
                                g_pApiProvider->LogToView(ss.str());
                            }
                            g_pApiProvider->LogToView(std::string("===area memory==="));
                            for (uint64_t maddr = stAddr; maddr <= addr; maddr += step) {
                                bool mresult = false;
                                uint64_t mval = ReadMemory(maddr, val_size, mresult);
                                std::stringstream ss;
                                ss << "addr: " << std::hex << maddr << " hex_val: " << mval << " dec_val: " << std::dec << mval;
                                g_pApiProvider->LogToView(ss.str());
                            }
                            break;
                        }
                    }
                }
            }
        }
    };
    class CmdSearchMemExp final : public Brace::SimpleBraceApiBase
    {
    public:
        CmdSearchMemExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 1)
                || argInfos[0].Type != Brace::BRACE_DATA_TYPE_OBJECT || argInfos[0].ObjectTypeId != CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY) {
                //error
                std::stringstream ss;
                ss << "expected searchmem(find_vals)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo = Brace::OperandLoadtimeInfo();
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            if (nullptr == g_pApiProvider)
                return;
            auto&& argInfo1 = argInfos[0];
            auto&& objPtr = Brace::VarGetObject(argInfo1.IsGlobal ? gvars : lvars, argInfo1.VarIndex);
            using Array64 = ArrayT<uint64_t>;
            auto* pArr = static_cast<Array64*>(objPtr.get());
            uint64_t start;
            uint64_t end;
            uint64_t step;
            uint64_t range;
            uint64_t val_size = sizeof(uint32_t);
            uint64_t max_count;
            auto&& system = g_pApiProvider->GetSystem();
            auto&& sniffer = system.MemorySniffer();
            sniffer.GetMemorySearchInfo(start, end, step, val_size, range, max_count);
            if (val_size < sizeof(uint8_t) || val_size > sizeof(uint64_t))
                val_size = sizeof(uint32_t);

            std::set<uint64_t> findVals{};
            for (auto&& fval : *pArr) {
                if (findVals.find(fval) == findVals.end())
                    findVals.insert(fval);
            }

            using IntIntHash64 = HashtableT<int64_t, int64_t>;
            IntIntHash64 hash64{};
            using PrioQueue = std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>>;
            PrioQueue pqueue{};
            std::vector<uint64_t> tempAddrs{};

            uint64_t ct = 0;
            for (uint64_t addr = start; addr <= end - val_size; addr += step) {
                bool result = false;
                uint64_t val = ReadMemory(addr, val_size, result);
                if (findVals.find(val) != findVals.end()) {
                    auto&& it = hash64.find(val);
                    if (it == hash64.end()) {
                        pqueue.push(addr);

                        int64_t k = static_cast<int64_t>(val);
                        int64_t v = static_cast<int64_t>(addr);
                        hash64.insert(std::make_pair(k, v));
                    }
                    else {
                        uint64_t oldAddr = static_cast<uint64_t>(it->second);

                        tempAddrs.clear();
                        bool find = false;
                        for (size_t ix = pqueue.size(); ix > 0; --ix) {
                            uint64_t minAddr = pqueue.top();
                            pqueue.pop();
                            if (minAddr != oldAddr) {
                                tempAddrs.push_back(minAddr);
                            }
                            else {
                                find = true;
                                break;
                            }
                        }
                        if (!find) {
                            assert(false);
                        }
                        for (auto&& maddr : tempAddrs) {
                            pqueue.push(maddr);
                        }
                        pqueue.push(addr);

                        it->second = static_cast<int64_t>(addr);
                    }
                    if (pqueue.size() == findVals.size()) {
                        uint64_t stAddr = pqueue.top();
                        if (addr - stAddr <= range) {
                            g_pApiProvider->LogToView(std::string("===search result==="));
                            for (auto&& pair : hash64) {
                                std::stringstream ss;
                                ss << "addr: " << std::hex << pair.second << " hex_val: " << pair.first << " dec_val: " << std::dec << pair.first;
                                g_pApiProvider->LogToView(ss.str());
                            }
                            g_pApiProvider->LogToView(std::string("===area memory==="));
                            for (uint64_t maddr = stAddr; maddr <= addr; maddr += step) {
                                bool mresult = false;
                                uint64_t mval = ReadMemory(maddr, val_size, mresult);
                                std::stringstream ss;
                                ss << "addr: " << std::hex << maddr << " hex_val: " << mval << " dec_val: " << std::dec << mval;
                                g_pApiProvider->LogToView(ss.str());
                            }

                            hash64.clear();
                            PrioQueue temp{};
                            pqueue.swap(temp);

                            ++ct;
                            if (ct >= max_count)
                                break;
                        }
                    }
                }
            }
        }
    };
    class FindMemoryExp final : public Brace::SimpleBraceApiBase
    {
    public:
        FindMemoryExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 5 && argInfos.size() != 6) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[3].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[3].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[4].Type != Brace::BRACE_DATA_TYPE_OBJECT || argInfos[4].ObjectTypeId != CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY
                || argInfos.size() == 6 && (argInfos[5].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[5].Type > Brace::BRACE_DATA_TYPE_UINT64)) {
                //error
                std::stringstream ss;
                ss << "expected findmemory(start, size, step, range, find_vals[, val_size]), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            auto&& argInfo4 = argInfos[3];
            auto&& argInfo5 = argInfos[4];
            uint64_t start = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t size = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            uint64_t step = static_cast<uint64_t>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));
            uint64_t range = static_cast<uint64_t>(Brace::VarGetI64(argInfo4.IsGlobal ? gvars : lvars, argInfo4.Type, argInfo4.VarIndex));
            auto&& objPtr = Brace::VarGetObject(argInfo5.IsGlobal ? gvars : lvars, argInfo5.VarIndex);
            using Array64 = ArrayT<uint64_t>;
            auto* pArr = static_cast<Array64*>(objPtr.get());
            uint64_t val_size = sizeof(uint32_t);
            if (argInfos.size() == 6) {
                auto&& argInfo6 = argInfos[5];
                val_size = static_cast<uint64_t>(Brace::VarGetI64(argInfo6.IsGlobal ? gvars : lvars, argInfo6.Type, argInfo6.VarIndex));
            }
            if (val_size < sizeof(uint8_t) || val_size > sizeof(uint64_t))
                val_size = sizeof(uint32_t);

            std::set<uint64_t> findVals{};
            for (auto&& fval : *pArr) {
                if (findVals.find(fval) == findVals.end())
                    findVals.insert(fval);
            }

            using IntIntHash64 = HashtableT<int64_t, int64_t>;
            auto&& hash64ptr = std::make_shared<IntIntHash64>();

            using PrioQueue = std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>>;
            PrioQueue pqueue{};
            std::vector<uint64_t> tempAddrs{};

            for (uint64_t addr = start; addr <= start + size - val_size; addr += step) {
                bool result = false;
                uint64_t val = ReadMemory(addr, val_size, result);
                if (findVals.find(val) != findVals.end()) {
                    auto&& it = hash64ptr->find(val);
                    if (it == hash64ptr->end()) {
                        pqueue.push(addr);

                        int64_t k = static_cast<int64_t>(val);
                        int64_t v = static_cast<int64_t>(addr);
                        hash64ptr->insert(std::make_pair(k, v));
                    }
                    else {
                        uint64_t oldAddr = static_cast<uint64_t>(it->second);

                        tempAddrs.clear();
                        bool find = false;
                        for (size_t ix = pqueue.size(); ix > 0; --ix) {
                            uint64_t minAddr = pqueue.top();
                            pqueue.pop();
                            if (minAddr != oldAddr) {
                                tempAddrs.push_back(minAddr);
                            }
                            else {
                                find = true;
                                break;
                            }
                        }
                        if (!find) {
                            assert(false);
                        }
                        for (auto&& maddr : tempAddrs) {
                            pqueue.push(maddr);
                        }
                        pqueue.push(addr);

                        it->second = static_cast<int64_t>(addr);
                    }
                    if (pqueue.size() == findVals.size()) {
                        if (addr - pqueue.top() <= range) {
                            break;
                        }
                    }
                }
            }

            Brace::VarSetObject(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, hash64ptr);
        }
    };
    class SearchMemoryExp final : public Brace::SimpleBraceApiBase
    {
    public:
        SearchMemoryExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 5 && argInfos.size() != 6 && argInfos.size() != 7) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[3].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[3].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[4].Type != Brace::BRACE_DATA_TYPE_OBJECT || argInfos[4].ObjectTypeId != CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY
                || argInfos.size() >= 6 && (argInfos[5].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[5].Type > Brace::BRACE_DATA_TYPE_UINT64)
                || argInfos.size() == 7 && (argInfos[6].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[6].Type > Brace::BRACE_DATA_TYPE_UINT64)) {
                //error
                std::stringstream ss;
                ss << "expected searchmemory(start, size, step, range, find_vals[, val_size, max_count]), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_OBJECT;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            auto&& argInfo4 = argInfos[3];
            auto&& argInfo5 = argInfos[4];
            uint64_t start = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t size = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            uint64_t step = static_cast<uint64_t>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));
            uint64_t range = static_cast<uint64_t>(Brace::VarGetI64(argInfo4.IsGlobal ? gvars : lvars, argInfo4.Type, argInfo4.VarIndex));
            auto&& objPtr = Brace::VarGetObject(argInfo5.IsGlobal ? gvars : lvars, argInfo5.VarIndex);
            using Array64 = ArrayT<uint64_t>;
            auto* pArr = static_cast<Array64*>(objPtr.get());
            uint64_t val_size = sizeof(uint32_t);
            uint64_t max_count = std::numeric_limits<uint64_t>::max();
            if (argInfos.size() >= 6) {
                auto&& argInfo6 = argInfos[5];
                val_size = static_cast<uint64_t>(Brace::VarGetI64(argInfo6.IsGlobal ? gvars : lvars, argInfo6.Type, argInfo6.VarIndex));
            }
            if (argInfos.size() == 7) {
                auto&& argInfo7 = argInfos[6];
                max_count = static_cast<uint64_t>(Brace::VarGetI64(argInfo7.IsGlobal ? gvars : lvars, argInfo7.Type, argInfo7.VarIndex));
            }
            if (val_size < sizeof(uint8_t) || val_size > sizeof(uint64_t))
                val_size = sizeof(uint32_t);

            std::set<uint64_t> findVals{};
            for (auto&& fval : *pArr) {
                if (findVals.find(fval) == findVals.end())
                    findVals.insert(fval);
            }

            using IntIntHash64 = HashtableT<int64_t, int64_t>;
            auto&& hash64ptr = std::make_shared<IntIntHash64>();

            IntIntHash64 hash64{};
            using PrioQueue = std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>>;
            PrioQueue pqueue{};
            std::vector<uint64_t> tempAddrs{};

            uint64_t ct = 0;
            for (uint64_t addr = start; addr <= start + size - val_size; addr += step) {
                bool result = false;
                uint64_t val = ReadMemory(addr, val_size, result);
                if (findVals.find(val) != findVals.end()) {
                    auto&& it = hash64.find(val);
                    if (it == hash64.end()) {
                        pqueue.push(addr);

                        int64_t k = static_cast<int64_t>(val);
                        int64_t v = static_cast<int64_t>(addr);
                        hash64.insert(std::make_pair(k, v));
                    }
                    else {
                        uint64_t oldAddr = static_cast<uint64_t>(it->second);

                        tempAddrs.clear();
                        bool find = false;
                        for (size_t ix = pqueue.size(); ix > 0; --ix) {
                            uint64_t minAddr = pqueue.top();
                            pqueue.pop();
                            if (minAddr != oldAddr) {
                                tempAddrs.push_back(minAddr);
                            }
                            else {
                                find = true;
                                break;
                            }
                        }
                        if (!find) {
                            assert(false);
                        }
                        for (auto&& maddr : tempAddrs) {
                            pqueue.push(maddr);
                        }
                        pqueue.push(addr);

                        it->second = static_cast<int64_t>(addr);
                    }
                    if (pqueue.size() == findVals.size()) {
                        if (addr - pqueue.top() <= range) {
                            for (auto&& pair : hash64) {
                                hash64ptr->insert(std::make_pair(pair.second, pair.first));
                            }

                            hash64.clear();
                            PrioQueue temp{};
                            pqueue.swap(temp);

                            ++ct;
                            if (ct >= max_count)
                                break;
                        }
                    }
                }
            }

            Brace::VarSetObject(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, hash64ptr);
        }
    };
    class ReadMemoryExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ReadMemoryExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 1 && argInfos.size() != 2) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos.size() == 2 && (argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64)) {
                //error
                std::stringstream ss;
                ss << "expected readmemory(addr[, val_size]), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            uint64_t addr = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t val_size = sizeof(uint32_t);
            if (argInfos.size() == 2) {
                auto&& argInfo2 = argInfos[1];
                val_size = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            }
            if (val_size < sizeof(uint8_t) || val_size > sizeof(uint64_t))
                val_size = sizeof(uint32_t);

            bool result = false;
            uint64_t val = ReadMemory(addr, val_size, result);
            if (!result && nullptr != g_pApiProvider) {
                std::stringstream ss;
                ss << "read addr:" << std::hex << addr << " size:" << std::dec << val_size << " failed.";
                g_pApiProvider->LogToView(ss.str());
            }

            Brace::VarSetUInt64(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, val);
        }
    };
    class WriteMemoryExp final : public Brace::SimpleBraceApiBase
    {
    public:
        WriteMemoryExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 2 && argInfos.size() != 3) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos.size() == 3 && (argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64)) {
                //error
                std::stringstream ss;
                ss << "expected writememory(addr, val[, val_size]), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            uint64_t addr = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t val = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            uint64_t val_size = sizeof(uint32_t);
            if (argInfos.size() == 3) {
                auto&& argInfo3 = argInfos[2];
                val_size = static_cast<uint64_t>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));
            }
            if (val_size < sizeof(uint8_t) || val_size > sizeof(uint64_t))
                val_size = sizeof(uint32_t);

            bool result = WriteMemory(addr, val_size, val);
            if (!result && nullptr != g_pApiProvider) {
                std::stringstream ss;
                ss << "write addr:" << std::hex << addr << " size:" << std::dec << val_size << " failed.";
                g_pApiProvider->LogToView(ss.str());
            }
            Brace::VarSetBool(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, result);
        }
    };
    class DumpMemoryExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DumpMemoryExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 3 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type != Brace::BRACE_DATA_TYPE_STRING) {
                //error
                std::stringstream ss;
                ss << "expected dumpmemory(uint64 addr, uint64 size, string file_path)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            uint64_t addr = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t size = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            const std::string& file_path = Brace::VarGetString(argInfo3.IsGlobal ? gvars : lvars, argInfo3.VarIndex);

            bool result = false;
            if (nullptr != g_pApiProvider) {
                auto&& system = g_pApiProvider->GetSystem();
                auto&& sniffer = system.MemorySniffer();
                std::ofstream file(get_absolutely_path(file_path), std::ios::out | std::ios::binary);
                if (file.is_open()) {
                    result = sniffer.DumpMemory(addr, size, file);
                    result = true;
                }
            }
            Brace::VarSetBool(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, result);
        }
    };
    class AddLogInstructionExp final : public Brace::SimpleBraceApiBase
    {
    public:
        AddLogInstructionExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 2 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected addloginst(mask, value), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            uint32_t mask = static_cast<uint32_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint32_t value = static_cast<uint32_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));

            auto&& system = g_pApiProvider->GetSystem();
            auto&& sniffer = system.MemorySniffer();

            sniffer.AddLogInstruction(mask, value);
        }
    };
    class ReplaceSourceShaderExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ReplaceSourceShaderExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 3 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type != Brace::BRACE_DATA_TYPE_STRING) {
                //error
                std::stringstream ss;
                ss << "expected replacesourceshader(uint64 shader_hash, int stage, string file_path)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            uint64_t hash = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            int stage = static_cast<int>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            const std::string& filePath = Brace::VarGetString(argInfo3.IsGlobal ? gvars : lvars, argInfo3.VarIndex);

            bool result = false;
            if (nullptr != g_pApiProvider) {
                std::string txt = read_file(filePath);
                if (txt.length() > 0) {
                    g_pApiProvider->ReplaceSourceShader(hash, stage, std::move(txt));
                    result = true;
                }
            }
            Brace::VarSetBool(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, result);
        }
    };
    class ReplaceSpirvShaderExp final : public Brace::SimpleBraceApiBase
    {
    public:
        ReplaceSpirvShaderExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 3 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type != Brace::BRACE_DATA_TYPE_STRING) {
                //error
                std::stringstream ss;
                ss << "expected replacespirvshader(uint64 shader_hash, int stage, string file_path)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_BOOL;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            static const std::size_t s_u32 = sizeof(uint32_t);

            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            uint64_t hash = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            int stage = static_cast<int>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            const std::string& filePath = Brace::VarGetString(argInfo3.IsGlobal ? gvars : lvars, argInfo3.VarIndex);

            bool result = false;
            if (nullptr != g_pApiProvider) {
                std::ifstream file(get_absolutely_path(filePath), std::ios::in | std::ios::binary);
                if (file.is_open()) {
                    file.seekg(0, std::ios::end);
                    std::streamsize file_size = file.tellg();
                    file.seekg(0, std::ios::beg);

                    std::vector<uint32_t> code;
                    if (file_size % s_u32 != 0) {
                        code.resize(file_size / s_u32 + 1);
                        code[code.size() - 1] = 0;
                    }
                    else {
                        code.resize(file_size / s_u32);
                    }
                    file.read(reinterpret_cast<char*>(code.data()), file_size);
                    if (code.size() > 0) {
                        g_pApiProvider->ReplaceSpirvShader(hash, stage, std::move(code));
                        result = true;
                    }
                }
            }
            Brace::VarSetBool(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, result);
        }
    };

    class DmntFileExp final : public Brace::AbstractBraceApi
    {
    public:
        DmntFileExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter), m_Args(), m_ArgInfos(), m_Statements()
        {
        }
    protected:
        virtual bool LoadFunction([[maybe_unused]] const Brace::FuncInfo& curFunc, const DslData::FunctionData& funcData, [[maybe_unused]] Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //dmnt_file(name, module[, file_dir[, build_id]]){...};
            if (funcData.IsHighOrder()) {
                auto&& callData = funcData.GetLowerOrderFunction();
                auto&& argLoadInfos = m_ArgInfos;
                auto&& args = m_Args;
                int num = callData.GetParamNum();
                if (num == 2 || num == 3 || num == 4) {
                    for (int ix = 0; ix < num; ++ix) {
                        auto* param = callData.GetParam(ix);
                        Brace::OperandLoadtimeInfo argLoadInfo;
                        auto p = LoadHelper(*param, argLoadInfo);
                        argLoadInfos.push_back(std::move(argLoadInfo));
                        args.push_back(std::move(p));
                    }
                    if (argLoadInfos[0].Type == Brace::BRACE_DATA_TYPE_STRING
                        && argLoadInfos[1].Type == Brace::BRACE_DATA_TYPE_STRING
                        && (num ==2 || (num >= 3 && argLoadInfos[2].Type == Brace::BRACE_DATA_TYPE_STRING))
                        && (num == 2 || (num == 4 && argLoadInfos[3].Type == Brace::BRACE_DATA_TYPE_STRING))) {
                        PushBlock();
                        for (int ix = 0; ix < funcData.GetParamNum(); ++ix) {
                            Brace::OperandLoadtimeInfo argLoadInfo;
                            auto statement = LoadHelper(*funcData.GetParam(ix), argLoadInfo);
                            if (!statement.isNull())
                                m_Statements.push_back(std::move(statement));
                        }
                        m_ObjVars = CurBlockObjVars();
                        PopBlock();
                        executor.attach(this, &DmntFileExp::Execute);
                        return true;
                    }
                }
            }
            //error
            std::stringstream ss;
            ss << "expected 'dmnt_file(name, module[, file_dir[, build_id]]){...};', line " << funcData.GetLine();
            return false;
        }
        virtual bool LoadStatement([[maybe_unused]] const Brace::FuncInfo& curFunc, const DslData::StatementData& data, [[maybe_unused]] Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //dmnt_file(name, module[, file_dir[, build_id]]) func(args);
            if (data.GetFunctionNum() == 2) {
                auto* first = data.GetFirst()->AsFunction();
                if (!first->HaveStatement() && !first->HaveExternScript()) {
                    auto* second = data.GetSecond();
                    auto* secondVal = second->AsValue();
                    auto* secondFunc = second->AsFunction();
                    if (nullptr != secondVal || (nullptr != secondFunc && secondFunc->HaveId() && !secondFunc->HaveStatement() && !secondFunc->HaveExternScript())) {
                        if (first->GetParamNum() > 0) {
                            auto* pCallData = first->AsFunction();
                            auto&& argLoadInfos = m_ArgInfos;
                            auto&& args = m_Args;
                            if (nullptr != pCallData) {
                                auto&& callData = *pCallData;
                                int num = callData.GetParamNum();
                                if (num == 2 || num == 3 || num == 4) {
                                    for (int ix = 0; ix < num; ++ix) {
                                        auto* param = callData.GetParam(ix);
                                        Brace::OperandLoadtimeInfo argLoadInfo;
                                        auto p = LoadHelper(*param, argLoadInfo);
                                        argLoadInfos.push_back(std::move(argLoadInfo));
                                        args.push_back(std::move(p));
                                    }
                                    if (argLoadInfos[0].Type == Brace::BRACE_DATA_TYPE_STRING
                                        && argLoadInfos[1].Type == Brace::BRACE_DATA_TYPE_STRING
                                        && (num == 2 || (num >= 3 && argLoadInfos[2].Type == Brace::BRACE_DATA_TYPE_STRING))
                                        && (num == 2 || (num == 4 && argLoadInfos[3].Type == Brace::BRACE_DATA_TYPE_STRING))) {
                                        PushBlock();
                                        Brace::OperandLoadtimeInfo argLoadInfo;
                                        auto statement = LoadHelper(*second, argLoadInfo);
                                        if (!statement.isNull())
                                            m_Statements.push_back(std::move(statement));
                                        m_ObjVars = CurBlockObjVars();
                                        PopBlock();
                                        executor.attach(this, &DmntFileExp::Execute);
                                        return true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            //error
            std::stringstream ss;
            ss << "expected 'dmnt_file(name, module[, file_dir[, build_id]]) func(...);', line " << data.GetLine();
            return false;
        }
    private:
        int Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& op : m_Args) {
                if (!op.isNull())
                    op(gvars, lvars);
            }
            auto&& argInfo1 = m_ArgInfos[0];
            auto&& argInfo2 = m_ArgInfos[1];
            auto&& name = Brace::VarGetString((argInfo1.IsGlobal ? gvars : lvars), argInfo1.VarIndex);
            auto&& _module = Brace::VarGetString((argInfo2.IsGlobal ? gvars : lvars), argInfo2.VarIndex);
            std::string file_dir{};
            if (m_ArgInfos.size() >= 3) {
                auto&& argInfo3 = m_ArgInfos[2];
                file_dir = Brace::VarGetString((argInfo3.IsGlobal ? gvars : lvars), argInfo3.VarIndex);
            }
            std::string bid = "unknown";
            if (m_ArgInfos.size() == 4) {
                auto&& argInfo4 = m_ArgInfos[3];
                bid = Brace::VarGetString((argInfo4.IsGlobal ? gvars : lvars), argInfo4.VarIndex);
            }

            auto&& system = g_pApiProvider->GetSystem();
            auto&& sniffer = system.MemorySniffer();

            std::string file_name = bid + ".txt";
            for (int ix = 0; ix < sniffer.GetModuleCount(); ++ix) {
                uint64_t addr;
                uint64_t size;
                std::string build_id;
                std::string mname;
                uint64_t base = sniffer.GetModuleBase(ix, addr, size, build_id, mname);
                if (mname == _module) {
                    g_DmntData.main_base = base;
                    g_DmntData.main_size = size;

                    bid = build_id;
                    file_name = build_id + ".txt";
                    break;
                }
            }
            //main,heap,alias,aslr
            auto&& ss = g_DmntData.ss;

            std::string file_path;
            if (file_dir.length() > 0) {
                std::filesystem::path p(file_dir);
                std::filesystem::path f(file_name);
                file_path = (p / f).string();
            }
            else {
                file_path = file_name;
            }

            uint64_t title_id = 0;
            if (system.ApplicationProcess()) {
                title_id = system.GetApplicationProcessProgramID();
            }

            ss.str("");
            ss << std::uppercase;
            ss << "{ " << name << " " << bid << " [" << std::setfill('0') << std::setw(sizeof(uint64_t) * 2) << std::hex << title_id << "] }" << std::endl;

            int v = Brace::BRACE_FLOW_CONTROL_NORMAL;
            for (auto&& statement : m_Statements) {
                v = statement(gvars, lvars);
                if (IsForceQuit()) {
                    FreeObjVars(lvars, m_ObjVars);
                    break;
                }
                if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                    break;
                }
                else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                    FreeObjVars(lvars, m_ObjVars);
                    break;
                }
            }
            FreeObjVars(lvars, m_ObjVars);

            //write to file
            write_file(file_path, ss.str());

            return v;
        }
    private:
        std::vector<Brace::BraceApiExecutor> m_Args;
        std::vector<Brace::OperandRuntimeInfo> m_ArgInfos;
        std::vector<Brace::BraceApiExecutor> m_Statements;
        std::vector<int> m_ObjVars;
    };
    class DmntIfExp final : public Brace::AbstractBraceApi
    {
    public:
        DmntIfExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter), m_Clauses()
        {
        }
    protected:
        virtual bool LoadFunction([[maybe_unused]] const Brace::FuncInfo& curFunc, const DslData::FunctionData& funcData, [[maybe_unused]] Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //dmnt_if(exp){...};
            if (funcData.IsHighOrder()) {
                auto* cond = funcData.GetLowerOrderFunction().GetParam(0);
                Brace::OperandLoadtimeInfo loadInfo;
                Clause item;
                item.Condition = LoadHelper(*cond, loadInfo);
                item.ConditionInfo = loadInfo;
                PushBlock();
                for (int ix = 0; ix < funcData.GetParamNum(); ++ix) {
                    Brace::OperandLoadtimeInfo argLoadInfo;
                    auto statement = LoadHelper(*funcData.GetParam(ix), argLoadInfo);
                    if (!statement.isNull())
                        item.Statements.push_back(std::move(statement));
                }
                item.ObjVars = CurBlockObjVars();
                PopBlock();
                m_Clauses.push_back(std::move(item));
                executor.attach(this, &DmntIfExp::Execute);
            }
            else {
                //error
                std::stringstream ss;
                ss << "expected 'dmnt_if(exp){...};', line " << funcData.GetLine();
                LogError(ss.str());
            }
            return true;
        }
        virtual bool LoadStatement([[maybe_unused]] const Brace::FuncInfo& curFunc, const DslData::StatementData& data, [[maybe_unused]] Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //dmnt_if(exp) func(args); or dmnt_if(exp){...}elseif/elif(exp){...}else{...};
            int funcNum = data.GetFunctionNum();
            if (funcNum == 2) {
                auto* first = data.GetFirst()->AsFunction();
                if (!first->HaveStatement() && !first->HaveExternScript()) {
                    auto* second = data.GetSecond();
                    auto* secondVal = second->AsValue();
                    auto* secondFunc = second->AsFunction();
                    if (nullptr != secondVal || (nullptr != secondFunc && secondFunc->HaveId() && !secondFunc->HaveStatement() && !secondFunc->HaveExternScript())) {
                        Clause item;
                        if (first->GetParamNum() > 0) {
                            auto* cond = first->GetParam(0);
                            Brace::OperandLoadtimeInfo loadInfo;
                            item.Condition = LoadHelper(*cond, loadInfo);
                            item.ConditionInfo = loadInfo;
                        }
                        else {
                            //error
                            std::stringstream ss;
                            ss << "expected 'dmnt_if(exp) func(...);', line " << data.GetLine();
                            LogError(ss.str());
                        }
                        Brace::OperandLoadtimeInfo argLoadInfo;
                        auto statement = LoadHelper(*second, argLoadInfo);
                        if (!statement.isNull())
                            item.Statements.push_back(std::move(statement));
                        m_Clauses.push_back(std::move(item));
                        executor.attach(this, &DmntIfExp::Execute);
                        return true;
                    }
                }
            }
            //standard if
            for (int ix = 0; ix < data.GetFunctionNum(); ++ix) {
                auto* fd = data.GetFunction(ix);
                auto* fData = fd->AsFunction();
                if (fData->GetId() == "dmnt_if" || fData->GetId() == "elseif" || fData->GetId() == "elif") {
                    Clause item;
                    if (fData->IsHighOrder() && fData->GetLowerOrderFunction().GetParamNum() > 0) {
                        auto* cond = fData->GetLowerOrderFunction().GetParam(0);
                        Brace::OperandLoadtimeInfo loadInfo;
                        item.Condition = LoadHelper(*cond, loadInfo);
                        item.ConditionInfo = loadInfo;
                    }
                    else {
                        //error
                        std::stringstream ss;
                        ss << "expected 'dmnt_if(exp){...}elseif/elif(exp){...}else{...};', line " << data.GetLine();
                        LogError(ss.str());
                    }
                    PushBlock();
                    for (int iix = 0; iix < fData->GetParamNum(); ++iix) {
                        Brace::OperandLoadtimeInfo argLoadInfo;
                        auto statement = LoadHelper(*fData->GetParam(iix), argLoadInfo);
                        if (!statement.isNull())
                            item.Statements.push_back(std::move(statement));
                    }
                    item.ObjVars = CurBlockObjVars();
                    PopBlock();
                    m_Clauses.push_back(std::move(item));
                }
                else if (fData->GetId() == "else") {
                    if (fData != data.GetLast()) {
                        //error
                        std::stringstream ss;
                        ss << "expected 'dmnt_if(exp){...}elseif/elif(exp){...}else{...};', line " << data.GetLine();
                        LogError(ss.str());
                    }
                    else {
                        Clause item;
                        PushBlock();
                        for (int iix = 0; iix < fData->GetParamNum(); ++iix) {
                            Brace::OperandLoadtimeInfo argLoadInfo;
                            auto statement = LoadHelper(*fData->GetParam(iix), argLoadInfo);
                            if (!statement.isNull())
                                item.Statements.push_back(std::move(statement));
                        }
                        item.ObjVars = CurBlockObjVars();
                        PopBlock();
                        m_Clauses.push_back(std::move(item));
                    }
                }
                else {
                    //error
                    std::stringstream ss;
                    ss << "expected 'dmnt_if(exp){...}elseif/elif(exp){...}else{...};', line " << data.GetLine();
                    LogError(ss.str());
                }
            }
            executor.attach(this, &DmntIfExp::Execute);
            return true;
        }
    private:
        int Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            auto&& ss = g_DmntData.ss;

            int v = Brace::BRACE_FLOW_CONTROL_NORMAL;
            int ct = static_cast<int>(m_Clauses.size());
            for (int ix = 0; ix < ct; ++ix) {
                auto& clause = m_Clauses[ix];
                if (!clause.Condition.isNull()) {
                    //gen condition begin
                    clause.Condition(gvars, lvars);
                }
                for (auto&& statement : clause.Statements) {
                    v = statement(gvars, lvars);
                    if (IsForceQuit())
                        break;
                    if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                        FreeObjVars(lvars, clause.ObjVars);
                        break;
                    }
                }
                FreeObjVars(lvars, clause.ObjVars);
                if (ix < ct - 1) {
                    //gen else
                    //2X000000
                    //X: End type (0 = End, 1 = Else).
                    ss << "21000000" << std::endl;
                }
                else {
                    //gen end
                    //2X000000
                    //X: End type (0 = End, 1 = Else).
                    for (int i = 0; i < ct; ++i) {
                        ss << "20000000" << std::endl;
                    }
                }
                if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                    break;
                }
            }
            return v;
        }
    private:
        struct Clause
        {
            Brace::BraceApiExecutor Condition;
            Brace::OperandRuntimeInfo ConditionInfo;
            std::vector<Brace::BraceApiExecutor> Statements;
            std::vector<int> ObjVars;
        };

        std::vector<Clause> m_Clauses;
    };
    class DmntLoopExp final : public Brace::AbstractBraceApi
    {
    public:
        DmntLoopExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter), m_Args(), m_ArgInfos(), m_Statements()
        {
        }
    protected:
        virtual bool LoadFunction([[maybe_unused]] const Brace::FuncInfo& curFunc, const DslData::FunctionData& funcData, [[maybe_unused]] Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //dmnt_loop(reg, count){...};
            if (funcData.IsHighOrder()) {
                auto&& callData = funcData.GetLowerOrderFunction();
                auto&& argLoadInfos = m_ArgInfos;
                auto&& args = m_Args;
                int num = callData.GetParamNum();
                if (num == 2) {
                    for (int ix = 0; ix < num; ++ix) {
                        auto* param = callData.GetParam(ix);
                        Brace::OperandLoadtimeInfo argLoadInfo;
                        auto p = LoadHelper(*param, argLoadInfo);
                        argLoadInfos.push_back(std::move(argLoadInfo));
                        args.push_back(std::move(p));
                    }
                    if (argLoadInfos[0].Type >= Brace::BRACE_DATA_TYPE_INT8 && argLoadInfos[0].Type <= Brace::BRACE_DATA_TYPE_UINT64
                        && argLoadInfos[1].Type >= Brace::BRACE_DATA_TYPE_INT8 && argLoadInfos[1].Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                        PushBlock();
                        for (int ix = 0; ix < funcData.GetParamNum(); ++ix) {
                            Brace::OperandLoadtimeInfo argLoadInfo;
                            auto statement = LoadHelper(*funcData.GetParam(ix), argLoadInfo);
                            if (!statement.isNull())
                                m_Statements.push_back(std::move(statement));
                        }
                        m_ObjVars = CurBlockObjVars();
                        PopBlock();
                        executor.attach(this, &DmntLoopExp::Execute);
                        return true;
                    }
                }
            }
            //error
            std::stringstream ss;
            ss << "expected 'dmnt_loop(reg, count){...};', line " << funcData.GetLine();
            return false;
        }
        virtual bool LoadStatement([[maybe_unused]] const Brace::FuncInfo& curFunc, const DslData::StatementData& data, [[maybe_unused]] Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //dmnt_loop(reg, count) func(args);
            if (data.GetFunctionNum() == 2) {
                auto* first = data.GetFirst()->AsFunction();
                if (!first->HaveStatement() && !first->HaveExternScript()) {
                    auto* second = data.GetSecond();
                    auto* secondVal = second->AsValue();
                    auto* secondFunc = second->AsFunction();
                    if (nullptr != secondVal || (nullptr != secondFunc && secondFunc->HaveId() && !secondFunc->HaveStatement() && !secondFunc->HaveExternScript())) {
                        if (first->GetParamNum() > 0) {
                            auto* pCallData = first->AsFunction();
                            auto&& argLoadInfos = m_ArgInfos;
                            auto&& args = m_Args;
                            if (nullptr != pCallData) {
                                auto&& callData = *pCallData;
                                int num = callData.GetParamNum();
                                if (num == 2) {
                                    for (int ix = 0; ix < num; ++ix) {
                                        auto* param = callData.GetParam(ix);
                                        Brace::OperandLoadtimeInfo argLoadInfo;
                                        auto p = LoadHelper(*param, argLoadInfo);
                                        argLoadInfos.push_back(std::move(argLoadInfo));
                                        args.push_back(std::move(p));
                                    }
                                    if (argLoadInfos[0].Type >= Brace::BRACE_DATA_TYPE_INT8 && argLoadInfos[0].Type <= Brace::BRACE_DATA_TYPE_UINT64
                                        && argLoadInfos[1].Type >= Brace::BRACE_DATA_TYPE_INT8 && argLoadInfos[1].Type <= Brace::BRACE_DATA_TYPE_UINT64) {
                                        PushBlock();
                                        Brace::OperandLoadtimeInfo argLoadInfo;
                                        auto statement = LoadHelper(*second, argLoadInfo);
                                        if (!statement.isNull())
                                            m_Statements.push_back(std::move(statement));
                                        m_ObjVars = CurBlockObjVars();
                                        PopBlock();
                                        executor.attach(this, &DmntLoopExp::Execute);
                                        return true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            //error
            std::stringstream ss;
            ss << "expected 'dmnt_loop(reg, count) func(...);', line " << data.GetLine();
            return false;
        }
    private:
        int Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars)const
        {
            for (auto&& op : m_Args) {
                if (!op.isNull())
                    op(gvars, lvars);
            }
            auto&& argInfo1 = m_ArgInfos[0];
            auto&& argInfo2 = m_ArgInfos[1];
            int reg = static_cast<int>(Brace::VarGetI64((argInfo1.IsGlobal ? gvars : lvars), argInfo1.Type, argInfo1.VarIndex));
            int ct = static_cast<int>(Brace::VarGetI64((argInfo2.IsGlobal ? gvars : lvars), argInfo2.Type, argInfo2.VarIndex));
            //gen begin
            //300R0000 VVVVVVVV
            //R: Register to use as loop counter.
            //V : Number of iterations to loop.
            auto&& ss = g_DmntData.ss;
            ss << "300" << std::setfill('0') << std::setw(1) << std::hex << reg << "0000 ";
            ss << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << ct << std::endl;

            int v = Brace::BRACE_FLOW_CONTROL_NORMAL;
            for (auto&& statement : m_Statements) {
                v = statement(gvars, lvars);
                if (IsForceQuit()) {
                    FreeObjVars(lvars, m_ObjVars);
                    break;
                }
                if (v == Brace::BRACE_FLOW_CONTROL_CONTINUE) {
                    break;
                }
                else if (v != Brace::BRACE_FLOW_CONTROL_NORMAL) {
                    FreeObjVars(lvars, m_ObjVars);
                    break;
                }
            }
            FreeObjVars(lvars, m_ObjVars);

            //gen end
            //310R0000
            //R: Register to use as loop counter.
            ss << "310" << std::setfill('0') << std::setw(1) << std::hex << reg << "0000" << std::endl;

            return v;
        }
    private:
        std::vector<Brace::BraceApiExecutor> m_Args;
        std::vector<Brace::OperandRuntimeInfo> m_ArgInfos;
        std::vector<Brace::BraceApiExecutor> m_Statements;
        std::vector<int> m_ObjVars;
    };

    class DmntKeyExp final : public Brace::AbstractBraceApi
    {
    public:
        DmntKeyExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter)
        {
        }
    protected:
        virtual bool LoadFunction([[maybe_unused]] const Brace::FuncInfo& curFunc, const DslData::FunctionData& funcData, [[maybe_unused]] Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //dmnt_key(key)
            if (funcData.HaveParam() && funcData.GetParamNum() == 1) {
                const std::string& name = funcData.GetParamId(0);
                std::unordered_map<std::string, uint32_t> key2masks = {
                    {"A", 0x1},
                    {"B", 0x2},
                    {"X", 0x4},
                    {"Y", 0x8},
                    {"LS", 0x10},
                    {"RS", 0x20},
                    {"L", 0x40},
                    {"R", 0x80},
                    {"ZL", 0x100},
                    {"ZR", 0x200},
                    {"Plus", 0x400},
                    {"Minus", 0x800},
                    {"Left", 0x1000},
                    {"Up", 0x2000},
                    {"Right", 0x4000},
                    {"Down", 0x8000},
                    {"LSL", 0x10000},
                    {"LSU", 0x20000},
                    {"LSR", 0x40000},
                    {"LSD", 0x80000},
                    {"RSL", 0x100000},
                    {"RSU", 0x200000},
                    {"RSR", 0x400000},
                    {"RSD", 0x800000},
                    {"SL", 0x1000000},
                    {"SR", 0x2000000},
                };
                std::string varId;
                auto&& it = key2masks.find(name);
                if (it == key2masks.end()) {
                    //error
                    std::stringstream ss;
                    ss << "expected 'dmnt_key(key)' key:A|B|X|Y|LS|RS|L|R|ZL|ZR|Plus|Minus|Left|Up|Right|Down|LSL|LSU|LSR|LSD|RSL|RSU|RSR|RSD|SL|SR, line " << funcData.GetLine();
                    return false;
                }
                else {
                    varId = std::to_string(it->second);
                }
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
            //error
            std::stringstream ss;
            ss << "expected 'dmnt_key(key)' key:A|B|X|Y|LS|RS|L|R|ZL|ZR|Plus|Minus|Left|Up|Right|Down|LSL|LSU|LSR|LSD|RSL|RSU|RSR|RSD|SL|SR, line " << funcData.GetLine();
            return false;
        }
    };
    class DmntRegionExp final : public Brace::AbstractBraceApi
    {
    public:
        DmntRegionExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter)
        {
        }
    protected:
        virtual bool LoadFunction([[maybe_unused]] const Brace::FuncInfo& curFunc, const DslData::FunctionData& funcData, [[maybe_unused]] Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //dmnt_region(mem_region)
            if (funcData.HaveParam() && funcData.GetParamNum() == 1) {
                const std::string& name = funcData.GetParamId(0);
                //main,heap,alias,aslr
                std::string varId;
                if (name == "main") {
                    varId = "0";
                }
                else if (name == "heap") {
                    varId = "1";
                }
                else if (name == "alias") {
                    varId = "2";
                }
                else if (name == "aslr") {
                    varId = "3";
                }
                else {
                    //error
                    std::stringstream ss;
                    ss << "expected 'dmnt_region(mem_region)' mem_region:main|heap|alias|aslr, line " << funcData.GetLine();
                    return false;
                }
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
            //error
            std::stringstream ss;
            ss << "expected 'dmnt_region(mem_region)' mem_region:main|heap|alias|aslr, line " << funcData.GetLine();
            return false;
        }
    };
    class DmntOffsetExp final : public Brace::AbstractBraceApi
    {
    public:
        DmntOffsetExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter)
        {
        }
    protected:
        virtual bool LoadFunction([[maybe_unused]] const Brace::FuncInfo& curFunc, const DslData::FunctionData& funcData, [[maybe_unused]] Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //dmnt_offset(name)
            if (funcData.HaveParam() && funcData.GetParamNum() == 1) {
                const std::string& name = funcData.GetParamId(0);
                std::unordered_map<std::string, uint32_t> key2vals = {
                    {"no_offset", 0},
                    {"offset_reg", 1},
                    {"offset_fixed", 2},
                    {"region_and_base", 3},
                    {"region_and_relative", 4},
                    {"region_and_relative_and_offset", 5},
                };
                std::string varId;
                auto&& it = key2vals.find(name);
                if (it == key2vals.end()) {
                    //error
                    std::stringstream ss;
                    ss << "expected 'dmnt_offset(name)' name:no_offset|offset_reg|offset_fixed|region_and_base|region_and_relative|region_and_relative_and_offset, line " << funcData.GetLine();
                    return false;
                }
                else {
                    varId = std::to_string(it->second);
                }
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
            //error
            std::stringstream ss;
            ss << "expected 'dmnt_offset(name)' name:no_offset|offset_reg|offset_fixed|region_and_base|region_and_relative|region_and_relative_and_offset, line " << funcData.GetLine();
            return false;
        }
    };
    class DmntOperandExp final : public Brace::AbstractBraceApi
    {
    public:
        DmntOperandExp(Brace::BraceScript& interpreter) :Brace::AbstractBraceApi(interpreter)
        {
        }
    protected:
        virtual bool LoadFunction([[maybe_unused]] const Brace::FuncInfo& curFunc, const DslData::FunctionData& funcData, [[maybe_unused]] Brace::OperandLoadtimeInfo& resultInfo, Brace::BraceApiExecutor& executor) override
        {
            //dmnt_operand(name)
            if (funcData.HaveParam() && funcData.GetParamNum() == 1) {
                const std::string& name = funcData.GetParamId(0);
                std::unordered_map<std::string, uint32_t> key2vals = {
                    {"mem_and_relative", 0},
                    {"mem_and_offset", 1},
                    {"reg_and_relative", 2},
                    {"reg_and_offset", 3},
                    {"static_value", 4},
                    {"register_value", 4},
                    {"reg_other", 5},
                    {"restore_register", 0},
                    {"save_register", 1},
                    {"clear_saved_value", 2},
                    {"clear_register", 3},
                };
                std::string varId;
                auto&& it = key2vals.find(name);
                if (it == key2vals.end()) {
                    //error
                    std::stringstream ss;
                    ss << "expected 'dmnt_operand(name)' name:mem_and_relative|mem_and_offset|reg_and_relative|reg_and_offset|static_value|register_value|reg_other|restore_register|save_register|clear_saved_value|clear_register, line " << funcData.GetLine();
                    return false;
                }
                else {
                    varId = std::to_string(it->second);
                }
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
            //error
            std::stringstream ss;
            ss << "expected 'dmnt_operand(name)' name:mem_and_relative|mem_and_offset|reg_and_relative|reg_and_offset|static_value|register_value|reg_other|restore_register|save_register|clear_saved_value|clear_register, line " << funcData.GetLine();
            return false;
        }
    };

    class DmntCalcOffsetExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntCalcOffsetExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 3 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_calc_offset(offset, addr, region), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            uint64_t offset = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t addr = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            uint64_t region = static_cast<uint64_t>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));

            if (offset == 0) {
                auto&& system = g_pApiProvider->GetSystem();
                auto&& sniffer = system.MemorySniffer();

                uint64_t base = 0;
                uint64_t size = 0;
                switch (region) {
                case 0://main
                    base = g_DmntData.main_base;
                    break;
                case 1://heap
                    base = sniffer.GetHeapBase(size);
                    break;
                case 2://alias
                    base = sniffer.GetAliasBase(size);
                    break;
                case 3://aslr
                    base = sniffer.GetAliasCodeBase(size);
                    break;
                }
                offset = addr - base;
            }

            Brace::VarSetUInt64(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, offset);
        }
    };
    class DmntReadMemExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntReadMemExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 2 && argInfos.size() != 3) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || (argInfos.size() == 3 && (argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64))) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_read_mem(val, addr[, val_size]), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            resultInfo.Type = Brace::BRACE_DATA_TYPE_UINT64;
            resultInfo.Name = GenTempVarName();
            resultInfo.ObjectTypeId = Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ;
            resultInfo.VarIndex = AllocVariable(resultInfo.Name, resultInfo.Type, resultInfo.ObjectTypeId);
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            uint64_t val = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t addr = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            uint64_t val_size = sizeof(uint32_t);
            if (argInfos.size() == 3) {
                auto&& argInfo3 = argInfos[2];
                val_size = static_cast<uint64_t>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));
            }
            if (val_size < sizeof(uint8_t) || val_size > sizeof(uint64_t))
                val_size = sizeof(uint32_t);

            if (val == 0) {
                bool result = false;
                val = ReadMemory(addr, val_size, result);
                if (!result && nullptr != g_pApiProvider) {
                    std::stringstream ss;
                    ss << "read addr:" << std::hex << addr << " size:" << std::dec << val_size << " failed.";
                    g_pApiProvider->LogToView(ss.str());
                }
            }
            Brace::VarSetUInt64(resultInfo.IsGlobal ? gvars : lvars, resultInfo.VarIndex, val);
        }
    };
    class DmntCommentExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntCommentExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 1 || argInfos[0].Type != Brace::BRACE_DATA_TYPE_STRING) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_comment(str)," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            const std::string& cmt = Brace::VarGetString(argInfo1.IsGlobal ? gvars : lvars, argInfo1.VarIndex);

            auto&& ss = g_DmntData.ss;
            ss << "[ " << cmt << " ]" << std::endl;
        }
    };
    class DmntStoreValueToAddrExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntStoreValueToAddrExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 5 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[3].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[3].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[4].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[4].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_store_v2a(mem_width, mem_region, reg, offset, val), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            auto&& argInfo4 = argInfos[3];
            auto&& argInfo5 = argInfos[4];
            uint64_t mem_width = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t mem_region = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            uint64_t reg = static_cast<uint64_t>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));
            uint64_t offset = static_cast<uint64_t>(Brace::VarGetI64(argInfo4.IsGlobal ? gvars : lvars, argInfo4.Type, argInfo4.VarIndex));
            uint64_t val = static_cast<uint64_t>(Brace::VarGetI64(argInfo5.IsGlobal ? gvars : lvars, argInfo5.Type, argInfo5.VarIndex));

            uint32_t h32 = static_cast<uint32_t>(offset >> 32);
            uint32_t l32 = static_cast<uint32_t>(offset & 0xffffffffull);
            uint32_t vh32 = static_cast<uint32_t>(val >> 32);
            uint32_t vl32 = static_cast<uint32_t>(val & 0xffffffffull);

            //gen Code Type 0x0: Store Static Value to Memory
            //0TMR00AA AAAAAAAA VVVVVVVV (VVVVVVVV)
            //T: Width of memory write(1, 2, 4, or 8 bytes).
            //M : Memory region to write to(0 = Main NSO, 1 = Heap, 2 = Alias, 3 = Aslr).
            //R : Register to use as an offset from memory region base.
            //A : Immediate offset to use from memory region base.
            //V : Value to write.
            auto&& ss = g_DmntData.ss;
            ss << "0";
            ss << std::setfill('0') << std::setw(1) << std::hex << mem_width;
            ss << std::setfill('0') << std::setw(1) << std::hex << mem_region;
            ss << std::setfill('0') << std::setw(1) << std::hex << reg;
            ss << "00";
            ss << std::setfill('0') << std::setw(2) << std::hex << h32;
            ss << " ";
            ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << l32;
            ss << " ";
            if (mem_width == 8) {
                ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << vh32;
                ss << " ";
            }
            ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << vl32;
            ss << std::endl;
        }
    };
    class DmntConditionExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntConditionExp(Brace::BraceScript& interpreter, const std::string& op) :Brace::SimpleBraceApiBase(interpreter), m_Operator(op)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 4 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[3].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[3].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_xxx(mem_width, mem_region, offset, val), all type is integer, xxx:gt|ge|lt|le|eq|ne," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            auto&& argInfo4 = argInfos[3];
            uint64_t mem_width = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t mem_region = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            uint64_t offset = static_cast<uint64_t>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));
            uint64_t val = static_cast<uint64_t>(Brace::VarGetI64(argInfo4.IsGlobal ? gvars : lvars, argInfo4.Type, argInfo4.VarIndex));

            uint32_t h32 = static_cast<uint32_t>(offset >> 32);
            uint32_t l32 = static_cast<uint32_t>(offset & 0xffffffffull);
            uint32_t vh32 = static_cast<uint32_t>(val >> 32);
            uint32_t vl32 = static_cast<uint32_t>(val & 0xffffffffull);

            int op = 0;
            if (m_Operator == ">")
                op = 1;
            if (m_Operator == ">=")
                op = 2;
            if (m_Operator == "<")
                op = 3;
            if (m_Operator == "<=")
                op = 4;
            if (m_Operator == "==")
                op = 5;
            if (m_Operator == "!=")
                op = 6;

            //gen Code Type 0x1: Begin Conditional Block
            //1TMC00AA AAAAAAAA VVVVVVVV (VVVVVVVV)
            //T: Width of memory write(1, 2, 4, or 8 bytes).
            //M : Memory region to write to(0 = Main NSO, 1 = Heap, 2 = Alias, 3 = Aslr).
            //C : Condition to use, see below.
            //A : Immediate offset to use from memory region base.
            //V : Value to compare to.
            //Conditions
            //1 : >
            //2: >=
            //3 : <
            //4 : <=
            //5 : ==
            //6 : !=
            auto&& ss = g_DmntData.ss;
            ss << "1";
            ss << std::setfill('0') << std::setw(1) << std::hex << mem_width;
            ss << std::setfill('0') << std::setw(1) << std::hex << mem_region;
            ss << std::setfill('0') << std::setw(1) << std::hex << op;
            ss << "00";
            ss << std::setfill('0') << std::setw(2) << std::hex << h32;
            ss << " ";
            ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << l32;
            ss << " ";
            if (mem_width == 8) {
                ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << vh32;
                ss << " ";
            }
            ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << vl32;
            ss << std::endl;
        }
    private:
        std::string m_Operator;
    };
    class DmntLoadRegWithValueExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntLoadRegWithValueExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 2 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_load_v2r(reg, val), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            uint64_t reg = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t val = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));

            uint32_t vh32 = static_cast<uint32_t>(val >> 32);
            uint32_t vl32 = static_cast<uint32_t>(val & 0xffffffffull);

            //gen Code Type 0x4: Load Register with Static Value
            //400R0000 VVVVVVVV VVVVVVVV
            //R: Register to use.
            //V : Value to load.
            auto&& ss = g_DmntData.ss;
            ss << "400";
            ss << std::setfill('0') << std::setw(1) << std::hex << reg;
            ss << "0000";
            ss << " ";
            ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << vh32;
            ss << " ";
            ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << vl32;
            ss << std::endl;
        }
    };
    class DmntLoadRegWithMemoryExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntLoadRegWithMemoryExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 3 && argInfos.size() != 4) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64
                || (argInfos.size() == 4 && (argInfos[3].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[3].Type > Brace::BRACE_DATA_TYPE_UINT64))) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_load_m2r(mem_width[, mem_region], reg, offset), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            uint64_t v1 = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t v2 = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            uint64_t v3 = static_cast<uint64_t>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));
            uint64_t v4 = 0;

            int mem_width;
            int mem_region;
            int reg;
            uint64_t offset;
            bool fixed;
            if (argInfos.size() == 4) {
                auto&& argInfo4 = argInfos[3];
                v4 = static_cast<uint64_t>(Brace::VarGetI64(argInfo4.IsGlobal ? gvars : lvars, argInfo4.Type, argInfo4.VarIndex));

                mem_width = static_cast<int>(v1);
                mem_region = static_cast<int>(v2);
                reg = static_cast<int>(v3);
                offset = v4;
                fixed = true;
            }
            else {
                mem_width = static_cast<int>(v1);
                mem_region = 0;
                reg = static_cast<int>(v2);
                offset = v3;
                fixed = false;
            }

            uint32_t h32 = static_cast<uint32_t>(offset >> 32);
            uint32_t l32 = static_cast<uint32_t>(offset & 0xffffffffull);

            //gen Code Type 0x5: Load Register with Memory Value
            //5TMR00AA AAAAAAAA
            //5T0R10AA AAAAAAAA
            //oad From Fixed Address Encoding
            //5TMR00AA AAAAAAAA
            //
            //T : Width of memory read(1, 2, 4, or 8 bytes).
            //M : Memory region to write to(0 = Main NSO, 1 = Heap, 2 = Alias, 3 = Aslr).
            //R : Register to load value into.
            //A : Immediate offset to use from memory region base.
            //Load from Register Address Encoding
            //
            //5T0R10AA AAAAAAAA
            //
            //T : Width of memory read(1, 2, 4, or 8 bytes).
            //R : Register to load value into. (This register is also used as the base memory address).
            //A : Immediate offset to use from register R.
            auto&& ss = g_DmntData.ss;
            ss << "5";
            ss << std::setfill('0') << std::setw(1) << std::hex << mem_width;
            ss << std::setfill('0') << std::setw(1) << std::hex << mem_region;
            ss << std::setfill('0') << std::setw(1) << std::hex << reg;
            if (fixed)
                ss << "00";
            else
                ss << "10";
            ss << std::setfill('0') << std::setw(2) << std::hex << h32;
            ss << " ";
            ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << l32;
            ss << std::endl;
        }
    };
    class DmntStoreValueToMemoryExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntStoreValueToMemoryExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 4 && argInfos.size() != 5) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[3].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[3].Type > Brace::BRACE_DATA_TYPE_UINT64
                || (argInfos.size() == 5 && (argInfos[4].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[4].Type > Brace::BRACE_DATA_TYPE_UINT64))) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_store_v2m(mem_width, mem_reg, reg_inc_1or0, val[, offset_reg]), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            auto&& argInfo4 = argInfos[3];
            uint64_t mem_width = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t mem_reg = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            uint64_t reg_inc_1or0 = static_cast<uint64_t>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));
            uint64_t val = static_cast<uint64_t>(Brace::VarGetI64(argInfo4.IsGlobal ? gvars : lvars, argInfo4.Type, argInfo4.VarIndex));
            uint64_t offset_reg = 0;
            int use_offset = 0;
            if (argInfos.size() == 5) {
                auto&& argInfo5 = argInfos[4];
                offset_reg = static_cast<uint64_t>(Brace::VarGetI64(argInfo5.IsGlobal ? gvars : lvars, argInfo5.Type, argInfo5.VarIndex));
                use_offset = 1;
            }

            uint32_t vh32 = static_cast<uint32_t>(val >> 32);
            uint32_t vl32 = static_cast<uint32_t>(val & 0xffffffffull);

            //gen Code Type 0x6: Store Static Value to Register Memory Address
            //6T0RIor0 VVVVVVVV VVVVVVVV
            //T: Width of memory write(1, 2, 4, or 8 bytes).
            //R : Register used as base memory address.
            //I : Increment register flag(0 = do not increment R, 1 = increment R by T).
            //o : Offset register enable flag(0 = do not add r to address, 1 = add r to address).
            //r : Register used as offset when o is 1.
            //V : Value to write to memory.
            auto&& ss = g_DmntData.ss;
            ss << "6";
            ss << std::setfill('0') << std::setw(1) << std::hex << mem_width;
            ss << "0";
            ss << std::setfill('0') << std::setw(1) << std::hex << mem_reg;
            ss << std::setfill('0') << std::setw(1) << std::hex << reg_inc_1or0;
            ss << std::setfill('0') << std::setw(1) << std::hex << use_offset;
            ss << std::setfill('0') << std::setw(1) << std::hex << offset_reg;
            ss << "0";
            ss << " ";
            ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << vh32;
            ss << " ";
            ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << vl32;
            ss << std::endl;
        }
    };
    class DmntLegacyArithExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntLegacyArithExp(Brace::BraceScript& interpreter, const std::string& op) :Brace::SimpleBraceApiBase(interpreter), m_Operator(op)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 3 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_legacy_xxx(mem_width, reg, val), all type is integer, xxx:add|sub|mul|lshift|rshift," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            uint64_t mem_width = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t reg = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            uint64_t val = static_cast<uint64_t>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));

            [[maybe_unused]]uint32_t vh32 = static_cast<uint32_t>(val >> 32);
            uint32_t vl32 = static_cast<uint32_t>(val & 0xffffffffull);

            int op = 0;
            if (m_Operator == "+")
                op = 0;
            if (m_Operator == "-")
                op = 1;
            if (m_Operator == "*")
                op = 2;
            if (m_Operator == "<<")
                op = 3;
            if (m_Operator == ">>")
                op = 4;

            //gen Code Type 0x7: Legacy Arithmetic
            //7T0RC000 VVVVVVVV
            //T: Width of arithmetic operation(1, 2, 4, or 8 bytes).
            //R : Register to apply arithmetic to.
            //C : Arithmetic operation to apply, see below.
            //V : Value to use for arithmetic operation.
            //Arithmetic Types
            //0 : Addition
            //1 : Subtraction
            //2 : Multiplication
            //3 : Left Shift
            //4 : Right Shift
            auto&& ss = g_DmntData.ss;
            ss << "7";
            ss << std::setfill('0') << std::setw(1) << std::hex << mem_width;
            ss << "0";
            ss << std::setfill('0') << std::setw(1) << std::hex << reg;
            ss << std::setfill('0') << std::setw(1) << std::hex << op;
            ss << "000";
            ss << " ";
            ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << vl32;
            ss << std::endl;
        }
    private:
        std::string m_Operator;
    };
    class DmntKeyPressExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntKeyPressExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            //dmnt_keypress(key1,key2,...);
            for (auto&& argInfo : argInfos) {
                if (argInfo.Type < Brace::BRACE_DATA_TYPE_INT8 || argInfo.Type > Brace::BRACE_DATA_TYPE_UINT64) {
                    //error
                    std::stringstream ss;
                    ss << "expected dmnt_keypress(key1,key2,...); all type is integer, key can get by dmnt_key(const)," << data.GetId() << " line " << data.GetLine();
                    LogError(ss.str());
                    return false;
                }
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            uint32_t mask = 0;
            for (auto&& argInfo : argInfos) {
                mask |= static_cast<uint32_t>(Brace::VarGetI64(argInfo.IsGlobal ? gvars : lvars, argInfo.Type, argInfo.VarIndex));
            }

            //gen Code Type 0x8: Begin Keypress Conditional Block
            //8kkkkkkk
            //k: Keypad mask to check against, see below.
            //Note that for multiple button combinations, the bitmasks should be ORd together.
            //
            //Keypad Values
            //Note : This is the direct output of hidKeysDown().
            //
            //0000001 : A
            //0000002 : B
            //0000004 : X
            //0000008 : Y
            //0000010 : Left Stick Pressed
            //0000020 : Right Stick Pressed
            //0000040 : L
            //0000080 : R
            //0000100 : ZL
            //0000200 : ZR
            //0000400 : Plus
            //0000800 : Minus
            //0001000 : Left
            //0002000 : Up
            //0004000 : Right
            //0008000 : Down
            //0010000 : Left Stick Left
            //0020000 : Left Stick Up
            //0040000 : Left Stick Right
            //0080000 : Left Stick Down
            //0100000 : Right Stick Left
            //0200000 : Right Stick Up
            //0400000 : Right Stick Right
            //0800000 : Right Stick Down
            //1000000 : SL
            //2000000 : SR
            auto&& ss = g_DmntData.ss;
            ss << "8";
            ss << std::setfill('0') << std::setw(7) << std::hex << mask;
        }
    };
    class DmntArithExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntArithExp(Brace::BraceScript& interpreter, const std::string& op) :Brace::SimpleBraceApiBase(interpreter), m_Operator(op)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 4 && argInfos.size() != 5) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[3].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[3].Type > Brace::BRACE_DATA_TYPE_UINT64
                || (argInfos.size()==5 && (argInfos[4].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[4].Type > Brace::BRACE_DATA_TYPE_UINT64))) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_xxx(mem_width, result_reg, lhs_reg, rhs[, rhs_is_val_1or0]), all type is integer, xxx:add|sub|mul|lshift|rshift|and|or|not|xor|mov," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            auto&& argInfo4 = argInfos[3];
            uint64_t mem_width = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t reg = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            uint64_t lhs_reg = static_cast<uint64_t>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));
            uint64_t rhs = static_cast<uint64_t>(Brace::VarGetI64(argInfo4.IsGlobal ? gvars : lvars, argInfo4.Type, argInfo4.VarIndex));
            int rhs_is_val_1or0 = 0;
            if (argInfos.size() == 5) {
                auto&& argInfo5 = argInfos[4];
                rhs_is_val_1or0 = static_cast<int>(Brace::VarGetI64(argInfo5.IsGlobal ? gvars : lvars, argInfo5.Type, argInfo5.VarIndex));
            }

            uint32_t vh32 = static_cast<uint32_t>(rhs >> 32);
            uint32_t vl32 = static_cast<uint32_t>(rhs & 0xffffffffull);

            int op = 0;
            if (m_Operator == "+")
                op = 0;
            if (m_Operator == "-")
                op = 1;
            if (m_Operator == "*")
                op = 2;
            if (m_Operator == "<<")
                op = 3;
            if (m_Operator == ">>")
                op = 4;
            if (m_Operator == "&")
                op = 5;
            if (m_Operator == "|")
                op = 6;
            if (m_Operator == "~")
                op = 7;
            if (m_Operator == "^")
                op = 8;
            if (m_Operator == "=")
                op = 9;

            //gen Code Type 0x9: Perform Arithmetic
            //9TCRS0s0
            //9TCRS100 VVVVVVVV (VVVVVVVV)
            //Register Arithmetic Encoding
            //9TCRS0s0
            //
            //T : Width of arithmetic operation(1, 2, 4, or 8 bytes).
            //C : Arithmetic operation to apply, see below.
            //R : Register to store result in.
            //S : Register to use as left - hand operand.
            //s : Register to use as right - hand operand.
            //Immediate Value Arithmetic Encoding
            //9TCRS100 VVVVVVVV(VVVVVVVV)
            //
            //T : Width of arithmetic operation(1, 2, 4, or 8 bytes).
            //C : Arithmetic operation to apply, see below.
            //R : Register to store result in.
            //S : Register to use as left - hand operand.
            //V : Value to use as right - hand operand.
            //Arithmetic Types
            //0 : Addition
            //1 : Subtraction
            //2 : Multiplication
            //3 : Left Shift
            //4 : Right Shift
            //5 : Logical And
            //6 : Logical Or
            //7 : Logical Not(discards right - hand operand)
            //8 : Logical Xor
            //9 : None / Move(discards right - hand operand)
            auto&& ss = g_DmntData.ss;
            ss << "9";
            ss << std::setfill('0') << std::setw(1) << std::hex << mem_width;
            ss << std::setfill('0') << std::setw(1) << std::hex << op;
            ss << std::setfill('0') << std::setw(1) << std::hex << reg;
            ss << std::setfill('0') << std::setw(1) << std::hex << lhs_reg;
            if (rhs_is_val_1or0 == 0) {
                ss << "0";
                ss << std::setfill('0') << std::setw(1) << std::hex << rhs;
                ss << "0";
            }
            else {
                ss << "100";
                ss << " ";
                if (mem_width == 8) {
                    ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << vh32;
                    ss << " ";
                }
                ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << vl32;
            }
            ss << std::endl;
        }
    private:
        std::string m_Operator;
    };
    class DmntStoreRegToMemoryExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntStoreRegToMemoryExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 4 && argInfos.size() != 6 && argInfos.size() != 7) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[3].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[3].Type > Brace::BRACE_DATA_TYPE_UINT64
                || (argInfos.size() > 5 && (argInfos[4].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[4].Type > Brace::BRACE_DATA_TYPE_UINT64))
                || (argInfos.size() >= 6 && (argInfos[5].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[5].Type > Brace::BRACE_DATA_TYPE_UINT64))
                || (argInfos.size() == 7 && (argInfos[6].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[6].Type > Brace::BRACE_DATA_TYPE_UINT64))) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_store_r2m(mem_width, src_reg, mem_reg, reg_inc_1or0,[ offset_type, offset_or_reg_or_region[, offset]]), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            auto&& argInfo4 = argInfos[3];
            uint64_t mem_width = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            uint64_t src_reg = static_cast<uint64_t>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            uint64_t mem_reg = static_cast<uint64_t>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));
            uint64_t reg_inc_1or0 = static_cast<uint64_t>(Brace::VarGetI64(argInfo4.IsGlobal ? gvars : lvars, argInfo4.Type, argInfo4.VarIndex));
            int offset_type = 0;
            int reg_or_region = 0;
            uint64_t offset = 0;
            if (argInfos.size() >= 6) {
                auto&& argInfo5 = argInfos[4];
                auto&& argInfo6 = argInfos[5];
                offset_type = static_cast<int>(Brace::VarGetI64(argInfo5.IsGlobal ? gvars : lvars, argInfo5.Type, argInfo5.VarIndex));
                if (offset_type == 2) {
                    offset = static_cast<uint64_t>(Brace::VarGetI64(argInfo6.IsGlobal ? gvars : lvars, argInfo6.Type, argInfo6.VarIndex));
                }
                else {
                    reg_or_region = static_cast<int>(Brace::VarGetI64(argInfo6.IsGlobal ? gvars : lvars, argInfo6.Type, argInfo6.VarIndex));
                }
            }
            if (argInfos.size() >= 7) {
                auto&& argInfo7 = argInfos[6];
                offset = static_cast<uint64_t>(Brace::VarGetI64(argInfo7.IsGlobal ? gvars : lvars, argInfo7.Type, argInfo7.VarIndex));
            }

            uint32_t vh32 = static_cast<uint32_t>(offset >> 32);
            uint32_t vl32 = static_cast<uint32_t>(offset & 0xffffffffull);

            //gen Code Type 0xA: Store Register to Memory Address
            //ATSRIOxa (aaaaaaaa)
            //T: Width of memory write(1, 2, 4, or 8 bytes).
            //S : Register to write to memory.
            //R : Register to use as base address.
            //I : Increment register flag(0 = do not increment R, 1 = increment R by T).
            //O : Offset type, see below.
            //x : Register used as offset when O is 1, Memory type when O is 3, 4 or 5.
            //a : Value used as offset when O is 2, 4 or 5.
            //Offset Types
            //0 : No Offset
            //1 : Use Offset Register
            //2 : Use Fixed Offset
            //3 : Memory Region + Base Register
            //4 : Memory Region + Relative Address(ignore address register)
            //5 : Memory Region + Relative Address + Offset Register
            auto&& ss = g_DmntData.ss;
            ss << "A";
            ss << std::setfill('0') << std::setw(1) << std::hex << mem_width;
            ss << std::setfill('0') << std::setw(1) << std::hex << src_reg;
            ss << std::setfill('0') << std::setw(1) << std::hex << mem_reg;
            ss << std::setfill('0') << std::setw(1) << std::hex << reg_inc_1or0;
            ss << std::setfill('0') << std::setw(1) << std::hex << offset_type;
            ss << std::setfill('0') << std::setw(1) << std::hex << reg_or_region;
            ss << std::setfill('0') << std::setw(1) << std::hex << vh32;
            ss << " ";
            ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << vl32;
            ss << std::endl;
        }
    };
    class DmntRegCondExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntRegCondExp(Brace::BraceScript& interpreter, const std::string& op) :Brace::SimpleBraceApiBase(interpreter), m_Operator(op)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 4 && argInfos.size() != 5) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[3].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[3].Type > Brace::BRACE_DATA_TYPE_UINT64
                || (argInfos.size() == 5 && (argInfos[4].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[4].Type > Brace::BRACE_DATA_TYPE_UINT64))) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_reg_xxx(mem_width, src_reg, opd_type, val1[, val2]), all type is integer, xxx:gt|ge|lt|le|eq|ne," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            auto&& argInfo4 = argInfos[3];
            uint64_t mem_width = static_cast<uint64_t>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            int src_reg = static_cast<int>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            int opd_type = static_cast<int>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));
            uint64_t val1 = static_cast<uint64_t>(Brace::VarGetI64(argInfo4.IsGlobal ? gvars : lvars, argInfo4.Type, argInfo4.VarIndex));
            uint64_t val2 = 0;
            if (argInfos.size() == 5) {
                auto&& argInfo5 = argInfos[4];
                val2 = static_cast<uint64_t>(Brace::VarGetI64(argInfo5.IsGlobal ? gvars : lvars, argInfo5.Type, argInfo5.VarIndex));
            }

            uint32_t v1h32 = static_cast<uint32_t>(val1 >> 32);
            uint32_t v1l32 = static_cast<uint32_t>(val1 & 0xffffffffull);
            uint32_t v2h32 = static_cast<uint32_t>(val2 >> 32);
            uint32_t v2l32 = static_cast<uint32_t>(val2 & 0xffffffffull);

            int op = 0;
            if (m_Operator == ">")
                op = 1;
            if (m_Operator == ">=")
                op = 2;
            if (m_Operator == "<")
                op = 3;
            if (m_Operator == "<=")
                op = 4;
            if (m_Operator == "==")
                op = 5;
            if (m_Operator == "!=")
                op = 6;

            //gen Code Type 0xC0: Begin Register Conditional Block
            //C0TcSX##
            //C0TcS0Ma aaaaaaaa
            //C0TcS1Mr
            //C0TcS2Ra aaaaaaaa
            //C0TcS3Rr
            //C0TcS400 VVVVVVVV(VVVVVVVV)
            //C0TcS5X0
            //T: Width of memory write(1, 2, 4, or 8 bytes).
            //c : Condition to use, see below.
            //S : Source Register.
            //X : Operand Type, see below.
            //M : Memory Type(operand types 0 and 1).
            //R : Address Register(operand types 2 and 3).
            //a : Relative Address(operand types 0 and 2).
            //r : Offset Register(operand types 1 and 3).
            //X : Other Register(operand type 5).
            //V : Value to compare to(operand type 4).
            //Operand Type
            //0 : Memory Base + Relative Offset
            //1 : Memory Base + Offset Register
            //2 : Register + Relative Offset
            //3 : Register + Offset Register
            //4 : Static Value
            //5 : Other Register
            //Conditions
            //1 : >
            //2: >=
            //3 : <
            //4 : <=
            //5 : ==
            //6 : !=
            auto&& ss = g_DmntData.ss;
            ss << "C0";
            ss << std::setfill('0') << std::setw(1) << std::hex << mem_width;
            ss << std::setfill('0') << std::setw(1) << std::hex << op;
            ss << std::setfill('0') << std::setw(1) << std::hex << src_reg;
            ss << std::setfill('0') << std::setw(1) << std::hex << opd_type;
            switch (opd_type) {
            case 0:
                ss << std::setfill('0') << std::setw(1) << std::hex << val1;
                ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << v2h32;
                ss << " ";
                ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << v2l32;
                break;
            case 1:
                ss << std::setfill('0') << std::setw(1) << std::hex << val1;
                ss << std::setfill('0') << std::setw(1) << std::hex << val2;
                break;
            case 2:
                ss << std::setfill('0') << std::setw(1) << std::hex << val1;
                ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << v2h32;
                ss << " ";
                ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << v2l32;
                break;
            case 3:
                ss << std::setfill('0') << std::setw(1) << std::hex << val1;
                ss << std::setfill('0') << std::setw(1) << std::hex << val2;
                break;
            case 4:
                ss << "00";
                ss << " ";
                if (mem_width == 8) {
                    ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << v1h32;
                    ss << " ";
                }
                ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << v1l32;
                break;
            case 5:
                ss << std::setfill('0') << std::setw(1) << std::hex << val1;
                ss << "0";
                break;
            }
            ss << std::endl;
        }
    private:
        std::string m_Operator;
    };

    class DmntRegSaveRestoreExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntRegSaveRestoreExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 3 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_reg_sr(dest_reg, src_reg, opd_type), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            int dest_reg = static_cast<int>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            int src_reg = static_cast<int>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            int opd_type = static_cast<int>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));

            //gen Code Type 0xC1: Save or Restore Register
            //C10D0Sx0
            //D: Destination index.
            //S : Source index.
            //x : Operand Type, see below.
            //Operand Type
            //0 : Restore register
            //1 : Save register
            //2 : Clear saved value
            //3 : Clear register
            auto&& ss = g_DmntData.ss;
            ss << "C10";
            ss << std::setfill('0') << std::setw(1) << std::hex << dest_reg;
            ss << "0";
            ss << std::setfill('0') << std::setw(1) << std::hex << src_reg;
            ss << std::setfill('0') << std::setw(1) << std::hex << opd_type;
            ss << "0";
            ss << std::endl;
        }
    };
    class DmntRegSaveRestoreWithMaskExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntRegSaveRestoreWithMaskExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 2 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_reg_sr_mask(opd_type, mask), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            int opd_type = static_cast<int>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            int mask = static_cast<int>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));

            //gen Code Type 0xC2: Save or Restore Register with Mask
            //C2x0XXXX
            //x: Operand Type, see below.
            //X : 16 - bit bitmask, bit i == save or restore register i.
            //Operand Type
            //0 : Restore register
            //1 : Save register
            //2 : Clear saved value
            //3 : Clear register
            auto&& ss = g_DmntData.ss;
            ss << "C2";
            ss << std::setfill('0') << std::setw(1) << std::hex << opd_type;
            ss << "0";
            ss << std::setfill('0') << std::setw(4) << std::hex << mask;
            ss << std::endl;
        }
    };
    class DmntRegReadWriteExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntRegReadWriteExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 2 || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_reg_rw(static_reg_index, reg), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            int static_reg_index = static_cast<int>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            int reg = static_cast<int>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));

            //gen Code Type 0xC3: Read or Write Static Register
            //C3000XXx
            //XX: Static register index, 0x00 to 0x7F for reading or 0x80 to 0xFF for writing.
            //x: Register index.
            auto&& ss = g_DmntData.ss;
            ss << "C3000";
            ss << std::setfill('0') << std::setw(2) << std::hex << static_reg_index;
            ss << std::setfill('0') << std::setw(1) << std::hex << reg;
            ss << std::endl;
        }
    };
    class DmntPauseExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntPauseExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 0) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_pause()," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            //gen Code Type 0xFF0: Pause Process
            //FF0?????
            auto&& ss = g_DmntData.ss;
            ss << "FF000000";
            ss << std::endl;
        }
    };
    class DmntResumeExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntResumeExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if (argInfos.size() != 0) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_resume()," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            //gen Code Type 0xFF1: Resume Process
            //FF1?????
            auto&& ss = g_DmntData.ss;
            ss << "FF100000";
            ss << std::endl;
        }
    };
    class DmntDebugLogExp final : public Brace::SimpleBraceApiBase
    {
    public:
        DmntDebugLogExp(Brace::BraceScript& interpreter) :Brace::SimpleBraceApiBase(interpreter)
        {
        }
    protected:
        virtual bool TypeInference(const Brace::FuncInfo& func, const DslData::FunctionData& data, const std::vector<Brace::OperandLoadtimeInfo>& argInfos, Brace::OperandLoadtimeInfo& resultInfo) override
        {
            if ((argInfos.size() != 4 && argInfos.size() != 5) || argInfos[0].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[0].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[1].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[1].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[2].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[2].Type > Brace::BRACE_DATA_TYPE_UINT64
                || argInfos[3].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[3].Type > Brace::BRACE_DATA_TYPE_UINT64
                || (argInfos.size() == 5 && (argInfos[4].Type < Brace::BRACE_DATA_TYPE_INT8 || argInfos[4].Type > Brace::BRACE_DATA_TYPE_UINT64))) {
                //error
                std::stringstream ss;
                ss << "expected dmnt_debug(mem_width, log_id, opd_type, val1[, val2]), all type is integer," << data.GetId() << " line " << data.GetLine();
                LogError(ss.str());
                return false;
            }
            return true;
        }
        virtual void Execute(Brace::VariableInfo& gvars, Brace::VariableInfo& lvars, const std::vector<Brace::OperandRuntimeInfo>& argInfos, const Brace::OperandRuntimeInfo& resultInfo)const override
        {
            auto&& argInfo1 = argInfos[0];
            auto&& argInfo2 = argInfos[1];
            auto&& argInfo3 = argInfos[2];
            auto&& argInfo4 = argInfos[3];
            int mem_width = static_cast<int>(Brace::VarGetI64(argInfo1.IsGlobal ? gvars : lvars, argInfo1.Type, argInfo1.VarIndex));
            int log_id = static_cast<int>(Brace::VarGetI64(argInfo2.IsGlobal ? gvars : lvars, argInfo2.Type, argInfo2.VarIndex));
            int opd_type = static_cast<int>(Brace::VarGetI64(argInfo3.IsGlobal ? gvars : lvars, argInfo3.Type, argInfo3.VarIndex));
            uint64_t val1 = static_cast<uint64_t>(Brace::VarGetI64(argInfo4.IsGlobal ? gvars : lvars, argInfo4.Type, argInfo4.VarIndex));
            uint64_t val2 = 0;
            if (argInfos.size() == 5) {
                auto&& argInfo5 = argInfos[4];
                val2 = static_cast<uint64_t>(Brace::VarGetI64(argInfo5.IsGlobal ? gvars : lvars, argInfo5.Type, argInfo5.VarIndex));
            }

            uint32_t vh32 = static_cast<uint32_t>(val2 >> 32);
            uint32_t vl32 = static_cast<uint32_t>(val2 & 0xffffffffull);

            //gen Code Type 0xFFF: Debug Log
            //FFFTIX##
            //FFFTI0Ma aaaaaaaa
            //FFFTI1Mr
            //FFFTI2Ra aaaaaaaa
            //FFFTI3Rr
            //FFFTI4X0
            //T : Width of memory write(1, 2, 4, or 8 bytes).
            //I : Log id.
            //X : Operand Type, see below.
            //M : Memory Type(operand types 0 and 1).
            //R : Address Register(operand types 2 and 3).
            //a : Relative Address(operand types 0 and 2).
            //r : Offset Register(operand types 1 and 3).
            //X : Value Register(operand type 4).
            //Operand Type
            //0 : Memory Base + Relative Offset
            //1 : Memory Base + Offset Register
            //2 : Register + Relative Offset
            //3 : Register + Offset Register
            //4 : Register Value
            auto&& ss = g_DmntData.ss;
            ss << "FFF";
            ss << std::setfill('0') << std::setw(1) << std::hex << mem_width;
            ss << std::setfill('0') << std::setw(1) << std::hex << log_id;
            ss << std::setfill('0') << std::setw(1) << std::hex << opd_type;
            switch (opd_type) {
            case 0:
                ss << std::setfill('0') << std::setw(1) << std::hex << val1;
                ss << std::setfill('0') << std::setw(1) << std::hex << vh32;
                ss << " ";
                ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << vl32;
                break;
            case 1:
                ss << std::setfill('0') << std::setw(1) << std::hex << val1;
                ss << std::setfill('0') << std::setw(1) << std::hex << val2;
                break;
            case 2:
                ss << std::setfill('0') << std::setw(1) << std::hex << val1;
                ss << std::setfill('0') << std::setw(1) << std::hex << vh32;
                ss << " ";
                ss << std::setfill('0') << std::setw(sizeof(uint32_t) * 2) << std::hex << vl32;
                break;
            case 3:
                ss << std::setfill('0') << std::setw(1) << std::hex << val1;
                ss << std::setfill('0') << std::setw(1) << std::hex << val2;
                break;
            case 4:
                ss << std::setfill('0') << std::setw(1) << std::hex << val1;
                ss << "0";
                break;
            }
            ss << std::endl;
        }
    };

    inline void BraceScriptManager::InitGlobalBraceObjectInfo()
    {
        //add predefined map, obj table id <-> obj category
        g_ObjectInfoMgr.AddBraceObjectInfo(Brace::PREDEFINED_BRACE_OBJECT_TYPE_ANY, BRACE_OBJECT_CATEGORY_INTERNAL_FIXED_OBJECT, "any");  //fake obj info for any
        g_ObjectInfoMgr.AddBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_STRING, BRACE_OBJECT_CATEGORY_INTERNAL_FIXED_OBJECT, "string");  //fake obj info for string
        g_ObjectInfoMgr.AddBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO, BRACE_OBJECT_CATEGORY_SPECIAL, "MemoryModifyInfo");

        g_ObjectInfoMgr.AddBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY, BRACE_OBJECT_CATEGORY_INTERNAL_FIXED_OBJECT, "array<:bool:>");
        g_ObjectInfoMgr.SetBraceObjectTypeParams(CUSTOM_BRACE_OBJECT_TYPE_BOOL_ARRAY, Brace::BRACE_DATA_TYPE_BOOL, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);

        g_ObjectInfoMgr.AddBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY, BRACE_OBJECT_CATEGORY_INTERNAL_FIXED_OBJECT, "array<:int64:>");
        g_ObjectInfoMgr.SetBraceObjectTypeParams(CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY, Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);

        g_ObjectInfoMgr.AddBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY, BRACE_OBJECT_CATEGORY_INTERNAL_FIXED_OBJECT, "array<:double:>");
        g_ObjectInfoMgr.SetBraceObjectTypeParams(CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY, Brace::BRACE_DATA_TYPE_DOUBLE, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);

        g_ObjectInfoMgr.AddBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY, BRACE_OBJECT_CATEGORY_INTERNAL_FIXED_OBJECT, "array<:string:>");
        g_ObjectInfoMgr.SetBraceObjectTypeParams(CUSTOM_BRACE_OBJECT_TYPE_STR_ARRAY, Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);

        g_ObjectInfoMgr.AddBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE, BRACE_OBJECT_CATEGORY_INTERNAL_FIXED_OBJECT, "hashtable<:int64,bool:>");
        g_ObjectInfoMgr.SetBraceObjectTypeParams(CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE, Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ, Brace::BRACE_DATA_TYPE_BOOL, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);

        g_ObjectInfoMgr.AddBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, BRACE_OBJECT_CATEGORY_INTERNAL_FIXED_OBJECT, "hashtable<:int64,int64:>");
        g_ObjectInfoMgr.SetBraceObjectTypeParams(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ, Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);

        g_ObjectInfoMgr.AddBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, BRACE_OBJECT_CATEGORY_INTERNAL_FIXED_OBJECT, "hashtable<:int64,double:>");
        g_ObjectInfoMgr.SetBraceObjectTypeParams(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ, Brace::BRACE_DATA_TYPE_DOUBLE, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);

        g_ObjectInfoMgr.AddBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE, BRACE_OBJECT_CATEGORY_INTERNAL_FIXED_OBJECT, "hashtable<:int64,string:>");
        g_ObjectInfoMgr.SetBraceObjectTypeParams(CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE, Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ, Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);

        g_ObjectInfoMgr.AddBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE, BRACE_OBJECT_CATEGORY_INTERNAL_FIXED_OBJECT, "hashtable<:string,bool:>");
        g_ObjectInfoMgr.SetBraceObjectTypeParams(CUSTOM_BRACE_OBJECT_TYPE_STR_BOOL_HASHTABLE, Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ, Brace::BRACE_DATA_TYPE_BOOL, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);

        g_ObjectInfoMgr.AddBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE, BRACE_OBJECT_CATEGORY_INTERNAL_FIXED_OBJECT, "hashtable<:string,int64:>");
        g_ObjectInfoMgr.SetBraceObjectTypeParams(CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE, Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ, Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);

        g_ObjectInfoMgr.AddBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE, BRACE_OBJECT_CATEGORY_INTERNAL_FIXED_OBJECT, "hashtable<:string,double:>");
        g_ObjectInfoMgr.SetBraceObjectTypeParams(CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE, Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ, Brace::BRACE_DATA_TYPE_DOUBLE, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);

        g_ObjectInfoMgr.AddBraceObjectInfo(CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE, BRACE_OBJECT_CATEGORY_INTERNAL_FIXED_OBJECT, "hashtable<:string,string:>");
        g_ObjectInfoMgr.SetBraceObjectTypeParams(CUSTOM_BRACE_OBJECT_TYPE_STR_STR_HASHTABLE, Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ, Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ);

        //----------------
        {
            std::string objArrayKey = "array<:MemoryModifyInfo:>";
            int objTypeId = g_ObjectInfoMgr.GetObjectTypeId(objArrayKey);
            if (Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN == objTypeId) {
                objTypeId = g_ObjectInfoMgr.AddNewObjectTypeId(objArrayKey);
            }
            auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
            if (nullptr == pInfo) {
                pInfo = g_ObjectInfoMgr.AddBraceObjectInfo(objTypeId, BRACE_OBJECT_CATEGORY_OBJ_ARRAY, std::move(objArrayKey));
                g_ObjectInfoMgr.SetBraceObjectTypeParams(objTypeId, Brace::BRACE_DATA_TYPE_OBJECT, CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO);
            }
        }
        //----------------
        {
            std::string anyArrayKey = "array<:any:>";
            int objTypeId = g_ObjectInfoMgr.GetObjectTypeId(anyArrayKey);
            if (Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN == objTypeId) {
                objTypeId = g_ObjectInfoMgr.AddNewObjectTypeId(anyArrayKey);
            }
            auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
            if (nullptr == pInfo) {
                pInfo = g_ObjectInfoMgr.AddBraceObjectInfo(objTypeId, BRACE_OBJECT_CATEGORY_OBJ_ARRAY, std::move(anyArrayKey));
                g_ObjectInfoMgr.SetBraceObjectTypeParams(objTypeId, Brace::BRACE_DATA_TYPE_OBJECT, Brace::PREDEFINED_BRACE_OBJECT_TYPE_ANY);
            }
        }
        //----------------
        {
            std::string strObjHashKey = "hashtable<:string,MemoryModifyInfo:>";
            int objTypeId = g_ObjectInfoMgr.GetObjectTypeId(strObjHashKey);
            if (Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN == objTypeId) {
                objTypeId = g_ObjectInfoMgr.AddNewObjectTypeId(strObjHashKey);
            }
            auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
            if (nullptr == pInfo) {
                pInfo = g_ObjectInfoMgr.AddBraceObjectInfo(objTypeId, BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE, std::move(strObjHashKey));
                g_ObjectInfoMgr.SetBraceObjectTypeParams(objTypeId, Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ, Brace::BRACE_DATA_TYPE_OBJECT, CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO);
            }
        }
        //----------------
        {
            std::string intObjHashKey = "hashtable<:int64,MemoryModifyInfo:>";
            int objTypeId = g_ObjectInfoMgr.GetObjectTypeId(intObjHashKey);
            if (Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN == objTypeId) {
                objTypeId = g_ObjectInfoMgr.AddNewObjectTypeId(intObjHashKey);
                g_ObjectInfoMgr.AddBraceObjectAlias(objTypeId, "hashtable<:int8,MemoryModifyInfo:>");
                g_ObjectInfoMgr.AddBraceObjectAlias(objTypeId, "hashtable<:int16,MemoryModifyInfo:>");
                g_ObjectInfoMgr.AddBraceObjectAlias(objTypeId, "hashtable<:int32,MemoryModifyInfo:>");
                g_ObjectInfoMgr.AddBraceObjectAlias(objTypeId, "hashtable<:uint8,MemoryModifyInfo:>");
                g_ObjectInfoMgr.AddBraceObjectAlias(objTypeId, "hashtable<:uint16,MemoryModifyInfo:>");
                g_ObjectInfoMgr.AddBraceObjectAlias(objTypeId, "hashtable<:uint32,MemoryModifyInfo:>");
                g_ObjectInfoMgr.AddBraceObjectAlias(objTypeId, "hashtable<:uint64,MemoryModifyInfo:>");
            }
            auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
            if (nullptr == pInfo) {
                pInfo = g_ObjectInfoMgr.AddBraceObjectInfo(objTypeId, BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE, std::move(intObjHashKey));
                g_ObjectInfoMgr.SetBraceObjectTypeParams(objTypeId, Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ, Brace::BRACE_DATA_TYPE_OBJECT, CUSTOM_BRACE_OBJECT_TYPE_CPP_MEM_MODIFY_INFO);
            }
        }
        //----------------
        {
            std::string strAnyHashKey = "hashtable<:string,any:>";
            int objTypeId = g_ObjectInfoMgr.GetObjectTypeId(strAnyHashKey);
            if (Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN == objTypeId) {
                objTypeId = g_ObjectInfoMgr.AddNewObjectTypeId(strAnyHashKey);
            }
            auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
            if (nullptr == pInfo) {
                pInfo = g_ObjectInfoMgr.AddBraceObjectInfo(objTypeId, BRACE_OBJECT_CATEGORY_STR_OBJ_HASHTABLE, std::move(strAnyHashKey));
                g_ObjectInfoMgr.SetBraceObjectTypeParams(objTypeId, Brace::BRACE_DATA_TYPE_STRING, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ, Brace::BRACE_DATA_TYPE_OBJECT, Brace::PREDEFINED_BRACE_OBJECT_TYPE_ANY);
            }
        }
        //----------------
        {
            std::string intAnyHashKey = "hashtable<:int64,any:>";
            int objTypeId = g_ObjectInfoMgr.GetObjectTypeId(intAnyHashKey);
            if (Brace::PREDEFINED_BRACE_OBJECT_TYPE_UNKNOWN == objTypeId) {
                objTypeId = g_ObjectInfoMgr.AddNewObjectTypeId(intAnyHashKey);
                g_ObjectInfoMgr.AddBraceObjectAlias(objTypeId, "hashtable<:int8,any:>");
                g_ObjectInfoMgr.AddBraceObjectAlias(objTypeId, "hashtable<:int16,any:>");
                g_ObjectInfoMgr.AddBraceObjectAlias(objTypeId, "hashtable<:int32,any:>");
                g_ObjectInfoMgr.AddBraceObjectAlias(objTypeId, "hashtable<:uint8,any:>");
                g_ObjectInfoMgr.AddBraceObjectAlias(objTypeId, "hashtable<:uint16,any:>");
                g_ObjectInfoMgr.AddBraceObjectAlias(objTypeId, "hashtable<:uint32,any:>");
                g_ObjectInfoMgr.AddBraceObjectAlias(objTypeId, "hashtable<:uint64,any:>");
            }
            auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
            if (nullptr == pInfo) {
                pInfo = g_ObjectInfoMgr.AddBraceObjectInfo(objTypeId, BRACE_OBJECT_CATEGORY_INT_OBJ_HASHTABLE, std::move(intAnyHashKey));
                g_ObjectInfoMgr.SetBraceObjectTypeParams(objTypeId, Brace::BRACE_DATA_TYPE_INT64, Brace::PREDEFINED_BRACE_OBJECT_TYPE_NOTOBJ, Brace::BRACE_DATA_TYPE_OBJECT, Brace::PREDEFINED_BRACE_OBJECT_TYPE_ANY);
            }
        }
        //----------------
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY, "array<:int8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY, "array<:int16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY, "array<:int32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY, "array<:uint8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY, "array<:uint16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY, "array<:uint32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_ARRAY, "array<:uint64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_FLOAT_ARRAY, "array<:float:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE, "hashtable<:int8,bool:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int8,int64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, "hashtable<:int8,double:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE, "hashtable<:int8,string:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE, "hashtable<:int16,bool:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int16,int64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, "hashtable<:int16,double:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE, "hashtable<:int16,string:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE, "hashtable<:int32,bool:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int32,int64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, "hashtable<:int32,double:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE, "hashtable<:int32,string:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE, "hashtable<:uint8,bool:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint8,int64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, "hashtable<:uint8,double:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE, "hashtable<:uint8,string:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE, "hashtable<:uint16,bool:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint16,int64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, "hashtable<:uint16,double:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE, "hashtable<:uint16,string:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE, "hashtable<:uint32,bool:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint32,int64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, "hashtable<:uint32,double:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE, "hashtable<:uint32,string:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_BOOL_HASHTABLE, "hashtable<:uint64,bool:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint64,int64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, "hashtable<:uint64,double:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_STR_HASHTABLE, "hashtable<:uint64,string:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE, "hashtable<:string,int8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE, "hashtable<:string,int16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE, "hashtable<:string,int32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE, "hashtable<:string,uint8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE, "hashtable<:string,uint16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE, "hashtable<:string,uint32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_STR_INT_HASHTABLE, "hashtable<:string,uint64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_STR_FLOAT_HASHTABLE, "hashtable<:string,float:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, "hashtable<:int8,float:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, "hashtable<:int16,float:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, "hashtable<:int32,float:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, "hashtable<:int64,float:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, "hashtable<:uint8,float:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, "hashtable<:uint16,float:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, "hashtable<:uint32,float:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_FLOAT_HASHTABLE, "hashtable<:uint64,float:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int8,int8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int16,int8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int32,int8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int64,int8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint8,int8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint16,int8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint32,int8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint64,int8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int8,int16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int16,int16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int32,int16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int64,int16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint8,int16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint16,int16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint32,int16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint64,int16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int8,int32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int16,int32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int32,int32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int64,int32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint8,int32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint16,int32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint32,int32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint64,int32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int8,uint8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int16,uint8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int32,uint8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int64,uint8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint8,uint8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint16,uint8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint32,uint8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint64,uint8:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int8,uint16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int16,uint16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int32,uint16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int64,uint16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint8,uint16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint16,uint16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint32,uint16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint64,uint16:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int8,uint32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int16,uint32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int32,uint32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int64,uint32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint8,uint32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint16,uint32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint32,uint32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint64,uint32:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int8,uint64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int16,uint64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int32,uint64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:int64,uint64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint8,uint64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint16,uint64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint32,uint64:>");
        g_ObjectInfoMgr.AddBraceObjectAlias(CUSTOM_BRACE_OBJECT_TYPE_INT_INT_HASHTABLE, "hashtable<:uint64,uint64:>");
    }
    inline void BraceScriptManager::InitBraceScript(Brace::BraceScript*& pBraceScript, bool isCallback)
    {
        pBraceScript = new Brace::BraceScript();
        if (!isCallback) {
            pBraceScript->OnGetRuntimeStack = std::bind(&BraceScriptManager::GetRuntimeStack, this);
        }
        pBraceScript->OnInfo = [](auto& str) { g_pApiProvider->LogToView(std::string("[Output]: ") + str); };
        pBraceScript->OnWarn = [](auto& str) { g_pApiProvider->LogToView(std::string("[Warn]: ") + str); };
        pBraceScript->OnError = [](auto& str) { g_pApiProvider->LogToView(std::string("[Error]: ") + str); };

        pBraceScript->OnGetObjectTypeId = [](const DslData::ISyntaxComponent& syntax, const Brace::LoadTypeInfoDelegation& doLoadTypeInfo) {
            int objTypeId;
            g_ObjectInfoMgr.TryGetOrAddBraceObjectInfo(syntax, doLoadTypeInfo, objTypeId);
            return objTypeId;
        };
        pBraceScript->OnGetObjectTypeName = [](int objTypeId) {
            auto* pInfo = g_ObjectInfoMgr.GetBraceObjectInfo(objTypeId);
            if (nullptr != pInfo)
                return pInfo->TypeName.c_str();
            return "unknown";
        };
        pBraceScript->OnObjectAssignCheck = [](int destObjTypeId, int srcObjTypeId) {
            if (destObjTypeId == srcObjTypeId)
                return true;
            return false;
        };

        /// This is a specific implementation for yuzu, not a general language feature
        /// missing language features
        pBraceScript->RegisterApi("membercall", "object member call", new Brace::BraceApiFactory<MemberCallExp>());
        pBraceScript->RegisterApi("memberset", "object property set", new Brace::BraceApiFactory<MemberSetExp>());
        pBraceScript->RegisterApi("memberget", "object property get", new Brace::BraceApiFactory<MemberGetExp>());
        pBraceScript->RegisterApi("collectioncall", "collection member call", new Brace::BraceApiFactory<CollectionCallExp>());
        pBraceScript->RegisterApi("collectionset", "collection property set", new Brace::BraceApiFactory<CollectionSetExp>());
        pBraceScript->RegisterApi("collectionget", "collection property get", new Brace::BraceApiFactory<CollectionGetExp>());
        pBraceScript->RegisterApi("linq", "objs.where(args) or objs.orderby(args) or objs.orderbydesc(args) or objs.top(args) or linq(objs,method_str,arg1,arg2,...) linq expression", new Brace::BraceApiFactory<LinqExp>());
        pBraceScript->RegisterApi("select", "select(fields)top(10)from(objlist)where(exp)oderby(exps)groupby(exps)having(exp){statements;}; statement", new Brace::BraceApiFactory<SelectExp>());
        pBraceScript->RegisterApi("lambda", "lambda expression, (args) => {...} or (args)int => {...} or [...](args) => {...} or [...](args)int => {...} lambda expression", new Brace::BraceApiFactory<LambdaExp>());
        pBraceScript->RegisterApi("array", "[v1,v2,...] or array(v1,v2,...) or array<:type:>(v1,v2,...) object", new Brace::BraceApiFactory<ArrayExp>());
        pBraceScript->RegisterApi("hashtable", "{k1=>v1,k2=>v2,...} or {k1:v1,k2:v2,...} or hashtable(k1=>v1,k2=>v2,...) or hashtable(k1:v1,k2:v2,...) or hashtable<:key_type,val_type:>(k1=>v1,k2=>v2,...) or hashtable<:key_type,val_type:>(k1:v1,k2:v2,...) object", new Brace::BraceApiFactory<HashtableExp>());
        pBraceScript->RegisterApi("looplist", "looplist(list)func(args); or looplist(list){...}; statement, iterator is $$", new Brace::BraceApiFactory<LoopListExp>());
        pBraceScript->RegisterApi("cast", "cast(exp,type) api", new Brace::BraceApiFactory<CastExp>());
        pBraceScript->RegisterApi("typetag", "typetag(type) or typetag(exp) api", new Brace::BraceApiFactory<TypeTagExp>());
        pBraceScript->RegisterApi("typeid", "typeid(type) or typeid(exp) api", new Brace::BraceApiFactory<TypeIdExp>());
        pBraceScript->RegisterApi("objtypeid", "objtypeid(type) or objtypeid(exp) api", new Brace::BraceApiFactory<ObjTypeIdExp>());
        pBraceScript->RegisterApi("getobjtypename", "getobjtypename(objtypeid) api", new Brace::BraceApiFactory<GetObjTypeNameExp>());
        pBraceScript->RegisterApi("getobjcategory", "getobjcategory(objtypeid) api", new Brace::BraceApiFactory<GetObjCategoryExp>());
        pBraceScript->RegisterApi("gettypeparamcount", "gettypeparamcount(objtypeid) api", new Brace::BraceApiFactory<GetTypeParamCountExp>());
        pBraceScript->RegisterApi("gettypeparamtype", "gettypeparamtype(objtypeid,index) api", new Brace::BraceApiFactory<GetTypeParamTypeExp>());
        pBraceScript->RegisterApi("gettypeparamobjtypeid", "gettypeparamobjtypeid(objtypeid,index) api", new Brace::BraceApiFactory<GetTypeParamObjTypeIdExp>());
        pBraceScript->RegisterApi("swap", "swap(var1,var2) api", new Brace::BraceApiFactory<SwapExp>());
        pBraceScript->RegisterApi("struct", "struct(name){a:int32;b:int32;...}, define struct", new Brace::BraceApiFactory<StructExp>());
        pBraceScript->RegisterApi("newstruct", "newstruct(struct_type) api", new Brace::BraceApiFactory<NewStructExp>());
        pBraceScript->RegisterApi("reinterpretas", "reinterpret_cast(uint,struct_type) api", new Brace::BraceApiFactory<ReInterpretAsExp>());

        /// register api
        if (isCallback) {
            pBraceScript->RegisterApi("oncallback", "oncallback(msg)args($a:int,$b:int,...){...}; statement", new Brace::BraceApiFactory<MessageHandlerExp>());
        }
        else {
            pBraceScript->RegisterApi("onmessage", "onmessage(msg[,pool_num])args($a:int,$b:int,...){...}; statement", new Brace::BraceApiFactory<MessageHandlerExp>());
            pBraceScript->RegisterApi("clearmessages", "clearmessages() api", new Brace::BraceApiFactory<ClearMessagesExp>());
        }
        pBraceScript->RegisterApi("qcmd", "qcmd(str,...) api", new Brace::BraceApiFactory<QCmdExp>());
        pBraceScript->RegisterApi("cmd", "cmd(str,...) api", new Brace::BraceApiFactory<CmdExp>());
        if (isCallback) {
            pBraceScript->RegisterApi("wait", "wait(ms,...) api", new Brace::BraceApiFactoryWithArgs<WaitExp, bool>(true));
        }
        else {
            pBraceScript->RegisterApi("wait", "wait(ms,...) api", new Brace::BraceApiFactoryWithArgs<WaitExp, bool>(false));
            pBraceScript->RegisterApi("waituntilquit", "waituntilquit() api", new Brace::BraceApiFactory<WaitUntilQuitExp>());
        }
        pBraceScript->RegisterApi("time", "time() api", new Brace::BraceApiFactory<TimeExp>());
        pBraceScript->RegisterApi("int2char", "int2char(val) api", new Brace::BraceApiFactory<Int2CharExp>());
        pBraceScript->RegisterApi("char2int", "char2int(str) api", new Brace::BraceApiFactory<Char2IntExp>());
        pBraceScript->RegisterApi("int2hex", "int2hex(val) api", new Brace::BraceApiFactory<Int2HexExp>());
        pBraceScript->RegisterApi("hex2int", "hex2int(str) api", new Brace::BraceApiFactory<Hex2IntExp>());
        pBraceScript->RegisterApi("int2str", "int2str(val) api", new Brace::BraceApiFactory<Int2StrExp>());
        pBraceScript->RegisterApi("str2int", "str2int(str) api", new Brace::BraceApiFactory<Str2IntExp>());
        pBraceScript->RegisterApi("float2str", "float2str(num[,precise]) api", new Brace::BraceApiFactory<Float2StrExp>());
        pBraceScript->RegisterApi("str2float", "str2float(str) api", new Brace::BraceApiFactory<Str2FloatExp>());
        pBraceScript->RegisterApi("strconcat", "strconcat(str,str,...) api", new Brace::BraceApiFactory<StrConcatExp>());
        pBraceScript->RegisterApi("strcontainsone", "strcontainsone(str,str,...) api", new Brace::BraceApiFactory<StrContainsOneExp>());
        pBraceScript->RegisterApi("strcontainsall", "strcontainsall(str,str,...) api", new Brace::BraceApiFactory<StrContainsAllExp>());
        pBraceScript->RegisterApi("strindexof", "strindexof(str,str[,index]) api", new Brace::BraceApiFactory<StrIndexOfExp>());
        pBraceScript->RegisterApi("strlastindexof", "strlastindexof(str,str[,index]) api", new Brace::BraceApiFactory<StrLastIndexOfExp>());
        pBraceScript->RegisterApi("strlen", "strlen(str) api", new Brace::BraceApiFactory<StrLenExp>());
        pBraceScript->RegisterApi("substr", "substr(str,pos[,count]) api", new Brace::BraceApiFactory<SubStrExp>());
        pBraceScript->RegisterApi("strreplace", "strreplace(str,str,str) api", new Brace::BraceApiFactory<StrReplaceExp>());
        pBraceScript->RegisterApi("strsplit", "strsplit(str,str) api", new Brace::BraceApiFactory<StrSplitExp>());
        pBraceScript->RegisterApi("strjoin", "strjoin(array<:string:>,str) api", new Brace::BraceApiFactory<StrJoinExp>());
        pBraceScript->RegisterApi("csvecho", "csvecho(args) api", new Brace::BraceApiFactory<CsvEchoExp>());
        pBraceScript->RegisterApi("csvconcat", "csvconcat(args) api", new Brace::BraceApiFactory<CsvConcatExp>());
        pBraceScript->RegisterApi("csvdebug", "csvdebug(args) api", new Brace::BraceApiFactory<CsvDebugExp>());

        pBraceScript->RegisterApi("fileexists", "fileexists(file) api", new Brace::BraceApiFactory<FileExistsExp>());
        pBraceScript->RegisterApi("loadfile", "loadfile(file) api", new Brace::BraceApiFactory<LoadFileExp>());
        pBraceScript->RegisterApi("savefile", "savefile(str,file) api", new Brace::BraceApiFactory<SaveFileExp>());
        pBraceScript->RegisterApi("loadfiletoarray", "loadfiletoarray(file[,typetag(arr_type)]) api", new Brace::BraceApiFactory<LoadFileToArrayExp>());
        pBraceScript->RegisterApi("savearraytofile", "savearraytofile(arr,file) api", new Brace::BraceApiFactory<SaveArrayToFileExp>());

        pBraceScript->RegisterApi("savehashtable", "savehashtable(hashtable,file) api", new Brace::BraceApiFactory<SaveHashtableExp>());
        pBraceScript->RegisterApi("loadhashtable", "loadhashtable(file[,typetag(hashtable_type)]) api", new Brace::BraceApiFactory<LoadHashtableExp>());
        pBraceScript->RegisterApi("calcnewitems", "calcnewitems(hashtable1,hashtable2) api", new Brace::BraceApiFactory<CalcNewItemsExp>());
        pBraceScript->RegisterApi("calcsameitems", "calcsameitems(hashtable1,hashtable2) api", new Brace::BraceApiFactory<CalcSameItemsExp>());
        pBraceScript->RegisterApi("calcitemsunion", "calcitemsunion(hashtable1,hashtable2) api", new Brace::BraceApiFactory<CalcItemsUnionExp>());
        pBraceScript->RegisterApi("itemsadd", "itemsadd(hashtable1,hashtable2) api", new Brace::BraceApiFactory<ItemsAddExp>());
        pBraceScript->RegisterApi("itemssub", "itemssub(hashtable1,hashtable2) api", new Brace::BraceApiFactory<ItemsSubExp>());
        pBraceScript->RegisterApi("itemsmul", "itemsmul(hashtable1,hashtable2) api", new Brace::BraceApiFactory<ItemsMulExp>());
        pBraceScript->RegisterApi("itemsdiv", "itemsdiv(hashtable1,hashtable2) api", new Brace::BraceApiFactory<ItemsDivExp>());

        pBraceScript->RegisterApi("arrayadd", "arrayadd(arr1,arr2) api", new Brace::BraceApiFactory<ArrayAddExp>());
        pBraceScript->RegisterApi("arraysub", "arraysub(arr1,arr2) api", new Brace::BraceApiFactory<ArraySubExp>());
        pBraceScript->RegisterApi("arraymul", "arraymul(arr1,arr2) api", new Brace::BraceApiFactory<ArrayMulExp>());
        pBraceScript->RegisterApi("arraydiv", "arraydiv(arr1,arr2) api", new Brace::BraceApiFactory<ArrayDivExp>());

        pBraceScript->RegisterApi("arraymodify", "arraymodify(array,modify_exp) api, iterator is $$", new Brace::BraceApiFactory<ArrayModifyExp>());
        pBraceScript->RegisterApi("hashtablemodify", "hashtablemodify(hashtable,modify_exp) api, iterator is $$k and $$v", new Brace::BraceApiFactory<HashtableModifyExp>());

        pBraceScript->RegisterApi("sqrt", "sqrt(number) api", new Brace::BraceApiFactory<SqrtExp>());
        pBraceScript->RegisterApi("cbrt", "cbrt(number) api", new Brace::BraceApiFactory<CbrtExp>());
        pBraceScript->RegisterApi("pow", "pow(base,exp) api", new Brace::BraceApiFactory<PowExp>());
        pBraceScript->RegisterApi("hypot", "hypot(x,y) or hypot(x,y,z) api", new Brace::BraceApiFactory<HypotExp>());
        pBraceScript->RegisterApi("abs", "abs(number) api", new Brace::BraceApiFactory<AbsExp>());
        pBraceScript->RegisterApi("ceil", "ceil(number) api", new Brace::BraceApiFactory<CeilExp>());
        pBraceScript->RegisterApi("floor", "floor(number) api", new Brace::BraceApiFactory<FloorExp>());
        pBraceScript->RegisterApi("sin", "sin(number) api", new Brace::BraceApiFactory<SinExp>());
        pBraceScript->RegisterApi("cos", "cos(number) api", new Brace::BraceApiFactory<CosExp>());
        pBraceScript->RegisterApi("tan", "tan(number) api", new Brace::BraceApiFactory<TanExp>());
        pBraceScript->RegisterApi("asin", "asin(number) api", new Brace::BraceApiFactory<AsinExp>());
        pBraceScript->RegisterApi("acos", "acos(number) api", new Brace::BraceApiFactory<AcosExp>());
        pBraceScript->RegisterApi("atan", "atan(number) api", new Brace::BraceApiFactory<AtanExp>());
        pBraceScript->RegisterApi("atan2", "atan2(y,x) api", new Brace::BraceApiFactory<Atan2Exp>());
        pBraceScript->RegisterApi("deg2rad", "deg2rad(number) api", new Brace::BraceApiFactory<Deg2RadExp>());
        pBraceScript->RegisterApi("rad2deg", "rad2deg(number) api", new Brace::BraceApiFactory<Rad2DegExp>());
        pBraceScript->RegisterApi("randint", "randint() or randint(max_num) or randint(min_num,max_num) api", new Brace::BraceApiFactory<RandIntExp>());
        pBraceScript->RegisterApi("randfloat", "randfloat() or randfloat(max_num) or randfloat(min_num,max_num) api", new Brace::BraceApiFactory<RandFloatExp>());

        pBraceScript->RegisterApi("max", "max(number,...) api", new Brace::BraceApiFactory<MaxExp>());
        pBraceScript->RegisterApi("min", "min(number,...) api", new Brace::BraceApiFactory<MinExp>());
        pBraceScript->RegisterApi("sum", "sum(number,...) api", new Brace::BraceApiFactory<SumExp>());
        pBraceScript->RegisterApi("avg", "avg(number,...) api", new Brace::BraceApiFactory<AvgExp>());
        pBraceScript->RegisterApi("devsq", "devsq(number,...) api", new Brace::BraceApiFactory<DevSqExp>());

        pBraceScript->RegisterApi("arraymax", "arraymax(int_array) or arraymax(float_array) api", new Brace::BraceApiFactory<ArrayMaxExp>());
        pBraceScript->RegisterApi("arraymin", "arraymin(int_array) or arraymin(float_array) api", new Brace::BraceApiFactory<ArrayMinExp>());
        pBraceScript->RegisterApi("arraysum", "arraysum(int_array) or arraysum(float_array) api", new Brace::BraceApiFactory<ArraySumExp>());
        pBraceScript->RegisterApi("arrayavg", "arrayavg(int_array) or arrayavg(float_array) api", new Brace::BraceApiFactory<ArrayAvgExp>());
        pBraceScript->RegisterApi("arraydevsq", "arraydevsq(int_array) or arraydevsq(float_array) api", new Brace::BraceApiFactory<ArrayDevSqExp>());

        pBraceScript->RegisterApi("hashtablemax", "hashtablemax(int_int_hash) or hashtablemax(str_int_hash) or hashtablemax(int_float_hash) or hashtablemax(str_float_hash) api", new Brace::BraceApiFactory<HashtableMaxExp>());
        pBraceScript->RegisterApi("hashtablemin", "hashtablemin(int_int_hash) or hashtablemin(str_int_hash) or hashtablemin(int_float_hash) or hashtablemin(str_float_hash) api", new Brace::BraceApiFactory<HashtableMinExp>());
        pBraceScript->RegisterApi("hashtablesum", "hashtablesum(int_int_hash) or hashtablesum(str_int_hash) or hashtablesum(int_float_hash) or hashtablesum(str_float_hash) api", new Brace::BraceApiFactory<HashtableSumExp>());
        pBraceScript->RegisterApi("hashtableavg", "hashtableavg(int_int_hash) or hashtableavg(str_int_hash) or hashtableavg(int_float_hash) or hashtableavg(str_float_hash) api", new Brace::BraceApiFactory<HashtableAvgExp>());
        pBraceScript->RegisterApi("hashtabledevsq", "hashtabledevsq(int_int_hash) or hashtabledevsq(str_int_hash) or hashtabledevsq(int_float_hash) or hashtabledevsq(str_float_hash) api", new Brace::BraceApiFactory<HashtableDevSqExp>());

        pBraceScript->RegisterApi("linearregression", "linearregression(array<:array<:double:>:>,array<:double:>) or linearregression(array<:array<:double:>:>,array<:double:>,bool_debug) api", new Brace::BraceApiFactory<LinearRegressionExp>());

        //
        pBraceScript->RegisterApi("getexepath", "getexepath() api", new Brace::BraceApiFactory<GetExePathExp>());
        pBraceScript->RegisterApi("cd", "cd(dir) api", new Brace::BraceApiFactory<SetCurDirExp>());
        pBraceScript->RegisterApi("pwd", "pwd() api", new Brace::BraceApiFactory<GetCurDirExp>());
        pBraceScript->RegisterApi("showui", "showui(index,bit_flags) api", new Brace::BraceApiFactory<ShowUiExp>());
        pBraceScript->RegisterApi("getscriptinput", "getscriptinput() api", new Brace::BraceApiFactory<GetScriptInputExp>());
        pBraceScript->RegisterApi("setscriptinputlabel", "setscriptinputlabel(str) api", new Brace::BraceApiFactory<SetScriptInputLabelExp>());
        pBraceScript->RegisterApi("setscriptbtncaption", "setscriptbtncaption(index,str) api", new Brace::BraceApiFactory<SetScriptBtnCaptionExp>());

        pBraceScript->RegisterApi("getpixel", "getpixel(x,y) api", new Brace::BraceApiFactory<GetPixelExp>());
        pBraceScript->RegisterApi("getcursorx", "getcursorx() api", new Brace::BraceApiFactory<GetCursorXExp>());
        pBraceScript->RegisterApi("getcursory", "getcursory() api", new Brace::BraceApiFactory<GetCursorYExp>());
        pBraceScript->RegisterApi("getscreenwidth", "getscreenwidth() api", new Brace::BraceApiFactory<GetScreenWidthExp>());
        pBraceScript->RegisterApi("getscreenheight", "getscreenheight() api", new Brace::BraceApiFactory<GetScreenHeightExp>());
        pBraceScript->RegisterApi("readbuttonparam", "readbuttonparam(index) api", new Brace::BraceApiFactory<ReadButtonParamExp>());
        pBraceScript->RegisterApi("readstickparam", "readstickparam(index) api", new Brace::BraceApiFactory<ReadStickParamExp>());
        pBraceScript->RegisterApi("readmotionparam", "readmotionparam(index) api", new Brace::BraceApiFactory<ReadMotionParamExp>());
        pBraceScript->RegisterApi("readparampackage", "readparampackage(str) api", new Brace::BraceApiFactory<ReadParamPackageExp>());
        pBraceScript->RegisterApi("hasparam", "hasparam(key) api", new Brace::BraceApiFactory<HasParamExp>());
        pBraceScript->RegisterApi("getintparam", "getintparam(key,def) api", new Brace::BraceApiFactory<GetIntParamExp>());
        pBraceScript->RegisterApi("getfloatparam", "getfloatparam(key,def) api", new Brace::BraceApiFactory<GetFloatParamExp>());
        pBraceScript->RegisterApi("getstrparam", "getstrparam(key,def) api", new Brace::BraceApiFactory<GetStrParamExp>());
        pBraceScript->RegisterApi("keypress", "keypress(modifier,key) api", new Brace::BraceApiFactory<KeyPressExp>());
        pBraceScript->RegisterApi("keyrelease", "keyrelease(modifier,key) api", new Brace::BraceApiFactory<KeyReleaseExp>());
        pBraceScript->RegisterApi("mousepress", "mousepress(x,y,button) api", new Brace::BraceApiFactory<MousePressExp>());
        pBraceScript->RegisterApi("mouserelease", "mouserelease(button) api", new Brace::BraceApiFactory<MouseReleaseExp>());
        pBraceScript->RegisterApi("mousemove", "mousemove(x,y) api", new Brace::BraceApiFactory<MouseMoveExp>());
        pBraceScript->RegisterApi("mousewheelchange", "mousewheelchange(x,y) api", new Brace::BraceApiFactory<MouseWheelChangeExp>());
        pBraceScript->RegisterApi("touchpress", "touchpress(x,y,id) api", new Brace::BraceApiFactory<TouchPressExp>());
        pBraceScript->RegisterApi("touchupdatebegin", "touchupdatebegin() api", new Brace::BraceApiFactory<TouchUpdateBeginExp>());
        pBraceScript->RegisterApi("touchmove", "touchmove(x,y,id) api", new Brace::BraceApiFactory<TouchMoveExp>());
        pBraceScript->RegisterApi("touchupdateend", "touchupdateend() api", new Brace::BraceApiFactory<TouchUpdateEndExp>());
        pBraceScript->RegisterApi("touchend", "touchend() api", new Brace::BraceApiFactory<TouchEndExp>());

        pBraceScript->RegisterApi("getbuttonstate", "getbuttonstate(id) api", new Brace::BraceApiFactory<GetButtonStateExp>());
        pBraceScript->RegisterApi("setbuttonstate", "setbuttonstate(uint_player_index,int_button_id,bool_value) api", new Brace::BraceApiFactory<SetButtonStateExp>());
        pBraceScript->RegisterApi("setstickpos", "setstickpos(uint_player_index,int_axis_id,float_x,float_y) api", new Brace::BraceApiFactory<SetStickPositionExp>());
        pBraceScript->RegisterApi("setmotionstate", "setmotionstate(uint_player_index,uint64_delta_time,float_gyro_x,float_gyro_y,float_gyro_z,float_accel_x,float_accel_y,float_accel_z) api", new Brace::BraceApiFactory<SetMotionStateExp>());

        pBraceScript->RegisterApi("getresultinfo", "getresultinfo() api", new Brace::BraceApiFactory<GetResultInfoExp>());
        pBraceScript->RegisterApi("getlastinfo", "getlastinfo() api", new Brace::BraceApiFactory<GetLastInfoExp>());
        pBraceScript->RegisterApi("gethistoryinfocount", "gethistoryinfocount() api", new Brace::BraceApiFactory<GetHistoryInfoCountExp>());
        pBraceScript->RegisterApi("gethistoryinfo", "gethistoryinfo(index) api", new Brace::BraceApiFactory<GetHistoryInfoExp>());
        pBraceScript->RegisterApi("getrollbackinfocount", "getrollbackinfocount() api", new Brace::BraceApiFactory<GetRollbackInfoCountExp>());
        pBraceScript->RegisterApi("getrollbackinfo", "getrollbackinfo(index) api", new Brace::BraceApiFactory<GetRollbackInfoExp>());
        pBraceScript->RegisterApi("setresultinfo", "setresultinfo(hashtable<:int64,MemoryModifyInfo:>) api", new Brace::BraceApiFactory<SetResultInfoExp>());

        pBraceScript->RegisterApi("newmemorymodifyinfo", "newmemorymodifyinfo() api", new Brace::BraceApiFactory<NewMemoryModifyInfoExp>());
        pBraceScript->RegisterApi("addtoresult", "addtoresult(addr[,val_size]) api", new Brace::BraceApiFactory<AddToResultExp>());
        pBraceScript->RegisterApi("addtolast", "addtolast(addr[,val_size]) api", new Brace::BraceApiFactory<AddToLastExp>());

        pBraceScript->RegisterApi("gettitleid", "gettitleid() api", new Brace::BraceApiFactory<GetTitleIdExp>());
        pBraceScript->RegisterApi("getmodulecount", "getmodulecount() api", new Brace::BraceApiFactory<GetModuleCountExp>());
        pBraceScript->RegisterApi("getmodulebase", "getmodulebase(index) api", new Brace::BraceApiFactory<GetModuleBaseExp>());
        pBraceScript->RegisterApi("getmoduleaddr", "getmoduleaddr(index) api", new Brace::BraceApiFactory<GetModuleAddrExp>());
        pBraceScript->RegisterApi("getmodulesize", "getmodulesize(index) api", new Brace::BraceApiFactory<GetModuleSizeExp>());
        pBraceScript->RegisterApi("getmoduleid", "getmoduleid(index) api", new Brace::BraceApiFactory<GetModuleIdExp>());
        pBraceScript->RegisterApi("getmodulename", "getmodulename(index) api", new Brace::BraceApiFactory<GetModuleNameExp>());
        pBraceScript->RegisterApi("getheapbase", "getheapbase() api", new Brace::BraceApiFactory<GetHeapBaseExp>());
        pBraceScript->RegisterApi("getheapsize", "getheapsize() api", new Brace::BraceApiFactory<GetHeapSizeExp>());
        pBraceScript->RegisterApi("getstackbase", "getstackbase() api", new Brace::BraceApiFactory<GetStackBaseExp>());
        pBraceScript->RegisterApi("getstacksize", "getstacksize() api", new Brace::BraceApiFactory<GetStackSizeExp>());

        pBraceScript->RegisterApi("markmemdebug", "markmemdebug(addr,size[,debug])", new Brace::BraceApiFactory<CmdMarkMemDebugExp>());
        pBraceScript->RegisterApi("addsniffing", "addsniffing(addr,size[,step,val])", new Brace::BraceApiFactory<CmdAddSniffingExp>());
        pBraceScript->RegisterApi("addsniffingfromsearch", "addsniffingfromsearch(find_vals)", new Brace::BraceApiFactory<CmdAddSniffingFromSearchExp>());
        pBraceScript->RegisterApi("showmem", "showmem(addr,size[,step])", new Brace::BraceApiFactory<CmdShowMemExp>());
        pBraceScript->RegisterApi("findmem", "findmem(find_vals), results show on ui", new Brace::BraceApiFactory<CmdFindMemExp>());
        pBraceScript->RegisterApi("searchmem", "searchmem(find_vals), results show on ui", new Brace::BraceApiFactory<CmdSearchMemExp>());

        pBraceScript->RegisterApi("findmemory", "findmemory(start,size,step,range,find_vals[,val_size])", new Brace::BraceApiFactory<FindMemoryExp>());
        pBraceScript->RegisterApi("searchmemory", "searchmemory(start,size,step,range,find_vals[,val_size,max_count])", new Brace::BraceApiFactory<SearchMemoryExp>());
        pBraceScript->RegisterApi("readmemory", "readmemory(addr[,val_size])", new Brace::BraceApiFactory<ReadMemoryExp>());
        pBraceScript->RegisterApi("writememory", "writememory(addr,val[,val_size])", new Brace::BraceApiFactory<WriteMemoryExp>());
        pBraceScript->RegisterApi("dumpmemory", "dumpmemory(addr,size,file_path)", new Brace::BraceApiFactory<DumpMemoryExp>());

        pBraceScript->RegisterApi("addloginst", "addloginst(mask, value), all type is int32", new Brace::BraceApiFactory<AddLogInstructionExp>());

        pBraceScript->RegisterApi("replacesourceshader",
            "replacesourceshader(hash,shader_type,shader_src_file), shader_type:0--vertex 3--geometry 4--fragment 5--compute",
            new Brace::BraceApiFactory<ReplaceSourceShaderExp>());
        pBraceScript->RegisterApi("replacespirvshader",
            "replacespirvshader(hash,shader_type,shader_spriv_file), shader_type:0--vertex 3--geometry 4--fragment 5--compute",
            new Brace::BraceApiFactory<ReplaceSpirvShaderExp>());

        if (!isCallback) {
            //dmnt
            pBraceScript->RegisterApi("dmnt_file", "dmnt_file(name,module[,file_dir[,build_id]]){...}; statement", new Brace::BraceApiFactory<DmntFileExp>());
            pBraceScript->RegisterApi("dmnt_if", "dmnt_if(exp){...}; or dmnt_if(exp){...}elseif/elif(exp){...}else{...}; or dmnt_if(exp)func(...); statement", new Brace::BraceApiFactory<DmntIfExp>());
            pBraceScript->RegisterApi("dmnt_loop", "dmnt_loop(reg,ct){...}; statement", new Brace::BraceApiFactory<DmntLoopExp>());

            pBraceScript->RegisterApi("dmnt_key", "dmnt_key(key) key:A|B|X|Y|LS|RS|L|R|ZL|ZR|Plus|Minus|Left|Up|Right|Down|LSL|LSU|LSR|LSD|RSL|RSU|RSR|RSD|SL|SR", new Brace::BraceApiFactory<DmntKeyExp>());
            pBraceScript->RegisterApi("dmnt_region", "dmnt_region(mem_region) mem_region:main|heap|alias|aslr", new Brace::BraceApiFactory<DmntRegionExp>());
            pBraceScript->RegisterApi("dmnt_offset", "dmnt_offset(name) name:no_offset|offset_reg|offset_fixed|region_and_base|region_and_relative|region_and_relative_and_offset", new Brace::BraceApiFactory<DmntOffsetExp>());
            pBraceScript->RegisterApi("dmnt_operand", "dmnt_operand(name) name:mem_and_relative|mem_and_offset|reg_and_relative|reg_and_offset|static_value|register_value|reg_other|restore_register|save_register|clear_saved_value|clear_register", new Brace::BraceApiFactory<DmntOperandExp>());

            pBraceScript->RegisterApi("dmnt_calc_offset", "dmnt_calc_offset(offset,addr,region), all type is integer", new Brace::BraceApiFactory<DmntCalcOffsetExp>());
            pBraceScript->RegisterApi("dmnt_read_mem", "dmnt_read_mem(val,addr[,val_size]), all type is integer", new Brace::BraceApiFactory<DmntReadMemExp>());
            pBraceScript->RegisterApi("dmnt_comment", "dmnt_comment(str)", new Brace::BraceApiFactory<DmntCommentExp>());
            pBraceScript->RegisterApi("dmnt_store_v2a", "dmnt_store_v2a(mem_width,mem_region,reg,offset,val), all type is integer", new Brace::BraceApiFactory<DmntStoreValueToAddrExp>());
            pBraceScript->RegisterApi("dmnt_gt", "dmnt_xxx(mem_width,mem_region,offset,val), all type is integer, xxx:gt|ge|lt|le|eq|ne", new Brace::BraceApiFactoryWithArgs<DmntConditionExp, const std::string&>(">"));
            pBraceScript->RegisterApi("dmnt_ge", "dmnt_xxx(mem_width,mem_region,offset,val), all type is integer, xxx:gt|ge|lt|le|eq|ne", new Brace::BraceApiFactoryWithArgs<DmntConditionExp, const std::string&>(">="));
            pBraceScript->RegisterApi("dmnt_lt", "dmnt_xxx(mem_width,mem_region,offset,val), all type is integer, xxx:gt|ge|lt|le|eq|ne", new Brace::BraceApiFactoryWithArgs<DmntConditionExp, const std::string&>("<"));
            pBraceScript->RegisterApi("dmnt_le", "dmnt_xxx(mem_width,mem_region,offset,val), all type is integer, xxx:gt|ge|lt|le|eq|ne", new Brace::BraceApiFactoryWithArgs<DmntConditionExp, const std::string&>("<="));
            pBraceScript->RegisterApi("dmnt_eq", "dmnt_xxx(mem_width,mem_region,offset,val), all type is integer, xxx:gt|ge|lt|le|eq|ne", new Brace::BraceApiFactoryWithArgs<DmntConditionExp, const std::string&>("=="));
            pBraceScript->RegisterApi("dmnt_ne", "dmnt_xxx(mem_width,mem_region,offset,val), all type is integer, xxx:gt|ge|lt|le|eq|ne", new Brace::BraceApiFactoryWithArgs<DmntConditionExp, const std::string&>("!="));
            pBraceScript->RegisterApi("dmnt_load_v2r", "dmnt_load_v2r(reg,val), all type is integer", new Brace::BraceApiFactory<DmntLoadRegWithValueExp>());
            pBraceScript->RegisterApi("dmnt_load_m2r", "dmnt_load_m2r(mem_width[,mem_region],reg,offset), all type is integer", new Brace::BraceApiFactory<DmntLoadRegWithMemoryExp>());
            pBraceScript->RegisterApi("dmnt_store_v2m", "dmnt_store_v2m(mem_width,mem_reg,reg_inc_1or0,val[,offset_reg]), all type is integer", new Brace::BraceApiFactory<DmntStoreValueToMemoryExp>());
            pBraceScript->RegisterApi("dmnt_legacy_add", "dmnt_legacy_xxx(mem_width,reg,val), all type is integer, xxx:add|sub|mul|lshift|rshift", new Brace::BraceApiFactoryWithArgs<DmntLegacyArithExp, const std::string&>("+"));
            pBraceScript->RegisterApi("dmnt_legacy_sub", "dmnt_legacy_xxx(mem_width,reg,val), all type is integer, xxx:add|sub|mul|lshift|rshift", new Brace::BraceApiFactoryWithArgs<DmntLegacyArithExp, const std::string&>("-"));
            pBraceScript->RegisterApi("dmnt_legacy_mul", "dmnt_legacy_xxx(mem_width,reg,val), all type is integer, xxx:add|sub|mul|lshift|rshift", new Brace::BraceApiFactoryWithArgs<DmntLegacyArithExp, const std::string&>("*"));
            pBraceScript->RegisterApi("dmnt_legacy_lshift", "dmnt_legacy_xxx(mem_width,reg,val), all type is integer, xxx:add|sub|mul|lshift|rshift", new Brace::BraceApiFactoryWithArgs<DmntLegacyArithExp, const std::string&>("<<"));
            pBraceScript->RegisterApi("dmnt_legacy_rshift", "dmnt_legacy_xxx(mem_width,reg,val), all type is integer, xxx:add|sub|mul|lshift|rshift", new Brace::BraceApiFactoryWithArgs<DmntLegacyArithExp, const std::string&>(">>"));
            pBraceScript->RegisterApi("dmnt_keypress", "dmnt_keypress(key1,key2,...); all type is integer, key can get by dmnt_key(const)", new Brace::BraceApiFactory<DmntKeyPressExp>());
            pBraceScript->RegisterApi("dmnt_add", "dmnt_xxx(mem_width,result_reg,lhs_reg,rhs[,rhs_is_val_1or0]), all type is integer, xxx:add|sub|mul|lshift|rshift|and|or|not|xor|mov", new Brace::BraceApiFactoryWithArgs<DmntArithExp, const std::string&>("+"));
            pBraceScript->RegisterApi("dmnt_sub", "dmnt_xxx(mem_width,result_reg,lhs_reg,rhs[,rhs_is_val_1or0]), all type is integer, xxx:add|sub|mul|lshift|rshift|and|or|not|xor|mov", new Brace::BraceApiFactoryWithArgs<DmntArithExp, const std::string&>("-"));
            pBraceScript->RegisterApi("dmnt_mul", "dmnt_xxx(mem_width,result_reg,lhs_reg,rhs[,rhs_is_val_1or0]), all type is integer, xxx:add|sub|mul|lshift|rshift|and|or|not|xor|mov", new Brace::BraceApiFactoryWithArgs<DmntArithExp, const std::string&>("*"));
            pBraceScript->RegisterApi("dmnt_lshift", "dmnt_xxx(mem_width,result_reg,lhs_reg,rhs[,rhs_is_val_1or0]), all type is integer, xxx:add|sub|mul|lshift|rshift|and|or|not|xor|mov", new Brace::BraceApiFactoryWithArgs<DmntArithExp, const std::string&>("<<"));
            pBraceScript->RegisterApi("dmnt_rshift", "dmnt_xxx(mem_width,result_reg,lhs_reg,rhs[,rhs_is_val_1or0]), all type is integer, xxx:add|sub|mul|lshift|rshift|and|or|not|xor|mov", new Brace::BraceApiFactoryWithArgs<DmntArithExp, const std::string&>(">>"));
            pBraceScript->RegisterApi("dmnt_and", "dmnt_xxx(mem_width,result_reg,lhs_reg,rhs[,rhs_is_val_1or0]), all type is integer, xxx:add|sub|mul|lshift|rshift|and|or|not|xor|mov", new Brace::BraceApiFactoryWithArgs<DmntArithExp, const std::string&>("&"));
            pBraceScript->RegisterApi("dmnt_or", "dmnt_xxx(mem_width,result_reg,lhs_reg,rhs[,rhs_is_val_1or0]), all type is integer, xxx:add|sub|mul|lshift|rshift|and|or|not|xor|mov", new Brace::BraceApiFactoryWithArgs<DmntArithExp, const std::string&>("|"));
            pBraceScript->RegisterApi("dmnt_not", "dmnt_xxx(mem_width,result_reg,lhs_reg,rhs[,rhs_is_val_1or0]), all type is integer, xxx:add|sub|mul|lshift|rshift|and|or|not|xor|mov", new Brace::BraceApiFactoryWithArgs<DmntArithExp, const std::string&>("~"));
            pBraceScript->RegisterApi("dmnt_xor", "dmnt_xxx(mem_width,result_reg,lhs_reg,rhs[,rhs_is_val_1or0]), all type is integer, xxx:add|sub|mul|lshift|rshift|and|or|not|xor|mov", new Brace::BraceApiFactoryWithArgs<DmntArithExp, const std::string&>("^"));
            pBraceScript->RegisterApi("dmnt_mov", "dmnt_xxx(mem_width,result_reg,lhs_reg,rhs[,rhs_is_val_1or0]), all type is integer, xxx:add|sub|mul|lshift|rshift|and|or|not|xor|mov", new Brace::BraceApiFactoryWithArgs<DmntArithExp, const std::string&>("="));
            pBraceScript->RegisterApi("dmnt_store_r2m", "dmnt_store_r2m(mem_width,src_reg,mem_reg,reg_inc_1or0,[offset_type,offset_or_reg_or_region[,offset]]), all type is integer", new Brace::BraceApiFactory<DmntStoreRegToMemoryExp>());
            pBraceScript->RegisterApi("dmnt_reg_gt", "dmnt_reg_xxx(mem_width,src_reg,opd_type,val1[,val2]), all type is integer, xxx:gt|ge|lt|le|eq|ne", new Brace::BraceApiFactoryWithArgs<DmntRegCondExp, const std::string&>(">"));
            pBraceScript->RegisterApi("dmnt_reg_ge", "dmnt_reg_xxx(mem_width,src_reg,opd_type,val1[,val2]), all type is integer, xxx:gt|ge|lt|le|eq|ne", new Brace::BraceApiFactoryWithArgs<DmntRegCondExp, const std::string&>(">="));
            pBraceScript->RegisterApi("dmnt_reg_lt", "dmnt_reg_xxx(mem_width,src_reg,opd_type,val1[,val2]), all type is integer, xxx:gt|ge|lt|le|eq|ne", new Brace::BraceApiFactoryWithArgs<DmntRegCondExp, const std::string&>("<"));
            pBraceScript->RegisterApi("dmnt_reg_le", "dmnt_reg_xxx(mem_width,src_reg,opd_type,val1[,val2]), all type is integer, xxx:gt|ge|lt|le|eq|ne", new Brace::BraceApiFactoryWithArgs<DmntRegCondExp, const std::string&>("<="));
            pBraceScript->RegisterApi("dmnt_reg_eq", "dmnt_reg_xxx(mem_width,src_reg,opd_type,val1[,val2]), all type is integer, xxx:gt|ge|lt|le|eq|ne", new Brace::BraceApiFactoryWithArgs<DmntRegCondExp, const std::string&>("=="));
            pBraceScript->RegisterApi("dmnt_reg_ne", "dmnt_reg_xxx(mem_width,src_reg,opd_type,val1[,val2]), all type is integer, xxx:gt|ge|lt|le|eq|ne", new Brace::BraceApiFactoryWithArgs<DmntRegCondExp, const std::string&>("!="));
            pBraceScript->RegisterApi("dmnt_reg_sr", "dmnt_reg_sr(dest_reg,src_reg,opd_type), all type is integer", new Brace::BraceApiFactory<DmntRegSaveRestoreExp>());
            pBraceScript->RegisterApi("dmnt_reg_sr_mask", "dmnt_reg_sr_mask(opd_type,mask), all type is integer", new Brace::BraceApiFactory<DmntRegSaveRestoreWithMaskExp>());
            pBraceScript->RegisterApi("dmnt_reg_rw", "dmnt_reg_rw(static_reg_index,reg), all type is integer, static_reg_index: 0x00 to 0x7F for reading or 0x80 to 0xFF for writing", new Brace::BraceApiFactory<DmntRegReadWriteExp>());
            pBraceScript->RegisterApi("dmnt_pause", "dmnt_pause()", new Brace::BraceApiFactory<DmntPauseExp>());
            pBraceScript->RegisterApi("dmnt_resume", "dmnt_resume()", new Brace::BraceApiFactory<DmntResumeExp>());
            pBraceScript->RegisterApi("dmnt_debug", "dmnt_debug(mem_width,log_id,opd_type,val1[,val2]), all type is integer", new Brace::BraceApiFactory<DmntDebugLogExp>());

        }
    }

    static inline void Prepare()
    {
        BraceScriptManager::InitScript();
    }

    int SplitCmd(const std::string& cmdLine, std::string& first, std::string& second)
    {
        std::string cmdStr(cmdLine);
        cmdStr = trim_string(cmdStr);
        size_t pos1 = cmdStr.find('(');
        size_t pos2 = cmdStr.rfind(')');
        if (pos1 != std::string::npos && pos2 != std::string::npos) {
            if (nullptr == g_pDslBufferForCommand) {
                g_pDslBufferForCommand = new DslBufferForCommand();
            }
            else {
                g_pDslBufferForCommand->Reset();
            }

            DslParser::DslFile parsedFile(*g_pDslBufferForCommand);
            parsedFile.Parse(cmdStr.c_str());
            if (!parsedFile.HasError()) {
                //Commands can be expressed as functions with simple arguments
                bool maybeCommand = false;
                if (parsedFile.GetDslInfoNum() == 1) {
                    auto* pComp = parsedFile.GetDslInfo(0);
                    if (pComp->GetSyntaxType() == DslParser::ISyntaxComponent::TYPE_FUNCTION) {
                        auto* pFunc = static_cast<DslParser::FunctionData*>(pComp);
                        if (!pFunc->IsHighOrder()) {
                            maybeCommand = true;
                            for (int ix = 0; ix < pFunc->GetParamNum(); ++ix) {
                                auto* pParam = pFunc->GetParam(ix);
                                if (pParam->GetSyntaxType() != DslParser::ISyntaxComponent::TYPE_VALUE) {
                                    maybeCommand = false;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!maybeCommand) {
                    first = cmdStr;
                    return 1;
                }
                else {
                    replace_all(cmdStr, "(", " ");
                    replace_all(cmdStr, ",", " ");
                    replace_all(cmdStr, ";", " ");
                    replace_all(cmdStr, ")", "");
                    cmdStr = trim_string(cmdStr);
                }
            }
        }
        size_t pos;
        std::string firstStr = get_first_unquoted_arg(cmdStr, pos);
        if (pos >= cmdStr.length()) {
            first = firstStr;
            return 1;
        }
        else {
            first = firstStr;
            second = trim_string(cmdStr.substr(pos + 1));
            std::string temp = get_first_unquoted_arg(second, pos);
            if (pos >= second.length()) {
                second = temp;
            }
            return 2;
        }
    }
    uint64_t GetTimeUs()
    {
        auto cv = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = cv - g_start_time_point;
        auto tv = static_cast<uint64_t>(diff.count() * 1000'000);
        return tv;
    }
    void Init(IBraceScriptApiProvider* pApiProvider)
    {
        if (nullptr != g_pApiProvider) {
            delete g_pApiProvider;
            g_pApiProvider = nullptr;
        }
        g_pApiProvider = pApiProvider;

        g_start_time_point = std::chrono::high_resolution_clock::now();
    }
    const std::map<std::string, std::string>& GetApiDocs()
    {
        Prepare();
        return BraceScriptManager::GetApiDocs();
    }
    bool Send(std::string&& msg)
    {
        return BraceScriptManager::SendMessage(std::move(msg));
    }
    bool Send(std::string&& msgId, MessageArgs&& args)
    {
        return BraceScriptManager::SendMessage(std::move(msgId), std::move(args));
    }
    bool Exec(std::string&& cmdStr)
    {
        std::string cmd, arg;
        SplitCmd(cmdStr, cmd, arg);
        if (cmd == "import") {
            std::string txt = read_file(arg);
            if (!txt.empty()) {
                Prepare();
                BraceScriptManager::AddImportScript(arg);
                BraceScriptManager::PushScript(std::move(arg));
            }
            return true;
        }
        else if (cmd == "clrimports") {
            Prepare();
            BraceScriptManager::ClearImportScripts();
            return true;
        }
        else if (cmd == "reset") {
            Prepare();
            BraceScriptManager::ResetScript();
            return true;
        }
        else if (cmd == "load") {
            std::string txt = read_file(arg);
            if (!txt.empty()) {
                Prepare();
                BraceScriptManager::ResetScript();
                BraceScriptManager::SetScript(std::move(txt));
            }
            return true;
        }
        else if (cmd == "qload") {
            std::string txt = read_file(arg);
            if (!txt.empty()) {
                Prepare();
                BraceScriptManager::PushScript(std::move(arg));
            }
            return true;
        }
        else if (cmd == "run") {
            Prepare();
            BraceScriptManager::ResetScript();
            BraceScriptManager::SetScript(std::move(arg));
            return true;
        }
        else if (cmd == "qrun") {
            Prepare();
            BraceScriptManager::PushScript(std::move(arg));
            return true;
        }
        else if (cmd == "send") {
            Prepare();
            BraceScriptManager::SendMessage(std::move(arg));
            return true;
        }
        else if (cmd == "resetcallback") {
            Prepare();
            BraceScriptManager::ResetCallback();
            return true;
        }
        else if (cmd == "loadcallback") {
            std::string txt = read_file(arg);
            if (!txt.empty()) {
                Prepare();
                BraceScriptManager::ResetCallback();
                BraceScriptManager::LoadCallback(std::move(txt));
            }
            return true;
        }
        else {
            bool handled = false;
            if (nullptr != g_pApiProvider) {
                handled = g_pApiProvider->ExecCommand(std::move(cmd), std::move(arg));
            }
            if (!handled) {
                Prepare();
                BraceScriptManager::ResetScript();
                BraceScriptManager::SetScript(std::move(cmdStr));
                return true;
            }
        }
        return false;
    }
    bool RunCallback(std::string&& msgId, MessageArgs&& args)
    {
        return BraceScriptManager::RunCallback(std::move(msgId), std::move(args));
    }
    void Tick()
    {
        if (nullptr == g_pApiProvider) {
            return;
        }
        if (BraceScriptManager::ExistsCommands()) {
            std::string cmdStr{};
            if (BraceScriptManager::TryPopCommand(cmdStr)) {
                std::string cmd, arg;
                SplitCmd(cmdStr, cmd, arg);
                g_pApiProvider->ExecCommand(std::move(cmd), std::move(arg));
            }
        }
        BraceScriptManager::Go();
    }
    void Release()
    {
        BraceScriptManager::SetQuitting(true);
        BraceScriptManager::WaitQuitting();
        BraceScriptManager::FreeScript();
        if (nullptr != g_pDslBufferForCommand) {
            delete g_pDslBufferForCommand;
            g_pDslBufferForCommand = nullptr;
        }
        if (nullptr != g_pApiProvider) {
            delete g_pApiProvider;
            g_pApiProvider = nullptr;
        }
    }
}

#else
namespace BraceScriptInterpreter
{
    int SplitCmd(const std::string& cmdLine, std::string& first, std::string& second)
    {
        return 0;
    }
    uint64_t GetTimeUs()
    {
        return 0;
    }
    void Init(IBraceScriptApiProvider* pApiProvider)
    {
    }
    bool Send(std::string&& msg)
    {
        return false;
    }
    bool Send(std::string&& msgId, MessageArgs&& args)
    {
        return false;
    }
    bool Exec(std::string&& cmdLine)
    {
        return false;
    }
    bool RunCallback(std::string&& msgId, MessageArgs&& args);
    void Tick()
    {
    }
    void Release()
    {
    }
}
#endif //BRACE_SUPPORTED_PLATFORM
