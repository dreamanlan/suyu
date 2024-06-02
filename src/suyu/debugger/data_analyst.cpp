// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <functional>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <chrono>
#include <mutex>
#include <fmt/format.h>

#include "suyu/debugger/data_analyst.h"
#include "suyu/uisettings.h"
#include "suyu/bootmanager.h"

#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/frontend/graphics_context.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "core/memory.h"
#include "core/memory/memory_sniffer.h"
#include "core/memory/brace_script/brace_script_interpreter.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "input_common/drivers/keyboard.h"
#include "input_common/drivers/mouse.h"
#include "input_common/drivers/touch_screen.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "common/hex_util.h"

#include <QFileDialog>
#include <QMessageBox>

namespace Core {
    struct MainThreadCaller::Impl {
        Impl(DataAnalystWidget& widget) :data_widget(widget) {

        }

        /// Request a host GPU memory flush from the CPU.
        template <typename Func>
        [[nodiscard]] u64 RequestSyncOperation(Func&& action) {
            std::unique_lock lck{ sync_request_mutex };
            const u64 fence = ++last_sync_fence;
            sync_requests.emplace_back(action);
            return fence;
        }

        /// Obtains current flush request fence id.
        [[nodiscard]] u64 CurrentSyncRequestFence() const {
            return current_sync_fence.load(std::memory_order_relaxed);
        }

        void WaitForSyncOperation(const u64 fence) {
            std::unique_lock lck{ sync_request_mutex };
            sync_request_cv.wait(lck, [this, fence] { return CurrentSyncRequestFence() >= fence; });
        }

        template <typename Func>
        inline void RequestAsyncOperation(Func&& action) {
            std::scoped_lock lock{async_request_mutex};
            async_requests.emplace_back(action);
        }

        void TickWork() {
            std::unique_lock lck{ sync_request_mutex };
            while (!sync_requests.empty()) {
                auto request = std::move(sync_requests.front());
                sync_requests.pop_front();

                sync_request_mutex.unlock();
                request();
                current_sync_fence.fetch_add(1, std::memory_order_release);
                sync_request_mutex.lock();
                sync_request_cv.notify_all();
            }
            {
                std::scoped_lock lock(async_request_mutex);
                while (!async_requests.empty()) {
                    auto request = std::move(async_requests.front());
                    async_requests.pop_front();
                    request();
                }
            }
        }

        std::condition_variable sync_cv;

        std::list<std::function<void()>> sync_requests;
        std::atomic<u64> current_sync_fence{};
        u64 last_sync_fence{};
        std::mutex sync_request_mutex;
        std::condition_variable sync_request_cv;

        std::list<std::function<void()>> async_requests;
        std::mutex async_request_mutex;

        DataAnalystWidget& data_widget;
    };

    MainThreadCaller g_MainThreadCaller;

    MainThreadCaller::MainThreadCaller():impl(nullptr) {

    }
    void MainThreadCaller::Init(DataAnalystWidget& widget) {
        impl = std::make_unique<Impl>(widget);
    }
    void MainThreadCaller::TickWork() {
        if (impl)
            impl->TickWork();
    }
    void MainThreadCaller::SyncLogToView(const std::string& info)const {
        impl->data_widget.AddLog(info);
    }
    void MainThreadCaller::RequestLogToView(std::string&& msg) {
        if (impl) {
            impl->RequestAsyncOperation([this, msg = std::move(msg)]() {
                impl->data_widget.AddLog(msg);
            });
        }
    }
    void MainThreadCaller::RequestSyncCallback(const Kernel::KThread* pThread) {
        if (impl) {
            u64 fence = impl->RequestSyncOperation([pThread]() {
                uint64_t pthread = reinterpret_cast<uint64_t>(pThread);
                BraceScriptInterpreter::MessageArgs args;
                args.push_back(pthread);
                BraceScriptInterpreter::RunCallback("breakpoint", std::move(args));
            });
            impl->WaitForSyncOperation(fence);
        }
    }
    void MainThreadCaller::RequestSyncCallback(int watchType, uint64_t addr, const Kernel::KThread* pThread) {
        if (impl) {
            u64 fence = impl->RequestSyncOperation([watchType, addr, pThread]() {
                uint64_t pthread = reinterpret_cast<uint64_t>(pThread);
                BraceScriptInterpreter::MessageArgs args;
                args.push_back(watchType);
                args.push_back(addr);
                args.push_back(pthread);
                BraceScriptInterpreter::RunCallback("watchpoint", std::move(args));
            });
            impl->WaitForSyncOperation(fence);
        }
    }
    void MainThreadCaller::RequestSyncCallback(int watchType, uint64_t addr, std::size_t size, const Kernel::KThread* pThread) {
        if (impl) {
            u64 fence = impl->RequestSyncOperation([watchType, addr, size, pThread]() {
                uint64_t pthread = reinterpret_cast<uint64_t>(pThread);
                BraceScriptInterpreter::MessageArgs args;
                args.push_back(watchType);
                args.push_back(addr);
                args.push_back(size);
                args.push_back(pthread);
                BraceScriptInterpreter::RunCallback("watchpoint_range", std::move(args));
            });
            impl->WaitForSyncOperation(fence);
        }
    }
}

