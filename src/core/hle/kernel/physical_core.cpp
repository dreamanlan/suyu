// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/debugger/debugger.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/svc.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "core/memory/memory_sniffer.h"

namespace Kernel {

PhysicalCore::PhysicalCore(KernelCore& kernel, std::size_t core_index)
    : m_kernel{ kernel }, m_core_index{ core_index }, m_pccount{false}, m_tracing{false}, m_addrForEnableBreakPoint{0} {
    m_is_single_core = !kernel.IsMulticore();
}
PhysicalCore::~PhysicalCore() = default;

void PhysicalCore::StartPcCount() {
    m_pccount = true;
}

void PhysicalCore::StopPcCount() {
    m_pccount = true;
}

void PhysicalCore::StartTrace() {
    m_tracing = true;
}

void PhysicalCore::StopTrace() {
    m_tracing = false;
}

void PhysicalCore::RunThread(Kernel::KThread* thread) {
    auto* process = thread->GetOwnerProcess();
    auto& system = m_kernel.System();
    auto* interface = process->GetArmInterface(m_core_index);

    const auto EnterContext = [&]() {
        system.EnterCPUProfile();

        // Lock the core context.
        std::scoped_lock lk{m_guard};

        // Check if we are already interrupted. If we are, we can just stop immediately.
        if (m_is_interrupted) {
            return false;
        }

        // Mark that we are running.
        m_arm_interface = interface;
        m_current_thread = thread;

        // Acquire the lock on the thread parameters.
        // This allows us to force synchronization with Interrupt.
        interface->LockThread(thread);

        return true;
    };

    const auto ExitContext = [&]() {
        // Unlock the thread.
        interface->UnlockThread(thread);

        // Lock the core context.
        std::scoped_lock lk{m_guard};

        // On exit, we no longer are running.
        m_arm_interface = nullptr;
        m_current_thread = nullptr;

        system.ExitCPUProfile();
    };

    auto&& ctx = thread->GetContext();
    uint64_t stepCount = 0;
    while (true) {
        // If the thread is scheduled for termination, exit.
        if (thread->HasDpc() && thread->IsTerminationRequested()) {
            thread->Exit();
        }

        if (system.DebuggerEnabled()) {
            // Notify the debugger and go to sleep if a step was performed
            // and this thread has been scheduled again.
            if (thread->GetStepState() == StepState::StepPerformed) {
                system.GetDebugger().NotifyThreadStopped(thread);
                thread->RequestSuspend(SuspendType::Debug);
                return;
            }
        }

        // Otherwise, run the thread.
        Core::HaltReason hr{};
        {
            // If we were interrupted, exit immediately.
            if (!EnterContext()) {
                return;
            }

            bool isInScope = false;
            if (m_tracing) {
                u64 pc = ctx.pc;
                if (system.MemorySniffer().GetStopTraceAddr() == pc &&
                    system.MemorySniffer().IsTraceProcess(*process)) {
                    system.MemorySniffer().TryLogCallStack(*thread);
                    m_tracing = false;
                    system.MemorySniffer().RemoveBreakPoint(*process, pc);
                } else if (system.MemorySniffer().IsInTraceScope(pc) &&
                           system.MemorySniffer().IsTraceProcess(*process)) {
                    system.MemorySniffer().TryLogCallStack(*thread);
                    isInScope = true;
                }
            }

            if (((m_tracing || m_addrForEnableBreakPoint != 0) && isInScope &&
                 stepCount < system.MemorySniffer().GetMaxStepCount() &&
                 system.MemorySniffer().IsTraceProcess(*process)) ||
                thread->GetStepState() == StepState::StepPending) {
                hr = interface->StepThread(thread);
                interface->GetContext(ctx);
                ++stepCount;

                if (True(hr & Core::HaltReason::StepThread)) {
                    if (m_tracing || m_addrForEnableBreakPoint != 0)
                        thread->SetStepState(StepState::NotStepping);
                    else
                        thread->SetStepState(StepState::StepPerformed);
                }

                u64 pc = ctx.pc;
                if (m_addrForEnableBreakPoint != 0 && m_addrForEnableBreakPoint != pc) {
                    system.MemorySniffer().EnableBreakPoint(*process, m_addrForEnableBreakPoint);
                    m_addrForEnableBreakPoint = 0;
                }
            } else {
                if (m_pccount && system.MemorySniffer().IsTraceProcess(*process)) {
                    u64 pc = ctx.pc;
                    u32 inst = process->GetMemory().Read32(pc);
                    if (system.MemorySniffer().IsInTraceScope(pc) && system.MemorySniffer().IsStepInstruction(inst)) {
                        system.MemorySniffer().LogContext(*thread);
                    }
                }
                hr = interface->RunThread(thread);
                interface->GetContext(ctx);
            }

            ExitContext();
        }

        // Determine why we stopped.
        const bool supervisor_call = True(hr & Core::HaltReason::SupervisorCall);
        const bool prefetch_abort = True(hr & Core::HaltReason::PrefetchAbort);
        const bool breakpoint = True(hr & Core::HaltReason::InstructionBreakpoint);
        const bool data_abort = True(hr & Core::HaltReason::DataAbort);
        const bool interrupt = True(hr & Core::HaltReason::BreakLoop);

        // Since scheduling may occur here, we cannot use any cached
        // state after returning from calls we make.

        // Notify the debugger and go to sleep if a breakpoint was hit,
        // or if the thread is unable to continue for any reason.
        if (breakpoint || prefetch_abort) {
            if (breakpoint) {
                interface->RewindBreakpointInstruction();
            }
            u64 pc = ctx.pc;
            if (system.MemorySniffer().IsBreakPoint(pc) &&
                system.MemorySniffer().IsTraceProcess(*process)) {
                system.MemorySniffer().TryLogCallStack(*thread);
                if (system.MemorySniffer().GetStartTraceAddr() == pc) {
                    m_tracing = true;
                    system.MemorySniffer().RemoveBreakPoint(*process, pc);
                }
                else if (system.MemorySniffer().GetStopTraceAddr() == pc) {
                    m_tracing = false;
                    system.MemorySniffer().RemoveBreakPoint(*process, pc);
                }
                else {
                    m_addrForEnableBreakPoint = pc;
                    system.MemorySniffer().DisableBreakPoint(*process, pc);
                }
                thread->Resume(SuspendType::Backtrace);
            }
            else {
                if (system.DebuggerEnabled()) {
                    system.GetDebugger().NotifyThreadStopped(thread);
                }
                else {
                    interface->LogBacktrace(process);
                }
                thread->RequestSuspend(SuspendType::Debug);
            }
            return;
        }

        // Notify the debugger and go to sleep on data abort.
        if (data_abort) {
            if (system.DebuggerEnabled()) {
                system.GetDebugger().NotifyThreadWatchpoint(thread, *interface->HaltedWatchpoint());
            }
            thread->RequestSuspend(SuspendType::Debug);
            return;
        }

        // Handle system calls.
        if (supervisor_call) {
            // Perform call.
            Svc::Call(system, interface->GetSvcNumber());
            return;
        }

        // Handle external interrupt sources.
        if (interrupt || m_is_single_core) {
            return;
        }
    }
}

void PhysicalCore::LoadContext(const KThread* thread) {
    auto* const process = thread->GetOwnerProcess();
    if (!process) {
        // Kernel threads do not run on emulated CPU cores.
        return;
    }

    auto* interface = process->GetArmInterface(m_core_index);
    if (interface) {
        interface->SetContext(thread->GetContext());
        interface->SetTpidrroEl0(GetInteger(thread->GetTlsAddress()));
        interface->SetWatchpointArray(&process->GetWatchpoints());
    }
}

void PhysicalCore::LoadSvcArguments(const KProcess& process, std::span<const uint64_t, 8> args) {
    process.GetArmInterface(m_core_index)->SetSvcArguments(args);
}

void PhysicalCore::SaveContext(KThread* thread) const {
    auto* const process = thread->GetOwnerProcess();
    if (!process) {
        // Kernel threads do not run on emulated CPU cores.
        return;
    }

    auto* interface = process->GetArmInterface(m_core_index);
    if (interface) {
        interface->GetContext(thread->GetContext());
    }
}

void PhysicalCore::SaveSvcArguments(KProcess& process, std::span<uint64_t, 8> args) const {
    process.GetArmInterface(m_core_index)->GetSvcArguments(args);
}

void PhysicalCore::CloneFpuStatus(KThread* dst) const {
    auto* process = dst->GetOwnerProcess();

    Svc::ThreadContext ctx{};
    process->GetArmInterface(m_core_index)->GetContext(ctx);

    dst->GetContext().fpcr = ctx.fpcr;
    dst->GetContext().fpsr = ctx.fpsr;
}

void PhysicalCore::LogBacktrace() {
    auto* process = GetCurrentProcessPointer(m_kernel);
    if (!process) {
        return;
    }

    auto* interface = process->GetArmInterface(m_core_index);
    if (interface) {
        interface->LogBacktrace(process);
    }
}

void PhysicalCore::Idle() {
    std::unique_lock lk{m_guard};
    m_on_interrupt.wait(lk, [this] { return m_is_interrupted; });
}

bool PhysicalCore::IsInterrupted() const {
    return m_is_interrupted;
}

void PhysicalCore::Interrupt() {
    // Lock core context.
    std::scoped_lock lk{m_guard};

    // Load members.
    auto* arm_interface = m_arm_interface;
    auto* thread = m_current_thread;

    // Add interrupt flag.
    m_is_interrupted = true;

    // Interrupt ourselves.
    m_on_interrupt.notify_one();

    // If there is no thread running, we are done.
    if (arm_interface == nullptr) {
        return;
    }

    // Interrupt the CPU.
    arm_interface->SignalInterrupt(thread);
}

void PhysicalCore::ClearInterrupt() {
    std::scoped_lock lk{m_guard};
    m_is_interrupted = false;
}

bool PhysicalCore::IsAArch64() const {
    return m_arm_interface->GetArchitecture() == Core::Architecture::AArch64;
}
bool PhysicalCore::IsAArch32() const {
    return m_arm_interface->GetArchitecture() == Core::Architecture::AArch32;
}

} // namespace Kernel
