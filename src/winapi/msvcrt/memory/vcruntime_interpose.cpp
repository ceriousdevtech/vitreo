// SPDX-License-Identifier: LGPL-2.1-only

/// @file vcruntime_interpose.cpp
/// @brief Checked host bodies for the CRT memory/string primitives and the
///        vcruntime140.dll lookup interposer. See vcruntime_interpose.hpp.
///
/// Contract sources: Microsoft Learn C runtime reference (memcpy, memmove,
/// memset, memcmp, memchr, strchr/wcschr, strrchr/wcsrchr, strstr/wcsstr),
/// and the winoracle behaviour records in tests/oracle_data/
/// (vcruntime_memstr_probe{32,64}, crt_memcmp_sign_probe{32,64},
/// crt_memcpy_overlap_probe{32,64}).

#include "winapi/msvcrt/memory/vcruntime_interpose.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guard_page_traps.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/msvcrt/guest_accel.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <cstdint>
#include <cstring>

namespace vitreo::winapi::vcrt {

namespace {

constexpr u64 kPage = guard_page::kGuestPage;

struct Names {
    const char* pub;
    const char* internal;
};

constexpr std::array<Names, kFnCount> kNames{{
    {"memcpy", "__vitreo_vcrt_memcpy"},
    {"memmove", "__vitreo_vcrt_memmove"},
    {"memset", "__vitreo_vcrt_memset"},
    {"memcmp", "__vitreo_vcrt_memcmp"},
    {"memchr", "__vitreo_vcrt_memchr"},
    {"strchr", "__vitreo_vcrt_strchr"},
    {"strrchr", "__vitreo_vcrt_strrchr"},
    {"strstr", "__vitreo_vcrt_strstr"},
    {"wcschr", "__vitreo_vcrt_wcschr"},
    {"wcsrchr", "__vitreo_vcrt_wcsrchr"},
    {"wcsstr", "__vitreo_vcrt_wcsstr"},
}};

/// Bytes from @p a to the end of its guest page.
[[nodiscard]] inline u64 room_up(GuestAddr a) noexcept {
    return kPage - (a & (kPage - 1));
}
/// Bytes from the start of the page holding byte @p end - 1 up to @p end.
[[nodiscard]] inline u64 room_down(GuestAddr end) noexcept {
    return ((end - 1) & (kPage - 1)) + 1;
}

[[nodiscard]] inline bool guard_armed(GuestAddr a) noexcept {
    return guard_page::any_armed() && guard_page::is_armed(a);
}

/// Host pointer for a guest READ of [a, a+len), len within one guest page.
/// PAGE_GUARD is tested first: an armed guard page's host mapping is
/// PROT_NONE whatever its guest protection says.
[[nodiscard]] const Byte* read_piece(const GuestMemory& mem, GuestAddr a, u64 len,
                                     std::optional<Fault>& fault) noexcept {
    if (guard_armed(a)) {
        fault = Fault{false, a, kStatusGuardPageViolation};
        return nullptr;
    }
    const Byte* p = mem.host_ptr(a, static_cast<usize>(len));
    if (p == nullptr) fault = Fault{false, a, kStatusAccessViolation};
    return p;
}

/// Host pointer for a guest STORE (protection-checked, write-watch + SMC
/// notified through host_ptr_store), len within one guest page.
[[nodiscard]] Byte* write_piece(GuestMemory& mem, GuestAddr a, u64 len,
                                std::optional<Fault>& fault) noexcept {
    if (guard_armed(a)) {
        fault = Fault{true, a, kStatusGuardPageViolation};
        return nullptr;
    }
    Byte* p = mem.host_ptr_store(a, static_cast<usize>(len));
    if (p == nullptr) fault = Fault{true, a, kStatusAccessViolation};
    return p;
}

[[nodiscard]] inline int sign_of_first_difference(const Byte* a, const Byte* b,
                                                  usize n) noexcept {
    for (usize i = 0; i < n; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

/// Byte-granular reader that keeps the current page's host pointer; used by
/// the scans whose stopping point is only known while reading.
class Reader {
public:
    explicit Reader(const GuestMemory& mem) noexcept : mem_(mem) {}

    [[nodiscard]] bool byte(GuestAddr a, u8& out) noexcept {
        const GuestAddr page = a & ~(kPage - 1);
        if (page != page_ || base_ == nullptr) {
            base_ = read_piece(mem_, page, kPage, fault);
            if (base_ == nullptr) {
                // Report the first byte actually touched (inside the page).
                fault->address = a;
                return false;
            }
            page_ = page;
        }
        out = base_[a - page];
        return true;
    }
    [[nodiscard]] bool unit(GuestAddr a, u8& out) noexcept { return byte(a, out); }
    [[nodiscard]] bool unit(GuestAddr a, u16& out) noexcept {
        u8 lo = 0, hi = 0;
        if (!byte(a, lo) || !byte(a + 1, hi)) return false;
        out = static_cast<u16>(lo | (static_cast<u16>(hi) << 8));
        return true;
    }

    std::optional<Fault> fault;

private:
    const GuestMemory& mem_;
    GuestAddr page_{~GuestAddr{0}};
    const Byte* base_{nullptr};
};

/// strstr/wcsstr: empty needle -> haystack; otherwise the first position
/// where the whole needle matches. A haystack unit is read only once every
/// earlier unit of the candidate matched a non-terminator needle unit, so
/// nothing past the haystack's terminator is ever read.
template <typename Unit>
FindResult find_substring(const GuestMemory& mem, GuestAddr h, GuestAddr needle) noexcept {
    Reader rn(mem);
    std::vector<Unit> nd;
    for (GuestAddr a = needle;; a += sizeof(Unit)) {
        Unit u{};
        if (!rn.unit(a, u)) return {0, rn.fault};
        if (u == 0) break;
        nd.push_back(u);
    }
    if (nd.empty()) return {h, std::nullopt};
    Reader rh(mem);
    for (GuestAddr i = h;; i += sizeof(Unit)) {
        Unit first{};
        if (!rh.unit(i, first)) return {0, rh.fault};
        if (first == 0) return {0, std::nullopt};
        if (first != nd[0]) continue;
        usize j = 1;
        for (; j < nd.size(); ++j) {
            Unit u{};
            if (!rh.unit(i + j * sizeof(Unit), u)) return {0, rh.fault};
            if (u != nd[j]) break;
        }
        if (j == nd.size()) return {i, std::nullopt};
    }
}

}  // namespace

std::optional<Fn> fn_for_export(std::string_view name) noexcept {
    for (usize i = 0; i < kFnCount; ++i) {
        if (name == kNames[i].pub) return static_cast<Fn>(i);
    }
    return std::nullopt;
}

const char* export_name(Fn fn) noexcept {
    return kNames[static_cast<usize>(fn)].pub;
}

const char* internal_export_name(Fn fn) noexcept {
    return kNames[static_cast<usize>(fn)].internal;
}

std::optional<Fault> move_bytes(GuestMemory& mem, GuestAddr dst, GuestAddr src,
                                u64 n, u64 resume_done, u64* done_out) noexcept {
    if (resume_done > n) resume_done = 0;
    if (done_out != nullptr) *done_out = resume_done;
    if (n == resume_done) return std::nullopt;
    // The direction is a function of the call's arguments alone, so a
    // resumed call takes the same one as the call that faulted.
    const bool backward = dst > src && dst - src < n;
    // What is left: forward, [resume_done, n); backward, [0, n - resume_done).
    const u64 rest = n - resume_done;
    const GuestAddr rs = backward ? src : src + resume_done;
    const GuestAddr rd = backward ? dst : dst + resume_done;
    // Fast path: the rest is one accessible host span on both sides.
    if (!guard_page::any_armed()) {
        if (const Byte* sp = mem.host_ptr(rs, static_cast<usize>(rest))) {
            if (Byte* dp = mem.host_ptr_store(rd, static_cast<usize>(rest))) {
                std::memmove(dp, sp, static_cast<usize>(rest));
                if (done_out != nullptr) *done_out = n;
                return std::nullopt;
            }
        }
    }
    std::optional<Fault> fault;
    if (!backward) {
        u64 done = resume_done;
        while (done < n) {
            const GuestAddr s = src + done, d = dst + done;
            const u64 len = std::min({n - done, room_up(s), room_up(d)});
            const Byte* sp = read_piece(mem, s, len, fault);
            if (sp == nullptr) return fault;
            Byte* dp = write_piece(mem, d, len, fault);
            if (dp == nullptr) return fault;
            std::memmove(dp, sp, static_cast<usize>(len));
            done += len;
            if (done_out != nullptr) *done_out = done;
        }
        return std::nullopt;
    }
    // Overlapping with dst above src: copy from the top down, so the source
    // tail is read (and faults) first, as Windows' memmove does (C16).
    u64 left = rest;
    while (left > 0) {
        const GuestAddr se = src + left, de = dst + left;
        const u64 len = std::min({left, room_down(se), room_down(de)});
        const Byte* sp = read_piece(mem, se - len, len, fault);
        if (sp == nullptr) return fault;
        Byte* dp = write_piece(mem, de - len, len, fault);
        if (dp == nullptr) return fault;
        std::memmove(dp, sp, static_cast<usize>(len));
        left -= len;
        if (done_out != nullptr) *done_out = n - left;
    }
    return std::nullopt;
}

std::optional<Fault> fill_bytes(GuestMemory& mem, GuestAddr dst, u8 c, u64 n,
                                u64 resume_done, u64* done_out) noexcept {
    if (resume_done > n) resume_done = 0;
    if (done_out != nullptr) *done_out = resume_done;
    if (n == resume_done) return std::nullopt;
    if (!guard_page::any_armed()) {
        if (Byte* dp = mem.host_ptr_store(dst + resume_done,
                                          static_cast<usize>(n - resume_done))) {
            std::memset(dp, c, static_cast<usize>(n - resume_done));
            if (done_out != nullptr) *done_out = n;
            return std::nullopt;
        }
    }
    std::optional<Fault> fault;
    u64 done = resume_done;
    while (done < n) {
        const GuestAddr d = dst + done;
        const u64 len = std::min(n - done, room_up(d));
        Byte* dp = write_piece(mem, d, len, fault);
        if (dp == nullptr) return fault;
        std::memset(dp, c, static_cast<usize>(len));
        done += len;
        if (done_out != nullptr) *done_out = done;
    }
    return std::nullopt;
}

CmpResult compare_bytes(const GuestMemory& mem, GuestAddr a, GuestAddr b,
                        u64 n) noexcept {
    if (n == 0) return {};
    if (!guard_page::any_armed()) {
        const Byte* pa = mem.host_ptr(a, static_cast<usize>(n));
        const Byte* pb = pa ? mem.host_ptr(b, static_cast<usize>(n)) : nullptr;
        if (pa && pb) {
            const int r = std::memcmp(pa, pb, static_cast<usize>(n));
            return {r < 0 ? -1 : (r > 0 ? 1 : 0), std::nullopt};
        }
    }
    CmpResult out;
    u64 done = 0;
    while (done < n) {
        const GuestAddr x = a + done, y = b + done;
        const u64 len = std::min({n - done, room_up(x), room_up(y)});
        const Byte* pa = read_piece(mem, x, len, out.fault);
        if (pa == nullptr) return out;
        const Byte* pb = read_piece(mem, y, len, out.fault);
        if (pb == nullptr) return out;
        if (std::memcmp(pa, pb, static_cast<usize>(len)) != 0) {
            out.value = sign_of_first_difference(pa, pb, static_cast<usize>(len));
            return out;
        }
        done += len;
    }
    return out;
}

FindResult find_byte(const GuestMemory& mem, GuestAddr p, u8 c, u64 n) noexcept {
    FindResult out;
    u64 done = 0;
    while (done < n) {
        const GuestAddr x = p + done;
        const u64 len = std::min(n - done, room_up(x));
        const Byte* hp = read_piece(mem, x, len, out.fault);
        if (hp == nullptr) return out;
        if (const void* hit = std::memchr(hp, c, static_cast<usize>(len))) {
            out.at = x + static_cast<GuestAddr>(static_cast<const Byte*>(hit) - hp);
            return out;
        }
        done += len;
    }
    return out;
}

FindResult find_char(const GuestMemory& mem, GuestAddr s, u8 c, bool last) noexcept {
    FindResult out;
    GuestAddr cur = s;
    for (;;) {
        const u64 room = room_up(cur);
        const Byte* hp = read_piece(mem, cur, room, out.fault);
        if (hp == nullptr) {
            out.at = 0;
            return out;
        }
        const void* nul = std::memchr(hp, 0, static_cast<usize>(room));
        const usize span = nul ? static_cast<usize>(static_cast<const Byte*>(nul) - hp) + 1
                               : static_cast<usize>(room);
        if (!last) {
            if (const void* hit = std::memchr(hp, c, span)) {
                out.at = cur + static_cast<GuestAddr>(static_cast<const Byte*>(hit) - hp);
                return out;
            }
        } else {
            for (usize i = span; i-- > 0;) {
                if (hp[i] == c) {
                    out.at = cur + i;
                    break;
                }
            }
        }
        if (nul) return out;
        cur += room;
    }
}

FindResult find_wchar(const GuestMemory& mem, GuestAddr s, u16 c, bool last) noexcept {
    FindResult out;
    GuestAddr cur = s;
    auto take = [&](u16 v, GuestAddr at) -> bool {  // true: scan is finished
        if (v == c) {
            out.at = at;
            if (!last) return true;
        }
        return v == 0;
    };
    for (;;) {
        const u64 room = room_up(cur);
        const Byte* hp = read_piece(mem, cur, room, out.fault);
        if (hp == nullptr) {
            out.at = 0;
            return out;
        }
        const u64 units = room / 2;
        for (u64 i = 0; i < units; ++i) {
            u16 v = 0;
            std::memcpy(&v, hp + i * 2, sizeof(v));
            if (take(v, cur + i * 2)) return out;
        }
        cur += units * 2;
        if (room & 1) {
            // A unit straddling two pages: its high byte is the next page's
            // first byte, and reading it is what faults.
            const Byte* next = read_piece(mem, cur + 1, 1, out.fault);
            if (next == nullptr) {
                out.at = 0;
                return out;
            }
            const u16 v = static_cast<u16>(hp[room - 1] | (static_cast<u16>(next[0]) << 8));
            if (take(v, cur)) return out;
            cur += 2;
        }
    }
}

FindResult find_str(const GuestMemory& mem, GuestAddr h, GuestAddr needle) noexcept {
    return find_substring<u8>(mem, h, needle);
}

FindResult find_wstr(const GuestMemory& mem, GuestAddr h, GuestAddr needle) noexcept {
    return find_substring<u16>(mem, h, needle);
}

namespace {

constexpr u64 kResumeMagic = 0x314D5352'4F525456ull;  // "VTRORSM1"

/// The resume record (vcruntime_interpose.hpp, publish_resume_record).
struct ResumeRecord {
    u64 magic;
    u64 thunk_base;
    u64 slot[2];  ///< indexed by ResumeKind
};
static_assert(sizeof(ResumeRecord) <= CRT_ACCEL_STUB_SLOT_X86);

/// Where the record of the process whose thunk is running at @p rip lives.
/// x86: the fixed arena. x64: the arena is thunk_base + X64_ACCEL_OFFSET and
/// the thunk region lies below it within one X64_ACCEL_OFFSET-aligned block
/// (thunk bases are 4 GiB-aligned strides, x64_high_arena.hpp); the record
/// re-checks that rip is inside [thunk_base, thunk_base + X64_ACCEL_OFFSET).
[[nodiscard]] GuestAddr record_address(bool x64, GuestAddr rip) noexcept {
    if (!x64) return CRT_ACCEL_BASE_X86 + CRT_ACCEL_RESUME_OFFSET_X86;
    const GuestAddr block = rip & ~(static_cast<GuestAddr>(X64_ACCEL_OFFSET) - 1);
    return block + X64_ACCEL_OFFSET + X64_ACCEL_RESUME_OFFSET;
}

/// The resume entry for @p kind of the running process, or 0.
[[nodiscard]] GuestAddr resume_slot(Marshaler& m, ResumeKind kind) noexcept {
    const cpu::CpuState* cpu = m.cpu();
    const bool x64 = cpu->is_64bit();
    const GuestAddr rip = static_cast<GuestAddr>(cpu->rip);
    const Byte* p = m.memory().host_ptr(record_address(x64, rip), sizeof(ResumeRecord));
    if (p == nullptr) return 0;
    ResumeRecord r{};
    std::memcpy(&r, p, sizeof(r));
    if (r.magic != kResumeMagic) return 0;
    if (rip < r.thunk_base || rip - r.thunk_base >= X64_ACCEL_OFFSET) return 0;
    return static_cast<GuestAddr>(r.slot[static_cast<usize>(kind)]);
}

constexpr std::array<const char*, 2> kResumeNames{
    "__vitreo_vcrt_memmove_resume", "__vitreo_vcrt_memset_resume"};

}  // namespace

void deliver(Marshaler& m, const Fault& f, std::optional<Resume> resume) noexcept {
    if (!m.raise_access_violation(f.write, f.address)) return;
    // A memory fault, not a RaiseException: the context is the faulting
    // "instruction", with the caller's return address and arguments still
    // in place, so the dispatcher must not simulate the RET.
    cpu::CpuState* cpu = m.cpu();
    cpu->pending_raise.from_helper = true;
    if (resume) {
        // A store-side body continues where it stopped (Windows resumes a
        // faulting memcpy at the faulting access): RIP = its resume entry,
        // EAX/RAX = the progress. Re-running a partly done overlapping move
        // from the start would copy from a source it already overwrote.
        if (const GuestAddr at = resume_slot(m, resume->kind); at != 0) {
            cpu->rip = at;
            if (cpu->is_64bit()) {
                cpu->gpr.rax = resume->done;
            } else {
                cpu->gpr.set32(0, static_cast<u32>(resume->done));
            }
        }
        // No record (a registry without the resume entries): RIP stays at
        // this body's entry and continuing re-runs the call.
    }
    if (f.code == kStatusGuardPageViolation) {
        cpu->pending_raise.code = kStatusGuardPageViolation;
        // PAGE_GUARD is one-shot: the system clears it as it raises.
        (void)guard_page::handle_fault(f.address);
    }
}

const char* resume_export_name(ResumeKind kind) noexcept {
    return kResumeNames[static_cast<usize>(kind)];
}

void run_resume(ResumeKind kind, Marshaler& m) noexcept {
    if (!m.has_cpu()) return;
    GuestMemory& mem = m.memory();
    const u64 n = m.arg(2);
    u64 from = m.cpu()->is_64bit() ? m.cpu()->gpr.rax : m.cpu()->gpr.get32(0);
    if (from > n) from = 0;  // a handler rewrote the progress: start over
    u64 done = from;
    std::optional<Fault> fault;
    if (kind == ResumeKind::Move) {
        fault = move_bytes(mem, m.arg_ptr(0), m.arg_ptr(1), n, from, &done);
    } else {
        fault = fill_bytes(mem, m.arg_ptr(0), static_cast<u8>(m.arg(1)), n, from, &done);
    }
    m.set_return(m.arg(0));
    if (fault) deliver(m, *fault, Resume{kind, done});
}

bool publish_resume_record(GuestMemory& mem, ThunkRegistry& registry, bool x64) noexcept {
    ResumeRecord r{};
    r.magic = kResumeMagic;
    r.thunk_base = registry.thunk_base();
    for (usize i = 0; i < kResumeNames.size(); ++i) {
        auto slot = registry.get_thunk_address("ucrtbase.dll", kResumeNames[i], true);
        if (slot.is_err()) return false;
        r.slot[i] = slot.value();
    }
    GuestAddr at = 0;
    if (x64) {
        if (!ensure_x64_accel_arena(mem, registry.thunk_base())) return false;
        at = registry.thunk_base() + X64_ACCEL_OFFSET + X64_ACCEL_RESUME_OFFSET;
        // resume_slot finds the record from a thunk address by alignment.
        if (record_address(true, registry.thunk_base()) != at) return false;
    } else {
        if (!ensure_crt_accel_arena_x86(mem)) return false;
        at = CRT_ACCEL_BASE_X86 + CRT_ACCEL_RESUME_OFFSET_X86;
    }
    const Byte* live = mem.fetch_ptr(at, sizeof(r));
    if (live != nullptr && std::memcmp(live, &r, sizeof(r)) == 0) return true;
    return write_accel_code(mem, at, reinterpret_cast<const u8*>(&r), sizeof(r));
}

void run_body(Fn fn, Marshaler& m) noexcept {
    GuestMemory& mem = m.memory();
    std::optional<Fault> fault;
    std::optional<Resume> resume;
    u64 done = 0;
    switch (fn) {
        case Fn::Memcpy:
        case Fn::Memmove:
            fault = move_bytes(mem, m.arg_ptr(0), m.arg_ptr(1), m.arg(2), 0, &done);
            m.set_return(m.arg(0));
            resume = Resume{ResumeKind::Move, done};
            break;
        case Fn::Memset:
            fault = fill_bytes(mem, m.arg_ptr(0), static_cast<u8>(m.arg(1)), m.arg(2), 0, &done);
            m.set_return(m.arg(0));
            resume = Resume{ResumeKind::Fill, done};
            break;
        case Fn::Memcmp: {
            const CmpResult r = compare_bytes(mem, m.arg_ptr(0), m.arg_ptr(1), m.arg(2));
            fault = r.fault;
            m.set_return(static_cast<u64>(static_cast<i64>(r.value)));
            break;
        }
        default: {
            FindResult r;
            switch (fn) {
                case Fn::Memchr:
                    r = find_byte(mem, m.arg_ptr(0), static_cast<u8>(m.arg(1)), m.arg(2));
                    break;
                case Fn::Strchr:
                case Fn::Strrchr:
                    r = find_char(mem, m.arg_ptr(0), static_cast<u8>(m.arg(1)),
                                  fn == Fn::Strrchr);
                    break;
                case Fn::Wcschr:
                case Fn::Wcsrchr:
                    r = find_wchar(mem, m.arg_ptr(0), static_cast<u16>(m.arg(1)),
                                   fn == Fn::Wcsrchr);
                    break;
                case Fn::Strstr:
                    r = find_str(mem, m.arg_ptr(0), m.arg_ptr(1));
                    break;
                case Fn::Wcsstr:
                    r = find_wstr(mem, m.arg_ptr(0), m.arg_ptr(1));
                    break;
                default:
                    break;
            }
            fault = r.fault;
            m.set_return(r.at);
            break;
        }
    }
    if (fault) deliver(m, *fault, resume);
}

// ---------------------------------------------------------------------------
// Stub encoding
// ---------------------------------------------------------------------------

namespace {

void put32(std::vector<u8>& v, u32 x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i)));
}

[[nodiscard]] bool rel32_reaches(GuestAddr next, GuestAddr target, i64& rel) noexcept {
    rel = static_cast<i64>(target - next);
    return rel >= INT32_MIN && rel <= INT32_MAX;
}

}  // namespace

