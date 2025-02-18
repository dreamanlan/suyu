#include "core/core.h"
namespace Kernel {
    class KThread;
}
namespace Core {
    struct MainThreadCaller::Impl {
    };

    MainThreadCaller g_MainThreadCaller;

    MainThreadCaller::MainThreadCaller() :impl(nullptr) {}
    void MainThreadCaller::Init(DataAnalystWidget& widget) {}
    void MainThreadCaller::SyncLogToView(const std::string& info)const {}
    void MainThreadCaller::TickWork() {}
    void MainThreadCaller::RequestLogToView(std::string&& msg) {}
    void MainThreadCaller::RequestSyncCallback(const Kernel::KThread* pThread) { }
    void MainThreadCaller::RequestSyncCallback(int watchType, uint64_t addr, const Kernel::KThread* pThread) { }
    void MainThreadCaller::RequestSyncCallback(int watchType, uint64_t addr, std::size_t size, const Kernel::KThread* pThread) { }
}