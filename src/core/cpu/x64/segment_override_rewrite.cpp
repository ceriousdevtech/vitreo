// SPDX-License-Identifier: LGPL-2.1-only

#include "core/cpu/x64/segment_override_rewrite.hpp"

#include "core/cpu/decode/decoder.hpp"
#include "core/memory/guest_memory.hpp"

#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <cerrno>

#include <algorithm>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace vitreo::cpu::x64 {
namespace {

class SpanReader final : public ByteReader {
public:
    SpanReader(const u8* data, usize size) : m_data(data), m_size(size) {}
    [[nodiscard]] bool read_u8(u8& out) noexcept override {
        if (m_pos >= m_size) return false;
        out = m_data[m_pos++];
        return true;
    }
    [[nodiscard]] usize position() const noexcept override { return m_pos; }

private:
    const u8* m_data;
    usize m_size;
    usize m_pos{0};
};

[[nodiscard]] bool is_legacy_prefix(u8 byte) noexcept {
    switch (byte) {
        case 0xF0: case 0xF2: case 0xF3:
        case 0x2E: case 0x36: case 0x3E: case 0x26: case 0x64: case 0x65:
        case 0x66: case 0x67:
            return true;
        default:
            return false;
    }
}

/// True for the rel8/rel32 conditional jumps, which are contiguous in the
/// opcode table.
[[nodiscard]] bool is_conditional_branch(Opcode opcode) noexcept {
    return (opcode >= Opcode::JO_REL8 && opcode <= Opcode::JNLE_REL8) ||
           (opcode >= Opcode::JO_REL32 && opcode <= Opcode::JNLE_REL32);
}

/// Translate the segment prefix of one decoded instruction, if it has one
/// that actually addresses memory. Returns 1 when a byte was rewritten.
/// @param translated One bit per byte of the span, marking prefix bytes this
///        rewrite has already touched. A misaligned decode -- after a resync,
///        or from a bogus branch target -- can claim the same prefix byte as a
///        differently-aligned instruction, and translating it twice turns
///        guest GS into host FS and then into DS, whose base is zero.
///        A launcher image faulted reading absolute 0x58 exactly that way.
[[nodiscard]] usize rewrite_at(u8* code, usize offset,
                               const DecodedInstruction& instr,
                               std::vector<bool>& translated) noexcept {
    usize active_segment = instr.length;
    for (usize i = 0; i < instr.length; ++i) {
        const u8 byte = code[offset + i];
        if (is_legacy_prefix(byte)) {
            if (byte == 0x64 || byte == 0x65) active_segment = i;
            continue;
        }
        // REX prefixes may follow legacy prefixes in 64-bit mode.
        if (byte >= 0x40 && byte <= 0x4F) continue;
        break;
    }
    if (active_segment >= instr.length) return 0;

    // A segment override only has architectural effect on a memory operand.
    // PE .text sections may contain inline jump tables; treating an RVA byte
    // such as 0x64 followed by bytes that happen to decode as a branch as an
    // FS-prefixed instruction corrupts the table even though the processor
    // ignores that prefix. Do not rewrite an inert prefix.
    bool has_memory_operand = false;
    for (usize i = 0; i < instr.operand_count; ++i) {
        if (is_memory_operand(instr.operands[i].type)) {
            has_memory_operand = true;
            break;
        }
    }
    if (!has_memory_operand) return 0;

    const usize prefix_offset = offset + active_segment;
    if (prefix_offset < translated.size() && translated[prefix_offset]) {
        return 0;
    }
    u8& prefix = code[prefix_offset];
    if (instr.prefixes.segment == SegReg::GS && prefix == 0x65) {
        prefix = 0x64; // guest GS -> host FS (TEB carrier)
        if (prefix_offset < translated.size()) translated[prefix_offset] = true;
        return 1;
    }
    // Win64 guest code does not use FS -- the TEB is GS, and a Win64 FS
    // access is a decode artefact far more often than a real one. Translating
    // it is therefore mostly a way to write 0x3E over a byte that is not an
    // instruction. Opt in with VITREO_NATIVE64_TRANSLATE_FS=1.
    static const bool translate_fs =
        std::getenv("VITREO_NATIVE64_TRANSLATE_FS") != nullptr;
    if (translate_fs && instr.prefixes.segment == SegReg::FS &&
        prefix == 0x64) {
        // Win64 guest code effectively never uses FS, so this branch is
        // mostly a hazard: it also fires on a byte an earlier pass already
        // translated from GS, and DS has a zero base, which faults at the
        // raw displacement. Say where it happened.
        if (std::getenv("VITREO_NATIVE64_TRACE_SEGMENTS") != nullptr) {
            std::fprintf(stderr,
                         "[native64-seg] FS->DS at span offset 0x%zx\n",
                         prefix_offset);
        }
        prefix = 0x3E; // guest FS -> host DS (zero base)
        if (prefix_offset < translated.size()) translated[prefix_offset] = true;
        return 1;
    }
    return 0;
}

/// Rewrite one instruction stream in [begin, end).
///
/// @param resync_on_error true keeps the legacy linear behaviour: a byte that
///        does not decode is skipped and decoding resumes one byte later.
///        false abandons the stream, which is what a caller with real
///        instruction boundaries wants -- past an undecodable byte the stream
///        is no longer known to be code.
[[nodiscard]] bool looks_like_teb_access(
    const DecodedInstruction& instr) noexcept;

/// @param teb_shape_only accept only instructions carrying a real TEB-access
///        shape. A linear decode of a real module desynchronises on the data
///        every .text carries, so without this gate it writes 0x64/0x3E over
///        bytes that are not instructions.
[[nodiscard]] usize rewrite_span(u8* code, usize begin, usize end,
                                 bool resync_on_error,
                                 bool teb_shape_only = false) noexcept {
    std::vector<bool> translated(end, false);
    usize rewritten = 0;
    usize offset = begin;
    while (offset < end) {
        SpanReader reader(code + offset, end - offset);
        DecodedInstruction instr{};
        const DecodeError error =
            decode_instruction(reader, DecodeMode::Mode64, instr);
        if (error != DecodeError::Success || instr.length == 0) {
            if (!resync_on_error) break;
            ++offset;
            continue;
        }
        if (!teb_shape_only || looks_like_teb_access(instr)) {
            rewritten += rewrite_at(code, offset, instr, translated);
        }
        offset += instr.length;
    }
    return rewritten;
}

/// The shape a Win64 TEB access always has: a segment-prefixed memory operand
/// with no base, no index and a small displacement -- `mov rax, gs:[0x30]`.
///
/// This is the acceptance test for the bytes BETWEEN the functions .pdata
/// names, which hold both the leaf functions it omits and literal data.
/// Measured over a launcher's modules: in those gaps this shape matches
/// exactly the real TEB reads (three `mov rax, gs:[0x30]` in each of three
/// modules) and none of the 150-170 junk decodes per module
/// that a plain gap sweep produces -- those all carry a base or index register,
/// or are `movabs` forms with nonsense 64-bit displacements.
[[nodiscard]] bool looks_like_teb_access(const DecodedInstruction& instr) noexcept {
    for (usize i = 0; i < instr.operand_count; ++i) {
        if (!is_memory_operand(instr.operands[i].type)) continue;
        const MemoryOperand& mem = instr.operands[i].mem;
        if (mem.has_absolute) return false;      // A0-A3 moffs: never a TEB read
        if (mem.has_base() || mem.is_rip_relative()) return false;
        if (mem.index != 255) return false;
        return mem.disp >= 0 && mem.disp < 0x10000;
    }
    return false;
}

/// True when [begin, end) contains a byte that could be a segment-override
/// prefix at all.
///
/// A rewrite only ever changes a 0x65 (GS) or 0x64 (FS) prefix byte, and that
/// byte must lie inside the instruction being decoded. A stretch of code with
/// neither byte therefore cannot produce a single rewrite, no matter how it
/// decodes -- so decoding it is pure cost. `memchr` scans it at memory speed
/// instead, and startup on the Intel backend is dominated by exactly this
/// decode: `SpanReader::read_u8` plus `decode_modrm` were ~85% of Vitreo's
/// samples while a launcher loaded, because every browser process decodes the
/// whole of the embedded-browser library's .text.
[[nodiscard]] bool span_has_segment_prefix_byte(const u8* code, usize begin,
                                                usize end) noexcept {
    if (begin >= end) return false;
    const usize len = end - begin;
    const void* gs = std::memchr(code + begin, 0x65, len);
    if (gs != nullptr) return true;
    return std::memchr(code + begin, 0x64, len) != nullptr;
}

/// Recursive descent over one executable span.
///
/// .pdata names most functions but not all: the x64 ABI lets a leaf function
/// that touches neither the stack nor a non-volatile register be omitted, and
/// those functions do contain TEB accesses -- a launcher helper reads
/// `gs:[0x30]` from two of them and spun on a fault storm when they were left
/// untranslated. Every such function still has to be *called*, so following
/// the direct rel8/rel32 branch targets out of the functions .pdata does name
/// finds them, without ever decoding a byte that is not reached as code.
class Walker {
public:
    /// Longest x86-64 instruction encoding.
    static constexpr usize kMaxInstructionLength = 15;

