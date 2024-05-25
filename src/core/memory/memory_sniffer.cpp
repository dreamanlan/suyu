#include <iterator>
#include <list>
#include <locale>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "brace_script/brace_script_interpreter.h"
#include "common/hex_util.h"
#include "common/microprofile.h"
#include "common/swap.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/memory.h"
#include "core/memory/memory_sniffer.h"

namespace Core::Memory {

using MemoryModifyInfoList = std::list<MemoryModifyInfoMap>;
static MemoryModifyInfoMap g_InvalidMemModifyInfo;
static std::mutex g_session_lock;
static std::mutex g_breakpoint_lock;
static std::mutex g_trace_buffer_lock;
static std::mutex g_trace_pc_lock;

struct ModuleMemoryArg {
    std::string name;
    std::string buildId;
    u64 base;
    u64 addr;
    u64 size;
    u64 progId;
    u64 pid;
};
using ModuleMemArgs = std::vector<ModuleMemoryArg>;

struct SessionInfo {
    uint64_t id;
    std::string name;
    u32 handle;
};
using SessionNameMap = std::map<uint64_t, SessionInfo>;
using BreakPointInstructionMap = std::map<uint64_t, u32>;
using WatchPoints = std::unordered_set<uint64_t>;
using PcCountInfo = std::unordered_map<uint64_t, uint64_t>;
using PcCountMap = std::map<uint64_t, uint64_t>;
// pair<mask,value>, log when inst_op & mask == value
using LogInstructions = std::vector<std::pair<uint32_t, uint32_t>>;

static inline u32 BreakPointInstructionOn32() {
    // A32: trap
    // T32: trap + b #4
    return 0xe7ffdefe;
}

static inline u32 BreakPointInstructionOn64() {
    // A64: brk #0
    return 0xd4200000;
}

bool MemoryModifyInfo::IsIncreased() const {
    switch (type) {
    case type_u8:
        return u8OldVal < u8Val;
    case type_u16:
        return u16OldVal < u16Val;
    case type_u32:
        return u32OldVal < u32Val;
    case type_u64:
        return u64OldVal < u64Val;
    }
    return false;
}
bool MemoryModifyInfo::IsDecreased() const {
    switch (type) {
    case type_u8:
        return u8OldVal > u8Val;
    case type_u16:
        return u16OldVal > u16Val;
    case type_u32:
        return u32OldVal > u32Val;
    case type_u64:
        return u64OldVal > u64Val;
    }
    return false;
}
bool MemoryModifyInfo::IsChanged() const {
    switch (type) {
    case type_u8:
        return u8OldVal != u8Val;
    case type_u16:
        return u16OldVal != u16Val;
    case type_u32:
        return u32OldVal != u32Val;
    case type_u64:
        return u64OldVal != u64Val;
    }
    return false;
}
bool MemoryModifyInfo::IsUnchanged() const {
    switch (type) {
    case type_u8:
        return u8OldVal == u8Val;
    case type_u16:
        return u16OldVal == u16Val;
    case type_u32:
        return u32OldVal == u32Val;
    case type_u64:
        return u64OldVal == u64Val;
    }
    return false;
}

struct MemorySniffer::Impl {
    explicit Impl(Core::System& system_)
        : system{system_}, heapBase(0), heapSize(0), aliasStart(0), aliasSize(0), stackStart(0),
          stackSize(0), kernelStart(0), kernelSize(0), codeStart(0), codeSize(0), aliasCodeStart(0),
          aliasCodeSize(0), addrSpaceStart(0), addrSpaceSize(0), usePcCountArray(true),
          maxPcCount(10), memSearchProcessId(0), memSearchScopeBegin(0), memSearchScopeEnd(0),
          memSearchStep(4), memSearchValueSize(4), memSearchResultRange(1024),
          memSearchMaxCount(10), enabled(false), debugSnapshot(false), maxStepCount(20000),
          traceModule("main"), traceProcessId(0), traceScopeBegin(0), traceScopeEnd(0),
          startTraceAddr(0), stopTraceAddr(0), swiForTrace(-1), sessionHandle(0) {
        std::memset(pcCountArray, 0, sizeof(pcCountArray));
    }

    static const int c_max_pc_entry_num = 65536;
    static const int c_pc_num_per_entry = 8;
    static const u64 c_pc_hash_mask = 0x3ffffull;
    static const int c_pc_hash_shift = 0x2;
    static const u64 c_pc_other_mask = 0xfffffffffffc0000ull;
    static const int c_pc_max_count = 0x3ffff;

    Core::System& system;
    u64 heapBase;
    u64 heapSize;
    u64 aliasStart;
    u64 aliasSize;
    u64 stackStart;
    u64 stackSize;
    u64 kernelStart;
    u64 kernelSize;
    u64 codeStart;
    u64 codeSize;
    u64 aliasCodeStart;
    u64 aliasCodeSize;
    u64 addrSpaceStart;
    u64 addrSpaceSize;
    ModuleMemArgs moduleMemArgs;
    SessionNameMap sessionInfo;
    BreakPointInstructionMap breakPointInfo;
    PcCountInfo pcCountInfo;
    u64 pcCountArray[c_max_pc_entry_num * c_pc_num_per_entry];
    bool usePcCountArray;
    int maxPcCount;

    PcCountInfo lastPcCountInfo;
    PcCountMap orderedPcCounts;

    uint64_t memSearchProcessId;
    uint64_t memSearchScopeBegin;
    uint64_t memSearchScopeEnd;
    uint64_t memSearchStep;
    uint64_t memSearchValueSize;
    uint64_t memSearchResultRange;
    uint64_t memSearchMaxCount;

    bool enabled;
    bool debugSnapshot;

    uint64_t maxStepCount;
    WatchPoints traceAddrsOnRead;
    WatchPoints traceAddrsOnWrite;
    WatchPoints traceAddrsOnGetPointer;
    WatchPoints traceAddrsOnReadCString;

    LogInstructions logInstructions;
    std::string traceModule;
    uint64_t traceProcessId;
    uint64_t traceScopeBegin;
    uint64_t traceScopeEnd;
    uint64_t startTraceAddr;
    uint64_t stopTraceAddr;
    int swiForTrace;
    u32 sessionHandle;

    std::stringstream traceBuffer;

