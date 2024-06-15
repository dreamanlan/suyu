#pragma once

namespace Service::SM {
class ServiceManager;
}

namespace Core {
class System;
}

namespace Service::Diag {

void LoopProcess(Core::System& system);

} // namespace Service::Diag