// The host branch is an INDIRECT jmp through a cell in the stub itself: the
// JIT serves a thunk crossing in-chain (L3-K) only from an indirect CALL/JMP
// tail (emit_linked_tail_reg's api_call_tail); a direct branch to the slot
// would take the dispatcher crossing on every call.

std::vector<u8> encode_stub_x86(GuestAddr at, u32 threshold_value, GuestAddr host_slot,
                                GuestAddr original) {
    std::vector<u8> v{0x81, 0x7C, 0x24, 0x0C};  // cmp dword [esp+0Ch], imm32
    put32(v, threshold_value);
    v.push_back(0x0F);  // jb rel32 -> original
    v.push_back(0x82);
    put32(v, static_cast<u32>(original) - static_cast<u32>(at + v.size() + 4));
    v.push_back(0xFF);  // jmp dword [cell] -> host slot
    v.push_back(0x25);
    put32(v, static_cast<u32>(at + v.size() + 4));
    put32(v, static_cast<u32>(host_slot));  // cell
    v.resize(CRT_ACCEL_STUB_SLOT_X86, 0xCC);
    return v;
}

std::vector<u8> encode_stub_x64(GuestAddr at, u32 threshold_value, GuestAddr host_slot,
                                GuestAddr original) {
    if (threshold_value > static_cast<u32>(INT32_MAX)) return {};
    std::vector<u8> v{0x49, 0x81, 0xF8};  // cmp r8, imm32 (sign-extended, >= 0)
    put32(v, threshold_value);
    i64 rel = 0;
    if (!rel32_reaches(at + v.size() + 6, original, rel)) return {};
    v.push_back(0x0F);  // jb rel32 -> original
    v.push_back(0x82);
    put32(v, static_cast<u32>(rel));
    v.insert(v.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});  // jmp qword [rip+0]
    put32(v, static_cast<u32>(host_slot));
    put32(v, static_cast<u32>(host_slot >> 32));
    v.resize(X64_ACCEL_STUB_SLOT, 0xCC);
    return v;
}

