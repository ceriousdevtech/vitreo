// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Execution loop: fatal and trace dump helpers (pctrace caller dump,
// fast-fail context, PC-watch memory, fatal guest context, stack scan).
// Split out of guest_run_loop.cpp as verbatim code motion.

#include "runtime/execution/guest_run_loop_internal.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "core/memory/guest_memory.hpp"
#include <vitreo/diagnostics.hpp>
#include "runtime/diagnostics/pc_trace.hpp"
#include "runtime/diagnostics/hostcall_ring.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

/// VITREO_PCTRACE_CALLERDUMP: when a traced-range exception fires, dump guest
/// bytes around the return address at [rsp]. Names the (runtime-decrypted)
/// call instruction that sent execution into the traced range — static
/// disassembly cannot, because protected images only hold those bytes in
/// memory. Diagnostic only; enabled by env, prints at most a few lines.
void pctrace_dump_caller(std::FILE* out, vitreo::cpu::CpuState& cpu,
                         vitreo::GuestMemory& memory) {
    static const bool enabled =
        std::getenv("VITREO_PCTRACE_CALLERDUMP") != nullptr;
    if (!enabled) return;
    const auto ret = memory.read_u64(cpu.sp());
    if (ret.is_err()) {
        std::fprintf(out, "[pctrace] callerdump: [rsp] unreadable\n");
        return;
    }
    const vitreo::u64 ra = ret.value();
    std::fprintf(out, "[pctrace] callerdump ret=[rsp]=0x%llx rsp=0x%llx\n",
                 static_cast<unsigned long long>(ra),
                 static_cast<unsigned long long>(cpu.sp()));
    // 32 bytes before the return address cover any x86 call encoding; 16
    // after give the landing context.
    for (vitreo::u64 base = ra >= 32 ? ra - 32 : 0; base < ra + 16;
         base += 16) {
        char line[16 * 3 + 1] = {0};
        vitreo::usize pos = 0;
        for (vitreo::u64 a = base; a < base + 16; ++a) {
            const auto b = memory.read_u8(a);
            if (b.is_err()) {
                pos += static_cast<vitreo::usize>(
                    std::snprintf(line + pos, sizeof line - pos, " ??"));
            } else {
                pos += static_cast<vitreo::usize>(
                    std::snprintf(line + pos, sizeof line - pos, " %02x",
                                  b.value()));
            }
        }
        std::fprintf(out, "[pctrace]   0x%llx:%s\n",
                     static_cast<unsigned long long>(base), line);
    }
    std::fflush(out);
}

