// SPDX-License-Identifier: LGPL-2.1-only

/// @file cpuid_intercept.cpp
/// @brief See cpuid_intercept.hpp for why the guest must not see the host CPU.

#include "core/cpu/x64/cpuid_intercept.hpp"

#include "core/cpu/decode/decoder.hpp"
#include "core/memory/guest_memory.hpp"

#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>

namespace vitreo::cpu::x64 {

namespace {

std::mutex               g_sites_mu;
std::unordered_set<u32>  g_sites;

/// Reads guest bytes straight out of the host mapping. Under identity mapping
/// the guest address is the host address, so this is a plain span walk.
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

} // namespace

usize intercept_cpuid_in_range(GuestMemory& memory, u32 base, u32 size) {
    // On by default: a natively-executing guest must not see the host CPU.
    // VITREO_NO_CPUID_PATCH=1 disables the rewrite for bisection.
    if (std::getenv("VITREO_NO_CPUID_PATCH") != nullptr) return 0;
    if (size < 2) return 0;

    u8* const host = memory.host_ptr_mut(static_cast<GuestAddr>(base), size);
    if (host == nullptr) return 0;

    // Guest code is mapped read+execute, so the rewrite has to open the pages
    // first. Under identity mapping the host page carries the guest's own
    // protection, and writing straight through faults.
    const usize page = 0x1000;
    auto* const lo = reinterpret_cast<u8*>(
        reinterpret_cast<uintptr_t>(host) & ~(page - 1));
    auto* const hi = reinterpret_cast<u8*>(
        (reinterpret_cast<uintptr_t>(host) + size + page - 1) & ~(page - 1));
    const usize span = static_cast<usize>(hi - lo);
    const bool opened =
        mprotect(lo, span, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
    if (!opened) {
        std::fprintf(stderr,
                     "[cpuid] could not open 0x%x+0x%x for patching; guest "
                     "will see the host CPU in that range\n", base, size);
        return 0;
    }

    usize patched = 0;
    usize offset = 0;

    // Linear sweep. Decoding forward from the section start keeps the scan on
    // instruction boundaries, so an immediate or displacement that happens to
    // contain 0F A2 is never mistaken for the opcode. A sweep can still
    // desynchronise where data is interleaved with code, so a candidate is
    // rewritten only when the decoder both succeeds AND reports CPUID -- a
    // desynchronised stream almost always fails to decode instead.
    while (offset + 2 <= size) {
        SpanReader reader(host + offset, size - offset);
        DecodedInstruction instr{};
        const DecodeError err =
            decode_instruction(reader, DecodeMode::Mode32, instr);
        if (err != DecodeError::Success || instr.length == 0) {
            // Undecodable byte: step one and resynchronise.
            ++offset;
            continue;
        }
        if (instr.opcode == Opcode::CPUID && instr.length == 2 &&
            host[offset] == 0x0F && host[offset + 1] == 0xA2) {
            // 0F 0B = UD2. Same length, so nothing downstream moves.
            host[offset + 1] = 0x0B;
            {
                std::lock_guard<std::mutex> lk(g_sites_mu);
                g_sites.insert(base + static_cast<u32>(offset));
            }
            ++patched;
        }
        offset += instr.length;
    }

    // Restore execute-only-ish protection. Leaving guest code writable would
    // hide genuine guest write-to-code bugs that Windows would fault on.
    (void)mprotect(lo, span, PROT_READ | PROT_EXEC);

    if (patched != 0) {
        std::fprintf(stderr,
                     "[cpuid] intercepted %zu CPUID site(s) in 0x%x-0x%x\n",
                     patched, base, base + size);
    }
    return patched;
}

bool is_cpuid_patch_site(u32 addr) noexcept {
    std::lock_guard<std::mutex> lk(g_sites_mu);
    return g_sites.find(addr) != g_sites.end();
}

usize cpuid_patch_count() noexcept {
    std::lock_guard<std::mutex> lk(g_sites_mu);
    return g_sites.size();
}

} // namespace vitreo::cpu::x64