    Walker(u8* code, usize size)
        : m_code(code), m_size(size), m_start(size, false),
          m_translated(size, false) {}

    /// A function named by .pdata: its extent is known, so decode all of it.
    void seed(usize offset, usize length) noexcept {
        if (offset >= m_size) return;
        walk(offset, std::min(m_size, offset + length), /*bounded=*/true);
    }

    /// Translate the TEB accesses in the bytes between the seeded functions.
    ///
    /// Call this after drain(): anything the descent already reached is
    /// marked, and what is left is padding, literals, and the leaf functions
    /// .pdata omits and no direct call reaches (one module has three).
    void sweep_gaps() noexcept {
        usize offset = 0;
        // Position of the next byte that could be a segment prefix. Only an
        // offset within one instruction length (15 bytes) of such a byte can
        // produce a rewrite, so everything before that is skipped without
        // decoding it.
        usize next_candidate = 0;
        while (offset < m_size) {
            if (m_start[offset]) { ++offset; continue; }
            if (next_candidate < offset) {
                const usize len = m_size - offset;
                const void* gs = std::memchr(m_code + offset, 0x65, len);
                const void* fs = std::memchr(m_code + offset, 0x64, len);
                const auto pos = [&](const void* p) {
                    return p == nullptr
                        ? m_size
                        : static_cast<usize>(static_cast<const u8*>(p) - m_code);
                };
                next_candidate = std::min(pos(gs), pos(fs));
            }
            if (next_candidate >= m_size) return;
            if (offset + kMaxInstructionLength <= next_candidate) {
                offset = next_candidate - (kMaxInstructionLength - 1);
                continue;
            }
            const u8 byte = m_code[offset];
            if (byte == 0xCC || byte == 0x90) { ++offset; continue; }
            SpanReader reader(m_code + offset, m_size - offset);
            DecodedInstruction instr{};
            const DecodeError error =
                decode_instruction(reader, DecodeMode::Mode64, instr);
            if (error != DecodeError::Success || instr.length == 0) {
                ++offset;
                continue;
            }
            if (looks_like_teb_access(instr)) {
                const usize done = rewrite_at(m_code, offset, instr,
                                              m_translated);
                m_rewritten += done;
                if (done != 0 &&
                    std::getenv("VITREO_NATIVE64_TRACE_SEGMENTS") != nullptr) {
                    std::fprintf(stderr,
                                 "[native64-seg] gap site at 0x%zx len=%u\n",
                                 offset, static_cast<unsigned>(instr.length));
                }
                offset += instr.length;
                continue;
            }
            ++offset;
        }
    }

