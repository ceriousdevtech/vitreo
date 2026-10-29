// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - shared CPUID synthesis
//
// Every leaf follows Intel SDM Vol. 2A, CPUID. Feature leaves describe the ISA
// Vitreo implements; identity leaves describe the host machine as recorded in
// platform::host_hardware(), so the guest sees the real processor.

#include "core/cpu/cpuid_synth.hpp"

#include "platform/host_hardware.hpp"

#include <vitreo/virtual_tsc.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <vector>

namespace vitreo::cpu::ops {

namespace {

// VITREO_CPUID_NO_* bisection knobs. Presence-tested (getenv != nullptr, so an
// empty value counts) and read once, on the first CPUID, instead of 12 environ
// scans per leaf-1/7 query.
struct CpuidKnobMasks {
    u32 leaf1_ecx_clear = 0;
    u32 leaf7_ebx_clear = 0;
};

const CpuidKnobMasks& cpuid_knob_masks() noexcept {
    static const CpuidKnobMasks masks = [] {
        const auto set = [](const char* name) { return std::getenv(name) != nullptr; };
        CpuidKnobMasks m;
        if (set("VITREO_CPUID_NO_AESNI"))  m.leaf1_ecx_clear |= 1u << 25;
        if (set("VITREO_CPUID_NO_PCLMUL")) m.leaf1_ecx_clear |= 1u << 1;
        if (set("VITREO_CPUID_NO_SSE42"))  m.leaf1_ecx_clear |= 1u << 20;
        if (set("VITREO_CPUID_NO_SSE41"))  m.leaf1_ecx_clear |= 1u << 19;
        if (set("VITREO_CPUID_NO_SSSE3"))  m.leaf1_ecx_clear |= 1u << 9;
        if (set("VITREO_CPUID_NO_AVX"))    m.leaf1_ecx_clear |= (1u << 28) | (1u << 12);  // AVX + FMA
        if (set("VITREO_CPUID_NO_FMA"))    m.leaf1_ecx_clear |= 1u << 12;  // FMA only
        if (set("VITREO_CPUID_NO_SSE3"))   m.leaf1_ecx_clear |= 1u << 0;
        if (set("VITREO_CPUID_NO_SHA"))    m.leaf7_ebx_clear |= 1u << 29;
        if (set("VITREO_CPUID_NO_AVX2"))   m.leaf7_ebx_clear |= 1u << 5;
        if (set("VITREO_CPUID_NO_BMI"))    m.leaf7_ebx_clear |= (1u << 3) | (1u << 8);  // BMI1 + BMI2
        if (set("VITREO_CPUID_NO_ADX"))    m.leaf7_ebx_clear |= 1u << 19;  // ADCX/ADOX
        return m;
    }();
    return masks;
}

[[nodiscard]] u32 bits_for(u32 count) noexcept {
    u32 bits = 0;
    while ((1u << bits) < count) ++bits;
    return bits;
}

/// The guest's view of the host's processor topology. APIC IDs are laid out
/// hierarchically, as on real x86 parts: a cluster (the processors sharing an
/// L2) owns an aligned power-of-two block, so the sharing masks of leaf 4 and
/// the level shifts of leaves 0xB/0x1F select exactly the real groups. There is
/// no SMT, so every logical processor is a core.
struct GuestTopology {
    const platform::HostHardwareProfile* source = nullptr;
    std::vector<u32> apic_ids;      ///< indexed by logical processor
    u32 cluster_shift = 0;          ///< APIC ID bits of a core within its cluster
    u32 cluster_size = 1;           ///< logical processors in the largest cluster
    u32 package_shift = 0;          ///< APIC ID bits of every processor in the package
};

GuestTopology build_topology(const platform::HostHardwareProfile& host) {
    GuestTopology topology;
    topology.source = &host;
    const auto& processors = host.cpu.processors;
    if (processors.empty()) {
        topology.apic_ids.push_back(0);
        return topology;
    }
    // Dense cluster ordinals in order of first appearance, and each
    // processor's position inside its cluster.
    std::vector<std::pair<u32, u32>> clusters;  // (core class, cluster id)
    std::vector<u32> members;
    std::vector<std::pair<u32, u32>> placement(processors.size());
    for (usize i = 0; i < processors.size(); ++i) {
        const std::pair<u32, u32> key{processors[i].core_class, processors[i].cluster};
        auto it = std::find(clusters.begin(), clusters.end(), key);
        if (it == clusters.end()) {
            clusters.push_back(key);
            members.push_back(0);
            it = clusters.end() - 1;
        }
        const usize ordinal = static_cast<usize>(it - clusters.begin());
        placement[i] = {static_cast<u32>(ordinal), members[ordinal]++};
    }
    topology.cluster_size = *std::max_element(members.begin(), members.end());
    topology.cluster_shift = bits_for(topology.cluster_size);
    for (const auto& [cluster, member] : placement) {
        topology.apic_ids.push_back((cluster << topology.cluster_shift) | member);
    }
    topology.package_shift =
        bits_for(*std::max_element(topology.apic_ids.begin(), topology.apic_ids.end()) + 1);
    return topology;
}

/// Built once per thread and per profile: a guest thread asks CPUID far more
/// often than the profile changes (which only tests do).
const GuestTopology& topology() {
    thread_local GuestTopology cached;
    const platform::HostHardwareProfile& host = platform::host_hardware();
    if (cached.source != &host) cached = build_topology(host);
    return cached;
}

const std::array<u8, 48>& brand_bytes(const platform::HostHardwareProfile& host) {
    thread_local const platform::HostHardwareProfile* source = nullptr;
    thread_local std::array<u8, 48> bytes{};
    if (source != &host) {
        source = &host;
        bytes.fill(0);
        // The brand is the host processor's name. 47 characters at most: the
        // string is NUL-terminated within its 48 bytes.
        const std::string& name = host.cpu.brand.empty() ? std::string("Vitreo x86-64 Virtual CPU")
                                                         : host.cpu.brand;
        std::memcpy(bytes.data(), name.data(), std::min<usize>(name.size(), bytes.size() - 1));
    }
    return bytes;
}

const platform::HostCoreClass* core_class_of(const platform::HostHardwareProfile& host, u32 processor) {
    if (host.cpu.core_classes.empty()) return nullptr;
    const u32 index = processor < host.cpu.processors.size()
        ? host.cpu.processors[processor].core_class : 0;
    return &host.cpu.core_classes[std::min<usize>(index, host.cpu.core_classes.size() - 1)];
}

/// One leaf 4 entry: the host cache's exact size and line size, with the
/// associativity of platform::reported_cache_ways (macOS does not expose it).
void cache_leaf(u32 type, u32 level, u64 size, u32 line, u32 sharing_bits, u32 package_shift,
                u32& eax, u32& ebx, u32& ecx, u32& edx) noexcept {
    const u32 ways = platform::reported_cache_ways(level, size, line);
    if (ways == 0 || ways > 1024) return;
    const u64 sets = size / line / ways;
    eax = type | (level << 5) | (1u << 8) |
          (((1u << sharing_bits) - 1u) << 14) | (((1u << package_shift) - 1u) << 26);
    ebx = (line - 1u) | ((ways - 1u) << 22);   // one physical line partition
    ecx = static_cast<u32>(sets - 1);
    edx = 0;  // WBINVD scope, inclusiveness and indexing are not exposed
}

/// Leaf 0x80000006 L2 associativity code (Intel SDM; the same values on AMD).
[[nodiscard]] u32 l2_associativity_code(u32 ways) noexcept {
    switch (ways) {
        case 1: return 0x1;
        case 2: return 0x2;
        case 4: return 0x4;
        case 8: return 0x6;
        case 16: return 0x8;
        default: return 0x0;
    }
}

} // namespace

void synth_cpuid_on(u32 processor, u32 leaf, u32 subleaf, bool long_mode,
                    u32& eax, u32& ebx, u32& ecx, u32& edx) noexcept {
    eax = 0; ebx = 0; ecx = 0; edx = 0;
    const platform::HostHardwareProfile& host = platform::host_hardware();
    const GuestTopology& topo = topology();
    if (processor >= topo.apic_ids.size()) processor = 0;
    const u32 apic_id = topo.apic_ids[processor];
    const u32 line = host.cpu.cache_line_bytes;
    switch (leaf) {
        // Leaf 0: highest standard leaf and the vendor "GenuineIntel".
        case 0x00:
            eax = 0x1F;
            ebx = 0x756E6547;   // "Genu"
            edx = 0x49656E69;   // "ineI"
            ecx = 0x6C65746E;   // "ntel"
            break;

        // Leaf 1: signature, topology and feature flags.
        case 0x01: {
            eax = 0x000306C3;   // Family 6, Model 3C, Stepping 3 (Haswell-class ISA)
            // EBX: CLFLUSH line size in 8-byte units, addressable logical
            // processor IDs in the package, and this processor's APIC ID.
            ebx = ((line / 8) << 8) | (std::min(1u << topo.package_shift, 0xFFu) << 16) | (apic_id << 24);
            // SSE3|PCLMULQDQ|SSSE3|FMA|CX16|SSE4.1|SSE4.2|MOVBE|POPCNT|
            // AES-NI|XSAVE|OSXSAVE|AVX|F16C (Intel SDM Vol. 2 CPUID).
            ecx = 0x3ED83203;
            ecx |= (1u << 31);  // Hypervisor/translator present
            edx = 0x078BFBFD;   // FPU|DE|PSE|TSC|MSR|PAE|CX8|SEP|CMOV|CLFSH|MMX|FXSR|SSE|SSE2
            if (topo.apic_ids.size() > 1) edx |= 1u << 28;  // HTT: EBX[23:16] is valid
            // Diagnostic/bisection knobs: let us mask off individual
            // ECX bits to force guest crypto libraries onto their
            // software fallbacks. Used to isolate which hardware
            // intrinsic emulation (if any) corrupts output. Read once.
            ecx &= ~cpuid_knob_masks().leaf1_ecx_clear;
            break;
        }

        // Leaf 2: descriptor 0xFF, "use leaf 4 for cache parameters".
        case 0x02:
            eax = 0x0000FF01;
            break;

        // Leaf 4: deterministic cache parameters of this processor's core.
        case 0x04: {
            const platform::HostCoreClass* core = core_class_of(host, processor);
            if (core == nullptr) break;
            // L2 is shared by the cluster; an L3 (Intel Macs) by the package.
            const u32 l2_bits = topo.cluster_shift;
            switch (subleaf) {
                case 0: cache_leaf(1, 1, core->l1d_bytes, line, 0, topo.package_shift, eax, ebx, ecx, edx); break;
                case 1: cache_leaf(2, 1, core->l1i_bytes, line, 0, topo.package_shift, eax, ebx, ecx, edx); break;
                case 2: cache_leaf(3, 2, core->l2_bytes, line, l2_bits, topo.package_shift, eax, ebx, ecx, edx); break;
                case 3: cache_leaf(3, 3, host.cpu.l3_bytes, line, topo.package_shift, topo.package_shift,
                                   eax, ebx, ecx, edx); break;
                default: break;  // type 0: no more caches
            }
            break;
        }

        // Leaf 7: Structured extended features
        case 0x07:
            if (subleaf == 0) {
                // BMI1|BMI2|ADX|SHA-NI. AVX2 is currently exposed only in
                // long mode: the 32-bit execution path does not yet satisfy
                // the complete AVX2 contract, so advertising it lets guest
                // dispatchers select routines Vitreo cannot execute reliably.
                ebx = (1u << 3) | (1u << 8) | (1u << 19) | (1u << 29);
                if (long_mode) {
                    ebx |= (1u << 5);   // AVX2
                }
                ebx &= ~cpuid_knob_masks().leaf7_ebx_clear;
            }
            break;

        // Leaves 0xB / 0x1F: extended topology. 0xB has the SMT and Core
        // levels; 0x1F adds the Module level, the cluster sharing an L2.
        case 0x0B:
        case 0x1F: {
            edx = apic_id;
            ecx = subleaf & 0xFF;
            const u32 processors = static_cast<u32>(topo.apic_ids.size());
            if (subleaf == 0) {                               // SMT: one thread per core
                eax = 0;
                ebx = 1;
                ecx |= 1u << 8;
            } else if (subleaf == 1) {                        // Core
                if (leaf == 0x1F && topo.cluster_shift > 0) {
                    eax = topo.cluster_shift;
                    ebx = topo.cluster_size;
                } else {
                    eax = topo.package_shift;
                    ebx = processors;
                }
                ecx |= 2u << 8;
            } else if (subleaf == 2 && leaf == 0x1F && topo.cluster_shift > 0) {  // Module
                eax = topo.package_shift;
                ebx = processors;
                ecx |= 3u << 8;
            }
            break;
        }

        // XSAVE feature enumeration (Intel SDM Vol. 2 CPUID leaf 0DH).
        case 0x0D:
            if (subleaf == 0) {
                eax = 0x7;          // x87 | SSE | AVX valid in XCR0
                ebx = 832;          // XSAVE area size for current XCR0
                ecx = 832;          // maximum XSAVE area size
            } else if (subleaf == 1) {
                eax = 1;            // XSAVEOPT
                ebx = 832;
            } else if (subleaf == 2) {
                eax = 256;          // YMM high size
                ebx = 576;          // YMM high offset
            }
            break;

        // Leaf 0x15: TSC = crystal x EBX / EAX. The crystal is the host's
        // timebase, the clock every guest-visible counter is scaled from.
        case 0x15: {
            const u64 crystal = host.cpu.timebase_hz;
            const u64 tsc = guest_tsc_hz();
            if (crystal == 0 || crystal > 0xFFFFFFFFu) break;
            const u64 divisor = std::gcd(tsc, crystal);
            const u64 numerator = tsc / divisor;
            const u64 denominator = crystal / divisor;
            if (numerator > 0xFFFFFFFFu || denominator > 0xFFFFFFFFu) break;
            eax = static_cast<u32>(denominator);
            ebx = static_cast<u32>(numerator);
            ecx = static_cast<u32>(crystal);
            break;
        }

        // Leaf 0x16: base (the TSC rate), maximum and reference clock in MHz.
        case 0x16: {
            eax = static_cast<u32>(guest_tsc_hz() / 1'000'000);
            for (const platform::HostCoreClass& core : host.cpu.core_classes) {
                ebx = std::max(ebx, core.max_mhz);
            }
            ecx = static_cast<u32>(host.cpu.timebase_hz / 1'000'000);
            break;
        }

        // Leaf 0x80000000: highest extended leaf. 0x80000008 (address sizes)
        // is not offered: macOS does not expose a physical address width.
        case 0x80000000:
            eax = 0x80000007;
            break;

        // Leaf 0x80000001: Extended feature flags
        case 0x80000001:
            ecx = 0x00000021;   // LAHF in long mode | LZCNT (ABM)
            edx = 0x20000800;   // SYSCALL | Long Mode
            break;

        // Leaves 0x80000002-4: the processor brand string.
        case 0x80000002:
        case 0x80000003:
        case 0x80000004: {
            const auto& brand = brand_bytes(host);
            const usize offset = static_cast<usize>(leaf - 0x80000002) * 16;
            std::memcpy(&eax, brand.data() + offset + 0, 4);
            std::memcpy(&ebx, brand.data() + offset + 4, 4);
            std::memcpy(&ecx, brand.data() + offset + 8, 4);
            std::memcpy(&edx, brand.data() + offset + 12, 4);
            break;
        }

        // Leaf 0x80000006: this core's L2 (line size, associativity code,
        // size in KB), the same cache leaf 4 sub-leaf 2 describes.
        case 0x80000006: {
            const platform::HostCoreClass* core = core_class_of(host, processor);
            if (core == nullptr || core->l2_bytes == 0) break;
            const u32 ways = platform::reported_cache_ways(2, core->l2_bytes, line);
            ecx = (line & 0xFF) | (l2_associativity_code(ways) << 12) |
                  (static_cast<u32>(std::min<u64>(core->l2_bytes / 1024, 0xFFFF)) << 16);
            break;
        }

        // Leaf 0x80000007: invariant TSC. The guest TSC is a fixed scaling of
        // one monotonic host clock in every power state.
        case 0x80000007:
            edx = 1u << 8;
            break;

        // Hypervisor vendor leaf. The register order for this leaf is
        // EBX, ECX, EDX (unlike the CPU vendor leaf's EBX, EDX, ECX).
        case 0x40000000:
            eax = 0x40000000;
            ebx = 0x72746956;  // "Vitr"
            ecx = 0x69566F65;  // "eoVi"
            edx = 0x20207472;  // "rt  "
            break;

        default:
            break;              // Unknown leaf: all zeros
    }
}

void synth_cpuid(u32 leaf, u32 subleaf, bool long_mode,
                 u32& eax, u32& ebx, u32& ecx, u32& edx) noexcept {
    synth_cpuid_on(platform::current_processor(), leaf, subleaf, long_mode, eax, ebx, ecx, edx);
}

} // namespace vitreo::cpu::ops