void dump_fastfail_context(const vitreo::cpu::CpuState& cpu,
                                  vitreo::GuestMemory& memory, vitreo::u64 code) {
    vitreo_pctrace_dump_ring("guest-fast-fail");
    vitreo::diagnostics::dump_hostcalls("guest-fast-fail");
    // Learn: /cpp/intrinsics/fastfail — x86 INT 0x29 takes the code in ECX;
    // x64 in RCX. User-mode termination is STATUS_STACK_BUFFER_OVERRUN
    // (0xC0000409) and "no exception handlers are invoked".
    if (!cpu.is_64bit()) {
        std::fprintf(stderr,
            "[vitreo] fast fail code=0x%x at eip=0x%08x"
            " eax=0x%08x ecx=0x%08x edx=0x%08x ebx=0x%08x"
            " esp=0x%08x ebp=0x%08x esi=0x%08x edi=0x%08x"
            " — terminating process (0xC0000409)\n",
            static_cast<unsigned>(code),
            static_cast<unsigned>(cpu.rip),
            static_cast<unsigned>(cpu.gpr.rax),
            static_cast<unsigned>(cpu.gpr.rcx),
            static_cast<unsigned>(cpu.gpr.rdx),
            static_cast<unsigned>(cpu.gpr.rbx),
            static_cast<unsigned>(cpu.gpr.rsp),
            static_cast<unsigned>(cpu.gpr.rbp),
            static_cast<unsigned>(cpu.gpr.rsi),
            static_cast<unsigned>(cpu.gpr.rdi));
        for (vitreo::u32 slot = 0; slot < 16; ++slot) {
            const auto word = memory.read_u32(
                static_cast<vitreo::GuestAddr>(
                    static_cast<vitreo::u32>(cpu.gpr.rsp) + slot * 4u));
            if (word.is_err()) {
                std::fprintf(stderr, "[fastfail-stack] [%u] <unreadable>\n", slot);
                break;
            }
            std::fprintf(stderr, "[fastfail-stack] [%u] 0x%08x\n", slot,
                         word.value());
        }
        vitreo::u32 frame = static_cast<vitreo::u32>(cpu.gpr.rbp);
        for (int depth = 0; depth < 8; ++depth) {
            if (frame < 0x10000u) break;
            const auto saved_ebp = memory.read_u32(frame);
            const auto ret = memory.read_u32(frame + 4);
            if (saved_ebp.is_err() || ret.is_err()) {
                std::fprintf(stderr, "[fastfail-frame] [%d] ebp=0x%08x <unreadable>\n",
                             depth, frame);
                break;
            }
            std::fprintf(stderr, "[fastfail-frame] [%d] ebp=0x%08x ret=0x%08x\n",
                         depth, frame, ret.value());
            if (saved_ebp.value() <= frame) break;
            frame = saved_ebp.value();
        }
        return;
    }
    std::fprintf(stderr,
        "[vitreo] fast fail code=0x%llx at rip=0x%llx"
        " rax=0x%llx rcx=0x%llx rdx=0x%llx rbx=0x%llx"
        " rsp=0x%llx rbp=0x%llx rsi=0x%llx rdi=0x%llx"
        " r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx"
        " r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx"
        " — terminating process (0xC0000409)\n",
        static_cast<unsigned long long>(code),
        static_cast<unsigned long long>(cpu.rip),
        static_cast<unsigned long long>(cpu.gpr.rax),
        static_cast<unsigned long long>(cpu.gpr.rcx),
        static_cast<unsigned long long>(cpu.gpr.rdx),
        static_cast<unsigned long long>(cpu.gpr.rbx),
        static_cast<unsigned long long>(cpu.gpr.rsp),
        static_cast<unsigned long long>(cpu.gpr.rbp),
        static_cast<unsigned long long>(cpu.gpr.rsi),
        static_cast<unsigned long long>(cpu.gpr.rdi),
        static_cast<unsigned long long>(cpu.gpr.r8),
        static_cast<unsigned long long>(cpu.gpr.r9),
        static_cast<unsigned long long>(cpu.gpr.r10),
        static_cast<unsigned long long>(cpu.gpr.r11),
        static_cast<unsigned long long>(cpu.gpr.r12),
        static_cast<unsigned long long>(cpu.gpr.r13),
        static_cast<unsigned long long>(cpu.gpr.r14),
        static_cast<unsigned long long>(cpu.gpr.r15));
    for (vitreo::u32 slot = 0; slot < 16; ++slot) {
        const auto word = memory.read_u64(
            static_cast<vitreo::GuestAddr>(cpu.gpr.rsp + slot * 8u));
        if (word.is_err()) {
            std::fprintf(stderr, "[fastfail-stack] [%u] <unreadable>\n", slot);
            break;
        }
        std::fprintf(stderr, "[fastfail-stack] [%u] 0x%llx\n", slot,
                     static_cast<unsigned long long>(word.value()));
    }
    // x64 frames need no frame pointer, so walk by evidence instead: a stack
    // qword is reported as a return address when the bytes before it encode
    // a CALL (E8 rel32, or FF /2 in its register, [reg], disp8, disp32, SIB
    // and RIP-relative forms, with or without REX).
    {
        auto is_call_return = [&](vitreo::u64 ret) {
            vitreo::u8 b[7]{};
            if (ret < 0x10000ull || ret >= 0x0000800000000000ull ||
                memory.read_bytes(static_cast<vitreo::GuestAddr>(ret - 7), b, 7)
                    .is_err()) {
                return false;
            }
            const auto ff2 = [](vitreo::u8 op, vitreo::u8 modrm) {
                return op == 0xFF && ((modrm >> 3) & 7) == 2;
            };
            if (b[2] == 0xE8) return true;                      // E8 rel32
            if (ff2(b[5], b[6]) && (b[6] >> 6) == 3) return true;  // FF /2 reg
            if (ff2(b[5], b[6]) && (b[6] >> 6) == 0 &&
                (b[6] & 7) != 4 && (b[6] & 7) != 5) return true;   // [reg]
            if (ff2(b[4], b[5]) && (b[5] >> 6) == 1 && (b[5] & 7) != 4)
                return true;                                     // disp8
            if (ff2(b[3], b[4]) && (b[4] >> 6) == 1 && (b[4] & 7) == 4)
                return true;                                     // SIB disp8
            if (ff2(b[1], b[2]) &&
                (((b[2] >> 6) == 2 && (b[2] & 7) != 4) ||
                 ((b[2] >> 6) == 0 && (b[2] & 7) == 5)))
                return true;                                     // disp32 / RIP
            if (ff2(b[0], b[1]) && (b[1] >> 6) == 2 && (b[1] & 7) == 4)
                return true;                                     // SIB disp32
            return false;
        };
        int reported = 0;
        for (vitreo::u64 off = 0; off < 0x8000 && reported < 24; off += 8) {
            const auto word = memory.read_u64(
                static_cast<vitreo::GuestAddr>(cpu.gpr.rsp + off));
            if (word.is_err()) break;
            if (!is_call_return(word.value())) continue;
            std::fprintf(stderr, "[fastfail-ret] [rsp+0x%llx] 0x%llx\n",
                         static_cast<unsigned long long>(off),
                         static_cast<unsigned long long>(word.value()));
            ++reported;
        }
    }
    if (cpu.gpr.rbp >= 0x10000ull &&
        cpu.gpr.rbp < 0x0000800000000000ull) {
        for (const vitreo::i32 disp : {0, 0x10, 0x20, 0x400}) {
            const auto word = memory.read_u64(
                static_cast<vitreo::GuestAddr>(
                    cpu.gpr.rbp + static_cast<vitreo::u64>(disp)));
            if (word.is_ok()) {
                std::fprintf(stderr, "[fastfail-frame] [rbp+0x%x]=0x%llx\n",
                             disp,
                             static_cast<unsigned long long>(word.value()));
            }
        }
    }
}

