// SPDX-License-Identifier: LGPL-2.1-only
/// Guest fiber creation, context switching, and lifetime.

#include "fiber_fls.hpp"
#include "thread.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include <algorithm>
#include <vector>

namespace vitreo::winapi {

void FlsManager::bind_runtime(GuestMemory& memory, VirtualMemoryManager& vmm,
                              GuestAddr exit_thread) {
    m_memory = &memory;
    m_vmm = &vmm;
    m_exit_thread = exit_thread;
}

void FlsManager::publish_fiber(cpu::CpuState& cpu, u64 address, const Fiber& fiber) {
    const u64 teb = cpu.is_64bit() ? cpu.seg.gs_desc.base : cpu.seg.fs_desc.base;
    if (!teb) return;
    if (cpu.is_64bit()) {
        (void)m_memory->write_u64(teb + 0x08, fiber.stack_base);
        (void)m_memory->write_u64(teb + 0x10, fiber.stack_limit);
        (void)m_memory->write_u64(teb + 0x20, address);
    } else {
        (void)m_memory->write_u32(teb, static_cast<u32>(fiber.exception_list));
        (void)m_memory->write_u32(teb + 0x04, static_cast<u32>(fiber.stack_base));
        (void)m_memory->write_u32(teb + 0x08, static_cast<u32>(fiber.stack_limit));
        (void)m_memory->write_u32(teb + 0x10, static_cast<u32>(address));
    }
}

u64 FlsManager::convert_thread_to_fiber(u64 parameter, cpu::CpuState* cpu, u32 flags) {
    std::lock_guard lock(m_mutex);
    const u32 tid = ThreadManager::get_current_thread_id_static();
    if (flags & ~1u) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (m_current_fibers.contains(tid)) {
        set_last_error(1280); // ERROR_ALREADY_FIBER
        return 0;
    }
    if (!m_vmm) {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        return 0;
    }
    const u64 address = m_vmm->virtual_alloc(0, 0x1000, 0x3000, 0x04);
    if (!address) return 0;
    auto fiber = std::make_unique<Fiber>();
    fiber->parameter = parameter;
    fiber->flags = flags;
    fiber->converted = true;
    fiber->running_thread = tid;
    (void)m_memory->write_u64(address, parameter);
    if (cpu) {
        cpu->materialize_lazy_flags();
    fiber->state = *cpu;
        const u64 teb = cpu->is_64bit() ? cpu->seg.gs_desc.base : cpu->seg.fs_desc.base;
        if (teb) {
            if (cpu->is_64bit()) {
                fiber->stack_base = m_memory->read_u64(teb + 8).value_or(0);
                fiber->stack_limit = m_memory->read_u64(teb + 16).value_or(0);
            } else {
                fiber->exception_list = m_memory->read_u32(teb).value_or(0xffffffffu);
                fiber->stack_base = m_memory->read_u32(teb + 4).value_or(0);
                fiber->stack_limit = m_memory->read_u32(teb + 8).value_or(0);
            }
        }
        publish_fiber(*cpu, address, *fiber);
    }
    m_current_fibers[tid] = address;
    m_fibers[address] = std::move(fiber);
    return address;
}

bool FlsManager::convert_fiber_to_thread(cpu::CpuState* cpu) {
    std::lock_guard lock(m_mutex);
    const u32 tid = ThreadManager::get_current_thread_id_static();
    auto current = m_current_fibers.find(tid);
    if (current == m_current_fibers.end()) {
        set_last_error(1281); // ERROR_ALREADY_THREAD
        return false;
    }
    auto& fiber = *m_fibers.at(current->second);
    if (!fiber.converted) {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        return false;
    }
    if (cpu) publish_fiber(*cpu, 0, fiber);
    (void)m_vmm->virtual_free(current->second, 0, 0x8000);
    m_fibers.erase(current->second);
    m_current_fibers.erase(current);
    return true;
}

bool FlsManager::is_thread_a_fiber() const noexcept {
    std::lock_guard lock(m_mutex);
    return m_current_fibers.contains(ThreadManager::get_current_thread_id_static());
}

u64 FlsManager::get_fiber_data() const noexcept {
    std::lock_guard lock(m_mutex);
    auto it = m_current_fibers.find(ThreadManager::get_current_thread_id_static());
    return it == m_current_fibers.end() ? 0 : m_fibers.at(it->second)->parameter;
}

namespace {

/// The process image's SizeOfStackReserve / SizeOfStackCommit (PE optional
/// header offset 72: two ULONGLONGs in PE32+, two DWORDs in PE32), found
/// through TEB.ProcessEnvironmentBlock -> PEB.ImageBaseAddress. 0 when the
/// header cannot be read.
void image_stack_defaults(GuestMemory& memory, const cpu::CpuState& cpu, u64& reserve,
                          u64& commit) {
    reserve = 0;
    commit = 0;
    const bool x64 = cpu.is_64bit();
    const u64 teb = x64 ? cpu.seg.gs_desc.base : cpu.seg.fs_desc.base;
    if (teb == 0) return;
    const u64 peb = x64 ? memory.read_u64(teb + 0x60).value_or(0)
                        : memory.read_u32(teb + 0x30).value_or(0);
    if (peb == 0) return;
    const u64 image = x64 ? memory.read_u64(peb + 0x10).value_or(0)
                          : memory.read_u32(peb + 0x08).value_or(0);
    if (image == 0) return;
    const u32 lfanew = memory.read_u32(image + 0x3c).value_or(0);
    const u64 optional = image + lfanew + 24;
    if (x64) {
        reserve = memory.read_u64(optional + 72).value_or(0);
        commit = memory.read_u64(optional + 80).value_or(0);
    } else {
        reserve = memory.read_u32(optional + 72).value_or(0);
        commit = memory.read_u32(optional + 76).value_or(0);
    }
}

} // namespace

void FlsManager::create_fiber(Marshaler& m, bool extended) {
    auto* cpu = m.cpu();
    u64 commit = m.arg(0);
    u64 reserve = extended ? m.arg(1) : 0;
    const u32 flags = extended ? static_cast<u32>(m.arg(2)) : 0;
    const u64 start = m.arg_ptr(extended ? 3 : 1);
    const u64 parameter = m.arg(extended ? 4 : 2);
    if (!m_vmm || !cpu || !m_exit_thread || (flags & ~1u)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    // Stack sizing, measured on Windows 11 (tools/oracle_probe/
    // fiber_stack_size_probe.c, x86 and x64 identical): a zero commit or
    // reserve takes the image header's value; when the commit reaches the
    // reserve, the reserve becomes the commit rounded up to a multiple of
    // 1 MB; otherwise the reserve is rounded to the 64 KB allocation
    // granularity. CreateFiber's dwStackSize is the commit size.
    {
        u64 image_reserve = 0;
        u64 image_commit = 0;
        image_stack_defaults(*m_memory, *cpu, image_reserve, image_commit);
        if (reserve == 0) reserve = image_reserve ? image_reserve : 0x100000;
        if (commit == 0) commit = image_commit ? image_commit : 0x1000;
        commit = (commit + 0xfff) & ~0xfffull;
        if (commit >= reserve) reserve = (commit + 0xfffff) & ~0xfffffull;
    }
    if (reserve > 0xffffffffu - 0x10000) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        m.set_return(0);
        return;
    }
    reserve = (reserve + 0xffff) & ~0xffffull;
    // Match the runtime's fully backed guest thread stacks. A bottom no-access
    // page bounds the allocation without substituting a host stack.
    const u64 stack = m_vmm->virtual_alloc(0, reserve + 0x1000, 0x3000, 0x04);
    if (!stack) { m.set_return(0); return; }
    u32 old_protect = 0;
    (void)m_vmm->virtual_protect(stack, 0x1000, 0x01, &old_protect);
    const u64 address = m_vmm->virtual_alloc(0, 0x1000, 0x3000, 0x40);
    if (!address) {
        (void)m_vmm->virtual_free(stack, 0, 0x8000);
        m.set_return(0);
        return;
    }
    auto fiber = std::make_unique<Fiber>();
    fiber->state = *cpu;
    fiber->state.rip = start;
    fiber->parameter = parameter;
    fiber->stack_allocation = stack;
    fiber->stack_limit = stack + 0x1000;
    fiber->stack_base = stack + 0x1000 + reserve;
    fiber->exception_list = 0xffffffffu;
    fiber->flags = flags;
    (void)m_memory->write_u64(address, parameter);
    // Returning from FiberProc terminates the executing thread (MS Learn,
    // FiberProc); enter the real ExitThread thunk with exit code zero.
    const u64 trampoline = address + 0x40;
    if (cpu->is_64bit()) {
        const u8 code[] = {0x31,0xc9,0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};
        (void)m_memory->write_bytes(trampoline, code, sizeof(code));
        (void)m_memory->write_u64(trampoline + 4, m_exit_thread);
        fiber->state.set_sp(fiber->stack_base - 0x28);
        fiber->state.gpr.rcx = parameter;
        (void)m_memory->write_u64(fiber->state.sp(), trampoline);
    } else {
        const u8 code[] = {0x6a,0,0x6a,0,0xb8,0,0,0,0,0xff,0xe0};
        (void)m_memory->write_bytes(trampoline, code, sizeof(code));
        (void)m_memory->write_u32(trampoline + 5, static_cast<u32>(m_exit_thread));
        fiber->state.set_sp(fiber->stack_base - 8);
        (void)m_memory->write_u32(fiber->state.sp(), static_cast<u32>(trampoline));
        (void)m_memory->write_u32(fiber->state.sp() + 4, static_cast<u32>(parameter));
    }
    std::lock_guard lock(m_mutex);
    m_fibers[address] = std::move(fiber);
    m.set_return(address);
}

void FlsManager::switch_to_fiber(Marshaler& m) {
    auto* cpu = m.cpu();
    const u64 address = m.arg_ptr(0);
    std::lock_guard lock(m_mutex);
    const u32 tid = ThreadManager::get_current_thread_id_static();
    auto current = m_current_fibers.find(tid);
    auto next = m_fibers.find(address);
    if (!cpu || current == m_current_fibers.end() || next == m_fibers.end() ||
        current->second == address || next->second->running_thread != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        if (cpu) {
            cpu->pending_raise.active = true;
            cpu->pending_raise.code = 0xc0000005u;
        }
        return;
    }
    auto& previous = *m_fibers.at(current->second);
    auto& target = *next->second;
    cpu->materialize_lazy_flags();
    previous.state = *cpu;
    // Save the continuation after the SwitchToFiber thunk, not its gateway.
    const u64 sp = cpu->sp();
    previous.state.rip = cpu->is_64bit()
        ? m_memory->read_u64(sp).value_or(0)
        : m_memory->read_u32(sp).value_or(0);
    previous.state.set_sp(sp + 8);
    // Already under m_mutex, so reach the block directly rather than through
    // thread_slots(). Never replace an existing block: a live thread-local
    // cache holds its address.
    auto& live_slots = m_thread_values[tid];
    if (!live_slots) live_slots = std::make_unique<FlsThreadSlots>();
    for (u32 i = 0; i < MAX_FLS_SLOTS; ++i) {
        previous.values[i] = live_slots->values[i].load(std::memory_order_relaxed);
    }
    previous.running_thread = 0;
    const u64 teb = cpu->is_64bit() ? cpu->seg.gs_desc.base : cpu->seg.fs_desc.base;
    if (teb) {
        previous.stack_base = cpu->is_64bit() ? m_memory->read_u64(teb+8).value_or(0) : m_memory->read_u32(teb+4).value_or(0);
        previous.stack_limit = cpu->is_64bit() ? m_memory->read_u64(teb+16).value_or(0) : m_memory->read_u32(teb+8).value_or(0);
        if (!cpu->is_64bit()) previous.exception_list = m_memory->read_u32(teb).value_or(0xffffffffu);
    }
    cpu->gpr = target.state.gpr;
    cpu->rip = target.state.rip;
    cpu->flags = target.state.flags;
    cpu->flag_op = cpu::CpuState::FlagOp::None;
    cpu->cf_overlay = cpu->of_overlay = cpu->nzcv_kind = 0;
    if (cpu->is_64bit() || (target.flags & 1u)) {
        cpu->fpu = target.state.fpu;
        cpu->sse = target.state.sse;
        cpu->avx = target.state.avx;
    }
    for (u32 i = 0; i < MAX_FLS_SLOTS; ++i) {
        live_slots->values[i].store(target.values[i], std::memory_order_relaxed);
    }
    target.running_thread = tid;
    current->second = address;
    publish_fiber(*cpu, address, target);
    cpu->thunk_control_transfer = true;
}

void FlsManager::delete_fiber(Marshaler& m) {
    const u64 address = m.arg_ptr(0);
    std::unique_ptr<Fiber> fiber;
    std::vector<std::pair<GuestAddr,u64>> cleanup;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_fibers.find(address);
        if (it == m_fibers.end()) return;
        if (it->second->running_thread != 0) {
            if (it->second->running_thread == ThreadManager::get_current_thread_id_static()) {
                if (m.cpu()->is_64bit()) m.cpu()->gpr.rcx = 0;
                else (void)m_memory->write_u32(m.cpu()->sp() + 4, 0);
                m.cpu()->rip = m_exit_thread;
                m.cpu()->thunk_control_transfer = true;
            }
            return;
        }
        fiber = std::move(it->second);
        m_fibers.erase(it);
        for (u32 i = 0; i < MAX_FLS_SLOTS; ++i) {
            if (m_slots[i].allocated.load(std::memory_order_relaxed) &&
                m_slots[i].callback && fiber->values[i])
                cleanup.emplace_back(m_slots[i].callback, fiber->values[i]);
        }
    }
    for (auto [callback,value] : cleanup) if (m_cleanup) m_cleanup(callback,value);
    if (fiber->stack_allocation) (void)m_vmm->virtual_free(fiber->stack_allocation, 0, 0x8000);
    (void)m_vmm->virtual_free(address, 0, 0x8000);
}

} // namespace vitreo::winapi