// ---------------------------------------------------------------------------
// Interposer
// ---------------------------------------------------------------------------

namespace {

std::atomic<int> g_force{0};

// Size crossover per function: a call with size >= the value takes the host
// body. Measured 2026-10-29 21:28-21:37 (load average 3-5 on 14 cores: no
// build, ctest, guest app or Steam running) by
// tests/winapi/msvcrt/test_vcruntime_interpose_bench.cpp
// (VcrtBench.SweepAndCrossover, VITREO_WS1_BENCH=1) on the JIT run loop, L3-K
// in-chain on, at commit 003550c3f (x64 stubs beside the image): 5
// interleaved reps, median ns per call, sizes 0..64 step 1, powers of two to
// 1 MiB, plus 1000/4097/65537. Two in-tree guest arms stand in for
// vcruntime's paths: an AVX2 loop (VMOVDQU / VPBROADCASTB / VPCMPEQB +
// VPMOVMSKB) and the string-instruction forms (REP MOVSB / REP STOSB / REPE
// CMPSB / REPNE SCASB). Crossover rule: the smallest sampled size from which
// host <= 0.9 * (the faster guest arm) there and at every larger size.
//
// memcpy/memmove/memset: NO crossover. The JIT serves REP MOVSB / REP STOSB
// as a bulk host copy already, so the guest REP form matches or beats the
// crossing at every size (1 MiB: 13.2 vs 14.3 us x86, 14.5 vs 14.2 us x64,
// inside the 10% bar), and they are not interposed. memcmp/memchr: the
// REPE CMPSB / REPNE SCASB forms run per byte; the host wins from 1000 B on
// both bitnesses (4-14x at >= 4 KiB).
//
// str*/wcs* (no size argument) are interposed only when the host wins at
// every length from 0; it loses at 0 by ~200 ns on both bitnesses (the
// crossing), even against a unit-at-a-time guest loop, so they stay guest
// (crossovers 256/256/256/256/256/1000 x86, 256/256/1000/256/1000/1000 x64).
//
// median ns/call: AVX2 guest / REP guest / host
// 32-bit   memcpy               memmove              memset             memcmp               memchr
//   0      126/128/296          138/140/312          113/111/344        133/138/337          106/106/337
//   1      146/140/306          155/149/337          112/104/337        151/148/353          123/120/348
//   16     171/172/312          166/163/319          133/128/347        162/154/353          129/130/363
//   64     363/236/312          373/248/323          150/166/329        580/171/360          387/145/372
//   256    1017/148/308         941/145/309          277/124/357        1891/248/367         1154/219/388
//   512    1883/157/313         1717/141/311         415/123/360        3646/341/384         2103/295/384
//   1000   3443/158/320         3139/149/321         667/126/357        6886/497/356         4031/444/384
//   4096   13393/191/357        14108/198/380        2332/178/376       26576/1558/423       15996/1364/456
//   65536  210671/723/971       222162/820/921       33753/1241/1899    439548/22458/1829    259616/18938/1920
//   1M     3384951/13213/14288  3459082/14206/13374  539964/9899/10044  7201185/339503/25379 4045702/323195/26074
// 64-bit
//   0      23/58/192            21/51/193            23/38/219          22/55/232            23/37/248
//   1      26/57/207            25/53/200            24/38/227          29/70/235            26/51/253
//   16     120/73/214           111/67/205           84/48/222          134/75/240           97/57/260
//   64     58/109/187           64/117/218           60/75/230          113/89/234           107/70/258
//   256    171/68/189           180/77/218           166/52/217         370/159/242          365/138/263
//   512    314/70/189           344/77/229           319/50/235         723/237/245          706/215/265
//   1000   612/74/203           677/81/231           597/52/237         1478/423/258         1371/358/269
//   4096   2342/123/230         2528/118/255         2254/88/278        5901/1510/360        5467/1255/346
//   65536  40124/739/874        38589/707/845        35184/978/1364     94893/23252/1764     84993/19069/1797
//   1M     602256/14470/14226   624779/12940/14188   559716/9444/9672   1464786/348078/23660 1399183/307045/25040
constexpr std::array<u32, kFnCount> kThresholdX86{
    kNeverHost,  // memcpy   (no crossover: REP MOVSB is as fast)
    kNeverHost,  // memmove  (no crossover)
    kNeverHost,  // memset   (no crossover: REP STOSB is as fast)
    // memcmp/memchr win >=2x from 1000 B on x86 too, but the cmp [esp+0Ch]/jb
    // stub costs 2.6-5.6 ns per call below the crossover: an extra JIT block
    // hop that the <=1 ns gate does not allow. x86 stays on the guest body.
    kNeverHost,  // memcmp
    kNeverHost,  // memchr
    kNeverHost, kNeverHost, kNeverHost,   // strchr strrchr strstr
    kNeverHost, kNeverHost, kNeverHost};  // wcschr wcsrchr wcsstr
constexpr std::array<u32, kFnCount> kThresholdX64{
    kNeverHost,  // memcpy
    kNeverHost,  // memmove
    kNeverHost,  // memset
    1000,        // memcmp
    1000,        // memchr
    kNeverHost, kNeverHost, kNeverHost,   // strchr strrchr strstr
    kNeverHost, kNeverHost, kNeverHost};  // wcschr wcsrchr wcsstr

constexpr u32 kNoOverride = 0xFFFFFFFEu;
std::array<std::atomic<u32>, kFnCount> g_override_x86{};
std::array<std::atomic<u32>, kFnCount> g_override_x64{};
std::once_flag g_override_init;

void init_overrides() {
    std::call_once(g_override_init, [] {
        for (auto& o : g_override_x86) o.store(kNoOverride);
        for (auto& o : g_override_x64) o.store(kNoOverride);
    });
}

[[nodiscard]] bool has_size_argument(Fn fn) noexcept {
    return fn == Fn::Memcpy || fn == Fn::Memmove || fn == Fn::Memset ||
           fn == Fn::Memcmp || fn == Fn::Memchr;
}

std::mutex g_stub_mutex;

/// Bounds-checked little-endian reads inside [base, end).
struct ImageReader {
    const GuestMemory& mem;
    GuestAddr base;
    GuestAddr end;
    [[nodiscard]] bool in(GuestAddr a, u64 n) const noexcept {
        return a >= base && a <= end && n <= end - a;
    }
    [[nodiscard]] bool u16_at(GuestAddr a, u16& out) const noexcept {
        if (!in(a, 2)) return false;
        auto r = mem.read_u16(a);
        if (r.is_err()) return false;
        out = r.value();
        return true;
    }
    [[nodiscard]] bool u32_at(GuestAddr a, u32& out) const noexcept {
        if (!in(a, 4)) return false;
        auto r = mem.read_u32(a);
        if (r.is_err()) return false;
        out = r.value();
        return true;
    }
};

constexpr char kVcrtName[] = "vcruntime140.dll";
constexpr usize kVcrtNameBytes = sizeof(kVcrtName);  // with the NUL

/// Guest address of IMAGE_EXPORT_DIRECTORY.Name of the image, or 0.
[[nodiscard]] GuestAddr export_name_va(const GuestMemory& mem, GuestAddr base,
                                       GuestAddr image_end) noexcept {
    if (image_end <= base) return 0;
    const ImageReader r{mem, base, image_end};
    u16 mz = 0;
    u32 lfanew = 0, sig = 0;
    if (!r.u16_at(base, mz) || mz != 0x5A4D) return 0;          // "MZ"
    if (!r.u32_at(base + 0x3C, lfanew)) return 0;
    const GuestAddr nt = base + lfanew;
    if (!r.u32_at(nt, sig) || sig != 0x00004550u) return 0;     // "PE\0\0"
    const GuestAddr opt = nt + 24;  // after Signature + IMAGE_FILE_HEADER
    u16 magic = 0;
    if (!r.u16_at(opt, magic)) return 0;
    GuestAddr count_at = 0, dir_at = 0;
    if (magic == 0x10B) {        // PE32
        count_at = opt + 92;
        dir_at = opt + 96;
    } else if (magic == 0x20B) { // PE32+
        count_at = opt + 108;
        dir_at = opt + 112;
    } else {
        return 0;
    }
    u32 count = 0, export_rva = 0, export_size = 0;
    if (!r.u32_at(count_at, count) || count < 1) return 0;
    if (!r.u32_at(dir_at, export_rva) || !r.u32_at(dir_at + 4, export_size)) return 0;
    if (export_rva == 0 || export_size < 40) return 0;  // IMAGE_EXPORT_DIRECTORY
    u32 name_rva = 0;
    if (!r.u32_at(base + export_rva + 12, name_rva) || name_rva == 0) return 0;
    return base + name_rva;
}

/// Copy up to kVcrtNameBytes bytes at @p at (bounded by @p image_end).
[[nodiscard]] usize read_name(const GuestMemory& mem, GuestAddr at, GuestAddr image_end,
                              std::array<u8, kVcrtNameBytes>& out) noexcept {
    if (at >= image_end) return 0;
    const usize len = static_cast<usize>(std::min<u64>(kVcrtNameBytes, image_end - at));
    const Byte* p = mem.host_ptr(at, len);
    if (p == nullptr) return 0;
    std::memcpy(out.data(), p, len);
    return len;
}

[[nodiscard]] bool names_vcruntime140(const std::array<u8, kVcrtNameBytes>& b,
                                      usize len) noexcept {
    if (len != kVcrtNameBytes) return false;
    for (usize i = 0; i < kVcrtNameBytes; ++i) {
        u8 c = b[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<u8>(c - 'A' + 'a');
        if (c != static_cast<u8>(kVcrtName[i])) return false;  // [16] is NUL
    }
    return true;
}

// ---- Caches (all under g_stub_mutex) ---------------------------------------
//
// Keyed by the GuestMemory (one per process) and revalidated against guest
// memory on every hit, so an unloaded module, a different image loaded at
// the same base or a GuestMemory reallocated at the same host address can
// never be answered from a stale entry: the check reads only a few bytes.

/// Identity per (memory, base, end): the export-directory name bytes and
/// where they were. A hit re-reads only those bytes.
struct IdentityEntry {
    const GuestMemory* mem;
    GuestAddr base;
    GuestAddr end;
    GuestAddr name_va;  ///< 0: the image did not parse (not cached as a hit)
    std::array<u8, kVcrtNameBytes> name;
    usize name_len;
    bool result;
};
std::vector<IdentityEntry> g_identity;

/// A stub written for (memory, original, threshold, host) at @p at; a hit
/// re-compares the slot's bytes with the encoding.
struct StubEntry {
    const GuestMemory* mem;
    GuestAddr original;
    u32 threshold;
    GuestAddr host;
    GuestAddr at;
};
std::vector<StubEntry> g_stubs;

/// x64 stub pages: one 4 KiB RX page per reach window, slot 0 a header.
struct NearPage {
    const GuestMemory* mem;
    GuestAddr at;
};
std::vector<NearPage> g_near_pages;

constexpr u64 kNearPageSize = 0x1000;
constexpr u64 kNearGranularity = 0x10000;  // VirtualAlloc's allocation granularity
constexpr u64 kNearMagic = 0x3150'4753'5452'4356ull;  // "VCRTSGP1"
/// Largest |target - next IP| a rel32 may span, with a margin.
constexpr u64 kRel32Reach = 0x7FFF0000ull;

bool cached_is_vcruntime140(const GuestMemory& mem, GuestAddr base, GuestAddr end) noexcept {
    for (auto it = g_identity.begin(); it != g_identity.end(); ++it) {
        if (it->mem != &mem || it->base != base || it->end != end) continue;
        if (it->name_va != 0) {
            std::array<u8, kVcrtNameBytes> now{};
            const usize len = read_name(mem, it->name_va, end, now);
            if (len == it->name_len && std::memcmp(now.data(), it->name.data(), len) == 0) {
                return it->result;
            }
        }
        g_identity.erase(it);
        break;
    }
    IdentityEntry e{&mem, base, end, export_name_va(mem, base, end), {}, 0, false};
    if (e.name_va != 0) {
        e.name_len = read_name(mem, e.name_va, end, e.name);
        e.result = names_vcruntime140(e.name, e.name_len);
        if (g_identity.size() >= 256) g_identity.erase(g_identity.begin());
        g_identity.push_back(e);
    }
    return e.result;
}

[[nodiscard]] bool near_page_ok(const GuestMemory& mem, GuestAddr at) noexcept {
    const Byte* p = mem.fetch_ptr(at, 8);
    if (p == nullptr) return false;
    u64 magic = 0;
    std::memcpy(&magic, p, 8);
    return magic == kNearMagic;
}

/// Every stub on the page at @p page reaches every byte of [lo, hi).
[[nodiscard]] bool page_reaches(GuestAddr page, GuestAddr lo, GuestAddr hi) noexcept {
    const GuestAddr a = std::min(page, lo);
    const GuestAddr b = std::max<GuestAddr>(page + kNearPageSize, hi);
    return b - a <= kRel32Reach;
}

/// A stub page within rel32 reach of the image [lo, hi): an existing one,
/// else a new page mapped RX in the nearest free granule above the image,
/// else below it. 0 when none is free within reach.
GuestAddr near_page_x64(GuestMemory& mem, GuestAddr lo, GuestAddr hi) noexcept {
    for (auto it = g_near_pages.begin(); it != g_near_pages.end();) {
        if (it->mem != &mem) {
            ++it;
            continue;
        }
        if (!near_page_ok(mem, it->at)) {
            it = g_near_pages.erase(it);
            continue;
        }
        if (page_reaches(it->at, lo, hi)) return it->at;
        ++it;
    }
    auto claim = [&](GuestAddr at) -> GuestAddr {
        if (at == 0 || !page_reaches(at, lo, hi)) return 0;
        if (mem.map_region(at, kNearPageSize, MemoryProtection::ReadExecute).is_err()) return 0;
        u8 header[X64_ACCEL_STUB_SLOT];
        std::memset(header, 0xCC, sizeof(header));
        std::memcpy(header, &kNearMagic, 8);
        if (!write_accel_code(mem, at, header, sizeof(header))) {
            (void)mem.unmap_region(at, kNearPageSize);
            return 0;
        }
        g_near_pages.push_back({&mem, at});
        return at;
    };
    const GuestAddr above_from = (hi + kNearGranularity - 1) & ~(kNearGranularity - 1);
    if (const GuestAddr got = claim(mem.find_free_gap(above_from, kNearPageSize, kNearGranularity))) {
        return got;
    }
    const GuestAddr floor = lo > kRel32Reach ? lo - kRel32Reach + kNearPageSize : kNearGranularity;
    const GuestAddr below = mem.find_free_gap(floor, kNearPageSize, kNearGranularity);
    if (below != 0 && below + kNearPageSize <= lo) return claim(below);
    return 0;
}

}  // namespace

bool enabled() noexcept {
    switch (g_force.load(std::memory_order_relaxed)) {
        case static_cast<int>(Force::On): return true;
        case static_cast<int>(Force::Off): return false;
        default: break;
    }
    static const bool env_on = [] {
        const char* v = std::getenv("VITREO_WS1");
        return v == nullptr || std::strcmp(v, "0") != 0;
    }();
    return env_on;
}

void force_for_test(Force f) noexcept {
    g_force.store(static_cast<int>(f), std::memory_order_relaxed);
}

u32 threshold(Fn fn, u32 bitness) noexcept {
    init_overrides();
    const usize i = static_cast<usize>(fn);
    if (i >= kFnCount) return kNeverHost;
    const u32 o = (bitness == 64 ? g_override_x64 : g_override_x86)[i].load(
        std::memory_order_relaxed);
    if (o != kNoOverride) return o;
    return bitness == 64 ? kThresholdX64[i] : kThresholdX86[i];
}

void set_threshold_for_test(Fn fn, u32 bitness, std::optional<u32> value) noexcept {
    init_overrides();
    const usize i = static_cast<usize>(fn);
    if (i >= kFnCount) return;
    (bitness == 64 ? g_override_x64 : g_override_x86)[i].store(
        value.value_or(kNoOverride), std::memory_order_relaxed);
}

bool is_vcruntime140_image(const GuestMemory& mem, GuestAddr base,
                           GuestAddr image_end) noexcept {
    const GuestAddr at = export_name_va(mem, base, image_end);
    if (at == 0) return false;
    std::array<u8, kVcrtNameBytes> name{};
    return names_vcruntime140(name, read_name(mem, at, image_end, name));
}

GuestAddr interpose_lookup(GuestMemory& mem, ThunkRegistry& registry,
                           const LookupModule& mod, std::string_view export_name_v,
                           GuestAddr original) noexcept {
    if (original == 0 || !enabled()) return original;
    const auto fn = fn_for_export(export_name_v);
    if (!fn || (mod.bitness != 32 && mod.bitness != 64)) return original;
    const u32 t = threshold(*fn, mod.bitness);
    if (t == kNeverHost) return original;
    const bool sized = has_size_argument(*fn);
    // Without a size argument nothing can branch: by the crossover rule such
    // a function is interposed only when the host wins at every length.
    if (!sized && t != 0) return original;

    std::lock_guard<std::mutex> lock(g_stub_mutex);
    if (!cached_is_vcruntime140(mem, mod.base, mod.image_end)) return original;
    auto slot = registry.get_thunk_address("ucrtbase.dll", internal_export_name(*fn), true);
    if (slot.is_err()) return original;
    const GuestAddr host = slot.value();
    const bool x64 = mod.bitness == 64;
    // A host body that faults continues from its resume entry; the record
    // that names it must exist before any stub can reach the body.
    (void)publish_resume_record(mem, registry, x64);
    if (!sized) return host;

    auto encode = [&](GuestAddr at) {
        return x64 ? encode_stub_x64(at, t, host, original)
                   : encode_stub_x86(at, t, host, original);
    };
    for (auto it = g_stubs.begin(); it != g_stubs.end(); ++it) {
        if (it->mem != &mem || it->original != original || it->threshold != t ||
            it->host != host) {
            continue;
        }
        const auto want = encode(it->at);
        const Byte* live = want.empty() ? nullptr : mem.fetch_ptr(it->at, want.size());
        if (live != nullptr && std::memcmp(live, want.data(), want.size()) == 0) return it->at;
        g_stubs.erase(it);
        break;
    }

    GuestAddr region = 0;
    usize slot_size = 0, first = 0, slots = 0;
    if (x64) {
        // The guest branch must be a direct jb rel32 (no far form: see
        // encode_stub_x64), so the page sits within reach of the image.
        region = near_page_x64(mem, mod.base, mod.image_end);
        if (region == 0) return original;
        slot_size = X64_ACCEL_STUB_SLOT;
        first = 1;  // slot 0 is the page header
        slots = kNearPageSize / X64_ACCEL_STUB_SLOT;
    } else {
        if (!ensure_crt_accel_arena_x86(mem)) return original;
        region = CRT_ACCEL_BASE_X86 + CRT_ACCEL_STUB_OFFSET_X86;
        slot_size = CRT_ACCEL_STUB_SLOT_X86;
        slots = (CRT_ACCEL_RESUME_OFFSET_X86 - CRT_ACCEL_STUB_OFFSET_X86) / CRT_ACCEL_STUB_SLOT_X86;
    }
    const Byte* live = mem.fetch_ptr(region, slots * slot_size);
    if (live == nullptr) return original;
    // The region is its own index: a used slot holds a stub (never all
    // zero), and a stub's encoding at a given slot is unique, so a reloaded
    // module at the same base finds its stub again even past the cache.
    usize free_slot = slots;
    for (usize i = first; i < slots; ++i) {
        const GuestAddr at = region + i * slot_size;
        const auto code = encode(at);
        if (code.empty()) return original;
        const Byte* cur = live + i * slot_size;
        if (std::memcmp(cur, code.data(), slot_size) == 0) {
            g_stubs.push_back({&mem, original, t, host, at});
            return at;
        }
        if (free_slot == slots &&
            std::all_of(cur, cur + slot_size, [](Byte b) { return b == 0; })) {
            free_slot = i;
        }
    }
    if (free_slot == slots) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::fprintf(stderr,
                         "[vitreo] vcruntime140 interposer: stub region full (%zu "
                         "slots, %u-bit); further lookups keep the module's own body\n",
                         slots - first, mod.bitness);
        }
        return original;
    }
    const GuestAddr at = region + free_slot * slot_size;
    const auto code = encode(at);
    if (code.empty() || !write_accel_code(mem, at, code.data(), code.size())) return original;
    if (g_stubs.size() >= 1024) g_stubs.erase(g_stubs.begin());
    g_stubs.push_back({&mem, original, t, host, at});
    return at;
}

}  // namespace vitreo::winapi::vcrt