namespace vitreo::cli {

// Defined below, after the module-manager globals; the fatal dump uses it to
// attribute stack words to loaded images.
static void scan_stack_modules(vitreo::cpu::CpuState& cpu, GuestMemory& memory);


/// VITREO_PC_WATCH_MEM="<reg>[+<off>]:<dwords>|0xaddr:dwords[,...]" — append
/// guest memory a watched frame points at (e.g. "ecx+0x20c:4"), or an absolute
/// guest address ("0x930000:16"). Registers alone rarely say whether an object
/// graph was actually built.
void append_pc_watch_memory(const cpu::CpuState& cpu, GuestMemory& memory) {
    static const char* spec = std::getenv("VITREO_PC_WATCH_MEM");
    if (spec == nullptr) return;
    static const std::array<const char*, 8> kRegNames{
        "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    static const std::array<const char*, 16> kReg64Names{
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    const char* p = spec;
    while (*p != '\0') {
        while (*p == ',' || *p == ' ') ++p;
        if (*p == '\0') break;
        const char* name = p;
        while (*p != '\0' && *p != '+' && *p != '-' && *p != ':') ++p;
        const std::string reg(name, static_cast<usize>(p - name));
        i64 offset = 0;
        if (*p == '+' || *p == '-') {
            char* end = nullptr;
            offset = static_cast<i64>(std::strtoll(p, &end, 0));
            p = (end != nullptr) ? end : p + 1;
        }
        u32 count = 1;
        if (*p == ':') {
            char* end = nullptr;
            count = static_cast<u32>(std::strtoul(p + 1, &end, 0));
            p = (end != nullptr) ? end : p + 1;
        }
        GuestAddr addr = 0;
        bool known = false;
        // Absolute form: 0x... or bare hex/decimal guest address.
        if (!reg.empty() &&
            (reg[0] == '0' || (reg[0] >= '1' && reg[0] <= '9'))) {
            char* end = nullptr;
            const unsigned long long abs =
                std::strtoull(reg.c_str(), &end, 0);
            if (end != reg.c_str() && *end == '\0') {
                addr = static_cast<GuestAddr>(abs);
                known = true;
                std::fprintf(stderr, " [0x%llx]=",
                             static_cast<unsigned long long>(addr));
            }
        }
        if (!known) {
            u64 base = 0;
            for (u8 index = 0; index < kRegNames.size(); ++index) {
                if (reg == kRegNames[index]) {
                    base = cpu.gpr.get32(index);
                    known = true;
                    break;
                }
            }
            for (u8 index = 0; !known && index < kReg64Names.size(); ++index) {
                if (reg == kReg64Names[index]) {
                    base = cpu.gpr.get64(index);
                    known = true;
                }
            }
            if (!known) continue;
            addr = static_cast<GuestAddr>(static_cast<i64>(base) + offset);
            std::fprintf(stderr, " [%s%+lld@0x%llx]=", reg.c_str(),
                         static_cast<long long>(offset),
                         static_cast<unsigned long long>(addr));
        }
        if (count > 16) count = 16;
        for (u32 word = 0; word < count; ++word) {
            const auto value =
                memory.read_u32(addr + static_cast<GuestAddr>(word * 4));
            std::fprintf(stderr, "%s%08x", word == 0 ? "" : ":",
                         value.value_or(0xDEADDEADu));
        }
    }
}

/// Registers plus a raw window of the guest stack at a fatal exception.
///
/// A wild control transfer -- RIP inside the guest's own stack, or near-null --
/// is a SMASHED RETURN ADDRESS, and whatever overwrote it is usually still
/// sitting beside it, so this window is the primary evidence for that class of
/// bug. Both fatal x64 paths must print it: the SEH-walk-unhandled path did
/// not, which is why AASM's intermittent crash produced bare reports with no
/// attribution.
extern "C" void jit_dump_call_alignment_tally();

void dump_fatal_guest_context(vitreo::cpu::CpuState& cpu,
                                     GuestMemory& memory) {
    if (!cpu.is_64bit()) return;
    // Presence test, the same as every other VITREO_ABI_ALIGN_TRAP site.
    if (std::getenv("VITREO_ABI_ALIGN_TRAP") != nullptr) {
        jit_dump_call_alignment_tally();
    }
    std::fprintf(stderr,
        "[exc-ctx] rax=0x%llx rcx=0x%llx rdx=0x%llx rbx=0x%llx rsi=0x%llx "
        "rdi=0x%llx rbp=0x%llx rsp=0x%llx\n",
        (unsigned long long)cpu.gpr.rax, (unsigned long long)cpu.gpr.rcx,
        (unsigned long long)cpu.gpr.rdx, (unsigned long long)cpu.gpr.rbx,
        (unsigned long long)cpu.gpr.rsi, (unsigned long long)cpu.gpr.rdi,
        (unsigned long long)cpu.gpr.rbp, (unsigned long long)cpu.gpr.rsp);
    std::fprintf(stderr,
        "[exc-ctx] r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx r12=0x%llx "
        "r13=0x%llx r14=0x%llx r15=0x%llx\n",
        (unsigned long long)cpu.gpr.r8, (unsigned long long)cpu.gpr.r9,
        (unsigned long long)cpu.gpr.r10, (unsigned long long)cpu.gpr.r11,
        (unsigned long long)cpu.gpr.r12, (unsigned long long)cpu.gpr.r13,
        (unsigned long long)cpu.gpr.r14, (unsigned long long)cpu.gpr.r15);
    // The reported RIP is only actionable next to the bytes actually mapped
    // there: a fetch fault, a self-modified/hooked site, and a genuine data
    // fault are indistinguishable from registers alone.
    {
        u8 code[16];
        usize got = 0;
        for (; got < sizeof(code); ++got) {
            auto v = memory.read_u8(static_cast<GuestAddr>(cpu.rip + got));
            if (!v.is_ok()) break;
            code[got] = v.value();
        }
        if (got == 0) {
            std::fprintf(stderr, "[exc-code] rip=0x%llx UNREADABLE\n",
                         (unsigned long long)cpu.rip);
        } else {
            char hex[3 * sizeof(code) + 1];
            usize n = 0;
            for (usize i = 0; i < got; ++i) {
                n += static_cast<usize>(std::snprintf(
                    hex + n, sizeof(hex) - n, "%02x ", code[i]));
            }
            std::fprintf(stderr, "[exc-code] rip=0x%llx %s(%zu bytes)\n",
                         (unsigned long long)cpu.rip, hex, got);
        }
    }
    // Below RSP as well as above: a return address is smashed by a write that
    // ran BEFORE the RET popped it, so the overwriting data often sits in the
    // frame the callee has already released.
    // Reach past the deepest frame the .pdata walk can reach: runtime-generated
    // callers have no unwind info, so the only trace of them is the return
    // addresses still sitting in the frames above where the walk gave up.
    for (i64 off = -0x80; off < 0x1000; off += 0x20) {
        u64 word[4];
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            auto v = memory.read_u64(static_cast<GuestAddr>(
                static_cast<i64>(cpu.gpr.rsp) + off + 8 * i));
            if (!v.is_ok()) { ok = false; break; }
            word[i] = v.value();
        }
        if (!ok) continue;
        std::fprintf(stderr,
            "[exc-stack] rsp%c0x%03llx %016llx %016llx %016llx %016llx\n",
            off < 0 ? '-' : '+',
            (unsigned long long)(off < 0 ? -off : off),
            (unsigned long long)word[0], (unsigned long long)word[1],
            (unsigned long long)word[2], (unsigned long long)word[3]);
    }
    scan_stack_modules(cpu, memory);
}

// A runaway recursion buries its own origin: the fatal dump covers a few KiB,
// while the frames that started it sit megabytes away near the stack top. Walk
// the whole stack, keep only values inside a loaded PE image, and collapse the
// repeating cycle -- the last distinct entry is the caller that began it.
static void scan_stack_modules(vitreo::cpu::CpuState& cpu, GuestMemory& memory) {
    // Off by default: a runaway recursion prints hundreds of frames, which
    // would bury the ordinary one-frame crash dumps other bring-ups rely on.
    if (std::getenv("VITREO_STACK_SCAN") == nullptr) return;
    auto* mods = module_manager();
    std::fprintf(stderr, "[exc-scan] begin mods=%p rsp=0x%llx\n",
                 (void*)mods, (unsigned long long)cpu.gpr.rsp);
    if (mods == nullptr) return;
    std::string last;
    u64 repeats = 0, printed = 0, misses = 0;
    for (u64 off = 0; off < (8ull << 20) && printed < 400; off += 8) {
        auto v = memory.read_u64(static_cast<GuestAddr>(cpu.gpr.rsp + off));
        if (!v.is_ok()) { if (++misses > 4096) break; continue; }
        const auto at =
            mods->module_for_address(static_cast<GuestAddr>(v.value()));
        if (at.info == nullptr) continue;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s+0x%x", at.info->name.c_str(),
                      at.rva);
        if (last == buf) { ++repeats; continue; }
        if (repeats > 0) {
            std::fprintf(stderr, "[exc-scan]   (x%llu more)\n",
                         (unsigned long long)repeats);
            repeats = 0;
        }
        std::fprintf(stderr, "[exc-scan]   rsp+0x%08llx -> %s\n",
                     (unsigned long long)off, buf);
        last = buf;
        ++printed;
    }
    if (repeats > 0) {
        std::fprintf(stderr, "[exc-scan]   (x%llu more)\n",
                     (unsigned long long)repeats);
    }
    std::fprintf(stderr, "[exc-scan] end printed=%llu\n",
                 (unsigned long long)printed);