class BraceApiProvider : public BraceScriptInterpreter::IBraceScriptApiProvider {
public:
    BraceApiProvider(DataAnalystWidget& widget) :m_Widget(widget) {
    }
    virtual ~BraceApiProvider() = default;
public:
    virtual void LogToView(const std::string& info)const override {
        m_Widget.AddLog(info);
    }
    virtual bool ExecCommand(std::string&& cmd, std::string&& arg)const override {
        {
            std::stringstream ss;
            ss << "command: " << cmd;
            if (arg.length() > 0) {
                ss << " " << arg;
            }
            m_Widget.AddLog(ss.str());
        }

        if (cmd == "help") {
            m_Widget.ShowHelp(arg);
            return true;
        }
        else if (cmd == "enablesniffer") {
            m_Widget.EnableSniffer();
            return true;
        }
        else if (cmd == "disablesniffer") {
            m_Widget.DisableSniffer();
            return true;
        }
        else if (cmd == "refresh") {
            m_Widget.RefreshResultList(arg.c_str());
            return true;
        }
        else if (cmd == "showall") {
            m_Widget.RefreshResultList(arg.c_str(), true);
            return true;
        }
        else if (cmd == "clearall") {
            auto&& system = m_Widget.GetSystem();
            auto&& sniffer = system.MemorySniffer();
            sniffer.ClearAll();
            m_Widget.ClearResultList();
            return true;
        }
        else if (cmd == "setsniffingscope") {
            m_Widget.SetSniffingScope(arg);
            return true;
        }
        else if (cmd == "clearlist") {
            m_Widget.ClearResultList();
            return true;
        }
        else if (cmd == "savelist") {
            m_Widget.SaveResultList(arg);
            return true;
        }
        else if (cmd == "setmaxlist") {
            m_Widget.maxResultList = std::stoi(arg, nullptr, 0);
            return true;
        }
        else if (cmd == "setmaxrecords") {
            m_Widget.maxRecords = std::stoi(arg, nullptr, 0);
            return true;
        }
        else if (cmd == "setmaxhistories") {
            m_Widget.maxHistories = std::stoi(arg, nullptr, 0);
            return true;
        }
        else if (cmd == "setmaxrollbacks") {
            m_Widget.maxRollbacks = std::stoi(arg, nullptr, 0);
            return true;
        }
        else if (cmd == "enablecapture") {
            m_Widget.captureEnabled = true;
            return true;
        }
        else if (cmd == "disablecapture") {
            m_Widget.captureEnabled = false;
            return true;
        }
        else if (cmd == "captureinterval") {
            m_Widget.screenCaptureInterval = std::stoi(arg, nullptr, 0);
            return true;
        }
        else if (cmd == "logcapturetime") {
            m_Widget.logCaptureTimeConsuming = true;
            return true;
        }
        else if (cmd == "dontlogcapturetime") {
            m_Widget.logCaptureTimeConsuming = false;
            return true;
        }
        else if (cmd == "showbutton") {
            m_Widget.ShowButtonParam(std::stoi(arg, nullptr, 0));
            return true;
        }
        else if (cmd == "showstick") {
            m_Widget.ShowStickParam(std::stoi(arg, nullptr, 0));
            return true;
        }
        else if (cmd == "showmotion") {
            m_Widget.ShowMotionParam(std::stoi(arg, nullptr, 0));
            return true;
        }
        else if (cmd == "showinput") {
            m_Widget.ShowInputState();
            return true;
        }
        else if (cmd == "dumpshaderinfo") {
            auto&& system = m_Widget.GetSystem();
            if (system.ApplicationProcess() == nullptr) {
                m_Widget.AddLog("game isn't running.");
                return true;
            }
            auto& gpu = system.GPU();
            gpu.RequestDumpShaderInfo(std::move(arg));
            return true;
        }
        else if (cmd == "setpolygonmodeline") {
            VideoCore::g_IsPolygonModeLine = arg == "true" || (std::isdigit(arg[0]) && std::stoi(arg, nullptr, 0) != 0);
            return true;
        }
        else if (cmd == "setminvertexnum") {
            VideoCore::g_LineModeMinVertexNum = std::stoul(arg, nullptr, 0);
            return true;
        }
        else if (cmd == "setmaxvertexnum") {
            VideoCore::g_LineModeMaxVertexNum = std::stoul(arg, nullptr, 0);
            return true;
        }
        else if (cmd == "setmindrawcount") {
            VideoCore::g_LineModeMinDrawCount = std::stoul(arg, nullptr, 0);
            return true;
        }
        else if (cmd == "setmaxdrawcount") {
            VideoCore::g_LineModeMaxDrawCount = std::stoul(arg, nullptr, 0);
            return true;
        }
        else if (cmd == "addvshash") {
            uint64_t hash = std::stoull(arg, nullptr, 0);

            auto&& system = m_Widget.GetSystem();
            if (system.ApplicationProcess() == nullptr) {
                m_Widget.AddLog("game isn't running.");
                return true;
            }
            auto& gpu = system.GPU();
            gpu.RequestAddVsHash(hash);
            return true;
        }
        else if (cmd == "removevshash") {
            uint64_t hash = std::stoull(arg, nullptr, 0);

            auto&& system = m_Widget.GetSystem();
            if (system.ApplicationProcess() == nullptr) {
                m_Widget.AddLog("game isn't running.");
                return true;
            }
            auto& gpu = system.GPU();
            gpu.RequestRemoveVsHash(hash);
            return true;
        }
        else if (cmd == "clearvshashes") {
            auto&& system = m_Widget.GetSystem();
            if (system.ApplicationProcess() == nullptr) {
                m_Widget.AddLog("game isn't running.");
                return true;
            }
            auto& gpu = system.GPU();
            gpu.RequestClearVsHashes();
            return true;
        }
        else if (cmd == "addpshash") {
            uint64_t hash = std::stoull(arg, nullptr, 0);

            auto&& system = m_Widget.GetSystem();
            if (system.ApplicationProcess() == nullptr) {
                m_Widget.AddLog("game isn't running.");
                return true;
            }
            auto& gpu = system.GPU();
            gpu.RequestAddPsHash(hash);
            return true;
        }
        else if (cmd == "removepshash") {
            uint64_t hash = std::stoull(arg, nullptr, 0);

            auto&& system = m_Widget.GetSystem();
            if (system.ApplicationProcess() == nullptr) {
                m_Widget.AddLog("game isn't running.");
                return true;
            }
            auto& gpu = system.GPU();
            gpu.RequestRemovePsHash(hash);
            return true;
        }
        else if (cmd == "clearpshashes") {
            auto&& system = m_Widget.GetSystem();
            if (system.ApplicationProcess() == nullptr) {
                m_Widget.AddLog("game isn't running.");
                return true;
            }
            auto& gpu = system.GPU();
            gpu.RequestClearPsHashes();
            return true;
        }
        else if (cmd == "setlinemodelogframecount") {
            VideoCore::g_LineModeLogFrameCount = std::stoi(arg, nullptr, 0);
            return true;
        }
        else if (cmd == "requestlinemodelog") {
            VideoCore::g_LineModeLogRequest = true;
            return true;
        }
        else if (cmd == "clearlogpipelines") {
            auto&& system = m_Widget.GetSystem();
            if (system.ApplicationProcess() == nullptr) {
                m_Widget.AddLog("game isn't running.");
                return true;
            }
            auto& gpu = system.GPU();
            gpu.RequestClearLogPipelines();
            return true;
        }
        else if (cmd == "addlogpipeline") {
            uint64_t hash = std::stoull(arg, nullptr, 0);

            auto&& system = m_Widget.GetSystem();
            if (system.ApplicationProcess() == nullptr) {
                m_Widget.AddLog("game isn't running.");
                return true;
            }
            auto& gpu = system.GPU();
            gpu.RequestAddLogPipeline(hash);
            return true;
        }
        else if (cmd == "removelogpipeline") {
            uint64_t hash = std::stoull(arg, nullptr, 0);

            auto&& system = m_Widget.GetSystem();
            if (system.ApplicationProcess() == nullptr) {
                m_Widget.AddLog("game isn't running.");
                return true;
            }
            auto& gpu = system.GPU();
            gpu.RequestRemoveLogPipeline(hash);
            return true;
        }
        else {
            auto&& system = m_Widget.GetSystem();
            auto&& sniffer = system.MemorySniffer();
            return sniffer.Exec(cmd, arg);
        }
    }
    virtual Core::System& GetSystem(void)const override {
        auto&& system = m_Widget.GetSystem();
        return system;
    }
    virtual void ShowUI(int index, int flags)const override {
        switch (index) {
        case 0:
            m_Widget.scriptInputLabel->hide();
            m_Widget.scriptInputEdit->hide();
            if (flags & 0x01) {
                m_Widget.scriptInputLabel->show();
            }
            if (flags & 0x02) {
                m_Widget.scriptInputEdit->show();
            }
            break;
        case 1:
            m_Widget.scriptBtn1->hide();
            m_Widget.scriptBtn2->hide();
            m_Widget.scriptBtn3->hide();
            m_Widget.scriptBtn4->hide();
            if (flags & 0x01) {
                m_Widget.scriptBtn1->show();
            }
            if (flags & 0x02) {
                m_Widget.scriptBtn2->show();
            }
            if (flags & 0x04) {
                m_Widget.scriptBtn3->show();
            }
            if (flags & 0x08) {
                m_Widget.scriptBtn4->show();
            }
            break;
        }
    }
    virtual std::string GetScriptInput(void)const override {
        std::string txt = m_Widget.scriptInputEdit->text().toStdString();
        return txt;
    }
    virtual void SetScriptInputLabel(const std::string& label)const override {
        m_Widget.scriptInputLabel->setText(m_Widget.tr(label.c_str()));
    }
    virtual void SetScriptBtnCaption(int index, const std::string& caption)const override {
        QPushButton* btns[] = { m_Widget.scriptBtn1, m_Widget.scriptBtn2, m_Widget.scriptBtn3, m_Widget.scriptBtn4 };
        if (index >= 0 && index < static_cast<int>(sizeof(btns) / sizeof(QPushButton*))) {
            btns[index]->setText(m_Widget.tr(caption.c_str()));
        }
    }
    virtual uint32_t GetPixel(int x, int y)const override {
        return m_Widget.GetPixel(x, y);
    }
    virtual bool GetCursorPos(int& x, int& y)const override {
        return m_Widget.GetCursorPos(x, y);
    }
    virtual bool GetScreenSize(int& x, int& y)const override {
        return m_Widget.GetScreenSize(x, y);
    }
    virtual std::string ReadButtonParam(int index)const override {
        return m_Widget.ReadButtonParam(index);
    }
    virtual std::string ReadStickParam(int index)const override {
        return m_Widget.ReadStickParam(index);
    }
    virtual std::string ReadMotionParam(int index)const override {
        return m_Widget.ReadMotionParam(index);
    }
    virtual void ReadParamPackage(const std::string& str)const override {
        m_Widget.ReadParamPackage(str);
    }
    virtual bool HasParam(const std::string& key)const override {
        return m_Widget.HasParam(key);
    }
    virtual int GetIntParam(const std::string& key, int def)const override {
        return m_Widget.GetIntParam(key, def);
    }
    virtual float GetFloatParam(const std::string& key, float def)const override {
        return m_Widget.GetFloatParam(key, def);
    }
    virtual std::string GetStrParam(const std::string& key, const std::string& def)const override {
        return m_Widget.GetStrParam(key, def);
    }
    virtual void KeyPress(int modifier, int key)const override {
        m_Widget.KeyPress(modifier, key);
    }
    virtual void KeyRelease(int modifier, int key)const override {
        m_Widget.KeyRelease(modifier, key);
    }
    virtual void MousePress(int x, int y, int button)const override {
        m_Widget.MousePress(x, y, button);
    }
    virtual void MouseRelease(int button)const override {
        m_Widget.MouseRelease(button);
    }
    virtual void MouseMove(int x, int y)const override {
        m_Widget.MouseMove(x, y);
    }
    virtual void MouseWheelChange(int x, int y)const override {
        m_Widget.MouseWheelChange(x, y);
    }
    virtual void TouchPress(int x, int y, int id)const override {
        m_Widget.TouchPress(x, y, id);
    }
    virtual void TouchUpdateBegin()const override {
        m_Widget.TouchUpdateBegin();
    }
    virtual void TouchMove(int x, int y, int id)const override {
        m_Widget.TouchMove(x, y, id);
    }
    virtual void TouchUpdateEnd()const override {
        m_Widget.TouchUpdateEnd();
    }
    virtual void TouchEnd()const override {
        m_Widget.TouchEnd();
    }
    virtual bool GetButtonState(int button_id)const override {
        return m_Widget.GetButtonState(button_id);
    }
    virtual void SetButtonState(std::size_t player_index, int button_id, bool value)const override {
        m_Widget.SetButtonState(player_index, button_id, value);
    }
    virtual void SetStickPosition(std::size_t player_index, int axis_id, float x_value, float y_value)const override {
        m_Widget.SetStickPosition(player_index, axis_id, x_value, y_value);
    }
    virtual void SetMotionState(std::size_t player_index, u64 delta_timestamp, float gyro_x, float gyro_y,
        float gyro_z, float accel_x, float accel_y, float accel_z)const override {
        m_Widget.SetMotionState(player_index, delta_timestamp, gyro_x, gyro_y, gyro_z, accel_x, accel_y, accel_z);
    }
    virtual void ReplaceSourceShader(uint64_t hash, int stage, std::string&& code)const override {
        auto&& system = m_Widget.GetSystem();
        if (system.ApplicationProcess() == nullptr) {
            m_Widget.AddLog("game isn't running.");
            return;
        }
        auto&& gpu = system.GPU();
        gpu.RequestReplaceSourceShader(hash, stage, std::move(code));
    }
    virtual void ReplaceSpirvShader(uint64_t hash, int stage, std::vector<uint32_t>&& code)const override {
        auto&& system = m_Widget.GetSystem();
        if (system.ApplicationProcess() == nullptr) {
            m_Widget.AddLog("game isn't running.");
            return;
        }
        auto&& gpu = system.GPU();
        gpu.RequestReplaceSpirvShader(hash, stage, std::move(code));
    }
private:
    DataAnalystWidget& m_Widget;
};