    /// Follow every branch target discovered so far to a fixed point.
    void drain() noexcept {
        while (!m_pending.empty()) {
            const usize target = m_pending.back();
            m_pending.pop_back();
            if (m_start[target]) continue;
            walk(target, m_size, /*bounded=*/false);
        }
    }

    [[nodiscard]] usize rewritten() const noexcept { return m_rewritten; }

private:
    /// True when control cannot fall through to the next byte, so the walk
    /// must stop rather than decode whatever follows -- which is where the
    /// padding, jump tables and literals live. This holds inside a .pdata
    /// function range as much as outside one.
    [[nodiscard]] static bool is_terminator(Opcode opcode) noexcept {
        switch (opcode) {
            case Opcode::RET_NEAR:
            case Opcode::RET_NEAR_IMM16:
            case Opcode::RET_FAR:
            case Opcode::IRET:
            case Opcode::JMP_REL8:
            case Opcode::JMP_REL16:
            case Opcode::JMP_REL32:
            case Opcode::JMP_RM64:
            case Opcode::JMP_RM32:
            case Opcode::INT_3:
            case Opcode::UD2:
                return true;
            default:
                return false;
        }
    }

    /// The rel8/rel32 branch target of @p instr as a span offset, or m_size
    /// when this instruction has no direct target inside the span.
    [[nodiscard]] usize branch_target(const DecodedInstruction& instr,
                                      usize offset) const noexcept {
        const bool direct =
            instr.opcode == Opcode::CALL_REL32 ||
            instr.opcode == Opcode::JMP_REL8 ||
            instr.opcode == Opcode::JMP_REL16 ||
            instr.opcode == Opcode::JMP_REL32 ||
            is_conditional_branch(instr.opcode);
        if (!direct || instr.operand_count == 0) return m_size;
        if (!is_immediate_operand(instr.operands[0].type)) return m_size;
        const i64 next = static_cast<i64>(offset) + instr.length;
        const i64 target = next + instr.operands[0].imm;
        if (target < 0 || static_cast<usize>(target) >= m_size) return m_size;
        return static_cast<usize>(target);
    }