    // Print any printable ASCII reachable from the innermost frame's pointer
    // arguments, directly or one field behind them. Runtime and library
    // structures usually reach a name within a hop or two, and when a frame
    // repeats with identical arguments that name is what identifies the work
    // being redone.
    auto try_ascii = [&](const char* label, u64 addr) {
        char text[65];
        usize n = 0;
        for (; n < sizeof(text) - 1; ++n) {
            auto c = memory.read_u8(static_cast<GuestAddr>(addr + n));
            if (!c.is_ok()) break;
            const u8 ch = c.value();
            if (ch == 0) break;
            if (ch < 0x20 || ch > 0x7e) { n = 0; break; }
            text[n] = static_cast<char>(ch);
        }
        if (n >= 4) {
            text[n] = 0;
            std::fprintf(stderr, "[exc-name] %s 0x%llx -> \"%s\"\n", label,
                         (unsigned long long)addr, text);
            return true;
        }
        return false;
    };
    for (u64 off = 0; off < 0x1e0; off += 8) {
        auto v = memory.read_u64(static_cast<GuestAddr>(cpu.gpr.rsp + off));
        if (!v.is_ok()) continue;
        const u64 ptr = v.value();
        if (ptr < 0x10000 || ptr > 0x7fffffffffffull) continue;
        char lbl[48];
        std::snprintf(lbl, sizeof(lbl), "rsp+0x%03llx",
                      (unsigned long long)off);
        if (try_ascii(lbl, ptr)) continue;
        // One hop: a struct's first fields are often name pointers.
        for (u64 f = 0; f <= 0x40; f += 8) {
            auto inner = memory.read_u64(static_cast<GuestAddr>(ptr + f));
            if (!inner.is_ok()) continue;
            const u64 ip = inner.value();
            if (ip < 0x10000 || ip > 0x7fffffffffffull) continue;
            char lbl2[64];
            std::snprintf(lbl2, sizeof(lbl2), "rsp+0x%03llx[+0x%llx]",
                          (unsigned long long)off, (unsigned long long)f);
            if (try_ascii(lbl2, ip)) break;
        }
    }
}

} // namespace vitreo::cli