void DataAnalystWidget::InitCmdDocs() {
    //in DataAnalyst
    cmdDocs.insert(std::make_pair("help", "help filter, show commands and apis"));
    cmdDocs.insert(std::make_pair("enablesniffer", "enablesniffer"));
    cmdDocs.insert(std::make_pair("disablesniffer", "disablesniffer"));
    cmdDocs.insert(std::make_pair("refresh", "refresh tag, refresh output list"));
    cmdDocs.insert(std::make_pair("showall", "showall tag, show all output list"));
    cmdDocs.insert(std::make_pair("clearall", "clearall, clear all sniffer data and output list"));
    cmdDocs.insert(std::make_pair("setsniffingscope", "setsniffingscope section_key, set sniffing scope with memory section key (id or name)"));
    cmdDocs.insert(std::make_pair("clearlist", "clearlist, clear output list"));
    cmdDocs.insert(std::make_pair("savelist", "savelist file, save output list"));
    cmdDocs.insert(std::make_pair("setmaxlist", "setmaxlist ct, set max output list count"));
    cmdDocs.insert(std::make_pair("setmaxrecords", "setmaxrecords ct, set sniffer records showed in output list, def 10"));
    cmdDocs.insert(std::make_pair("setmaxhistories", "setmaxhistories ct, set sniffer history count showed in output list, def 10"));
    cmdDocs.insert(std::make_pair("setmaxrollbacks", "setmaxrollbacks ct, set sniffer rollback count showed in output list, def 10"));
    cmdDocs.insert(std::make_pair("enablecapture", "enablecapture, enable capture screen, capture current screen image to analysis"));
    cmdDocs.insert(std::make_pair("disablecapture", "disablecapture, disable capture screen"));
    cmdDocs.insert(std::make_pair("captureinterval", "captureinterval ms, capture screen interval time"));
    cmdDocs.insert(std::make_pair("logcapturetime", "logcapturetime, log capture screen cost for profiling"));
    cmdDocs.insert(std::make_pair("dontlogcapturetime", "dontlogcapturetime, close capture screen cost log"));
    cmdDocs.insert(std::make_pair("showbutton", "showbutton index, show button param package"));
    cmdDocs.insert(std::make_pair("showstick", "showstick index, show stick param package"));
    cmdDocs.insert(std::make_pair("showmotion", "showmotion index, show motion param package"));
    cmdDocs.insert(std::make_pair("showinput", "showinput, show gamepad input state"));
    cmdDocs.insert(std::make_pair("dumpshaderinfo", "dumpshaderinfo file, request dump shader hash info"));
    cmdDocs.insert(std::make_pair("setpolygonmodeline", "setpolygonmodeline 0_or_1, set line render mode"));
    cmdDocs.insert(std::make_pair("setminvertexnum", "setminvertexnum num, set min vertex num for line render mode, open interval, def 6"));
    cmdDocs.insert(std::make_pair("setmaxvertexnum", "setmaxvertexnum num, set max vertex num for line render mode, open interval, def 64"));
    cmdDocs.insert(std::make_pair("setmindrawcount", "setmindrawcount num, set min drawcount for line render mode, for indirect draw, open interval, def 2"));
    cmdDocs.insert(std::make_pair("setmaxdrawcount", "setmaxdrawcount num, set max drawcount for line render mode, for indirect draw, open interval, def 12"));
    cmdDocs.insert(std::make_pair("addvshash", "addvshash hash, set vs hash for line render mode"));
    cmdDocs.insert(std::make_pair("removevshash", "removevshash hash, remove vs hash for line render mode"));
    cmdDocs.insert(std::make_pair("clearvshashes", "clearvshashes, clear all vs hashes for line render mode"));
    cmdDocs.insert(std::make_pair("addpshash", "addpshash hash, set ps hash for line render mode"));
    cmdDocs.insert(std::make_pair("removepshash", "removepshash hash, remove ps hash for line render mode"));
    cmdDocs.insert(std::make_pair("clearpshashes", "clearpshashes, clear all ps hashes for line render mode"));
    cmdDocs.insert(std::make_pair("setlinemodelogframecount", "setlinemodelogframecount num, def 2"));
    cmdDocs.insert(std::make_pair("requestlinemodelog", "requestlinemodelog, log shader info in line render mode"));
    cmdDocs.insert(std::make_pair("clearlogpipelines", "clearlogpipelines, clear all logged pipelines"));
    cmdDocs.insert(std::make_pair("addlogpipeline", "addlogpipeline hash, add a logged pipeline"));
    cmdDocs.insert(std::make_pair("removelogpipeline", "removelogpipeline hash, remove a logged pipeline"));

    //in MemorySniffer
    cmdDocs.insert(std::make_pair("refreshsnapshot", "refreshsnapshot, snapshot sniffied memory data, same as UI"));
    cmdDocs.insert(std::make_pair("keepunchanged", "keepunchanged, keep unchanged memory data, not refresh data"));
    cmdDocs.insert(std::make_pair("keepchanged", "keepchanged, keep changed memory data, not refresh data"));
    cmdDocs.insert(std::make_pair("keepincreased", "keepincreased, keep increased memory data, not refresh data"));
    cmdDocs.insert(std::make_pair("keepdecreased", "keepdecreased, keep decreased memory data, not refresh data"));
    cmdDocs.insert(std::make_pair("keepvalue", "keepvalue val, keep memory data with value, not refresh data"));
    cmdDocs.insert(std::make_pair("addtotracewrite", "addtotracewrite, add result memory to trace write, same as UI"));
    cmdDocs.insert(std::make_pair("setdebugsnapshot", "setdebugsnapshot 0_or_1"));
    cmdDocs.insert(std::make_pair("clearloginsts", "clearloginsts, clear all log instructions"));
    cmdDocs.insert(std::make_pair("addlogbl", "addlogbl, add BL/BLR/BLRxxx to log instructions"));
    cmdDocs.insert(std::make_pair("addlogbc", "addlogbc, add B.cond/BC.cond/CBNZ/CBZ/TBNZ/TBZ to log instructions"));
    cmdDocs.insert(std::make_pair("addlogb", "addlogb, add B/BR/BRxxx to log instructions"));
    cmdDocs.insert(std::make_pair("addlogret", "addlogret, add RET/RETxxx to log instructions"));
    cmdDocs.insert(std::make_pair("settracescope", "settracescope section_key"));
    cmdDocs.insert(std::make_pair("settracescopebegin", "settracescopebegin addr"));
    cmdDocs.insert(std::make_pair("settracescopeend", "settracescopeend addr"));
    cmdDocs.insert(std::make_pair("settracepid", "settracepid pid"));
    cmdDocs.insert(std::make_pair("cleartrace", "cleartrace"));
    cmdDocs.insert(std::make_pair("starttrace", "starttrace or starttrace ix, start trace immediately"));
    cmdDocs.insert(std::make_pair("stoptrace", "stoptrace or stoptrace ix, stop trace immediately"));
    cmdDocs.insert(std::make_pair("setmaxstepcount", "setmaxstepcount num, max step count per break"));
    cmdDocs.insert(std::make_pair("addtraceread", "addtraceread addr"));
    cmdDocs.insert(std::make_pair("removetraceread", "removetraceread addr"));
    cmdDocs.insert(std::make_pair("addtracewrite", "addtracewrite addr"));
    cmdDocs.insert(std::make_pair("removetracewrite", "removetracewrite addr"));
    cmdDocs.insert(std::make_pair("addtracepointer", "addtracepointer addr"));
    cmdDocs.insert(std::make_pair("removetracepointer", "removetracepointer addr"));
    cmdDocs.insert(std::make_pair("addtracecstring", "addtracecstring addr"));
    cmdDocs.insert(std::make_pair("removetracecstring", "removetracecstring addr"));
    cmdDocs.insert(std::make_pair("addbp", "addbp addr, add breakpoint"));
    cmdDocs.insert(std::make_pair("removebp", "removebp addr, remove breakpoint"));
    cmdDocs.insert(std::make_pair("setstarttracebp", "setstarttracebp addr, add breakpoint for start trace"));
    cmdDocs.insert(std::make_pair("setstoptracebp", "setstoptracebp addr, add breakpoint for stop trace"));
    cmdDocs.insert(std::make_pair("settraceswi", "settraceswi swi, trace software interrupt"));
    cmdDocs.insert(std::make_pair("usepccountarray", "usepccountarray 0_or_1, record pc in trace"));
    cmdDocs.insert(std::make_pair("setmaxpccount", "setmaxpccount num, max pc count for save, def 10"));
    cmdDocs.insert(std::make_pair("startpccount", "startpccount or startpccount ix, start pc count immediately"));
    cmdDocs.insert(std::make_pair("stoppccount", "stoppccount or stoppccount ix, stop pc count immediately"));
    cmdDocs.insert(std::make_pair("clearpccount", "clearpccount, clear pc count info"));
    cmdDocs.insert(std::make_pair("storepccount", "storepccount, store current snapshot as last pc count info"));
    cmdDocs.insert(std::make_pair("keeppccount", "keeppccount, keep last and current pc count info"));
    cmdDocs.insert(std::make_pair("keepnewpccount", "keepnewpccount, keep that not in last pc count info"));
    cmdDocs.insert(std::make_pair("keepsamepccount", "keepsamepccount, keep that both in last and current pc count info"));
    cmdDocs.insert(std::make_pair("savepccount", "savepccount file, save result pc count info"));
    cmdDocs.insert(std::make_pair("cleartracebuffer", "cleartracebuffer"));
    cmdDocs.insert(std::make_pair("savetracebuffer", "savetracebuffer file, save trace buffer data"));
    cmdDocs.insert(std::make_pair("setsession", "setsession handle, set monitor session for software interrupt"));
    cmdDocs.insert(std::make_pair("clearmemscope", "clearmemscope, clear memory search scope"));
    cmdDocs.insert(std::make_pair("setmemscope", "setmemscope section_key, set memory search scope with section key (id or name)"));
    cmdDocs.insert(std::make_pair("setmemscopebegin", "setmemscopebegin addr, set memory search scope"));
    cmdDocs.insert(std::make_pair("setmemscopeend", "setmemscopeend addr, set memory search scope"));
    cmdDocs.insert(std::make_pair("setmempid", "setmempid pid, set memory search process id"));
    cmdDocs.insert(std::make_pair("setmemstep", "setmemstep num, set addr step for memory search"));
    cmdDocs.insert(std::make_pair("setmemsize", "setmemsize num, set data size for memory search"));
    cmdDocs.insert(std::make_pair("setmemrange", "setmemrange num, set data addr range for memory search"));
    cmdDocs.insert(std::make_pair("setmemcount", "setmemcount num, set max count for memory search"));
    cmdDocs.insert(std::make_pair("saveresult", "saveresult file, save result memory snapshot"));
    cmdDocs.insert(std::make_pair("savehistory", "savehistory file, save history memory snapshot"));
    cmdDocs.insert(std::make_pair("saverollback", "saverollback file, save rollback memory snapshot"));
    cmdDocs.insert(std::make_pair("dumpreg", "dumpreg, dump current register value of physics cores"));
    cmdDocs.insert(std::make_pair("dumpsession", "dumpsession, dump sessions info"));
    cmdDocs.insert(std::make_pair("listprocess", "listprocess, list processes info"));
}

