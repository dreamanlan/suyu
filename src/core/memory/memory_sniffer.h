// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>
#include <map>
#include <fstream>
#include "common/common_types.h"
#include "common/typed_address.h"

namespace Kernel {
class KProcess;
}

namespace Core {
class System;
class ArmInterface;
}

namespace Core::Memory {

struct MemoryModifyInfo
{
    enum ValueType
    {
        type_u8 = 0,
        type_u16,
        type_u32,
        type_u64,
    };

    Common::ProcessAddress addr;
    int type;
    union
    {
        u8 u8Val;
        u16 u16Val;
        u32 u32Val;
        u64 u64Val;
    };
    union
    {
        u8 u8OldVal;
        u16 u16OldVal;
        u32 u32OldVal;
        u64 u64OldVal;
    };
    uint64_t size;
    uint64_t pid;

    bool IsIncreased()const;
    bool IsDecreased()const;
    bool IsChanged()const;
    bool IsUnchanged()const;
    bool IsValue(uint64_t val)const;
};
using MemoryModifyInfoPtr = std::shared_ptr<MemoryModifyInfo>;
using MemoryModifyInfoMap = std::map<uint64_t, MemoryModifyInfoPtr>;

class MemorySniffer final {
public:
    using VisitMemoryArg = std::function<void(const char*, const char*, u64, u64, u64, u64, u64)>;
    enum class WatchPointType
    {
        NotWatchPoint = 0,
        Read,
        Write,
        GetPointer,
        ReadCString,
        MaxNum
    };
public:
    MemorySniffer(System& system_);
    ~MemorySniffer();

    void Initialize();
    void ClearModuleMemoryParameters();
    void AddModuleMemoryParameters(Kernel::KProcess& process, std::string&& file_name, std::string&& build_id, u64 base, u64 region_begin, u64 region_size);
    void VisitMemoryArgs(VisitMemoryArg visitor)const;
    uint64_t GetHeapBase(uint64_t& size)const;
    uint64_t GetStackBase(uint64_t& size)const;
    uint64_t GetCodeBase(uint64_t& size)const;
    uint64_t GetAliasBase(uint64_t& size)const;
    uint64_t GetAliasCodeBase(uint64_t& size)const;
    int GetModuleCount()const;
    uint64_t GetModuleBase(int ix, uint64_t& addr, uint64_t& size, std::string& build_id, std::string& name, uint64_t& progId, uint64_t& pid)const;
    void ClearSessionInfos();
    void AddSessionInfo(uint64_t id, const std::string& name, u32 handle);
    bool TryUpdateSession(uint64_t id, u32 handle);
    void ClearBreakPoints(Kernel::KProcess* process);
    bool AddBreakPoint(Kernel::KProcess& process, uint64_t addr);
    bool RemoveBreakPoint(Kernel::KProcess& process, uint64_t addr);
    bool EnableBreakPoint(Kernel::KProcess& process, uint64_t addr);
    bool DisableBreakPoint(Kernel::KProcess& process, uint64_t addr);
    bool IsBreakPoint(uint32_t addr)const;
    bool IsBreakPoint(uint64_t addr)const;
    uint64_t GetMaxStepCount()const;
public://call by core
    bool IsEnabled()const;
    bool IsTraceProcess(Kernel::KProcess& process)const;
    WatchPointType GetTraceOnAddr(WatchPointType watchType, uint64_t addr)const;
    WatchPointType GetTraceOnAddr(WatchPointType watchType, uint64_t addr, std::size_t size)const;
    bool IsStepInstruction(uint32_t inst)const;
    bool IsInTraceScope(uint64_t addr)const;
    uint64_t GetStartTraceAddr()const;
    uint64_t GetStopTraceAddr()const;
    bool TraceSvcCall(int swi, Core::ArmInterface& armIntf)const;