    MemoryModifyInfoMap resultMemModifyInfo;
    MemoryModifyInfoList historyMemModifyInfos;
    MemoryModifyInfoList rollbackMemModifyInfos;
};

MemorySniffer::MemorySniffer(Core::System& system_) : system{system_} {
    impl = std::make_unique<Impl>(system_);
}

MemorySniffer::~MemorySniffer() {}

void MemorySniffer::Initialize() {
    const auto& page_table = system.ApplicationProcess()->GetPageTable();
    impl->heapBase = GetInteger(page_table.GetHeapRegionStart());
    impl->heapSize = page_table.GetHeapRegionSize();
    impl->aliasStart = GetInteger(page_table.GetAliasRegionStart());
    impl->aliasSize = page_table.GetAliasRegionSize();
    impl->stackStart = GetInteger(page_table.GetStackRegionStart());
    impl->stackSize = page_table.GetStackRegionSize();
    impl->kernelStart = GetInteger(page_table.GetKernelMapRegionStart());
    impl->kernelSize = page_table.GetKernelMapRegionSize();
    impl->codeStart = GetInteger(page_table.GetCodeRegionStart());
    impl->codeSize = page_table.GetCodeRegionSize();
    impl->aliasCodeStart = GetInteger(page_table.GetAliasCodeRegionStart());
    impl->aliasCodeSize = page_table.GetAliasCodeRegionSize();
    impl->addrSpaceStart = GetInteger(page_table.GetAddressSpaceStart());
    impl->addrSpaceSize = page_table.GetAddressSpaceSize();
}

void MemorySniffer::ClearModuleMemoryParameters() {
    impl->moduleMemArgs.clear();
    impl->traceScopeBegin = 0;
    impl->traceScopeEnd = 0;
    impl->traceProcessId = 0;
    impl->traceModule = "main";
}

void MemorySniffer::AddModuleMemoryParameters(Kernel::KProcess& process, std::string&& file_name,
                                              std::string&& build_id, u64 base, u64 region_begin,
                                              u64 region_size) {
    if (file_name == impl->traceModule || build_id == impl->traceModule ||
        (impl->traceModule.empty() && file_name == "main")) {
        uint64_t addr_begin = region_begin;
        uint64_t addr_end = region_begin + region_size;
        if (impl->traceScopeBegin == 0 && impl->traceScopeBegin == impl->traceScopeEnd) {
            impl->traceScopeBegin = addr_begin;
            impl->traceScopeEnd = addr_end;
            impl->traceProcessId = process.GetProcessId();
        }

        for (int ix = 0; ix < static_cast<int>(Core::Hardware::NUM_CPU_CORES); ++ix) {
            auto* armIntf = process.GetArmInterface(ix);
            armIntf->InitJitOnceOnlyAfterJitCtor(addr_begin, addr_end);
        }
    }
    u64 progid = process.GetProgramId();
    u64 pid = process.GetProcessId();
    impl->moduleMemArgs.push_back(ModuleMemoryArg{std::move(file_name), std::move(build_id), base,
                                                  region_begin, region_size, progid, pid});
}

void MemorySniffer::VisitMemoryArgs(VisitMemoryArg visitor) const {
    if (nullptr == system.ApplicationProcess())
        return;
    for (auto&& minfo : impl->moduleMemArgs) {
        visitor(minfo.name.c_str(), minfo.buildId.c_str(), minfo.base, minfo.addr, minfo.size,
                minfo.progId, minfo.pid);
    }
    u64 progId = system.ApplicationProcess()->GetProgramId();
    u64 pid = system.ApplicationProcess()->GetProcessId();
    visitor("[app]", "heap", impl->heapBase, impl->heapBase, impl->heapSize, progId, pid);
    visitor("[app]", "alias", impl->aliasStart, impl->aliasStart, impl->aliasSize, progId, pid);
    visitor("[app]", "stack", impl->stackStart, impl->stackStart, impl->stackSize, progId, pid);
    visitor("[app]", "kernel map", impl->kernelStart, impl->kernelStart, impl->kernelSize, progId,
            pid);
    visitor("[app]", "code", impl->codeStart, impl->codeStart, impl->codeSize, progId, pid);
    visitor("[app]", "alias code", impl->aliasCodeStart, impl->aliasCodeStart, impl->aliasCodeSize,
            progId, pid);
    visitor("[app]", "addr space", impl->addrSpaceStart, impl->addrSpaceStart, impl->addrSpaceSize,
            progId, pid);
}

uint64_t MemorySniffer::GetHeapBase(uint64_t& size) const {
    size = impl->heapSize;
    return impl->heapBase;
}

uint64_t MemorySniffer::GetStackBase(uint64_t& size) const {
    size = impl->stackSize;
    return impl->stackStart;
}

uint64_t MemorySniffer::GetCodeBase(uint64_t& size) const {
    size = impl->codeSize;
    return impl->codeStart;
}

uint64_t MemorySniffer::GetAliasBase(uint64_t& size) const {
    size = impl->aliasSize;
    return impl->aliasStart;
}

uint64_t MemorySniffer::GetAliasCodeBase(uint64_t& size) const {
    size = impl->aliasCodeSize;
    return impl->aliasCodeStart;
}

int MemorySniffer::GetModuleCount() const {
    return static_cast<int>(impl->moduleMemArgs.size());
}

uint64_t MemorySniffer::GetModuleBase(int ix, uint64_t& addr, uint64_t& size, std::string& build_id,
                                      std::string& name, uint64_t& progId, uint64_t& pid) const {
    size = 0;
    build_id = std::string();
    if (ix < 0 || ix >= static_cast<int>(impl->moduleMemArgs.size()))
        return 0;
    auto&& arg = impl->moduleMemArgs[ix];
    addr = arg.addr;
    size = arg.size;
    build_id = arg.buildId;
    name = arg.name;
    progId = arg.progId;
    pid = arg.pid;
    return arg.base;
}

void MemorySniffer::ClearSessionInfos() {
    std::scoped_lock<std::mutex> lock(g_session_lock);

    impl->sessionInfo.clear();
}

void MemorySniffer::AddSessionInfo(uint64_t id, const std::string& name, u32 handle) {
    std::scoped_lock<std::mutex> lock(g_session_lock);

    auto&& it = impl->sessionInfo.find(id);
    if (it == impl->sessionInfo.end()) {
        SessionInfo info{.id = id, .name = name, .handle = handle};
        impl->sessionInfo.insert(std::make_pair(id, std::move(info)));
    } else {
        it->second.name = name;
        it->second.handle = handle;
    }
}

bool MemorySniffer::TryUpdateSession(uint64_t id, u32 handle) {
    std::scoped_lock<std::mutex> lock(g_session_lock);

    bool ret = false;
    auto&& it = impl->sessionInfo.find(id);
    if (it != impl->sessionInfo.end()) {
        it->second.handle = handle;
        ret = true;
    }
    return ret;
}

void MemorySniffer::ClearBreakPoints(Kernel::KProcess* process) {
    std::scoped_lock<std::mutex> lock(g_breakpoint_lock);

    if (nullptr != process) {
        for (auto&& pair : impl->breakPointInfo) {
            if (process->GetMemory().IsValidVirtualAddressRange(pair.first, sizeof(u32))) {
                process->GetMemory().Write32(pair.first, pair.second);
                Core::InvalidateInstructionCacheRange(process, pair.first, sizeof(u32));
            }
        }
    }
    impl->breakPointInfo.clear();
}

bool MemorySniffer::AddBreakPoint(Kernel::KProcess& process, uint64_t addr) {
    bool ret = false;
    if (process.GetMemory().IsValidVirtualAddressRange(addr, sizeof(u32))) {
        std::scoped_lock<std::mutex> lock(g_breakpoint_lock);

        auto&& phyCore = system.CurrentPhysicalCore();
        bool is32 = phyCore.IsAArch32();
        impl->breakPointInfo[addr] = process.GetMemory().Read32(addr);
        process.GetMemory().Write32(addr, is32 ? BreakPointInstructionOn32()
                                                      : BreakPointInstructionOn64());
        Core::InvalidateInstructionCacheRange(&process, addr, sizeof(u32));
        ret = true;
    }
    return ret;
}

bool MemorySniffer::RemoveBreakPoint(Kernel::KProcess& process, uint64_t addr) {
    std::scoped_lock<std::mutex> lock(g_breakpoint_lock);

    bool ret = false;
    auto&& it = impl->breakPointInfo.find(addr);
    if (it != impl->breakPointInfo.end()) {
        if (process.GetMemory().IsValidVirtualAddressRange(addr, sizeof(u32))) {
            process.GetMemory().Write32(it->first, it->second);
            Core::InvalidateInstructionCacheRange(&process, addr, sizeof(u32));

            impl->breakPointInfo.erase(it);
            ret = true;
        }
    }
    return ret;
}

bool MemorySniffer::EnableBreakPoint(Kernel::KProcess& process, uint64_t addr) {
    std::scoped_lock<std::mutex> lock(g_breakpoint_lock);

    bool ret = false;
    auto&& it = impl->breakPointInfo.find(addr);
    if (it != impl->breakPointInfo.end()) {
        if (process.GetMemory().IsValidVirtualAddressRange(addr, sizeof(u32))) {
            auto&& phyCore = system.CurrentPhysicalCore();
            bool is32 = phyCore.IsAArch32();
            process.GetMemory().Write32(addr, is32 ? BreakPointInstructionOn32()
                                                          : BreakPointInstructionOn64());
            Core::InvalidateInstructionCacheRange(&process, addr, sizeof(u32));

            ret = true;
        }
    }
    return ret;
}

bool MemorySniffer::DisableBreakPoint(Kernel::KProcess& process, uint64_t addr) {
    std::scoped_lock<std::mutex> lock(g_breakpoint_lock);

    bool ret = false;
    auto&& it = impl->breakPointInfo.find(addr);
    if (it != impl->breakPointInfo.end()) {
        if (process.GetMemory().IsValidVirtualAddressRange(addr, sizeof(u32))) {
            process.GetMemory().Write32(it->first, it->second);
            Core::InvalidateInstructionCacheRange(&process, addr, sizeof(u32));

            ret = true;
        }
    }
    return ret;
}

bool MemorySniffer::IsBreakPoint(uint32_t addr) const {
    uint64_t addr64 = addr;
    return IsBreakPoint(addr64);
}

bool MemorySniffer::IsBreakPoint(uint64_t addr) const {
    std::scoped_lock<std::mutex> lock(g_breakpoint_lock);

    bool ret = impl->breakPointInfo.contains(addr);
    return ret;
}

uint64_t MemorySniffer::GetMaxStepCount() const {
    return impl->maxStepCount;
}

void MemorySniffer::SetEnable(bool val) {
    impl->enabled = val;
}

bool MemorySniffer::IsEnabled() const {
    return impl->enabled;
}

bool MemorySniffer::IsTraceProcess(Kernel::KProcess& process) const {
    u64 pid = process.GetProcessId();
    return (impl->traceProcessId == pid ||
        impl->traceProcessId == 0 && system.ApplicationProcess() == &process);
}

MemorySniffer::WatchPointType MemorySniffer::GetTraceOnAddr(WatchPointType watchType,
                                                            uint64_t addr) const {
    WatchPoints::const_iterator it;
    switch (watchType) {
    case WatchPointType::Read:
        it = impl->traceAddrsOnRead.find(addr);
        if (it != impl->traceAddrsOnRead.end())
            return WatchPointType::Read;
        break;
    case WatchPointType::Write:
        it = impl->traceAddrsOnWrite.find(addr);
        if (it != impl->traceAddrsOnWrite.end())
            return WatchPointType::Write;
        break;
    case WatchPointType::GetPointer:
        it = impl->traceAddrsOnGetPointer.find(addr);
        if (it != impl->traceAddrsOnGetPointer.end())
            return WatchPointType::GetPointer;
        break;
    case WatchPointType::ReadCString:
        it = impl->traceAddrsOnReadCString.find(addr);
        if (it != impl->traceAddrsOnReadCString.end())
            return WatchPointType::ReadCString;
        break;
    default:
        break;
    }
    return WatchPointType::NotWatchPoint;
}

MemorySniffer::WatchPointType MemorySniffer::GetTraceOnAddr(WatchPointType watchType, uint64_t addr,
                                                            std::size_t size) const {
    switch (watchType) {
    case WatchPointType::Read:
        for (auto&& v : impl->traceAddrsOnRead) {
            if (addr <= v && v < addr + size) {
                return WatchPointType::Read;
            }
        }
        break;
    case WatchPointType::Write:
        for (auto&& v : impl->traceAddrsOnWrite) {
            if (addr <= v && v < addr + size) {
                return WatchPointType::Write;
            }
        }
        break;
    case WatchPointType::GetPointer:
        for (auto&& v : impl->traceAddrsOnGetPointer) {
            if (addr <= v && v < addr + size) {
                return WatchPointType::GetPointer;
            }
        }
        break;
    case WatchPointType::ReadCString:
        for (auto&& v : impl->traceAddrsOnReadCString) {
            if (addr <= v && v < addr + size) {
                return WatchPointType::ReadCString;
            }
        }
        break;
    default:
        break;
    }
    return WatchPointType::NotWatchPoint;
}

bool MemorySniffer::IsStepInstruction(uint32_t inst) const {
    bool ret = false;
    if (impl->logInstructions.size() == 0) {
        ret = true;
    } else {
        for (auto&& pair : impl->logInstructions) {
            uint32_t mask = pair.first;
            uint32_t val = pair.second;
            if ((inst & mask) == val) {
                ret = true;
                break;
            }
        }
    }
    return ret;
}

bool MemorySniffer::IsInTraceScope(uint64_t addr) const {
    return (impl->traceScopeBegin <= addr && addr < impl->traceScopeEnd) ||
           (impl->traceScopeBegin == 0 && impl->traceScopeBegin == impl->traceScopeEnd);
}

uint64_t MemorySniffer::GetStartTraceAddr() const {
    return impl->startTraceAddr;
}

uint64_t MemorySniffer::GetStopTraceAddr() const {
    return impl->stopTraceAddr;
}

bool MemorySniffer::TraceSvcCall(int swi, Core::ArmInterface& armIntf) const {
    if (impl->swiForTrace == swi || impl->swiForTrace == std::numeric_limits<int>::max()) {
        bool match = true;
        if (impl->sessionHandle != 0) {
            Kernel::Svc::ThreadContext ctx;
            armIntf.GetContext(ctx);
            switch (impl->swiForTrace) {
            case 0x20: // SendSyncRequestLight
                if (static_cast<u32>(ctx.r[0]) != impl->sessionHandle)
                    match = false;
                break;
            case 0x21: // SendSyncRequest
                if (static_cast<u32>(ctx.r[0]) != impl->sessionHandle)
                    match = false;
                break;
            case 0x22: // SendSyncRequestWithUserBuffer
                if (static_cast<u32>(ctx.r[2]) != impl->sessionHandle)
                    match = false;
                break;
            case 0x23: // SendAsyncRequestWithUserBuffer
                if (static_cast<u32>(ctx.r[3]) != impl->sessionHandle)
                    match = false;
                break;
            }
        }
        if (match) {
            std::scoped_lock<std::mutex> lock(g_trace_buffer_lock);

            auto&& ss = impl->traceBuffer;
            ss << "swi:" << std::hex << swi;
            ss << std::endl;
            auto&& phyCore = system.CurrentPhysicalCore();
            auto* pThread = phyCore.CurrentThread();
            if (pThread) {
                DumpRegisterValues(*pThread, ss, true);
                ss << std::endl;
            }
            std::stringstream ss2;
            ss2 << "swi:" << std::hex << swi;
            g_MainThreadCaller.RequestLogToView(ss2.str());
        }
    }
    return true;
}

void MemorySniffer::LogContext(const Kernel::KThread& thread) const {
    // using map here maybe ok
    // 30000~35000 down to 20000~25000
    std::scoped_lock<std::mutex> lock(g_trace_pc_lock);
    auto&& ctx = thread.GetContext();

    u64 pc = ctx.pc;
    bool find = false;
    if (impl->usePcCountArray) {
        int hash = static_cast<int>((pc & Impl::c_pc_hash_mask) >> Impl::c_pc_hash_shift);
        u64 other = pc & Impl::c_pc_other_mask;

        int startIx = hash * Impl::c_pc_num_per_entry;
        for (int ix = 0; ix < Impl::c_pc_num_per_entry; ++ix) {
            u64 v = impl->pcCountArray[startIx + ix];
            if (v == 0) {
                int ct = 1;
                impl->pcCountArray[startIx + ix] = (other | ct);
                find = true;
                break;
            } else if (other == (v & Impl::c_pc_other_mask)) {
                int ct = static_cast<int>(v & Impl::c_pc_hash_mask);
                ct = ct < Impl::c_pc_max_count ? ct + 1 : ct;
                impl->pcCountArray[startIx + ix] = (other | ct);
                find = true;
                break;
            }
        }
    }
    if (!find) {
        auto&& it = impl->pcCountInfo.find(pc);
        if (it == impl->pcCountInfo.end()) {
            impl->pcCountInfo.insert(std::make_pair(pc, 1));
        } else {
            it->second = it->second + 1;
        }
    }
}

void MemorySniffer::TryLogCallStack(const Kernel::KThread& thread) const {
    {
        std::scoped_lock<std::mutex> lock(g_trace_buffer_lock);

        auto&& ss = impl->traceBuffer;
        DumpRegisterValues(thread, ss, true);
        ss << std::endl;

        auto&& ctx = thread.GetContext();
        std::stringstream ss2;
        ss2 << "log call stack, pc:" << std::hex << ctx.pc;
        g_MainThreadCaller.RequestLogToView(ss2.str());
    }
    g_MainThreadCaller.RequestSyncCallback(&thread);
}

void MemorySniffer::TryLogCallStack(WatchPointType matchWatchPoint, uint64_t addr) const {
    WatchPointType watchType = GetTraceOnAddr(matchWatchPoint, addr);
    if (watchType != WatchPointType::NotWatchPoint) {
        auto&& phyCore = system.CurrentPhysicalCore();
        auto* pThread = phyCore.CurrentThread();
        {
            std::scoped_lock<std::mutex> lock(g_trace_buffer_lock);

            auto&& ss = impl->traceBuffer;
            if (pThread) {
                DumpRegisterValues(*pThread, ss, true);
                ss << std::endl;
            }

            std::stringstream ss2;
            ss2 << "log watch point:" << GetWatchTypeName(watchType) << ", addr:" << std::hex
                << addr;
            g_MainThreadCaller.RequestLogToView(ss2.str());
        }
        g_MainThreadCaller.RequestSyncCallback(static_cast<int>(watchType), addr, pThread);
    }
}

void MemorySniffer::TryLogCallStack(WatchPointType matchWatchPoint,
                                    const Common::ProcessAddress addr) const {
    WatchPointType watchType = GetTraceOnAddr(matchWatchPoint, addr.GetValue());
    if (watchType != WatchPointType::NotWatchPoint) {
        auto&& phyCore = system.CurrentPhysicalCore();
        auto* pThread = phyCore.CurrentThread();
        {
            std::scoped_lock<std::mutex> lock(g_trace_buffer_lock);

            auto&& ss = impl->traceBuffer;
            if (pThread) {
                DumpRegisterValues(*pThread, ss, true);
                ss << std::endl;
            }

            std::stringstream ss2;
            ss2 << "log watch point:" << GetWatchTypeName(watchType) << ", addr:" << std::hex
                << addr.GetValue();
            g_MainThreadCaller.RequestLogToView(ss2.str());
        }
        g_MainThreadCaller.RequestSyncCallback(static_cast<int>(watchType), addr.GetValue(),
                                               pThread);
    }
}

void MemorySniffer::TryLogCallStack(WatchPointType matchWatchPoint, uint64_t addr,
                                    std::size_t size) const {
    WatchPointType watchType = GetTraceOnAddr(matchWatchPoint, addr, size);
    if (watchType != WatchPointType::NotWatchPoint) {
        auto&& phyCore = system.CurrentPhysicalCore();
        auto* pThread = phyCore.CurrentThread();
        {
            std::scoped_lock<std::mutex> lock(g_trace_buffer_lock);

            auto&& ss = impl->traceBuffer;
            if (pThread) {
                DumpRegisterValues(*pThread, ss, true);
                ss << std::endl;
            }

            std::stringstream ss2;
            ss2 << "log watch point:" << GetWatchTypeName(watchType) << ", addr:" << std::hex
                << addr << " size:" << size;
            g_MainThreadCaller.RequestLogToView(ss2.str());
        }
        g_MainThreadCaller.RequestSyncCallback(static_cast<int>(watchType), addr, size, pThread);
    }
}

void MemorySniffer::TryLogCallStack(WatchPointType matchWatchPoint,
                                    const Common::ProcessAddress addr, std::size_t size) const {
    WatchPointType watchType = GetTraceOnAddr(matchWatchPoint, addr.GetValue(), size);
    if (watchType != WatchPointType::NotWatchPoint) {
        auto&& phyCore = system.CurrentPhysicalCore();
        auto* pThread = phyCore.CurrentThread();
        {
            std::scoped_lock<std::mutex> lock(g_trace_buffer_lock);

            auto&& ss = impl->traceBuffer;
            if (pThread) {
                DumpRegisterValues(*pThread, ss, true);
                ss << std::endl;
            }

            std::stringstream ss2;
            ss2 << "log watch point:" << GetWatchTypeName(watchType) << ", addr:" << std::hex
                << addr.GetValue() << " size:" << size;
            g_MainThreadCaller.RequestLogToView(ss2.str());
        }
        g_MainThreadCaller.RequestSyncCallback(static_cast<int>(watchType), addr.GetValue(), size,
                                               pThread);
    }
}

void MemorySniffer::GetMemorySearchInfo(uint64_t& scopeBegin, uint64_t& scopeEnd, uint64_t& step,
                                        uint64_t& valueSize, uint64_t& range, uint64_t& maxCount,
                                        uint64_t& pid) const {
    scopeBegin = impl->memSearchScopeBegin;
    scopeEnd = impl->memSearchScopeEnd;
    step = impl->memSearchStep;
    valueSize = impl->memSearchValueSize;
    range = impl->memSearchResultRange;
    maxCount = impl->memSearchMaxCount;
    pid = impl->memSearchProcessId;

    if (scopeBegin == scopeEnd) {
        scopeBegin = impl->aliasStart;
        scopeEnd = impl->aliasStart + impl->aliasSize;
    }
}

void MemorySniffer::MarkMemoryDebug(uint64_t pid, uint64_t addr, uint64_t size, bool debug) const {
    auto* pProcess = GetProcess(pid);
    if (nullptr != pProcess) {
        auto&& memory = pProcess->GetMemory();
        memory.MarkRegionDebug(addr, size, debug);
    }
}

void MemorySniffer::AddSniffing(uint64_t pid, uint64_t addr, uint64_t size, uint64_t step,
                                uint64_t cur_val) {
    if (!impl->enabled)
        return;

    auto&& result = impl->resultMemModifyInfo;

    static const size_t s_u8 = sizeof(uint8_t);
    static const size_t s_u16 = sizeof(uint16_t);
    static const size_t s_u32 = sizeof(uint32_t);
    static const size_t s_u64 = sizeof(uint64_t);

    auto* pProcess = GetProcess(pid);
    if (nullptr != pProcess) {
        auto&& memory = pProcess->GetMemory();
        for (uint64_t maddr = addr; maddr <= addr + size - step; maddr += step) {
            int type = 0;
            uint64_t newval = 0;
            uint64_t oldval = 0;
            switch (step) {
            case s_u8:
                type = MemoryModifyInfo::type_u8;
                if (memory.IsValidVirtualAddressRange(maddr, s_u8)) {
                    auto* pMem = memory.GetPointerSilent(maddr);
                    if (nullptr != pMem) {
                        newval = *reinterpret_cast<u8*>(pMem);
                    }
                }
                break;
            case s_u16:
                type = MemoryModifyInfo::type_u16;
                if (memory.IsValidVirtualAddressRange(maddr, s_u16)) {
                    auto* pMem = memory.GetPointerSilent(maddr);
                    if (nullptr != pMem) {
                        newval = *reinterpret_cast<u16*>(pMem);
                    }
                }
                break;
            case s_u32:
                type = MemoryModifyInfo::type_u32;
                if (memory.IsValidVirtualAddressRange(maddr, s_u32)) {
                    auto* pMem = memory.GetPointerSilent(maddr);
                    if (nullptr != pMem) {
                        newval = *reinterpret_cast<u32*>(pMem);
                    }
                }
                break;
            case s_u64:
                type = MemoryModifyInfo::type_u64;
                if (memory.IsValidVirtualAddressRange(maddr, s_u64)) {
                    auto* pMem = memory.GetPointerSilent(maddr);
                    if (nullptr != pMem) {
                        newval = *reinterpret_cast<u64*>(pMem);
                    }
                }
                break;
            }
            if (newval == cur_val || cur_val == 0) {
                auto&& newPtr = std::make_shared<MemoryModifyInfo>();
                newPtr->addr = maddr;
                newPtr->type = type;
                newPtr->size = step;
                switch (type) {
                case MemoryModifyInfo::type_u8:
                    newPtr->u8Val = static_cast<u8>(newval);
                    newPtr->u8OldVal = static_cast<u8>(oldval);
                    break;
                case MemoryModifyInfo::type_u16:
                    newPtr->u16Val = static_cast<u16>(newval);
                    newPtr->u16OldVal = static_cast<u16>(oldval);
                    break;
                case MemoryModifyInfo::type_u32:
                    newPtr->u32Val = static_cast<u32>(newval);
                    newPtr->u32OldVal = static_cast<u32>(oldval);
                    break;
                case MemoryModifyInfo::type_u64:
                    newPtr->u64Val = static_cast<u64>(newval);
                    newPtr->u64OldVal = static_cast<u64>(oldval);
                    break;
                }
                result.insert(std::make_pair(maddr, std::move(newPtr)));
            }
        }
    }
}

void MemorySniffer::AddLogInstruction(uint32_t mask, uint32_t value) {
    impl->logInstructions.push_back(std::make_pair(mask, value));
}

void MemorySniffer::SetResultMemoryModifyInfo(MemoryModifyInfoMap&& newResult) {
    std::swap(impl->resultMemModifyInfo, newResult);
}

const MemoryModifyInfoMap& MemorySniffer::GetResultMemoryModifyInfo() const {
    return impl->resultMemModifyInfo;
}

const MemoryModifyInfoMap& MemorySniffer::GetLastHistoryMemoryModifyInfo() const {
    int ct = GetHistoryMemoryModifyInfoCount();
    if (ct > 0) {
        return impl->historyMemModifyInfos.back();
    } else {
        return g_InvalidMemModifyInfo;
    }
}

int MemorySniffer::GetHistoryMemoryModifyInfoCount() const {
    return static_cast<int>(impl->historyMemModifyInfos.size());
}

const MemoryModifyInfoMap& MemorySniffer::GetHistoryMemoryModifyInfo(int index) const {
    int ct = GetHistoryMemoryModifyInfoCount();
    if (index >= 0 && index < ct) {
        auto it = impl->historyMemModifyInfos.begin();
        std::advance(it, static_cast<size_t>(index));
        return *it;
    } else {
        return g_InvalidMemModifyInfo;
    }
}

const MemoryModifyInfoMap& MemorySniffer::GetLastRollbackMemoryModifyInfo() const {
    int ct = GetRollbackMemoryModifyInfoCount();
    if (ct > 0) {
        return impl->rollbackMemModifyInfos.front();
    } else {
        return g_InvalidMemModifyInfo;
    }
}

int MemorySniffer::GetRollbackMemoryModifyInfoCount() const {
    return static_cast<int>(impl->rollbackMemModifyInfos.size());
}

const MemoryModifyInfoMap& MemorySniffer::GetRollbackMemoryModifyInfo(int index) const {
    int ct = GetRollbackMemoryModifyInfoCount();
    if (index >= 0 && index < ct) {
        auto it = impl->rollbackMemModifyInfos.begin();
        std::advance(it, static_cast<size_t>(index));
        return *it;
    } else {
        return g_InvalidMemModifyInfo;
    }
}

MemoryModifyInfoMap* MemorySniffer::GetResultMemoryModifyInfoPtr() {
    return &(impl->resultMemModifyInfo);
}

MemoryModifyInfoMap* MemorySniffer::GetLastHistoryMemoryModifyInfoPtr() {
    int ct = GetHistoryMemoryModifyInfoCount();
    if (ct > 0) {
        return &(impl->historyMemModifyInfos.back());
    } else {
        return nullptr;
    }
}

void MemorySniffer::ClearAll() {
    impl->rollbackMemModifyInfos.clear();
    impl->historyMemModifyInfos.clear();
    impl->resultMemModifyInfo.clear();
}

void MemorySniffer::RefreshSnapshot() {
    if (!impl->enabled)
        return;

    if (impl->resultMemModifyInfo.size() > 0) {
        impl->historyMemModifyInfos.push_back(impl->resultMemModifyInfo);
    }

    if (impl->historyMemModifyInfos.size() > 0) {
        impl->resultMemModifyInfo.clear();
        auto&& history = impl->historyMemModifyInfos.back();
        auto&& result = impl->resultMemModifyInfo;

        static const size_t s_u8 = sizeof(uint8_t);
        static const size_t s_u16 = sizeof(uint16_t);
        static const size_t s_u32 = sizeof(uint32_t);
        static const size_t s_u64 = sizeof(uint64_t);

        for (auto&& pair : history) {
            auto&& dataPtr = pair.second;
            bool add = false;
            uint64_t newval = 0;
            uint64_t oldval = 0;
            switch (dataPtr->type) {
            case MemoryModifyInfo::type_u8: {
                auto* pProcess = GetProcess(dataPtr->pid);
                if (nullptr != pProcess) {
                    auto&& memory = pProcess->GetMemory();
                    if (memory.IsValidVirtualAddressRange(dataPtr->addr, s_u8)) {
                        auto* pMem = memory.GetPointerSilent(dataPtr->addr);
                        if (nullptr != pMem) {
                            newval = *reinterpret_cast<u8*>(pMem);
                            oldval = dataPtr->u8Val;
                            add = true;
                        }
                    }
                }
            } break;
            case MemoryModifyInfo::type_u16: {
                auto* pProcess = GetProcess(dataPtr->pid);
                if (nullptr != pProcess) {
                    auto&& memory = pProcess->GetMemory();
                    if (memory.IsValidVirtualAddressRange(dataPtr->addr, s_u16)) {
                        auto* pMem = memory.GetPointerSilent(dataPtr->addr);
                        if (nullptr != pMem) {
                            newval = *reinterpret_cast<u16*>(pMem);
                            oldval = dataPtr->u16Val;
                            add = true;
                        }
                    }
                }
            } break;
            case MemoryModifyInfo::type_u32: {
                auto* pProcess = GetProcess(dataPtr->pid);
                if (nullptr != pProcess) {
                    auto&& memory = pProcess->GetMemory();
                    if (memory.IsValidVirtualAddressRange(dataPtr->addr, s_u32)) {
                        auto* pMem = memory.GetPointerSilent(dataPtr->addr);
                        if (nullptr != pMem) {
                            newval = *reinterpret_cast<u32*>(pMem);
                            oldval = dataPtr->u32Val;
                            add = true;
                        }
                    }
                }
            } break;
            case MemoryModifyInfo::type_u64: {
                auto* pProcess = GetProcess(dataPtr->pid);
                if (nullptr != pProcess) {
                    auto&& memory = pProcess->GetMemory();
                    if (memory.IsValidVirtualAddressRange(dataPtr->addr, s_u64)) {
                        auto* pMem = memory.GetPointerSilent(dataPtr->addr);
                        if (nullptr != pMem) {
                            newval = *reinterpret_cast<u64*>(pMem);
                            oldval = dataPtr->u64Val;
                            add = true;
                        }
                    }
                }
            } break;
            }
            if (add && newval != oldval) {
                auto&& newPtr = std::make_shared<MemoryModifyInfo>(*dataPtr);
                switch (dataPtr->type) {
                case MemoryModifyInfo::type_u8:
                    newPtr->u8Val = static_cast<u8>(newval);
                    newPtr->u8OldVal = static_cast<u8>(oldval);
                    break;
                case MemoryModifyInfo::type_u16:
                    newPtr->u16Val = static_cast<u16>(newval);
                    newPtr->u16OldVal = static_cast<u16>(oldval);
                    break;
                case MemoryModifyInfo::type_u32:
                    newPtr->u32Val = static_cast<u32>(newval);
                    newPtr->u32OldVal = static_cast<u32>(oldval);
                    break;
                case MemoryModifyInfo::type_u64:
                    newPtr->u64Val = static_cast<u64>(newval);
                    newPtr->u64OldVal = static_cast<u64>(oldval);
                    break;
                }
                result.insert(std::make_pair(pair.first, std::move(newPtr)));
            }
        }
    }
}

void MemorySniffer::KeepUnchanged() {
    if (!impl->enabled)
        return;

    if (impl->historyMemModifyInfos.size() > 0) {
        auto&& history = impl->historyMemModifyInfos.back();
        auto&& result = impl->resultMemModifyInfo;

        MemoryModifyInfoMap newResult;
        for (auto&& pair : history) {
            auto&& it = result.find(pair.first);
            if (it == result.end() || it->second->IsUnchanged()) {
                newResult.insert(pair);
            }
        }
        if (impl->debugSnapshot) {
            impl->historyMemModifyInfos.push_back(impl->resultMemModifyInfo);
        }
        std::swap(impl->resultMemModifyInfo, newResult);
    }
}

void MemorySniffer::KeepChanged() {
    if (!impl->enabled)
        return;

    if (impl->historyMemModifyInfos.size() > 0) {
        auto&& history = impl->historyMemModifyInfos.back();
        auto&& result = impl->resultMemModifyInfo;
        MemoryModifyInfoMap newResult;
        for (auto&& pair : history) {
            auto&& it = result.find(pair.first);
            if (it != result.end() && it->second->IsChanged()) {
                newResult.insert(*it);
            }
        }
        if (impl->debugSnapshot) {
            impl->historyMemModifyInfos.push_back(impl->resultMemModifyInfo);
        }
        std::swap(impl->resultMemModifyInfo, newResult);
    }
}

void MemorySniffer::KeepIncreased() {
    if (!impl->enabled)
        return;

    if (impl->historyMemModifyInfos.size() > 0) {
        auto&& history = impl->historyMemModifyInfos.back();
        auto&& result = impl->resultMemModifyInfo;
        MemoryModifyInfoMap newResult;
        for (auto&& pair : history) {
            auto&& it = result.find(pair.first);
            if (it != result.end() && it->second->IsIncreased()) {
                newResult.insert(*it);
            }
        }
        if (impl->debugSnapshot) {
            impl->historyMemModifyInfos.push_back(impl->resultMemModifyInfo);
        }
        std::swap(impl->resultMemModifyInfo, newResult);
    }
}

void MemorySniffer::KeepDecreased() {
    if (!impl->enabled)
        return;

    if (impl->historyMemModifyInfos.size() > 0) {
        auto&& history = impl->historyMemModifyInfos.back();
        auto&& result = impl->resultMemModifyInfo;
        MemoryModifyInfoMap newResult;
        for (auto&& pair : history) {
            auto&& it = result.find(pair.first);
            if (it != result.end() && it->second->IsDecreased()) {
                newResult.insert(*it);
            }
        }
        if (impl->debugSnapshot) {
            impl->historyMemModifyInfos.push_back(impl->resultMemModifyInfo);
        }
        std::swap(impl->resultMemModifyInfo, newResult);
    }
}

void MemorySniffer::Rollback() {
    if (!impl->enabled)
        return;

    int mct = GetHistoryMemoryModifyInfoCount();
    if (mct > 0) {
        auto&& last = impl->historyMemModifyInfos.back();
        std::swap(last, impl->resultMemModifyInfo);
        impl->rollbackMemModifyInfos.push_front(std::move(last));
        impl->historyMemModifyInfos.pop_back();
    }
}

void MemorySniffer::Unrollback() {
    if (!impl->enabled)
        return;

    int mct = GetRollbackMemoryModifyInfoCount();
    if (mct > 0) {
        auto&& last = impl->rollbackMemModifyInfos.front();
        std::swap(last, impl->resultMemModifyInfo);
        impl->historyMemModifyInfos.push_back(std::move(last));
        impl->rollbackMemModifyInfos.pop_front();
    }
}

bool MemorySniffer::Exec(const std::string& cmd, const std::string& arg) {
    if (cmd == "refreshsnapshot") {
        RefreshSnapshot();
        return true;
    } else if (cmd == "keepunchanged") {
        KeepUnchanged();
        return true;
    } else if (cmd == "keepchanged") {
        KeepChanged();
        return true;
    } else if (cmd == "keepincreased") {
        KeepIncreased();
        return true;
    } else if (cmd == "keepdecreased") {
        KeepDecreased();
        return true;
    } else if (cmd == "setdebugsnapshot") {
        impl->debugSnapshot =
            arg == "true" || (std::isdigit(arg[0]) && std::stoi(arg, nullptr, 0) != 0);
        return true;
    } else if (cmd == "clearloginsts") {
        impl->logInstructions.clear();
        return true;
    } else if (cmd == "addlogbl") {
        impl->logInstructions.push_back(std::make_pair(0xfc000000, 0x94000000)); // BL
        impl->logInstructions.push_back(std::make_pair(0xfffffc1f, 0xd63f0000)); // BLR
        impl->logInstructions.push_back(
            std::make_pair(0xfffff800, 0xd63f0800)); // BLRAA, BLRAAZ, BLRAB, BLRABZ
        return true;
    } else if (cmd == "addlogbc") {
        impl->logInstructions.push_back(std::make_pair(0xff000010, 0x54000000)); // B.cond
        impl->logInstructions.push_back(std::make_pair(0xff000010, 0x54000010)); // BC.cond
        impl->logInstructions.push_back(std::make_pair(0x7f000000, 0x35000000)); // CBNZ
        impl->logInstructions.push_back(std::make_pair(0x7f000000, 0x34000000)); // CBZ
        impl->logInstructions.push_back(std::make_pair(0x7f000000, 0x37000000)); // TBNZ
        impl->logInstructions.push_back(std::make_pair(0x7f000000, 0x36000000)); // TBZ
        return true;
    } else if (cmd == "addlogb") {
        impl->logInstructions.push_back(std::make_pair(0xfc000000, 0x14000000)); // B
        impl->logInstructions.push_back(std::make_pair(0xfffffc1f, 0xd61f0000)); // BR
        impl->logInstructions.push_back(
            std::make_pair(0xfffff800, 0xd61f0800)); // BRAA, BRAAZ, BRAB, BRABZ
        return true;
    } else if (cmd == "addlogret") {
        impl->logInstructions.push_back(std::make_pair(0xfffffc1f, 0xd65f0000)); // RET
        impl->logInstructions.push_back(std::make_pair(0xfffffbff, 0xd65f0bff)); // RETAA, RETAB
        impl->logInstructions.push_back(
            std::make_pair(0xffc0001f, 0x5500001f)); // RETAASPPC, RETABSPPC
        impl->logInstructions.push_back(
            std::make_pair(0xfffffbe0, 0xd65f0be0)); // RETAASPPC, RETABSPPC
        return true;
    } else if (cmd == "cleartracescope") {
        impl->traceScopeBegin = impl->traceScopeEnd = 0;
        impl->traceProcessId = 0;
        return true;
    } else if (cmd == "settracescope") {
        VisitMemoryArgs([this, &key = arg](const char* name, const char* id, u64 base, u64 addr,
                                           u64 size, u64 progId, u64 pid) {
            if (key == name || key == id) {
                impl->traceScopeBegin = addr;
                impl->traceScopeEnd = addr + size;
                impl->traceProcessId = pid;
            }
        });

        impl->traceModule = arg;

        std::stringstream ss;
        ss << cmd << " module:" << arg << " begin:" << std::hex << impl->traceScopeBegin
           << " end:" << impl->traceScopeEnd;
        g_MainThreadCaller.SyncLogToView(ss.str());
        return true;
    } else if (cmd == "settracescopebegin") {
        impl->traceScopeBegin = std::stoull(arg, nullptr, 0);
        return true;
    } else if (cmd == "settracescopeend") {
        impl->traceScopeEnd = std::stoull(arg, nullptr, 0);
        return true;
    } else if (cmd == "settracepid") {
        impl->traceProcessId = std::stoull(arg, nullptr, 0);
        return true;
    } else if (cmd == "cleartrace") {
        auto* pProcess = GetProcess(impl->traceProcessId);
        ClearBreakPoints(pProcess);
        impl->traceAddrsOnRead.clear();
        impl->traceAddrsOnWrite.clear();
        impl->traceAddrsOnGetPointer.clear();
        impl->traceAddrsOnReadCString.clear();
        impl->startTraceAddr = 0;
        impl->stopTraceAddr = 0;
        impl->swiForTrace = 0;
        return true;
    } else if (cmd == "starttrace") {
        if (arg.empty()) {
            for (int ix = 0; ix < static_cast<int>(Core::Hardware::NUM_CPU_CORES); ++ix) {
                auto&& phyCore = system.Kernel().PhysicalCore(ix);
                phyCore.StartTrace();
            }
        } else {
            int ix = std::stoi(arg, nullptr, 0);
            if (ix >= 0 && ix < static_cast<int>(Core::Hardware::NUM_CPU_CORES)) {
                auto&& phyCore = system.Kernel().PhysicalCore(ix);
                phyCore.StartTrace();
            }
        }
        return true;
    } else if (cmd == "stoptrace") {
        if (arg.empty()) {
            for (int ix = 0; ix < static_cast<int>(Core::Hardware::NUM_CPU_CORES); ++ix) {
                auto&& phyCore = system.Kernel().PhysicalCore(ix);
                phyCore.StopTrace();
            }
        } else {
            int ix = std::stoi(arg, nullptr, 0);
            if (ix >= 0 && ix < static_cast<int>(Core::Hardware::NUM_CPU_CORES)) {
                auto&& phyCore = system.Kernel().PhysicalCore(ix);
                phyCore.StopTrace();
            }
        }
        return true;
    } else if (cmd == "setmaxstepcount") {
        impl->maxStepCount = std::stoull(arg, nullptr, 0);
        return true;
    } else if (cmd == "addtraceread") {
        uint64_t addr = std::stoull(arg, nullptr, 0);
        impl->traceAddrsOnRead.insert(addr);
        return true;
    } else if (cmd == "removetraceread") {
        uint64_t addr = std::stoull(arg, nullptr, 0);
        impl->traceAddrsOnRead.erase(addr);
        return true;
    } else if (cmd == "addtracewrite") {
        uint64_t addr = std::stoull(arg, nullptr, 0);
        impl->traceAddrsOnWrite.insert(addr);
        return true;
    } else if (cmd == "removetracewrite") {
        uint64_t addr = std::stoull(arg, nullptr, 0);
        impl->traceAddrsOnWrite.erase(addr);
        return true;
    } else if (cmd == "addtracepointer") {
        uint64_t addr = std::stoull(arg, nullptr, 0);
        impl->traceAddrsOnGetPointer.insert(addr);
        return true;
    } else if (cmd == "removetracepointer") {
        uint64_t addr = std::stoull(arg, nullptr, 0);
        impl->traceAddrsOnGetPointer.erase(addr);
        return true;
    } else if (cmd == "addtracecstring") {
        uint64_t addr = std::stoull(arg, nullptr, 0);
        impl->traceAddrsOnReadCString.insert(addr);
        return true;
    } else if (cmd == "removetracecstring") {
        uint64_t addr = std::stoull(arg, nullptr, 0);
        impl->traceAddrsOnReadCString.erase(addr);
        return true;
    } else if (cmd == "addbp") {
        uint64_t addr = std::stoull(arg, nullptr, 0);

        auto* pProcess = GetProcess(impl->traceProcessId);
        if (pProcess && AddBreakPoint(*pProcess, addr)) {
            std::stringstream ss;
            ss << cmd << " " << std::hex << addr << " success.";
            g_MainThreadCaller.SyncLogToView(ss.str());
        }
        return true;
    } else if (cmd == "removebp") {
        uint64_t addr = std::stoull(arg, nullptr, 0);

        auto* pProcess = GetProcess(impl->traceProcessId);
        if (pProcess && RemoveBreakPoint(*pProcess, addr)) {
            std::stringstream ss;
            ss << cmd << " " << std::hex << addr << " success.";
            g_MainThreadCaller.SyncLogToView(ss.str());
        }
        return true;
    } else if (cmd == "setstarttracebp") {
        auto* pProcess = GetProcess(impl->traceProcessId);
        if (impl->startTraceAddr != 0 && pProcess) {
            RemoveBreakPoint(*pProcess, impl->startTraceAddr);
        }
        impl->startTraceAddr = std::stoull(arg, nullptr, 0);

        if (pProcess && AddBreakPoint(*pProcess, impl->startTraceAddr)) {
            std::stringstream ss;
            ss << cmd << " " << std::hex << impl->startTraceAddr << " success.";
            g_MainThreadCaller.SyncLogToView(ss.str());
        }
        return true;
    } else if (cmd == "setstoptracebp") {
        auto* pProcess = GetProcess(impl->traceProcessId);
        if (impl->stopTraceAddr != 0 && pProcess) {
            RemoveBreakPoint(*pProcess, impl->stopTraceAddr);
        }
        impl->stopTraceAddr = std::stoull(arg, nullptr, 0);

        if (pProcess && AddBreakPoint(*pProcess, impl->startTraceAddr)) {
            std::stringstream ss;
            ss << cmd << " " << std::hex << impl->stopTraceAddr << " success.";
            g_MainThreadCaller.SyncLogToView(ss.str());
        }
        return true;
    } else if (cmd == "settraceswi") {
        impl->swiForTrace = std::stoi(arg, nullptr, 0);
        return true;
    } else if (cmd == "usepccountarray") {
        impl->usePcCountArray =
            arg == "true" || (std::isdigit(arg[0]) && std::stoi(arg, nullptr, 0) != 0);
        return true;
    } else if (cmd == "setmaxpccount") {
        impl->maxPcCount = std::stoi(arg, nullptr, 0);
        return true;
    } else if (cmd == "startpccount") {
        if (arg.empty()) {
            for (int ix = 0; ix < static_cast<int>(Core::Hardware::NUM_CPU_CORES); ++ix) {
                auto&& phyCore = system.Kernel().PhysicalCore(ix);
                phyCore.StartPcCount();
            }
        } else {
            int ix = std::stoi(arg, nullptr, 0);
            if (ix >= 0 && ix < static_cast<int>(Core::Hardware::NUM_CPU_CORES)) {
                auto&& phyCore = system.Kernel().PhysicalCore(ix);
                phyCore.StartPcCount();
            }
        }
        return true;
    } else if (cmd == "stoppccount") {
        if (arg.empty()) {
            for (int ix = 0; ix < static_cast<int>(Core::Hardware::NUM_CPU_CORES); ++ix) {
                auto&& phyCore = system.Kernel().PhysicalCore(ix);
                phyCore.StopPcCount();
            }
        } else {
            int ix = std::stoi(arg, nullptr, 0);
            if (ix >= 0 && ix < static_cast<int>(Core::Hardware::NUM_CPU_CORES)) {
                auto&& phyCore = system.Kernel().PhysicalCore(ix);
                phyCore.StopPcCount();
            }
        }
        return true;
    } else if (cmd == "clearpccount") {
        std::scoped_lock<std::mutex> lock(g_trace_pc_lock);

        std::memset(impl->pcCountArray, 0, sizeof(impl->pcCountArray));
        impl->pcCountInfo.clear();
        return true;
    } else if (cmd == "storepccount") {
        std::scoped_lock<std::mutex> lock(g_trace_pc_lock);

        StorePcCount();
        return true;
    } else if (cmd == "keeppccount") {
        std::scoped_lock<std::mutex> lock(g_trace_pc_lock);

        KeepPcCount();
        return true;
    } else if (cmd == "keepnewpccount") {
        std::scoped_lock<std::mutex> lock(g_trace_pc_lock);

        KeepNewPcCount();
        return true;
    } else if (cmd == "keepsamepccount") {
        std::scoped_lock<std::mutex> lock(g_trace_pc_lock);

        KeepSamePcCount();
        return true;
    } else if (cmd == "savepccount") {
        std::scoped_lock<std::mutex> lock(g_trace_pc_lock);

        std::ofstream of(BraceScriptInterpreter::get_absolutely_path(arg), std::ios::out);
        if (of.is_open()) {
            DumpPcCount(of, impl->maxPcCount);
        }
        return true;
    } else if (cmd == "cleartracebuffer") {
        std::scoped_lock<std::mutex> lock(g_trace_buffer_lock);

        impl->traceBuffer.str("");
        return true;
    } else if (cmd == "savetracebuffer") {
        std::scoped_lock<std::mutex> lock(g_trace_buffer_lock);

        std::ofstream of(BraceScriptInterpreter::get_absolutely_path(arg), std::ios::out);
        if (of.is_open()) {
            of << impl->traceBuffer.str();
        }
        return true;
    } else if (cmd == "setsession") {
        impl->sessionHandle = static_cast<u32>(std::stoul(arg, nullptr, 0));
        return true;
    } else if (cmd == "clearmemscope") {
        impl->memSearchScopeBegin = impl->memSearchScopeEnd = 0;
        impl->memSearchProcessId = 0;
        return true;
    } else if (cmd == "setmemscope") {
        VisitMemoryArgs([this, &key = arg](const char* name, const char* id, u64 base, u64 addr,
                                           u64 size, u64 progId, u64 pid) {
            if (key == name || key == id) {
                impl->memSearchScopeBegin = addr;
                impl->memSearchScopeEnd = addr + size;
                impl->memSearchProcessId = pid;
            }
        });

        std::stringstream ss;
        ss << cmd << " begin:" << std::hex << impl->memSearchScopeBegin
           << " end:" << impl->memSearchScopeEnd << " pid:" << impl->memSearchProcessId;
        g_MainThreadCaller.SyncLogToView(ss.str());
        return true;
    } else if (cmd == "setmemscopebegin") {
        impl->memSearchScopeBegin = std::stoull(arg, nullptr, 0);
        return true;
    } else if (cmd == "setmemscopeend") {
        impl->memSearchScopeEnd = std::stoull(arg, nullptr, 0);
        return true;
    } else if (cmd == "setmempid") {
        impl->memSearchProcessId = std::stoull(arg, nullptr, 0);
        return true;
    } else if (cmd == "setmemstep") {
        impl->memSearchStep = std::stoull(arg, nullptr, 0);
        return true;
    } else if (cmd == "setmemsize") {
        impl->memSearchValueSize = std::stoull(arg, nullptr, 0);
        return true;
    } else if (cmd == "setmemrange") {
        impl->memSearchResultRange = std::stoull(arg, nullptr, 0);
        return true;
    } else if (cmd == "setmemcount") {
        impl->memSearchMaxCount = std::stoull(arg, nullptr, 0);
        return true;
    } else if (cmd == "saveresult") {
        SaveResult(arg.c_str());
        return true;
    } else if (cmd == "savehistory") {
        SaveHistory(arg.c_str());
        return true;
    } else if (cmd == "saverollback") {
        SaveRollback(arg.c_str());
        return true;
    } else if (cmd == "dumpreg") {
        std::stringstream ss;
        DumpRegisterValues(ss, true);
        g_MainThreadCaller.SyncLogToView(ss.str());
        return true;
    } else if (cmd == "dumpsession") {
        std::scoped_lock<std::mutex> lock(g_session_lock);

        std::stringstream ss;
        ss << "[sessions]";
        for (auto&& pair : impl->sessionInfo) {
            auto&& info = pair.second;
            if (arg.empty() || info.name.find(arg) != std::string::npos) {
                ss << std::endl;
                ss << "handle:" << std::hex << info.handle << " name:" << info.name
                   << " id:" << std::hex << info.id;
            }
        }
        g_MainThreadCaller.SyncLogToView(ss.str());
        return true;
    } else if (cmd == "listprocess") {
        std::stringstream ss;
        ss << "[processes]";
        for (auto&& proc : system.Kernel().GetProcessList()) {
            u64 id = proc->GetId();
            u64 progId = proc->GetProgramId();
            u64 procId = proc->GetProcessId();
            const char* name = proc->GetName();

            ss << std::endl;
            ss << "id:" << std::hex << id << " name:" << name << " program id:" << std::hex
               << progId << " pid:" << procId;
        }
        g_MainThreadCaller.SyncLogToView(ss.str());
        return true;
    }

    return false;
}

void MemorySniffer::SaveAbsAsCheatVM(const char* file_path, const char* tag) const {
    std::ofstream of(BraceScriptInterpreter::get_absolutely_path(file_path), std::ios_base::out);
    if (of.fail())
        return;
    of << std::uppercase;
    of << "{" << tag << "}" << std::endl;
    for (auto&& pair : impl->resultMemModifyInfo) {
        auto&& info = pair.second;
        u64 addr = info->addr.GetValue();
        u32 h32 = static_cast<u32>(addr >> 32);
        u32 l32 = static_cast<u32>(addr & 0xffffffffull);
        of << "400B0000 ";
        of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << h32 << " ";
        of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << l32 << std::endl;
        switch (info->type) {
        case MemoryModifyInfo::type_u8:
            of << "610B0000 ";
            of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << 0 << " ";
            of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex
               << static_cast<u16>(info->u8Val) << std::endl;
            break;
        case MemoryModifyInfo::type_u16:
            of << "620B0000 ";
            of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << 0 << " ";
            of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << info->u16Val
               << std::endl;
            break;
        case MemoryModifyInfo::type_u32:
            of << "640B0000 ";
            of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << 0 << " ";
            of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << info->u32Val
               << std::endl;
            break;
        case MemoryModifyInfo::type_u64:
            u32 vh32 = static_cast<u32>(info->u64Val >> 32);
            u32 vl32 = static_cast<u32>(info->u64Val & 0xffffffffull);
            of << "680B0000 ";
            of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << vh32 << " ";
            of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << vl32 << std::endl;
            break;
        }
    }
    of.close();
}

void MemorySniffer::SaveRelAsCheatVM(const char* file_path, const char* tag) const {
    std::ofstream of(BraceScriptInterpreter::get_absolutely_path(file_path), std::ios_base::out);
    if (of.fail())
        return;
    of << std::uppercase;
    std::string firstId;
    if (impl->moduleMemArgs.size() > 0) {
        firstId = impl->moduleMemArgs.front().buildId;
    }
    bool first = true;
    for (auto&& pair : impl->resultMemModifyInfo) {
        auto&& info = pair.second;
        u64 addr = info->addr.GetValue();
        std::string build_id;
        int mt = CalcMemoryType(addr, build_id);
        u32 h32 = static_cast<u32>(addr >> 32);
        u32 l32 = static_cast<u32>(addr & 0xffffffffull);
        if (first)
            of << "{" << build_id << (mt == 1 ? "_" + firstId : std::string()) << "_" << tag << "}"
               << std::endl;
        if (mt >= 0) {
            of << "40000000 00000000 00000000" << std::endl;
            switch (info->type) {
            case MemoryModifyInfo::type_u8:
                of << "01" << mt << "000" << std::setfill('0') << std::setw(2) << std::hex << h32
                   << " ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << l32 << " ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex
                   << static_cast<u16>(info->u8Val) << std::endl;
                break;
            case MemoryModifyInfo::type_u16:
                of << "02" << mt << "000" << std::setfill('0') << std::setw(2) << std::hex << h32
                   << " ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << l32 << " ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << info->u16Val
                   << std::endl;
                break;
            case MemoryModifyInfo::type_u32:
                of << "04" << mt << "000" << std::setfill('0') << std::setw(2) << std::hex << h32
                   << " ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << l32 << " ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << info->u32Val
                   << std::endl;
                break;
            case MemoryModifyInfo::type_u64:
                u32 vh32 = static_cast<u32>(info->u64Val >> 32);
                u32 vl32 = static_cast<u32>(info->u64Val & 0xffffffffull);
                of << "08" << mt << "000" << std::setfill('0') << std::setw(2) << std::hex << h32
                   << " ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << l32 << " ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << vh32 << " ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << vl32
                   << std::endl;
                break;
            }
        } else {
            of << "400B0000 ";
            of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << h32 << " ";
            of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << l32 << std::endl;
            switch (info->type) {
            case MemoryModifyInfo::type_u8:
                of << "610B0000 ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << 0 << " ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex
                   << static_cast<u16>(info->u8Val) << std::endl;
                break;
            case MemoryModifyInfo::type_u16:
                of << "620B0000 ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << 0 << " ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << info->u16Val
                   << std::endl;
                break;
            case MemoryModifyInfo::type_u32:
                of << "640B0000 ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << 0 << " ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << info->u32Val
                   << std::endl;
                break;
            case MemoryModifyInfo::type_u64:
                u32 vh32 = static_cast<u32>(info->u64Val >> 32);
                u32 vl32 = static_cast<u32>(info->u64Val & 0xffffffffull);
                of << "680B0000 ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << vh32 << " ";
                of << std::setfill('0') << std::setw(sizeof(u32) * 2) << std::hex << vl32
                   << std::endl;
                break;
            }
        }
        first = false;
    }
    of.close();
}

static inline void DumpModifyInfo(std::ostream& os, const MemoryModifyInfoMap& infos,
                                  std::function<int(u64&, std::string&)> calcMemoryType) {
    for (auto&& pair : infos) {
        auto&& info = pair.second;

        u64 addr = info->addr.GetValue();
        u64 vaddr = addr;
        std::string build_id;
        int mt = calcMemoryType(addr, build_id);

        os << "addr(" << build_id << ":" << std::dec << mt << "):";
        os << std::hex << addr;
        os << " type:";
        os << std::dec << info->type;
        os << std::hex;
        switch (info->type) {
        case MemoryModifyInfo::type_u8:
            os << " val:" << static_cast<u16>(info->u8Val);
            os << " old_val:" << static_cast<u16>(info->u8OldVal);
            break;
        case MemoryModifyInfo::type_u16:
            os << " val:" << info->u16Val;
            os << " old_val:" << info->u16OldVal;
            break;
        case MemoryModifyInfo::type_u32:
            os << " val:" << info->u32Val;
            os << " old_val:" << info->u32OldVal;
            break;
        case MemoryModifyInfo::type_u64:
            os << " val:" << info->u64Val;
            os << " old_val:" << info->u64OldVal;
            break;
        }
        os << std::dec;
        os << " size:";
        os << info->size;
        os << std::hex << " vaddr:" << vaddr;
        os << std::endl;
    }
}

void MemorySniffer::SaveResult(const char* file_path) const {
    std::ofstream of(BraceScriptInterpreter::get_absolutely_path(file_path), std::ios_base::out);
    if (of.fail())
        return;
    DumpMemoryTypes(of);
    of << "===modify info (count:" << std::dec << impl->resultMemModifyInfo.size()
       << ")===" << std::endl;
    DumpModifyInfo(of, impl->resultMemModifyInfo,
                   [this](u64& addr, std::string& bid) { return CalcMemoryType(addr, bid); });
}

void MemorySniffer::SaveHistory(const char* file_path) const {
    std::ofstream of(BraceScriptInterpreter::get_absolutely_path(file_path), std::ios_base::out);
    if (of.fail())
        return;
    DumpMemoryTypes(of);
    int ix = 0;
    for (auto&& v : impl->historyMemModifyInfos) {
        of << "===modify info " << std::dec << ix << " (count:" << v.size() << ")===" << std::endl;
        DumpModifyInfo(of, v,
                       [this](u64& addr, std::string& bid) { return CalcMemoryType(addr, bid); });
        ++ix;
    }
}

void MemorySniffer::SaveRollback(const char* file_path) const {
    std::ofstream of(BraceScriptInterpreter::get_absolutely_path(file_path), std::ios_base::out);
    if (of.fail())
        return;
    DumpMemoryTypes(of);
    int ix = 0;
    for (auto&& v : impl->rollbackMemModifyInfos) {
        of << "===rollback modify info " << std::dec << ix << " (count:" << v.size()
           << ")===" << std::endl;
        DumpModifyInfo(of, v,
                       [this](u64& addr, std::string& bid) { return CalcMemoryType(addr, bid); });
        ++ix;
    }
}

Kernel::KProcess* MemorySniffer::GetProcess(u64 pid) const {
    auto* pAppProc = system.ApplicationProcess();
    if (pAppProc && pAppProc->GetProcessId() == pid) {
        return pAppProc;
    }
    if (pid > 0) {
        auto&& list = system.Kernel().GetProcessList();
        for (auto&& p : list) {
            if (p->GetProcessId() == pid)
                return p.GetPointerUnsafe();
        }
    }
    return pAppProc;
}

uint64_t MemorySniffer::ReadMemory(Kernel::KProcess& process, uint64_t addr, uint64_t typeSizeOf,
                                   bool& succ) const {
    static const size_t s_u8 = sizeof(uint8_t);
    static const size_t s_u16 = sizeof(uint16_t);
    static const size_t s_u32 = sizeof(uint32_t);
    static const size_t s_u64 = sizeof(uint64_t);

    auto&& memory = process.GetMemory();
    succ = false;
    if (memory.IsValidVirtualAddressRange(addr, typeSizeOf)) {
        auto* pMem = memory.GetPointerSilent(addr);
        if (nullptr != pMem) {
            succ = true;
            switch (typeSizeOf) {
            case s_u8:
                return *reinterpret_cast<uint8_t*>(pMem);
            case s_u16:
                return *reinterpret_cast<uint16_t*>(pMem);
            case s_u32:
                return *reinterpret_cast<uint32_t*>(pMem);
            case s_u64:
                return *reinterpret_cast<uint64_t*>(pMem);
            }
        }
    }
    return 0;
}

bool MemorySniffer::WriteMemory(Kernel::KProcess& process, uint64_t addr, uint64_t typeSizeOf,
                                uint64_t val) const {
    static const size_t s_u8 = sizeof(uint8_t);
    static const size_t s_u16 = sizeof(uint16_t);
    static const size_t s_u32 = sizeof(uint32_t);
    static const size_t s_u64 = sizeof(uint64_t);

    auto&& memory = process.GetMemory();
    bool succ = false;
    if (memory.IsValidVirtualAddressRange(addr, typeSizeOf)) {
        auto* pMem = memory.GetPointerSilent(addr);
        if (nullptr != pMem) {
            succ = true;
            switch (typeSizeOf) {
            case s_u8:
                *reinterpret_cast<uint8_t*>(pMem) = static_cast<uint8_t>(val);
                break;
            case s_u16:
                *reinterpret_cast<uint16_t*>(pMem) = static_cast<uint16_t>(val);
                break;
            case s_u32:
                *reinterpret_cast<uint32_t*>(pMem) = static_cast<uint32_t>(val);
                break;
            case s_u64:
                *reinterpret_cast<uint64_t*>(pMem) = val;
                break;
            }
            Core::InvalidateInstructionCacheRange(&process, addr, typeSizeOf);
        }
    }
    return succ;
}

bool MemorySniffer::DumpMemory(Kernel::KProcess& process, uint64_t addr, uint64_t size,
                               std::ostream& os) const {
    auto&& memory = process.GetMemory();
    bool succ = false;
    if (memory.IsValidVirtualAddressRange(addr, size)) {
        auto* pMem = memory.GetPointerSilent(addr);
        if (nullptr != pMem) {
            succ = true;
            os.write(reinterpret_cast<const char*>(pMem), size);
        }
    }
    return succ;
}

void MemorySniffer::StorePcCount() const {
    auto&& lastPcCounts = impl->lastPcCountInfo;
    lastPcCounts.clear();

    for (int hash = 0; hash < Impl::c_max_pc_entry_num; ++hash) {
        int startIx = hash * Impl::c_pc_num_per_entry;
        for (int ix = 0; ix < Impl::c_pc_num_per_entry; ++ix) {
            u64 v = impl->pcCountArray[startIx + ix];
            if (v != 0) {
                u64 pc = ((v & Impl::c_pc_other_mask) | (hash << Impl::c_pc_hash_shift));
                u64 ct = (v & Impl::c_pc_hash_mask);
                lastPcCounts.insert(std::make_pair(pc, ct));
            }
        }
    }
    for (auto&& pair : impl->pcCountInfo) {
        lastPcCounts.insert(pair);
    }
    std::memset(impl->pcCountArray, 0, sizeof(impl->pcCountArray));
    impl->pcCountInfo.clear();

    std::stringstream ss;
    ss << "store pc count:" << std::dec << lastPcCounts.size();
    g_MainThreadCaller.SyncLogToView(ss.str());
}
void MemorySniffer::KeepPcCount() const {
    auto&& pcCounts = impl->orderedPcCounts;
    pcCounts.clear();

    for (int hash = 0; hash < Impl::c_max_pc_entry_num; ++hash) {
        int startIx = hash * Impl::c_pc_num_per_entry;
        for (int ix = 0; ix < Impl::c_pc_num_per_entry; ++ix) {
            u64 v = impl->pcCountArray[startIx + ix];
            if (v != 0) {
                u64 pc = ((v & Impl::c_pc_other_mask) | (hash << Impl::c_pc_hash_shift));
                u64 ct = (v & Impl::c_pc_hash_mask);
                pcCounts.insert(std::make_pair(pc, ct));
            }
        }
    }
    for (auto&& pair : impl->pcCountInfo) {
        pcCounts.insert(pair);
    }
    for (auto&& pair : impl->lastPcCountInfo) {
        pcCounts.insert(pair);
    }
    std::memset(impl->pcCountArray, 0, sizeof(impl->pcCountArray));
    impl->pcCountInfo.clear();

    std::stringstream ss;
    ss << "keep pc count:" << std::dec << pcCounts.size();
    g_MainThreadCaller.SyncLogToView(ss.str());
}
void MemorySniffer::KeepNewPcCount() const {
    auto&& pcCounts = impl->orderedPcCounts;
    pcCounts.clear();

    auto&& lastPcCounts = impl->lastPcCountInfo;

    for (int hash = 0; hash < Impl::c_max_pc_entry_num; ++hash) {
        int startIx = hash * Impl::c_pc_num_per_entry;
        for (int ix = 0; ix < Impl::c_pc_num_per_entry; ++ix) {
            u64 v = impl->pcCountArray[startIx + ix];
            if (v != 0) {
                u64 pc = ((v & Impl::c_pc_other_mask) | (hash << Impl::c_pc_hash_shift));
                u64 ct = (v & Impl::c_pc_hash_mask);
                auto&& it = lastPcCounts.find(pc);
                if (it == lastPcCounts.end()) {
                    pcCounts.insert(std::make_pair(pc, ct));
                }
            }
        }
    }
    for (auto&& pair : impl->pcCountInfo) {
        auto&& it = lastPcCounts.find(pair.first);
        if (it == lastPcCounts.end()) {
            pcCounts.insert(pair);
        }
    }
    std::memset(impl->pcCountArray, 0, sizeof(impl->pcCountArray));
    impl->pcCountInfo.clear();

    std::stringstream ss;
    ss << "keep new pc count:" << std::dec << pcCounts.size();
    g_MainThreadCaller.SyncLogToView(ss.str());
}
void MemorySniffer::KeepSamePcCount() const {
    auto&& pcCounts = impl->orderedPcCounts;
    pcCounts.clear();

    auto&& lastPcCounts = impl->lastPcCountInfo;

    for (int hash = 0; hash < Impl::c_max_pc_entry_num; ++hash) {
        int startIx = hash * Impl::c_pc_num_per_entry;
        for (int ix = 0; ix < Impl::c_pc_num_per_entry; ++ix) {
            u64 v = impl->pcCountArray[startIx + ix];
            if (v != 0) {
                u64 pc = ((v & Impl::c_pc_other_mask) | (hash << Impl::c_pc_hash_shift));
                u64 ct = (v & Impl::c_pc_hash_mask);
                auto&& it = lastPcCounts.find(pc);
                if (it != lastPcCounts.end()) {
                    pcCounts.insert(std::make_pair(pc, ct));
                }
            }
        }
    }
    for (auto&& pair : impl->pcCountInfo) {
        auto&& it = lastPcCounts.find(pair.first);
        if (it != lastPcCounts.end()) {
            pcCounts.insert(pair);
        }
    }
    std::memset(impl->pcCountArray, 0, sizeof(impl->pcCountArray));
    impl->pcCountInfo.clear();

    std::stringstream ss;
    ss << "keep same pc count:" << std::dec << pcCounts.size();
    g_MainThreadCaller.SyncLogToView(ss.str());
}

void MemorySniffer::DumpPcCount(std::ostream& os, uint64_t maxCount) const {
    auto&& pcCounts = impl->orderedPcCounts;
    for (auto&& pair : pcCounts) {
        u64 pc = pair.first;
        u64 ct = pair.second;
        if (ct <= maxCount) {
            u64 offset = pc;
            std::string build_id;
            std::string name;
            CalcMemoryType(offset, build_id, name);
            os << "trace pc: " << std::hex << pc << " offset: " << offset
               << " build_id: " << build_id << " name: " << name << " count: " << std::dec << ct
               << std::endl;
        }
    }
}

int MemorySniffer::CalcMemoryType(u64& addr, std::string& build_id) const {
    std::string name;
    return CalcMemoryType(addr, build_id, name);
}

int MemorySniffer::CalcMemoryType(u64& addr, std::string& build_id, std::string& name) const {
    int mt = -1;
    for (auto&& minfo : impl->moduleMemArgs) {
        if (minfo.base == minfo.addr) {
            if (addr >= minfo.base && addr < minfo.base + minfo.size) {
                mt = 0;
                build_id = minfo.buildId;
                addr -= minfo.base;
                break;
            }
        }
    }
    if (mt < 0) {
        if (addr >= impl->heapBase && addr < impl->heapBase + impl->heapSize) {
            mt = 1;
            build_id = "heap";
            addr -= impl->heapBase;
        } else if (addr >= impl->aliasStart && addr < impl->aliasStart + impl->aliasSize) {
            build_id = "alias";
        } else if (addr >= impl->stackStart && addr < impl->stackStart + impl->stackSize) {
            build_id = "stack";
        } else if (addr >= impl->kernelStart && addr < impl->kernelStart + impl->kernelSize) {
            build_id = "kernel map";
        } else if (addr >= impl->codeStart && addr < impl->codeStart + impl->codeSize) {
            build_id = "code";
        } else if (addr >= impl->aliasCodeStart &&
                   addr < impl->aliasCodeStart + impl->aliasCodeSize) {
            build_id = "alias code";
        } else if (addr >= impl->addrSpaceStart &&
                   addr < impl->addrSpaceStart + impl->addrSpaceSize) {
            build_id = "other addr space";
        } else {
            build_id = "unknown";
        }
    }
    return mt;
}

void MemorySniffer::DumpMemoryTypes(std::ostream& os) const {
    if (nullptr == system.ApplicationProcess()) {
        return;
    }
    os << "===memory info===" << std::endl;
    for (auto&& minfo : impl->moduleMemArgs) {
        os << "name:" << minfo.name << " build id:" << minfo.buildId << " base:" << std::hex
           << minfo.base << " size:" << minfo.size << " program id:" << minfo.progId
           << " pid:" << minfo.pid << std::endl;
    }
    u64 progId = system.ApplicationProcess()->GetProgramId();
    u64 procId = system.ApplicationProcess()->GetProcessId();
    os << "heap base:" << std::hex << impl->heapBase << " size:" << impl->heapSize
       << " program id:" << progId << " pid:" << procId << std::endl;
    os << "alias start:" << std::hex << impl->aliasStart << " size:" << impl->aliasSize
       << " program id:" << progId << " pid:" << procId << std::endl;
    os << "stack start:" << std::hex << impl->stackStart << " size:" << impl->stackSize
       << " program id:" << progId << " pid:" << procId << std::endl;
    os << "kernel map start:" << std::hex << impl->kernelStart << " size:" << impl->kernelSize
       << " program id:" << progId << " pid:" << procId << std::endl;
    os << "code start:" << std::hex << impl->codeStart << " size:" << impl->codeSize
       << " program id:" << progId << " pid:" << procId << std::endl;
    os << "alias code start:" << std::hex << impl->aliasCodeStart << " size:" << impl->aliasCodeSize
       << " program id:" << progId << " pid:" << procId << std::endl;
    os << "addr space start:" << std::hex << impl->addrSpaceStart << " size:" << impl->addrSpaceSize
       << " program id:" << progId << " pid:" << procId << std::endl;
}

void MemorySniffer::DumpRegisterValues(std::ostream& os, bool includeStack) const {
    for (int ix = 0; ix < static_cast<int>(Core::Hardware::NUM_CPU_CORES); ++ix) {
        auto&& phyCore = system.Kernel().PhysicalCore(ix);
        if (ix > 0)
            os << std::endl;
        auto* pThread = phyCore.CurrentThread();
        if (pThread) {
            DumpRegisterValues(*pThread, os, includeStack);
        }
    }
}

void MemorySniffer::DumpRegisterValues(const Kernel::KThread& thread, std::ostream& os,
                                       bool includeStack) const {
    const int c_RegNum = 29;
    const int c_VecNum = 32;
    const int c_TlsNum = 16;
    const int c_StackNum = 32;
    auto* pProcess = thread.GetOwnerProcess();
    auto&& memory = pProcess->GetMemory();
    auto&& ctx = thread.GetContext();
    os << "[program id:" << std::hex << pProcess->GetProgramId()
       << " pid:" << pProcess->GetProcessId() << " tid:" << thread.GetThreadId() << "]"
       << std::endl;
    os << "[core " << std::dec << thread.GetCurrentCore() << "]";
    for (int regIx = 0; regIx < c_RegNum; ++regIx) {
        if (regIx % 16 == 0)
            os << std::endl;
        else
            os << " ";
        u64 regVal = ctx.r[regIx];
        os << "[reg " << std::dec << regIx << "] : " << std::hex << regVal;
    }
    for (int vecIx = 0; vecIx < c_VecNum; ++vecIx) {
        if (vecIx % 16 == 0)
            os << std::endl;
        else
            os << " ";
        u128 vecVal = ctx.v[vecIx];
        os << "[vec " << std::dec << vecIx << "] : " << std::hex << vecVal[0] << "," << vecVal[1];
    }
    auto&& sp = ctx.sp;
    auto* pStack = reinterpret_cast<u64*>(memory.GetPointerSilent(sp));
    if (nullptr != pStack) {
        for (int stackIx = 0; stackIx < c_StackNum; ++stackIx) {
            u64 val = *reinterpret_cast<u64*>(pStack + stackIx);
            if (stackIx % 16 == 0)
                os << std::endl;
            else
                os << " ";
            os << "[stack " << std::dec << stackIx << "] : " << std::hex << val;
        }
    }
    auto&& tls = thread.GetTlsAddress();
    auto* pTls = reinterpret_cast<u64*>(memory.GetPointerSilent(tls));
    if (nullptr != pTls) {
        for (int tlsIx = 0; tlsIx < c_TlsNum; ++tlsIx) {
            u64 val = *reinterpret_cast<u64*>(pTls + tlsIx);
            if (tlsIx % 16 == 0)
                os << std::endl;
            else
                os << " ";
            os << "[tls " << std::dec << tlsIx << "] : " << std::hex << val;
        }
    }
    os << std::endl;
    os << "pc:" << std::hex << ctx.pc << " ";
    os << "sp:" << std::hex << ctx.sp << " ";
    os << "pstate:" << std::hex << ctx.pstate << " ";
    os << "tls:" << std::hex << thread.GetTlsAddress().GetValue() << " ";
    os << "el0:" << std::hex << thread.GetTpidrEl0();
    if (includeStack) {
        auto&& backtrace = GetBacktrace(&thread);
        for (auto&& entry : backtrace) {
            os << std::endl;
            os << "module:" << entry.module << " addr:" << std::hex << entry.address
               << " ori_addr:" << std::hex << entry.original_address << " offset:" << std::hex
               << entry.offset << " name:" << entry.name;
            auto* ptr = memory.GetPointerSilent(entry.address);
            if (ptr) {
                os << " vaddr:" << std::hex << reinterpret_cast<u64>(ptr);
            }
        }
    }
}

const char* MemorySniffer::GetWatchTypeName(WatchPointType watchType) {
    switch (watchType) {
    case WatchPointType::Read:
        return "read";
    case WatchPointType::Write:
        return "write";
    case WatchPointType::GetPointer:
        return "getpointer";
    case WatchPointType::ReadCString:
        return "readcstring";
    case WatchPointType::NotWatchPoint:
    default:
        return "not watch point";
    }
}

} // namespace Core::Memory