void DataAnalystWidget::ShowHelp(const std::string& filter)const {
    new QListWidgetItem(tr("=== Commands ==="), listWidget);
    for (auto&& pair : cmdDocs) {
        if (pair.first.find(filter) != std::string::npos || pair.second.find(filter) != std::string::npos) {
            std::string info = "[" + pair.first + "]:" + pair.second;
            new QListWidgetItem(tr(info.c_str()), listWidget);
        }
    }
    new QListWidgetItem(tr("=== APIs ==="), listWidget);
    auto&& apiDocs = BraceScriptInterpreter::GetApiDocs();
    for (auto&& pair : apiDocs) {
        if (pair.first.find(filter) != std::string::npos || pair.second.find(filter) != std::string::npos) {
            std::string info = "[" + pair.first + "]:" + pair.second;
            new QListWidgetItem(tr(info.c_str()), listWidget);
        }
    }
}

DataAnalystWidget::DataAnalystWidget(Core::System& system_, std::shared_ptr<InputCommon::InputSubsystem> input_subsystem_, GRenderWindow* renderWindow_, QWidget* parent)
    : QDockWidget(tr("&Data Analyst"), parent), system{ system_ }, inputSubSystem{ input_subsystem_ }, renderWindow{ renderWindow_ }, screenImage{}, lastTime(0),
    screenCaptureInterval(1000), captureEnabled(false), logCaptureTimeConsuming(false), mouseX(0), mouseY(0), maxResultList(16384),
    maxRecords(10), maxHistories(10), maxRollbacks(10),
    paramPackage{ std::make_shared<Common::ParamPackage>() } {
    setObjectName(QStringLiteral("DataAnalystWidget"));
    setEnabled(true);

    dockWidgetContents = new QWidget();
    layout = new QVBoxLayout();
    QHBoxLayout* buttonLayout1 = new QHBoxLayout();
    QHBoxLayout* buttonLayout2 = new QHBoxLayout();
    QHBoxLayout* buttonLayout3 = new QHBoxLayout();
    QHBoxLayout* saveLayout = new QHBoxLayout();
    QHBoxLayout* scriptInputLayout = new QHBoxLayout();
    QHBoxLayout* scriptBtnLayout = new QHBoxLayout();
    QHBoxLayout* commandLayout = new QHBoxLayout();

    enableCheckBox = new QCheckBox(tr("Sniffing"));
    QPushButton* runButton = new QPushButton(tr("Run Script"));
    QPushButton* clearAllButton = new QPushButton(tr("ClearAll"));
    QPushButton* addSniffingButton = new QPushButton(tr("AddSniffing"));
    QPushButton* keepUnchangedButton = new QPushButton(tr("Keep Unchanged"));
    QPushButton* keepChangedButton = new QPushButton(tr("Keep Changed"));
    QPushButton* keepIncreasedButton = new QPushButton(tr("Keep Increased"));
    QPushButton* keepDecreasedButton = new QPushButton(tr("Keep Decreased"));
    QPushButton* rollbackButton = new QPushButton(tr("Rollback"));
    QPushButton* unrollbackButton = new QPushButton(tr("Unrollback"));
    QPushButton* keepValueButton = new QPushButton(tr("KeepValue"));
    QPushButton* traceWriteButton = new QPushButton(tr("TraceWrite"));
    QPushButton* saveAbsButton = new QPushButton(tr("SaveAbs"));
    QPushButton* saveRelButton = new QPushButton(tr("SaveRel"));
    QPushButton* execButton = new QPushButton(tr("Exec Command"));
    QLabel* label = new QLabel(tr("Tag:"));
    QLabel* curValueLabel = new QLabel(tr("Value:"));
    QLabel* stepAddrLabel = new QLabel(tr("Step:"));
    QLabel* pidLabel = new QLabel(tr("Process:"));
    QLabel* startAddrLabel = new QLabel(tr("Start:"));
    QLabel* sizeAddrLabel = new QLabel(tr("Size:"));

    curValueEdit = new QLineEdit();
    stepAddrEdit = new QLineEdit();
    pidEdit = new QLineEdit();
    startAddrEdit = new QLineEdit();
    sizeAddrEdit = new QLineEdit();

    commandEdit = new QLineEdit();
    tagEdit = new QLineEdit();
    scriptInputLabel = new QLabel(tr("Script Input:"));
    scriptInputEdit = new QLineEdit();
    scriptBtn1 = new QPushButton(tr("Script Btn1"));
    scriptBtn2 = new QPushButton(tr("Script Btn2"));
    scriptBtn3 = new QPushButton(tr("Script Btn3"));
    scriptBtn4 = new QPushButton(tr("Script Btn4"));
    listWidget = new QListWidget();

    stepAddrEdit->setFixedWidth(20);
    pidEdit->setFixedWidth(80);
    runButton->setFixedWidth(80);
    clearAllButton->setFixedWidth(80);

    buttonLayout1->addWidget(runButton);
    buttonLayout1->addWidget(enableCheckBox);
    buttonLayout1->addWidget(curValueLabel);
    buttonLayout1->addWidget(curValueEdit);
    buttonLayout1->addWidget(stepAddrLabel);
    buttonLayout1->addWidget(stepAddrEdit);
    buttonLayout1->addWidget(pidLabel);
    buttonLayout1->addWidget(pidEdit);
    layout->addLayout(buttonLayout1);

    buttonLayout2->addWidget(clearAllButton);
    buttonLayout2->addWidget(startAddrLabel);
    buttonLayout2->addWidget(startAddrEdit);
    buttonLayout2->addWidget(sizeAddrLabel);
    buttonLayout2->addWidget(sizeAddrEdit);
    buttonLayout2->addWidget(addSniffingButton);
    layout->addLayout(buttonLayout2);

    rollbackButton->setFixedWidth(80);
    unrollbackButton->setFixedWidth(80);

    buttonLayout3->addWidget(rollbackButton);
    buttonLayout3->addWidget(keepUnchangedButton);
    buttonLayout3->addWidget(keepChangedButton);
    buttonLayout3->addWidget(keepIncreasedButton);
    buttonLayout3->addWidget(keepDecreasedButton);
    buttonLayout3->addWidget(unrollbackButton);
    layout->addLayout(buttonLayout3);

    keepValueButton->setFixedWidth(80);
    traceWriteButton->setFixedWidth(80);
    saveAbsButton->setFixedWidth(80);
    saveRelButton->setFixedWidth(80);

    saveLayout->addWidget(keepValueButton);
    saveLayout->addWidget(traceWriteButton);
    saveLayout->addWidget(label);
    saveLayout->addWidget(tagEdit);
    saveLayout->addWidget(saveAbsButton);
    saveLayout->addWidget(saveRelButton);
    layout->addLayout(saveLayout);

    scriptInputLayout->addWidget(scriptInputLabel);
    scriptInputLayout->addWidget(scriptInputEdit);
    layout->addLayout(scriptInputLayout);

    scriptBtnLayout->addWidget(scriptBtn1);
    scriptBtnLayout->addWidget(scriptBtn2);
    scriptBtnLayout->addWidget(scriptBtn3);
    scriptBtnLayout->addWidget(scriptBtn4);
    layout->addLayout(scriptBtnLayout);

    execButton->setFixedWidth(80);

    layout->addWidget(listWidget);
    commandLayout->addWidget(commandEdit);
    commandLayout->addWidget(execButton);
    layout->addLayout(commandLayout);

    dockWidgetContents->setLayout(layout);

    setWidget(dockWidgetContents);

    scriptInputLabel->hide();
    scriptInputEdit->hide();
    scriptBtn1->hide();
    scriptBtn2->hide();
    scriptBtn3->hide();
    scriptBtn4->hide();
    dockWidgetContents->show();

    using namespace std::placeholders;
    QObject::connect(enableCheckBox, &QCheckBox::stateChanged, std::bind(&DataAnalystWidget::OnEnableStateChanged, this, _1));
    QObject::connect(runButton, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnRunScript, this));
    QObject::connect(clearAllButton, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnClearAll, this));
    QObject::connect(addSniffingButton, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnAddSniffing, this));
    QObject::connect(keepUnchangedButton, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnKeepUnchanged, this));
    QObject::connect(keepChangedButton, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnKeepChanged, this));
    QObject::connect(keepIncreasedButton, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnKeepIncreased, this));
    QObject::connect(keepDecreasedButton, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnKeepDecreased, this));
    QObject::connect(rollbackButton, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnRollback, this));
    QObject::connect(unrollbackButton, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnUnrollback, this));
    QObject::connect(keepValueButton, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnKeepValue, this));
    QObject::connect(traceWriteButton, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnTraceWrite, this));
    QObject::connect(saveAbsButton, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnSaveAbs, this));
    QObject::connect(saveRelButton, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnSaveRel, this));
    QObject::connect(scriptBtn1, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnScriptBtn1, this));
    QObject::connect(scriptBtn2, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnScriptBtn2, this));
    QObject::connect(scriptBtn3, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnScriptBtn3, this));
    QObject::connect(scriptBtn4, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnScriptBtn4, this));
    QObject::connect(execButton, &QPushButton::pressed, std::bind(&DataAnalystWidget::OnExecCmd, this));
    updateTimer.setInterval(30);
    connect(&updateTimer, &QTimer::timeout, this, std::bind(&DataAnalystWidget::OnUpdate, this));

    startAddrEdit->setInputMask(tr("0xhhhhhhhhhhhhhhhh"));
    sizeAddrEdit->setInputMask(tr("0xhhhhhhhhhhhhhhhh"));
    pidEdit->setInputMask(tr("0xhhhhhhhh"));
    stepAddrEdit->setInputMask(tr("0"));
    curValueEdit->setInputMask(tr("0xhhhhhhhhhhhhhhhh"));
    pidEdit->setText(tr("0x0"));
    stepAddrEdit->setText(tr("4"));

    Core::g_MainThreadCaller.Init(*this);
    BraceScriptInterpreter::Init(new BraceApiProvider(*this));
    InitCmdDocs();

    new QListWidgetItem(tr("[help command]:help filter, search commands or apis"), listWidget);
}