    void LogContext(const Kernel::KThread& thread)const;
    void TryLogCallStack(const Kernel::KThread& thread)const;
    void TryLogCallStack(WatchPointType watchType, uint64_t addr)const;
    void TryLogCallStack(WatchPointType watchType, const Common::ProcessAddress addr)const;
    void TryLogCallStack(WatchPointType watchType, uint64_t addr, std::size_t size)const;
    void TryLogCallStack(WatchPointType watchType, const Common::ProcessAddress addr, std::size_t size)const;
public://call by user
    void SetEnable(bool val);
    void GetMemorySearchInfo(uint64_t& scopeBegin, uint64_t& scopeEnd, uint64_t& step, uint64_t& valueSize, uint64_t& range, uint64_t& maxCount, uint64_t& pid)const;
    void MarkMemoryDebug(uint64_t pid, uint64_t addr, uint64_t size, bool debug) const;
    void AddSniffing(uint64_t pid, uint64_t addr, uint64_t size, uint64_t step, uint64_t cur_val);
    void AddLogInstruction(uint32_t mask, uint32_t value);

    void SetResultMemoryModifyInfo(MemoryModifyInfoMap&& newResult);
    const MemoryModifyInfoMap& GetResultMemoryModifyInfo()const;
    const MemoryModifyInfoMap& GetLastHistoryMemoryModifyInfo()const;
    int GetHistoryMemoryModifyInfoCount()const;
    const MemoryModifyInfoMap& GetHistoryMemoryModifyInfo(int index)const;
    const MemoryModifyInfoMap& GetLastRollbackMemoryModifyInfo()const;
    int GetRollbackMemoryModifyInfoCount()const;
    const MemoryModifyInfoMap& GetRollbackMemoryModifyInfo(int index)const;

    MemoryModifyInfoMap* GetResultMemoryModifyInfoPtr();
    MemoryModifyInfoMap* GetLastHistoryMemoryModifyInfoPtr();

    void ClearAll();
    void RefreshSnapshot();
    void KeepUnchanged();
    void KeepChanged();
    void KeepIncreased();
    void KeepDecreased();
    void Rollback();
    void Unrollback();
    void KeepValue(uint64_t val);
    void AddToTraceWrite();
    bool Exec(const std::string& cmd, const std::string& arg);
    void SaveAbsAsCheatVM(const char* file_path, const char* tag)const;
    void SaveRelAsCheatVM(const char* file_path, const char* tag)const;

    void SaveResult(const char* file_path)const;
    void SaveHistory(const char* file_path)const;
    void SaveRollback(const char* file_path)const;

    Kernel::KProcess* GetProcess(u64 pid) const;
    uint64_t ReadMemory(Kernel::KProcess& process, uint64_t addr, uint64_t typeSizeOf,
                        bool& succ) const;
    bool WriteMemory(Kernel::KProcess& process, uint64_t addr, uint64_t typeSizeOf,
                     uint64_t val) const;
    bool DumpMemory(Kernel::KProcess& process, uint64_t addr, uint64_t size,
                    std::ostream& os) const;
    bool LoadMemory(Kernel::KProcess& process, uint64_t addr, uint64_t size,
                    std::istream& is) const;
    bool ProtectMemory(Kernel::KProcess& process, uint64_t addr, uint64_t size, int flag) const;
    bool MapMemory(Kernel::KProcess& process, uint64_t addr, uint64_t size) const;
    bool UnmapMemory(Kernel::KProcess& process, uint64_t addr, uint64_t size) const;
    uint64_t FindUnmapMemory(Kernel::KProcess& process, uint64_t addr, uint64_t size,
                             uint64_t expect_size) const;

    void StorePcCount()const;
    void KeepPcCount()const;
    void KeepNewPcCount()const;
    void KeepSamePcCount()const;
    void DumpPcCount(std::ostream& os, uint64_t maxCount)const;
private:
    int CalcMemoryType(u64& addr, std::string& build_id)const;
    int CalcMemoryType(u64& addr, std::string& build_id, std::string& name)const;
    void DumpMemoryTypes(std::ostream& os)const;
    void DumpRegisterValues(std::ostream& os, bool includeStack)const;
    void DumpRegisterValues(const Kernel::KThread& thread, std::ostream& os, bool includeStack)const;
private:
    Core::System& system;

    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    static const char* GetWatchTypeName(WatchPointType watchType);
};

} // namespace Core::Memory