    void walk(usize begin, usize end, bool bounded) noexcept {
        usize offset = begin;
        while (offset < end) {
            // Already decoded from here. An unbounded walk can stop: what
            // follows is known. A bounded one keeps going -- .pdata carries
            // overlapping fragments for one function and the walk must reach
            // the rest of the range -- but it must NOT translate the
            // instruction a second time: the first pass already turned guest
            // GS (0x65) into host FS (0x64), and translating that again reads
            // it as guest FS and produces DS, whose base is zero. That is how
            // a launcher image came to fault reading absolute 0x58.
            const bool already_seen = m_start[offset];
            if (already_seen && !bounded) return;
            SpanReader reader(m_code + offset, end - offset);
            DecodedInstruction instr{};
            const DecodeError error =
                decode_instruction(reader, DecodeMode::Mode64, instr);
            if (error != DecodeError::Success || instr.length == 0) {
                // Inside a function .pdata delimits, every byte is code, so a
                // byte this decoder does not know (it is an emulator's
                // decoder, not a disassembler's) must not cost us the rest of
                // the function -- and with it the calls that reach the leaf
                // functions .pdata omits. Resync a byte at a time. Outside a
                // known extent there is no such guarantee: stop.
                if (!bounded) return;
                ++offset;
                continue;
            }

            if (!already_seen) {
                m_start[offset] = true;
                m_rewritten += rewrite_at(m_code, offset, instr,
                                          m_translated);
            }

            const usize target = branch_target(instr, offset);
            if (target < m_size && !m_start[target]) {
                m_pending.push_back(target);
            }
            // Stop at a terminator even inside a .pdata range. "Every byte in
            // a named function is code" is false for MSVC x64: a switch puts
            // its jump table in .text immediately after the
            // `jmp qword ptr [table+rax*8]` that reads it, inside the
            // function's own .pdata extent. Decoding straight through it
            // desynchronises and rewrites table bytes -- an entry whose RVA
            // contains 0x65 becomes a different address, and the switch jumps
            // into whatever that names. A launcher died exactly there: an indirect
            // jmp into a read-write data buffer
            // (`10 00 18 00 20 01 28 01 ...`, prot=RW, exec=0).
            //
            // The rest of the function is not lost: conditional and direct
            // branch targets are already queued for the descent, and whatever
            // stays unreached is still covered by sweep_gaps(), which
            // translates only instructions carrying a real TEB access shape.
            if (is_terminator(instr.opcode)) return;
            offset += instr.length;
        }
    }

    u8* m_code;
    usize m_size;
    std::vector<bool> m_start;
    std::vector<bool> m_translated;
    std::vector<usize> m_pending;
    usize m_rewritten{0};
};

} // namespace

usize rewrite_segment_overrides(u8* code, usize size,
                                bool teb_shape_only) noexcept {
    if (size == 0) return 0;
    // Nothing to rewrite without a prefix byte, and this is the common case
    // for whole extents.
    if (!span_has_segment_prefix_byte(code, 0, size)) return 0;
    return rewrite_span(code, 0, size, /*resync_on_error=*/true,
                        teb_shape_only);
}
usize rewrite_segment_overrides_ranges(u8* code, usize size,
                                       const CodeRange* ranges,
                                       usize count) noexcept {
    if (code == nullptr || ranges == nullptr) return 0;
    Walker walker(code, size);
    for (usize i = 0; i < count; ++i) {
        // A function with no 0x64/0x65 byte anywhere in it cannot yield a
        // rewrite, so it never needs decoding.
        const usize begin = ranges[i].offset;
        const usize end = begin > size || ranges[i].length > size - begin
            ? size
            : begin + ranges[i].length;
        if (!span_has_segment_prefix_byte(code, begin, end)) continue;
        walker.seed(ranges[i].offset, ranges[i].length);
    }
    walker.drain();
    walker.sweep_gaps();
    return walker.rewritten();
}