DataAnalystWidget::~DataAnalystWidget() {
    if(updateTimer.isActive()){
        updateTimer.stop();
    }
    BraceScriptInterpreter::Release();
}

void DataAnalystWidget::showEvent(QShowEvent* ev) {
    updateTimer.start();
}

void DataAnalystWidget::hideEvent(QHideEvent* ev) {
    updateTimer.stop();
}

void DataAnalystWidget::closeEvent(QCloseEvent* event) {
    QDockWidget::closeEvent(event);
}

void DataAnalystWidget::OnUpdate() {
    Core::g_MainThreadCaller.TickWork();
    BraceScriptInterpreter::Tick();

    uint64_t curTime = BraceScriptInterpreter::GetTimeUs();
    if (lastTime == 0) {
        lastTime = curTime;
    }
    else if (curTime - lastTime >= screenCaptureInterval * 1000) {
        CaptureScreen();
        lastTime = curTime;
    }
}

void DataAnalystWidget::OnRunScript() {
    auto&& fileName = QFileDialog::getOpenFileName(this, tr("choose script file"), tr("."), tr("script files (*.scp *.txt)"));
    if (fileName.isEmpty())
        return;
    if(!updateTimer.isActive()){
        updateTimer.start();
    }
    auto&& strFileName = fileName.toStdString();
    BraceScriptInterpreter::Exec(("load " + strFileName).c_str());
    FocusRenderWindow();
}

void DataAnalystWidget::OnExecCmd() {
    if(!updateTimer.isActive()){
        updateTimer.start();
    }
    BraceScriptInterpreter::Exec(commandEdit->text().toStdString().c_str());
    FocusRenderWindow();
}

void DataAnalystWidget::OnEnableStateChanged(int state) {
    auto&& sniffer = system.MemorySniffer();
    sniffer.SetEnable(state != 0);

    if (state) {
        RefreshMemoryArgs();
    }
    FocusRenderWindow();
}

void DataAnalystWidget::OnClearAll() {
    int ret = QMessageBox::question(this, tr("Question"), tr("Are you sure?"), QMessageBox::Yes | QMessageBox::No);
    if (ret == QMessageBox::Yes) {
        auto&& sniffer = system.MemorySniffer();
        sniffer.ClearAll();

        ClearResultList();
        BraceScriptInterpreter::Send("OnClearAll");
    }
    FocusRenderWindow();
}

void DataAnalystWidget::OnAddSniffing() {
    const uint64_t c_max_data_count = 1000000;
    auto&& sniffer = system.MemorySniffer();

    std::string startStr = startAddrEdit->text().toStdString();
    std::string sizeStr = sizeAddrEdit->text().toStdString();
    std::string stepStr = stepAddrEdit->text().toStdString();
    std::string valueStr = curValueEdit->text().toStdString();
    std::string pidStr = pidEdit->text().toStdString();

    uint64_t start = std::strtoull(startStr.c_str(), nullptr, 0);
    uint64_t size = std::strtoull(sizeStr.c_str(), nullptr, 0);
    uint64_t step = std::strtoull(stepStr.c_str(), nullptr, 0);
    uint64_t val = std::strtoull(valueStr.c_str(), nullptr, 0);
    uint64_t pid = std::strtoull(pidStr.c_str(), nullptr, 0);

    if (step <= 0) {
        QMessageBox::warning(this, tr("Warning"), tr("step must be 1|2|4|8"), QMessageBox::Ok);
        return;
    }
    if (size / step > c_max_data_count && val == 0) {
        int ret = QMessageBox::question(this, tr("Question"), tr("So many datas, are you sure?"),
                                        QMessageBox::Yes | QMessageBox::No);
        if (ret != QMessageBox::Yes) {
            return;
        }
    }

    sniffer.AddSniffing(pid, start, size, step, val);

    RefreshResultList("Sniffing");
    BraceScriptInterpreter::Send("OnAddSniffing");
    FocusRenderWindow();
}

void DataAnalystWidget::OnKeepUnchanged() {
    auto&& sniffer = system.MemorySniffer();
    sniffer.RefreshSnapshot();
    sniffer.KeepUnchanged();

    RefreshResultList("KeepUnchanged");
    BraceScriptInterpreter::Send("OnKeepUnchanged");
    FocusRenderWindow();
}

void DataAnalystWidget::OnKeepChanged() {
    auto&& sniffer = system.MemorySniffer();
    sniffer.RefreshSnapshot();
    sniffer.KeepChanged();

    RefreshResultList("KeepChanged");
    BraceScriptInterpreter::Send("OnKeepChanged");
    FocusRenderWindow();
}

void DataAnalystWidget::OnKeepIncreased() {
    auto&& sniffer = system.MemorySniffer();
    sniffer.RefreshSnapshot();
    sniffer.KeepIncreased();

    RefreshResultList("KeepIncreased");
    BraceScriptInterpreter::Send("OnKeepIncreased");
    FocusRenderWindow();
}

void DataAnalystWidget::OnKeepDecreased() {
    auto&& sniffer = system.MemorySniffer();
    sniffer.RefreshSnapshot();
    sniffer.KeepDecreased();

    RefreshResultList("KeepDecreased");
    BraceScriptInterpreter::Send("OnKeepDecreased");
    FocusRenderWindow();
}

void DataAnalystWidget::OnRollback() {
    auto&& sniffer = system.MemorySniffer();
    sniffer.Rollback();

    RefreshResultList("Rollback");
    BraceScriptInterpreter::Send("OnRollback");
    FocusRenderWindow();
}

void DataAnalystWidget::OnUnrollback() {
    auto&& sniffer = system.MemorySniffer();
    sniffer.Unrollback();

    RefreshResultList("Unrollback");
    BraceScriptInterpreter::Send("OnUnrollback");
    FocusRenderWindow();
}

void DataAnalystWidget::OnKeepValue() {
    std::string curVal = curValueEdit->text().toStdString();
    uint64_t val = std::strtoull(curVal.c_str(), nullptr, 0);

    auto&& sniffer = system.MemorySniffer();
    sniffer.RefreshSnapshot();
    sniffer.KeepValue(val);

    RefreshResultList("KeepValue");
    BraceScriptInterpreter::MessageArgs args;
    args.push_back(val);
    BraceScriptInterpreter::Send("OnKeepValue", std::move(args));
    FocusRenderWindow();
}

