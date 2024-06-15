#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/diag/diag.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"

namespace Service::Diag {

class IDetailDriver final : public ServiceFramework<IDetailDriver> {
public:
    explicit IDetailDriver(Core::System& system_) : ServiceFramework{system_, "detail"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "LogImpl"},
            {1, nullptr, "AbortImpl"},
            {2, nullptr, "AbortImpl1"}
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class IDiagDriver final : public ServiceFramework<IDiagDriver> {
public:
    explicit IDiagDriver(Core::System& system_) : ServiceFramework{system_, "diag"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetBacktrace"},
            {1, nullptr, "GetBacktrace1"},
            {2, nullptr, "GetSymbolName"},
            {3, nullptr, "GetRequiredBufferSizeForGetAllModuleInfo"},
            {4, nullptr, "GetAllModuleInfo"},
            {5, nullptr, "GetSymbolSize"}
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("diag", std::make_shared<IDiagDriver>(system));
    server_manager->RegisterNamedService("detail", std::make_shared<IDetailDriver>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::Diag