usize rewrite_segment_overrides_in_range(GuestMemory& memory,
                                         GuestAddr base,
                                         usize size,
                                         const CodeRange* ranges,
                                         usize count,
                                         bool teb_shape_only) noexcept {
    if (size == 0) return 0;
    u8* const host = memory.host_ptr_mut(base, size);
    if (host == nullptr) return 0;

    constexpr usize kPage = 0x1000;
    auto* const lo = reinterpret_cast<u8*>(
        reinterpret_cast<uintptr_t>(host) & ~(kPage - 1));
    auto* const hi = reinterpret_cast<u8*>(
        (reinterpret_cast<uintptr_t>(host) + size + kPage - 1) &
        ~(kPage - 1));
    const usize span = static_cast<usize>(hi - lo);
    const bool was_writable =
        memory.has_protection(base, size, MemoryProtection::Write);

    // Everything from here to the restoring mprotect writes into guest code
    // pages that this thread has opened for writing. A concurrent guest
    // VirtualProtect on the same range -- and this sweep is *triggered* by
    // protection activity, so one is often in flight -- re-protects the host
    // pages read-only under us, and the next store dies as a host SIGBUS
    // (macOS reports a protection failure as SIGBUS, not SIGSEGV). Pinning
    // makes that window closed rather than merely narrow: protect_region
    // takes the same lock exclusively across its host mprotect.
    //
    // Nothing below may call back into GuestMemory: the pin is a
    // shared_mutex, which is not recursive.
    const auto protection_pin = memory.pin_protection();

    if (mprotect(lo, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        std::fprintf(stderr,
                     "[native64] could not open 0x%llx+0x%zx for segment "
                     "translation (errno=%d)\n",
                     static_cast<unsigned long long>(base), size, errno);
        return 0;
    }
    if (std::getenv("VITREO_NATIVE64_TRACE_SEGMENTS") != nullptr) {
        // The host regions the sweep is about to write through.
        //
        // Read this as a snapshot and nothing more: it describes the instant
        // it ran, not the sweep. It once showed a whole extent prot=7/max=7
        // immediately before that same extent faulted read-only mid-sweep,
        // which read as "protection is fine" and cost a long detour. The
        // protection pin below is what actually makes the span stable; when a
        // store still faults, the crash handler's own region query is the
        // authority, not this line.
        mach_vm_address_t addr = reinterpret_cast<mach_vm_address_t>(lo);
        const mach_vm_address_t end = reinterpret_cast<mach_vm_address_t>(hi);
        while (addr < end) {
            mach_vm_size_t rsize = 0;
            vm_region_basic_info_data_64_t info{};
            mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t obj = MACH_PORT_NULL;
            if (mach_vm_region(mach_task_self(), &addr, &rsize,
                               VM_REGION_BASIC_INFO_64,
                               reinterpret_cast<vm_region_info_t>(&info),
                               &cnt, &obj) != KERN_SUCCESS) {
                std::fprintf(stderr,
                             "[native64-seg] region 0x%llx: UNMAPPED\n",
                             static_cast<unsigned long long>(addr));
                break;
            }
            if (addr >= end) break;
            std::fprintf(stderr,
                         "[native64-seg] region 0x%llx+0x%llx prot=%d max=%d "
                         "shared=%d reserved=%d off=0x%llx (extent 0x%llx+0x%zx)\n",
                         static_cast<unsigned long long>(addr),
                         static_cast<unsigned long long>(rsize),
                         info.protection, info.max_protection,
                         info.shared ? 1 : 0, info.reserved ? 1 : 0,
                         static_cast<unsigned long long>(info.offset),
                         static_cast<unsigned long long>(base), size);
            addr += rsize;
        }
    }

    const usize rewritten =
        ranges != nullptr
            ? rewrite_segment_overrides_ranges(host, size, ranges, count)
            : rewrite_segment_overrides(host, size, teb_shape_only);
    int restore = PROT_READ | PROT_EXEC;
    if (was_writable) restore |= PROT_WRITE;
    (void)mprotect(lo, span, restore);
    return rewritten;
}

} // namespace vitreo::cpu::x64