void DataAnalystWidget::OnTraceWrite() {
    auto&& sniffer = system.MemorySniffer();
    sniffer.AddToTraceWrite();

    RefreshResultList("TraceWrite");
    BraceScriptInterpreter::Send("OnTraceWrite");
    FocusRenderWindow();
}

void DataAnalystWidget::OnSaveAbs() {
    auto&& fileName = QFileDialog::getSaveFileName(this, tr("choose save file"), tr("."), tr("text files (*.txt)"));
    if (fileName.isEmpty())
        return;
    auto&& strFileName = fileName.toStdString();
    auto&& tagName = tagEdit->text().toStdString();

    auto&& sniffer = system.MemorySniffer();
    sniffer.SaveAbsAsCheatVM(strFileName.c_str(), tagName.c_str());
    FocusRenderWindow();
}

void DataAnalystWidget::OnSaveRel() {
    auto&& fileName = QFileDialog::getSaveFileName(this, tr("choose save file"), tr("."), tr("text files (*.txt)"));
    if (fileName.isEmpty())
        return;
    auto&& strFileName = fileName.toStdString();
    auto&& tagName = tagEdit->text().toStdString();

    auto&& sniffer = system.MemorySniffer();
    sniffer.SaveRelAsCheatVM(strFileName.c_str(), tagName.c_str());
    FocusRenderWindow();
}

void DataAnalystWidget::OnScriptBtn1() {
    BraceScriptInterpreter::Send("onscriptbtn1");
    FocusRenderWindow();
}

void DataAnalystWidget::OnScriptBtn2() {
    BraceScriptInterpreter::Send("onscriptbtn2");
    FocusRenderWindow();
}

void DataAnalystWidget::OnScriptBtn3() {
    BraceScriptInterpreter::Send("onscriptbtn3");
    FocusRenderWindow();
}

void DataAnalystWidget::OnScriptBtn4() {
    BraceScriptInterpreter::Send("onscriptbtn4");
    FocusRenderWindow();
}

void DataAnalystWidget::OnKeyPress(int modifier, int key) {
    BraceScriptInterpreter::MessageArgs args;
    args.push_back(modifier);
    args.push_back(key);
    BraceScriptInterpreter::Send("onkeypress", std::move(args));
}

void DataAnalystWidget::OnKeyRelease(int modifier, int key) {
    BraceScriptInterpreter::MessageArgs args;
    args.push_back(modifier);
    args.push_back(key);
    BraceScriptInterpreter::Send("onkeyrelease", std::move(args));
}

void DataAnalystWidget::OnMousePress(int x, int y, int button) {
    mouseX = x;
    mouseY = y;
    BraceScriptInterpreter::MessageArgs args;
    args.push_back(x);
    args.push_back(y);
    args.push_back(button);
    BraceScriptInterpreter::Send("onmousepress", std::move(args));
}

void DataAnalystWidget::OnMouseRelease(int button) {
    BraceScriptInterpreter::MessageArgs args;
    args.push_back(button);
    BraceScriptInterpreter::Send("onmouserelease", std::move(args));
}

void DataAnalystWidget::OnMouseMove(int x, int y) {
    mouseX = x;
    mouseY = y;
}

void DataAnalystWidget::OnMouseWheel(int x, int y) {

}

void DataAnalystWidget::OnTouchPress(int x, int y, int id) {

}

void DataAnalystWidget::OnTouchUpdateBegin() {

}

void DataAnalystWidget::OnTouchMove(int x, int y, int id) {

}

void DataAnalystWidget::OnTouchUpdateEnd() {

}

void DataAnalystWidget::OnTouchEnd() {

}

void DataAnalystWidget::ClearResultList() {
    listWidget->clear();
}

void DataAnalystWidget::SetSniffingScope(const std::string& sectionId) {
    const uint64_t c_max_size = 0x200000000ull;

    auto&& sniffer = system.MemorySniffer();
    if (system.ApplicationProcess()) {
        sniffer.VisitMemoryArgs(
            [=](auto name, auto id, u64 base, u64 addr, u64 size, u64 progId, u64 pid) {
            std::stringstream ss;
            if (name == sectionId || id == sectionId) {
                ss.str("");
                ss << "0x" << std::hex << base;
                startAddrEdit->setText(tr(ss.str().c_str()));

                ss.str("");
                ss << "0x" << std::hex << (size < c_max_size ? size : c_max_size);
                sizeAddrEdit->setText(tr(ss.str().c_str()));

                ss.str("");
                ss << "0x" << std::hex << pid;
                pidEdit->setText(tr(ss.str().c_str()));
            }
        });
    }
}

void DataAnalystWidget::RefreshMemoryArgs() {
    static std::string s_SearchSection("alias");
    const uint64_t c_max_size = 0x200000000ull;

    auto&& sniffer = system.MemorySniffer();
    if (system.ApplicationProcess()) {
        u64 title_id = system.GetApplicationProcessProgramID();
        Core::System::CurrentBuildProcessID build_id = system.GetApplicationProcessBuildID();
        const auto build_id_raw = Common::HexToString(build_id, true);
        auto build_id_str = build_id_raw.substr(0, sizeof(u64) * 2);
        QString qstr =
            QString::fromStdString("title_id:") + QString::asprintf("%16.16llx", title_id);
        new QListWidgetItem(qstr, listWidget);
        QString qstr2 =
            QString::fromStdString("build_id:") + QString::fromStdString(build_id_str);
        new QListWidgetItem(qstr2, listWidget);
        sniffer.VisitMemoryArgs(
            [=](auto name, auto id, u64 base, u64 addr, u64 size, u64 progId, u64 pid) {
            std::stringstream ss;
            ss << "name:";
            ss << name;
            ss << " id:";
            ss << id;
            ss << ", base:";
            ss << std::hex << base;
            ss << ", addr:";
            ss << std::hex << addr;
            ss << ", size:";
            ss << std::hex << size;
            ss << ", program id:";
            ss << std::hex << progId;
            ss << ", pid:";
            ss << std::hex << pid;
            new QListWidgetItem(tr(ss.str().c_str()), listWidget);

            if (id == s_SearchSection) {
                auto txt1 = startAddrEdit->text();
                auto txt2 = sizeAddrEdit->text();
                auto txt3 = pidEdit->text();
                if (txt1.isEmpty()) {
                    ss.str("");
                    ss << "0x" << std::hex << base;
                    startAddrEdit->setText(tr(ss.str().c_str()));
                }
                if (txt2.isEmpty()) {
                    ss.str("");
                    ss << "0x" << std::hex << (size < c_max_size ? size : c_max_size);
                    sizeAddrEdit->setText(tr(ss.str().c_str()));
                }
                if (txt3.isEmpty()) {
                    ss.str("");
                    ss << "0x" << std::hex << pid;
                    pidEdit->setText(tr(ss.str().c_str()));
                }
            }
        });
    }
    new QListWidgetItem(tr("[set sniffing scope]:setsniffingscope section_id_or_name"), listWidget);
    new QListWidgetItem(tr("use findmem([val1,val2,...]) command to get a smaller memory range"), listWidget);
    new QListWidgetItem(tr("or"), listWidget);
    new QListWidgetItem(tr("use searchmem([val1,val2,...]) command to get a smaller memory range"), listWidget);
    new QListWidgetItem(tr("[mem search scope commands]:clearmemscope, setmemscope main, setmemscopebegin 0x80004000, setmemscopeend 0x87000000, setmempid 0x51"), listWidget);
    new QListWidgetItem(tr("[mem search arg commands]:setmemstep 4, setmemsize 4, setmemrange 256, setmemcount 10, showmem(0x21593f0000, 200)"), listWidget);
    new QListWidgetItem(tr("[mem read/write commands]:echo(readmemory(0x21593f0000, 4)), writememory(0x21593f0000, 127[, 1|2|4|8])"), listWidget);
    new QListWidgetItem(tr("[help command]:help filter, search commands or apis"), listWidget);
}

void DataAnalystWidget::RefreshResultList(const char* tag, bool full) {
    auto&& sniffer = system.MemorySniffer();
    auto&& result = sniffer.GetResultMemoryModifyInfo();
    int ct = static_cast<int>(result.size());
    int hct = sniffer.GetHistoryMemoryModifyInfoCount();
    int rct = sniffer.GetRollbackMemoryModifyInfoCount();

    int maxCount = maxRecords;
    if (full)
        maxCount = maxResultList;

    std::stringstream ss;
    ss << "===[" << tag << "]===";
    new QListWidgetItem(tr(ss.str().c_str()), listWidget);

    ss.str("");
    ss << std::dec;
    ss << "history count:";
    ss << hct;
    ss << " [";
    int st = hct - maxHistories;
    if (st < 0)
        st = 0;
    for (int i = st; i < hct; ++i) {
        if (i > st)
            ss << ",";
        auto&& history = sniffer.GetHistoryMemoryModifyInfo(i);
        ss << i << ":" << history.size();
    }
    ss << "]";
    new QListWidgetItem(tr(ss.str().c_str()), listWidget);

    ss.str("");
    ss << "rollback count:";
    ss << rct;
    ss << " [";
    for (int i = 0; i < rct && i < maxRollbacks; ++i) {
        if (i > 0)
            ss << ",";
        auto&& rollback = sniffer.GetRollbackMemoryModifyInfo(i);
        ss << i << ":" << rollback.size();
    }
    ss << "]";
    new QListWidgetItem(tr(ss.str().c_str()), listWidget);

    ss.str("");
    ss << "result count:";
    ss << std::to_string(ct);
    new QListWidgetItem(tr(ss.str().c_str()), listWidget);

    auto&& it = result.begin();
    for (int i = 0; i < maxCount && i < ct; ++i, ++it) {
        ss.str("no:");
        ss << std::to_string(i);
        ss << " vaddr:";
        ss << std::hex << it->second->addr.GetValue();
        ss << " type:" << std::dec << it->second->type;
        ss << " val:";
        ss << std::hex;
        switch (it->second->type) {
        case Core::Memory::MemoryModifyInfo::type_u8:
            ss << static_cast<u16>(it->second->u8Val);
            break;
        case Core::Memory::MemoryModifyInfo::type_u16:
            ss << it->second->u16Val;
            break;
        case Core::Memory::MemoryModifyInfo::type_u32:
            ss << it->second->u32Val;
            break;
        case Core::Memory::MemoryModifyInfo::type_u64:
            ss << it->second->u64Val;
            break;
        }
        ss << " old val:";
        switch (it->second->type) {
        case Core::Memory::MemoryModifyInfo::type_u8:
            ss << static_cast<u16>(it->second->u8OldVal);
            break;
        case Core::Memory::MemoryModifyInfo::type_u16:
            ss << it->second->u16OldVal;
            break;
        case Core::Memory::MemoryModifyInfo::type_u32:
            ss << it->second->u32OldVal;
            break;
        case Core::Memory::MemoryModifyInfo::type_u64:
            ss << it->second->u64OldVal;
            break;
        }
        ss << std::dec;
        ss << " size:" << it->second->size;
        new QListWidgetItem(tr(ss.str().c_str()), listWidget);
    }

    RemoveExcessResults();
}

void DataAnalystWidget::RemoveExcessResults() {
    while (listWidget->count() > maxResultList) {
        auto* pItem = listWidget->takeItem(0);
        if (nullptr != pItem) {
            delete pItem;
        }
        else {
            break;
        }
    }
}

void DataAnalystWidget::CaptureScreen() {
    if (!captureEnabled)
        return;
    auto& renderer = system.Renderer();
    const f32 res_scale = Settings::values.resolution_info.up_factor;

    if (renderer.IsScreenshotPending()) {
        return;
    }

    uint64_t stTime = BraceScriptInterpreter::GetTimeUs();
    const Layout::FramebufferLayout slayout{Layout::FrameLayoutFromResolutionScale(res_scale)};
    if (screenImage.width() != static_cast<int>(slayout.width) || screenImage.height() != static_cast<int>(slayout.height)) {
        screenImage = QImage(slayout.width, slayout.height, QImage::Format_RGB32);
    }
    renderer.RequestScreenshot(
        screenImage.bits(),
        [stTime, this](bool invert_y) {
        screenImage = std::move(screenImage).mirrored(false, invert_y);
        uint64_t edTime = BraceScriptInterpreter::GetTimeUs();
        if (logCaptureTimeConsuming) {
            AddLog("capture: " + std::to_string(edTime - stTime) + "us");
        }
    }, slayout);
}


void DataAnalystWidget::FocusRenderWindow() {
    if (!renderWindow->hasFocus())
        renderWindow->setFocus();
}

void DataAnalystWidget::ShowButtonParam(int index) {
    auto&& hidCore = system.HIDCore();
    auto* player_1 = hidCore.GetEmulatedController(Core::HID::NpadIdType::Player1);
    auto* handheld = hidCore.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    auto* controller = handheld->IsConnected() ? handheld : player_1;

    auto&& params = controller->GetButtonParam(index);
    AddLog("[button " + std::to_string(index) + "]: " + params.Serialize());
    FocusRenderWindow();
}

void DataAnalystWidget::ShowStickParam(int index) {
    auto&& hidCore = system.HIDCore();
    auto* player_1 = hidCore.GetEmulatedController(Core::HID::NpadIdType::Player1);
    auto* handheld = hidCore.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    auto* controller = handheld->IsConnected() ? handheld : player_1;

    auto&& params = controller->GetStickParam(index);
    AddLog("[stick " + std::to_string(index) + "]: " + params.Serialize());
    FocusRenderWindow();
}

void DataAnalystWidget::ShowMotionParam(int index) {
    auto&& hidCore = system.HIDCore();
    auto* player_1 = hidCore.GetEmulatedController(Core::HID::NpadIdType::Player1);
    auto* handheld = hidCore.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    auto* controller = handheld->IsConnected() ? handheld : player_1;

    auto&& params = controller->GetMotionParam(index);
    AddLog("[motion " + std::to_string(index) + "]: " + params.Serialize());
    FocusRenderWindow();
}

void DataAnalystWidget::ShowInputState() {
    auto&& hidCore = system.HIDCore();
    auto* player_1 = hidCore.GetEmulatedController(Core::HID::NpadIdType::Player1);
    auto* handheld = hidCore.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    auto* controller = handheld->IsConnected() ? handheld : player_1;

    auto&& npadButton = controller->GetNpadButtons();
    auto&& homeButton = controller->GetHomeButtons();
    auto&& capButton = controller->GetCaptureButtons();
    auto&& sticks = controller->GetSticks();
    auto&& motions = controller->GetMotions();

    std::stringstream ss;
    ss << "[input state]:" << std::endl;

    if (npadButton.a)
        ss << " a";
    if (npadButton.b)
        ss << " b";
    if (npadButton.x)
        ss << " x";
    if (npadButton.y)
        ss << " y";
    if (npadButton.stick_l)
        ss << " stick_l";
    if (npadButton.stick_r)
        ss << " stick_r";
    if (npadButton.l)
        ss << " l";
    if (npadButton.r)
        ss << " r";
    if (npadButton.zl)
        ss << " zl";
    if (npadButton.zr)
        ss << " zr";
    if (npadButton.plus)
        ss << " plus";
    if (npadButton.minus)
        ss << " minus";
    if (npadButton.left)
        ss << " left";
    if (npadButton.up)
        ss << " up";
    if (npadButton.right)
        ss << " right";
    if (npadButton.down)
        ss << " down";
    if (npadButton.stick_l_left)
        ss << " stick_l_left";
    if (npadButton.stick_l_up)
        ss << " stick_l_up";
    if (npadButton.stick_l_right)
        ss << " stick_l_right";
    if (npadButton.stick_l_down)
        ss << " stick_l_down";
    if (npadButton.stick_r_left)
        ss << " stick_r_left";
    if (npadButton.stick_r_up)
        ss << " stick_r_up";
    if (npadButton.stick_r_right)
        ss << " stick_r_right";
    if (npadButton.stick_r_down)
        ss << " stick_r_down";
    if (npadButton.left_sl)
        ss << " left_sl";
    if (npadButton.left_sr)
        ss << " left_sr";
    if (npadButton.right_sl)
        ss << " right_sl";
    if (npadButton.right_sr)
        ss << " right_sr";
    if (npadButton.palma)
        ss << " palma";
    if (npadButton.verification)
        ss << " verification";
    if (npadButton.handheld_left_b)
        ss << " handheld_left_b";
    if (npadButton.lagon_c_left)
        ss << " lagon_c_left";
    if (npadButton.lagon_c_up)
        ss << " lagon_c_up";
    if (npadButton.lagon_c_right)
        ss << " lagon_c_right";
    if (npadButton.lagon_c_down)
        ss << " lagon_c_down";
    if (homeButton.home)
        ss << " home";
    if (capButton.capture)
        ss << " capture";

    ss << std::endl;

    ss << " stick left:" << sticks.left.x << ", " << sticks.left.y << std::endl;
    ss << " stick right:" << sticks.right.x << ", " << sticks.right.y << std::endl;

    auto&& mo_left = motions[0];
    auto&& mo_right = motions[1];
    ss << "motion left: gyro(" << mo_left.gyro.x << ", " << mo_left.gyro.y << ", " << mo_left.gyro.z << ") accel(" << mo_left.accel.x << ", " << mo_left.accel.y << ", " << mo_left.accel.z << ") " << mo_left.is_at_rest << std::endl;
    ss << "motion right: gyro(" << mo_right.gyro.x << ", " << mo_right.gyro.y << ", " << mo_right.gyro.z << ") accel(" << mo_right.accel.x << ", " << mo_right.accel.y << ", " << mo_right.accel.z << ") " << mo_right.is_at_rest << std::endl;

    AddLog(ss.str());
    FocusRenderWindow();
}

void DataAnalystWidget::SaveResultList(const std::string& file_path) const {
    std::ofstream of(BraceScriptInterpreter::get_absolutely_path(file_path), std::ios::out);
    if (of.fail())
        return;
    int ct = listWidget->count();
    for (int ix = 0; ix < ct; ++ix) {
        auto* pItem = listWidget->item(ix);
        if (nullptr != pItem) {
            of << pItem->text().toStdString() << std::endl;
        }
    }
}

void DataAnalystWidget::AddLog(const std::string& info) {
    new QListWidgetItem(tr(info.c_str()), listWidget);

    RemoveExcessResults();
}

void DataAnalystWidget::Reset() {
    startAddrEdit->setText(tr(""));
    sizeAddrEdit->setText(tr(""));
    curValueEdit->setText(tr(""));
    stepAddrEdit->setText(tr("4"));
    pidEdit->setText(tr("0x0"));
}

void DataAnalystWidget::EnableSniffer() {
    enableCheckBox->setChecked(true);
}

void DataAnalystWidget::DisableSniffer() {
    enableCheckBox->setChecked(false);
}

uint32_t DataAnalystWidget::GetPixel(int x, int y)const {
    return screenImage.pixel(x, y);
}

bool DataAnalystWidget::GetCursorPos(int& x, int& y)const {
    x = mouseX;
    y = mouseY;
    return true;
}

bool DataAnalystWidget::GetScreenSize(int& x, int& y)const {
    const f32 res_scale = Settings::values.resolution_info.up_factor;
    const Layout::FramebufferLayout slayout{Layout::FrameLayoutFromResolutionScale(res_scale)};
    x = static_cast<int>(slayout.width);
    y = static_cast<int>(slayout.height);
    return true;
}

std::string DataAnalystWidget::ReadButtonParam(int index)const {
    auto&& hidCore = system.HIDCore();
    auto* player_1 = hidCore.GetEmulatedController(Core::HID::NpadIdType::Player1);
    auto* handheld = hidCore.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    auto* controller = handheld->IsConnected() ? handheld : player_1;

    *paramPackage = controller->GetButtonParam(index);
    return paramPackage->Serialize();
}

std::string DataAnalystWidget::ReadStickParam(int index)const {
    auto&& hidCore = system.HIDCore();
    auto* player_1 = hidCore.GetEmulatedController(Core::HID::NpadIdType::Player1);
    auto* handheld = hidCore.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    auto* controller = handheld->IsConnected() ? handheld : player_1;

    *paramPackage = controller->GetStickParam(index);
    return paramPackage->Serialize();
}

std::string DataAnalystWidget::ReadMotionParam(int index)const {
    auto&& hidCore = system.HIDCore();
    auto* player_1 = hidCore.GetEmulatedController(Core::HID::NpadIdType::Player1);
    auto* handheld = hidCore.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    auto* controller = handheld->IsConnected() ? handheld : player_1;

    *paramPackage = controller->GetMotionParam(index);
    return paramPackage->Serialize();
}

void DataAnalystWidget::ReadParamPackage(const std::string& str)const {
    *paramPackage = Common::ParamPackage(str);
}

bool DataAnalystWidget::HasParam(const std::string& key)const {
    return paramPackage->Has(key);
}

int DataAnalystWidget::GetIntParam(const std::string& key, int def)const {
    return paramPackage->Get(key, def);
}

float DataAnalystWidget::GetFloatParam(const std::string& key, float def)const {
    return paramPackage->Get(key, def);
}

std::string DataAnalystWidget::GetStrParam(const std::string& key, const std::string& def)const {
    return paramPackage->Get(key, def);
}

void DataAnalystWidget::KeyPress(int m, int k) {
    FocusRenderWindow();
    const auto modifier = renderWindow->QtModifierToSwitchModifier(static_cast<Qt::KeyboardModifier>(m));
    const auto key = renderWindow->QtKeyToSwitchKey(static_cast<Qt::Key>(k));
    inputSubSystem->GetKeyboard()->SetKeyboardModifiers(modifier);
    inputSubSystem->GetKeyboard()->PressKeyboardKey(key);
    // This is used for gamepads that can have any key mapped
    inputSubSystem->GetKeyboard()->PressKey(k);
}

void DataAnalystWidget::KeyRelease(int m, int k) {
    FocusRenderWindow();
    const auto modifier = renderWindow->QtModifierToSwitchModifier(static_cast<Qt::KeyboardModifier>(m));
    const auto key = renderWindow->QtKeyToSwitchKey(static_cast<Qt::Key>(k));
    inputSubSystem->GetKeyboard()->SetKeyboardModifiers(modifier);
    inputSubSystem->GetKeyboard()->ReleaseKeyboardKey(key);
    // This is used for gamepads that can have any key mapped
    inputSubSystem->GetKeyboard()->ReleaseKey(k);
}

void DataAnalystWidget::MousePress(int px, int py, int btn) {
    FocusRenderWindow();
    const auto [x, y] = renderWindow->ScaleTouch(QPointF(px, py));
    const auto [touch_x, touch_y] = renderWindow->MapToTouchScreen(x, y);
    const auto button = renderWindow->QtButtonToMouseButton(static_cast<Qt::MouseButton>(btn));

    inputSubSystem->GetMouse()->PressMouseButton(button);
    inputSubSystem->GetMouse()->PressButton(px, py, button);
    inputSubSystem->GetMouse()->PressTouchButton(touch_x, touch_y, button);
}

void DataAnalystWidget::MouseRelease(int btn) {
    FocusRenderWindow();
    const auto button = renderWindow->QtButtonToMouseButton(static_cast<Qt::MouseButton>(btn));
    inputSubSystem->GetMouse()->ReleaseButton(button);
}

void DataAnalystWidget::MouseMove(int px, int py) {
    FocusRenderWindow();
    const auto [x, y] = renderWindow->ScaleTouch(QPointF(px, py));
    const auto [touch_x, touch_y] = renderWindow->MapToTouchScreen(x, y);
    const int center_x = width() / 2;
    const int center_y = height() / 2;

    inputSubSystem->GetMouse()->MouseMove(touch_x, touch_y);
    inputSubSystem->GetMouse()->TouchMove(touch_x, touch_y);
    inputSubSystem->GetMouse()->Move(px, py, center_x, center_y);
}

void DataAnalystWidget::MouseWheelChange(int x, int y) {
    FocusRenderWindow();
    inputSubSystem->GetMouse()->MouseWheelChange(x, y);
}

void DataAnalystWidget::TouchPress(int px, int py, int id) {
    FocusRenderWindow();
    const auto [x, y] = renderWindow->ScaleTouch(QPointF(px, py));
    const auto [touch_x, touch_y] = renderWindow->MapToTouchScreen(x, y);
    inputSubSystem->GetTouchScreen()->TouchPressed(touch_x, touch_y, id);
}

void DataAnalystWidget::TouchUpdateBegin() {
    FocusRenderWindow();
    inputSubSystem->GetTouchScreen()->ClearActiveFlag();
}

void DataAnalystWidget::TouchMove(int px, int py, int id) {
    FocusRenderWindow();
    const auto [x, y] = renderWindow->ScaleTouch(QPointF(px, py));
    const auto [touch_x, touch_y] = renderWindow->MapToTouchScreen(x, y);
    inputSubSystem->GetTouchScreen()->TouchMoved(touch_x, touch_y, id);
}

void DataAnalystWidget::TouchUpdateEnd() {
    FocusRenderWindow();
    inputSubSystem->GetTouchScreen()->ReleaseInactiveTouch();
}

void DataAnalystWidget::TouchEnd() {
    FocusRenderWindow();
    inputSubSystem->GetTouchScreen()->ReleaseAllTouch();
}

bool DataAnalystWidget::GetButtonState(int button_id) {
    auto&& hidCore = system.HIDCore();
    auto* player_1 = hidCore.GetEmulatedController(Core::HID::NpadIdType::Player1);
    auto* handheld = hidCore.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    auto* controller = handheld->IsConnected() ? handheld : player_1;
    auto&& npadButton = controller->GetNpadButtons();
    auto&& homeButton = controller->GetHomeButtons();
    auto&& capButton = controller->GetCaptureButtons();

    auto&& id = static_cast<InputCommon::VirtualGamepad::VirtualButton>(button_id);
    switch (id) {
    case InputCommon::VirtualGamepad::VirtualButton::ButtonA:
        if (npadButton.a)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::ButtonB:
        if (npadButton.b)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::ButtonX:
        if (npadButton.x)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::ButtonY:
        if (npadButton.y)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::StickL:
        if (npadButton.stick_l)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::StickR:
        if (npadButton.stick_r)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::TriggerL:
        if (npadButton.l)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::TriggerR:
        if (npadButton.r)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::TriggerZL:
        if (npadButton.zl)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::TriggerZR:
        if (npadButton.zr)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::ButtonPlus:
        if (npadButton.plus)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::ButtonMinus:
        if (npadButton.minus)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::ButtonLeft:
        if (npadButton.left)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::ButtonUp:
        if (npadButton.up)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::ButtonRight:
        if (npadButton.right)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::ButtonDown:
        if (npadButton.down)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::ButtonSL:
        if (npadButton.left_sl || npadButton.right_sl)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::ButtonSR:
        if (npadButton.left_sr || npadButton.right_sr)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::ButtonHome:
        if (homeButton.home)
            return true;
        break;
    case InputCommon::VirtualGamepad::VirtualButton::ButtonCapture:
        if (capButton.capture)
            return true;
        break;
    }
    return false;
}
void DataAnalystWidget::SetButtonState(std::size_t player_index, int button_id, bool value) {
    FocusRenderWindow();
    inputSubSystem->GetVirtualGamepad()->SetButtonState(player_index, button_id, value);
}
void DataAnalystWidget::SetStickPosition(std::size_t player_index, int axis_id, float x_value, float y_value) {
    FocusRenderWindow();
    inputSubSystem->GetVirtualGamepad()->SetStickPosition(player_index, axis_id, x_value, y_value);
}
void DataAnalystWidget::SetMotionState(std::size_t player_index, u64 delta_timestamp, float gyro_x, float gyro_y,
    float gyro_z, float accel_x, float accel_y, float accel_z) {
    FocusRenderWindow();
    inputSubSystem->GetVirtualGamepad()->SetMotionState(player_index, delta_timestamp, gyro_x, gyro_y, gyro_z, accel_x, accel_y, accel_z);
}
