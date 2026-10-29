/// @file sysinfo.cpp
/// @brief Implementation of system information APIs (WIN32-064)

#include "core/pe/os_version.hpp"
#include "winapi/kernel32/system/processor_topology.hpp"
#include "sysinfo.hpp"
#include <vitreo/virtual_tsc.hpp>
#include <vitreo/time_diag.hpp>
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/system/time.hpp"
#include "virtual_firmware.hpp"
#include "graphics/common/perf_diag.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/cpu/cpuid_synth.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <mach/mach_time.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/statvfs.h>
#endif

namespace vitreo::winapi {

// Windows epoch offset: 1601-01-01 to 1970-01-01 in 100-ns intervals
static constexpr u64 EPOCH_OFFSET = 116444736000000000ULL;

// =============================================================================
// SysInfoManager Implementation
// =============================================================================

u32 host_active_processor_count() {
    return processor_topology::processor_count();
}

u64 host_active_processor_mask() {
    return processor_topology::processor_mask();
}

SysInfoManager::SysInfoManager(const GuestMemory* memory)
    : m_memory(memory) {
    m_info.number_of_processors = host_active_processor_count();
    m_info.active_processor_mask = host_active_processor_mask();
}

SystemInfo SysInfoManager::get_system_info() const noexcept {
    SystemInfo info = m_info;
    if (m_memory != nullptr && m_memory->maximum_user_address() != 0) {
        info.max_app_address = std::min(
            info.max_app_address,
            static_cast<u64>(m_memory->maximum_user_address()));
    }
    return info;
}

bool SysInfoManager::get_version_ex(u32 info_size, OsVersionInfo& out) const {
    constexpr u32 os_version_info_a_size = 148;
    constexpr u32 os_version_info_ex_a_size = 156;
    constexpr u32 os_version_info_w_size = 276;
    constexpr u32 os_version_info_ex_w_size = 284;
    if (info_size != os_version_info_a_size &&
        info_size != os_version_info_ex_a_size &&
        info_size != os_version_info_w_size &&
        info_size != os_version_info_ex_w_size) {
        return false;
    }
    if (m_windows_10_compatible) {
        out.major_version = pe::kOsMajorVersion;
        out.minor_version = pe::kOsMinorVersion;
        out.build_number = pe::kOsBuildNumber;
    } else if (m_windows_81_compatible) {
        // Manifested for Windows 8.1 only → 6.3.9600 (Learn targeting page).
        out.major_version = 6;
        out.minor_version = 3;
        out.build_number = 9600;
    } else {
        out.major_version = 6;
        out.minor_version = 2;
        out.build_number = 9200;
    }
    out.platform_id = 2;  // VER_PLATFORM_WIN32_NT
    return true;
}

u64 SysInfoManager::get_system_time_as_filetime() const {
    auto now = std::chrono::system_clock::now();
    auto se = now.time_since_epoch();
    auto hns = std::chrono::duration_cast<
        std::chrono::duration<u64, std::ratio<1, 10000000>>>(se);
    return hns.count() + EPOCH_OFFSET;
}

u64 SysInfoManager::query_performance_counter() const {
    return virtual_qpc_ticks();
}

u64 SysInfoManager::query_performance_frequency() const {
    return kQpcHz;
}

u64 SysInfoManager::query_unbiased_interrupt_time_precise() const {
    mach_timebase_info_data_t info{};
    (void)mach_timebase_info(&info);
    const unsigned __int128 nanoseconds =
        static_cast<unsigned __int128>(mach_absolute_time()) * info.numer /
        info.denom;
    return static_cast<u64>(nanoseconds / 100);
}

// Learn: processthreadsapi/nf-processthreadsapi-isprocessorfeaturepresent.
// Every x86/x64 answer is derived from the CPUID the virtual CPU presents in
// the caller's execution mode, using the rules tools/oracle_probe/
// processor_features_probe.c verified on Windows 11 (x64 and WOW64; see its
// .expected files): a feature bit, gated on XCR0 for AVX-state features.
bool processor_feature_present_for_cpu(u32 feature, bool long_mode) noexcept {
    u32 a = 0, b = 0, c1ecx = 0, c1edx = 0, c7ebx = 0, c7ecx = 0;
    u32 e1ecx = 0, e1edx = 0, xcr0 = 0;
    cpu::ops::synth_cpuid(1, 0, long_mode, a, b, c1ecx, c1edx);
    cpu::ops::synth_cpuid(7, 0, long_mode, a, c7ebx, c7ecx, b);
    cpu::ops::synth_cpuid(0x80000001, 0, long_mode, a, b, e1ecx, e1edx);
    // Vitreo's "OS" enables every XSAVE component the CPU supports: XGETBV
    // returns CpuState::xcr0, which equals CPUID.(EAX=0DH,ECX=0):EAX.
    u32 d = 0;
    cpu::ops::synth_cpuid(0x0D, 0, long_mode, xcr0, a, b, d);
    const auto bit = [](u32 r, u32 n) { return ((r >> n) & 1u) != 0; };
    const bool osxsave = bit(c1ecx, 27);
    const bool avx_os = bit(c1ecx, 28) && osxsave && (xcr0 & 6u) == 6u;
    switch (feature) {
    case pf::PF_COMPARE_EXCHANGE_DOUBLE:        return bit(c1edx, 8);
    case pf::PF_MMX_INSTRUCTIONS_AVAILABLE:     return bit(c1edx, 23);
    case pf::PF_XMMI_INSTRUCTIONS_AVAILABLE:    return bit(c1edx, 25);
    case pf::PF_3DNOW_INSTRUCTIONS_AVAILABLE:   return bit(e1edx, 31);
    case pf::PF_RDTSC_INSTRUCTION_AVAILABLE:    return bit(c1edx, 4);
    case pf::PF_XMMI64_INSTRUCTIONS_AVAILABLE:  return bit(c1edx, 26);
    case pf::PF_SSE3_INSTRUCTIONS_AVAILABLE:    return bit(c1ecx, 0);
    case pf::PF_COMPARE_EXCHANGE128:            return bit(c1ecx, 13);
    case pf::PF_XSAVE_ENABLED:                  return bit(c1ecx, 26) && osxsave;
    case pf::PF_RDWRFSGSBASE_AVAILABLE:         return bit(c7ebx, 0);
    case pf::PF_RDRAND_INSTRUCTION_AVAILABLE:   return bit(c1ecx, 30);
    case pf::PF_RDTSCP_INSTRUCTION_AVAILABLE:   return bit(e1edx, 27);
    case pf::PF_RDPID_INSTRUCTION_AVAILABLE:    return bit(c7ecx, 22);
    case pf::PF_MONITORX_INSTRUCTION_AVAILABLE: return bit(e1ecx, 29);
    case pf::PF_SSSE3_INSTRUCTIONS_AVAILABLE:   return bit(c1ecx, 9);
    case pf::PF_SSE4_1_INSTRUCTIONS_AVAILABLE:  return bit(c1ecx, 19);
    case pf::PF_SSE4_2_INSTRUCTIONS_AVAILABLE:  return bit(c1ecx, 20);
    case pf::PF_AVX_INSTRUCTIONS_AVAILABLE:     return avx_os;
    case pf::PF_AVX2_INSTRUCTIONS_AVAILABLE:    return avx_os && bit(c7ebx, 5);
    case pf::PF_AVX512F_INSTRUCTIONS_AVAILABLE:
        return avx_os && bit(c7ebx, 16) && (xcr0 & 0xE6u) == 0xE6u;
    case pf::PF_ERMS_AVAILABLE:                 return bit(c7ebx, 9);
    case pf::PF_BMI2_INSTRUCTIONS_AVAILABLE:    return bit(c7ebx, 8);
    case pf::PF_MOVDIR64B_INSTRUCTION_AVAILABLE: return bit(c7ecx, 28);
    // OS state, TRUE on every Windows 10/11 installation in both bitnesses
    // (probe .expected: PF 9 / PF 12 value=1). _fastfail is INT 0x29, which
    // the run loop dispatches (guest_run_loop.cpp dump_fastfail_context).
    case pf::PF_PAE_ENABLED:
    case pf::PF_NX_ENABLED:
    case pf::PF_FASTFAIL_AVAILABLE:
        return true;
    // Learn: "Second Level Address Translation is supported by the hardware"
    // / "Virtualization is enabled in the firmware and made available by the
    // operating system". The virtual CPU exposes no VMX (CPUID.1:ECX[5]).
    case pf::PF_SECOND_LEVEL_ADDRESS_TRANSLATION:
    case pf::PF_VIRT_FIRMWARE_ENABLED:
        return bit(c1ecx, 5);
    // PF_SSE_DAZ_MODE_AVAILABLE is FALSE on x64 Windows and WOW64 even on a
    // DAZ-capable CPU (probe .expected: PF 11 value=0). Every other index is
    // an ARM/Alpha/PPC/Itanium feature or undefined: FALSE on x86/x64.
    default:
        return false;
    }
}

bool SysInfoManager::is_processor_feature_present(u32 feature,
                                                  bool long_mode) const {
    return processor_feature_present_for_cpu(feature, long_mode);
}

const std::string& SysInfoManager::get_computer_name() const noexcept {
    return m_computer_name;
}

void SysInfoManager::set_computer_name(std::string name) {
    if (name.size() > MAX_COMPUTERNAME_LENGTH) {
        name.resize(MAX_COMPUTERNAME_LENGTH);
    }
    m_computer_name = std::move(name);
}

bool SysInfoManager::get_computer_name_a(GuestAddr buffer, GuestAddr size_ptr, Marshaler& m) const {
    if (buffer == 0 || size_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    auto size_result = m.read_u32(size_ptr);
    if (!size_result.is_ok()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    u32 buf_size = size_result.value();
    u32 name_len = static_cast<u32>(m_computer_name.size());

    if (buf_size < name_len + 1) {
        (void)m.write_u32(size_ptr, name_len + 1);
        set_last_error(error_codes::ERROR_BUFFER_OVERFLOW);
        return false;
    }

    auto wr = m.write_ansi_string(buffer, m_computer_name);
    if (wr.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    (void)m.write_u32(size_ptr, name_len);
    return true;
}

bool SysInfoManager::get_computer_name_w(GuestAddr buffer, GuestAddr size_ptr, Marshaler& m) const {
    if (buffer == 0 || size_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    auto size_result = m.read_u32(size_ptr);
    if (!size_result.is_ok()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    u32 buf_size = size_result.value();
    u32 name_len = static_cast<u32>(m_computer_name.size());

    if (buf_size < name_len + 1) {
        (void)m.write_u32(size_ptr, name_len + 1);
        set_last_error(error_codes::ERROR_BUFFER_OVERFLOW);
        return false;
    }

    auto wr = m.write_wide_string(buffer, m_computer_name);
    if (wr.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    (void)m.write_u32(size_ptr, name_len);
    return true;
}


// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

void marshal_system_info(GuestAddr p, const SystemInfo& info, Marshaler& m);

// Vitreo models 64-bit Windows. 32-bit guests are WOW64 processes: GetSystemInfo
// reports the 32-bit environment; GetNativeSystemInfo / IsWow64Process* report
// the native AMD64 host. Learn: GetSystemInfo / IsWow64Process / IsWow64Process2.
constexpr u16 kProcessorArchitectureIntel = 0;
constexpr u32 kProcessorIntelPentium = 586;
constexpr u16 kImageFileMachineUnknown = 0;
constexpr u16 kImageFileMachineI386 = 0x014c;
constexpr u16 kImageFileMachineAmd64 = 0x8664;

void impl_GetSystemInfo(Marshaler& m, SysInfoManager& mgr) {
    SystemInfo info = mgr.get_system_info();
    if (!m.cpu_is_64bit()) {
        info.processor_architecture = kProcessorArchitectureIntel;
        info.processor_type = kProcessorIntelPentium;
        // WOW64 non-LAA ceiling is 0x7FFEFFFF; LAA guests use the reservation
        // ceiling from GuestMemory (up to ~4 GB - 64 KiB).
        if (info.max_app_address == 0 ||
            info.max_app_address > 0xFFFEFFFFull) {
            info.max_app_address = 0x7FFEFFFFull;
        }
    }
    marshal_system_info(m.arg_ptr(0), info, m);
}

void impl_GetVersionExW(Marshaler& m, SysInfoManager& mgr) {
    constexpr u32 os_version_info_w_size = 276;
    constexpr u32 os_version_info_ex_w_size = 284;
    GuestAddr info_ptr = m.arg_ptr(0);
    if (info_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    auto size_result = m.read_u32(info_ptr);
    u32 info_size = size_result.is_ok() ? size_result.value() : 0;
    OsVersionInfo ver;
    bool ok = (info_size == os_version_info_w_size ||
               info_size == os_version_info_ex_w_size) &&
              mgr.get_version_ex(info_size, ver);
    if (!ok) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    std::array<Byte, os_version_info_ex_w_size - 4> zeros{};
    if (m.memory().write_bytes(info_ptr + 4, zeros.data(), info_size - 4).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    (void)m.write_u32(info_ptr + 4,  ver.major_version);
    (void)m.write_u32(info_ptr + 8,  ver.minor_version);
    (void)m.write_u32(info_ptr + 12, ver.build_number);
    (void)m.write_u32(info_ptr + 16, ver.platform_id);
    if (info_size == os_version_info_ex_w_size) {
        (void)m.write_u16(info_ptr + 280, 0x0100);
        (void)m.write_u8(info_ptr + 282, 1);
    }
    m.set_return(1u);
}

void impl_GetVersionExA(Marshaler& m, SysInfoManager& mgr) {
    constexpr u32 os_version_info_a_size = 148;
    constexpr u32 os_version_info_ex_a_size = 156;
    GuestAddr info_ptr = m.arg_ptr(0);
    if (info_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    auto size_result = m.read_u32(info_ptr);
    u32 info_size = size_result.is_ok() ? size_result.value() : 0;
    OsVersionInfo ver;
    bool ok = (info_size == os_version_info_a_size ||
               info_size == os_version_info_ex_a_size) &&
              mgr.get_version_ex(info_size, ver);
    if (!ok) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    std::array<Byte, os_version_info_ex_a_size - 4> zeros{};
    if (m.memory().write_bytes(info_ptr + 4, zeros.data(), info_size - 4).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    (void)m.write_u32(info_ptr + 4,  ver.major_version);
    (void)m.write_u32(info_ptr + 8,  ver.minor_version);
    (void)m.write_u32(info_ptr + 12, ver.build_number);
    (void)m.write_u32(info_ptr + 16, ver.platform_id);
    if (info_size == os_version_info_ex_a_size) {
        (void)m.write_u16(info_ptr + 152, 0x0100);
        (void)m.write_u8(info_ptr + 154, 1);
    }
    m.set_return(1u);
}

void impl_GetSystemTimeAsFileTime(Marshaler& m, SysInfoManager& mgr) {
    GuestAddr ft_ptr = m.arg_ptr(0);
    u64 ft = mgr.get_system_time_as_filetime();
    (void)m.write_u64(ft_ptr, ft);
}

void impl_QueryPerformanceCounter(Marshaler& m, SysInfoManager& mgr) {
    perf::count(perf::Counter::k32_query_perf_counter);
    GuestAddr counter_ptr = m.arg_ptr(0);
    u64 count = mgr.query_performance_counter();
    (void)m.write_u64(counter_ptr, count);
    time_diag_observe(TimeApi::Qpc, count);

    // A tight polling loop can call QPC millions of times per second.  Log each
    // guest call site once per thread so traces retain the evidence needed to
    // identify the loop without growing without bound. Caller reconstruction is
    // diagnostic work, not part of the Windows contract, so skip every guest
    // stack read and lookup when tracing is disabled.
    static const bool s_qpc_diag =
        std::getenv("VITREO_QPC_DIAG") != nullptr;
    static const bool s_trace_time_callers =
        s_qpc_diag || std::getenv("VITREO_TRACE") != nullptr;
    if (!s_trace_time_callers) {
        m.set_return(1);
        return;
    }

    u64 caller_ret = 0;
    u64 frame1_ret = 0;
    u64 frame2_ret = 0;
    if (auto* cpu = m.cpu()) {
        if (cpu->is_64bit()) {
            auto ret_r = m.memory().read_u64(cpu->sp());
            if (ret_r.is_ok()) caller_ret = ret_r.value();

            const GuestAddr frame = cpu->gpr.rbp;
            auto frame_ret_r = m.memory().read_u64(frame + 8);
            if (frame_ret_r.is_ok()) frame1_ret = frame_ret_r.value();
            auto previous_frame_r = m.memory().read_u64(frame);
            if (previous_frame_r.is_ok()) {
                auto previous_ret_r =
                    m.memory().read_u64(previous_frame_r.value() + 8);
                if (previous_ret_r.is_ok()) frame2_ret = previous_ret_r.value();
            }
        } else {
            auto ret_r = m.memory().read_u32(cpu->sp());
            if (ret_r.is_ok()) caller_ret = static_cast<u64>(ret_r.value());

            const GuestAddr frame = static_cast<u32>(cpu->gpr.rbp);
            auto frame_ret_r = m.memory().read_u32(frame + 4);
            if (frame_ret_r.is_ok()) {
                frame1_ret = static_cast<u64>(frame_ret_r.value());
            }
            auto previous_frame_r = m.memory().read_u32(frame);
            if (previous_frame_r.is_ok()) {
                auto previous_ret_r =
                    m.memory().read_u32(previous_frame_r.value() + 4);
                if (previous_ret_r.is_ok()) {
                    frame2_ret = static_cast<u64>(previous_ret_r.value());
                }
            }
        }
    }

    static thread_local u64 seen_callers[64]{};
    static thread_local std::size_t seen_caller_count = 0;
    bool seen = false;
    for (std::size_t i = 0; i < seen_caller_count; ++i) {
        if (seen_callers[i] == caller_ret) {
            seen = true;
            break;
        }
    }
    if (!seen && seen_caller_count < 64) {
        seen_callers[seen_caller_count++] = caller_ret;
        if (s_qpc_diag) {
            std::fprintf(
                stderr,
                "[qpc-diag] caller_ret=0x%llx frame1_ret=0x%llx "
                "frame2_ret=0x%llx -> %llu\n",
                static_cast<unsigned long long>(caller_ret),
                static_cast<unsigned long long>(frame1_ret),
                static_cast<unsigned long long>(frame2_ret),
                static_cast<unsigned long long>(count));
        } else {
            VITREO_TRACE(
                "[time] QueryPerformanceCounter caller_ret=0x%llx "
                "frame1_ret=0x%llx frame2_ret=0x%llx -> %llu\n",
                static_cast<unsigned long long>(caller_ret),
                static_cast<unsigned long long>(frame1_ret),
                static_cast<unsigned long long>(frame2_ret),
                static_cast<unsigned long long>(count));
        }
    }
    m.set_return(1);
}

void impl_QueryPerformanceFrequency(Marshaler& m, SysInfoManager& mgr) {
    GuestAddr freq_ptr = m.arg_ptr(0);
    u64 freq = mgr.query_performance_frequency();
    (void)m.write_u64(freq_ptr, freq);
    VITREO_TRACE("[time] QueryPerformanceFrequency -> %llu\n",
                 (unsigned long long)freq);
    m.set_return(1);
}

void impl_QueryUnbiasedInterruptTimePrecise(
    Marshaler& m, SysInfoManager& mgr) {
    const GuestAddr time_ptr = m.arg_ptr(0);
    const u64 time_100ns = mgr.query_unbiased_interrupt_time_precise();
    (void)m.write_u64(time_ptr, time_100ns);
    VITREO_TRACE(
        "[time] QueryUnbiasedInterruptTimePrecise -> %llu (100ns)\n",
        static_cast<unsigned long long>(time_100ns));
}

// Learn realtimeapiset QueryInterruptTimePrecise / QueryInterruptTime:
// VOID fn(PULONGLONG), interrupt time in 100 ns units. Interrupt time, unlike
// the *Unbiased* clock, keeps counting across sleep: mach_continuous_time,
// the clock GetTickCount64 already reads. The coarse variant is the
// tick-granular value KUSER_SHARED_DATA.InterruptTime holds; measured on
// Windows 11 (s4_interrupt_time_probe): coarse / 10000 tracks GetTickCount64,
// precise - coarse stays under one tick, precise >= unbiased precise.
// Both are KernelBase exports; the API-set fallback routes
// api-ms-win-core-realtime-* to kernel32, so they are registered here and
// reach KernelBase through its shared surface.
static u64 interrupt_time_precise_100ns() {
    static const mach_timebase_info_data_t info = [] {
        mach_timebase_info_data_t i{};
        (void)mach_timebase_info(&i);
        return i;
    }();
    const unsigned __int128 ns =
        static_cast<unsigned __int128>(mach_continuous_time()) * info.numer /
        info.denom;
    return static_cast<u64>(ns / 100);
}

static void impl_QueryInterruptTimePrecise(Marshaler& m) {
    (void)m.write_u64(m.arg_ptr(0), interrupt_time_precise_100ns());
}

static void impl_QueryInterruptTime(Marshaler& m) {
    (void)m.write_u64(m.arg_ptr(0), tick_count_ms() * 10000ULL);
}

void impl_QueryUnbiasedInterruptTime(Marshaler& m, SysInfoManager& mgr) {
    // MS Learn: BOOL QueryUnbiasedInterruptTime(PULONGLONG UnbiasedTime)
    // Same 100ns interrupt-time clock as the Precise variant (excluding sleep).
    // A missing-export fail-thunk returns 0xFFFFFFFF and leaves
    // *UnbiasedTime untouched, which breaks timeouts.
    const GuestAddr time_ptr = m.arg_ptr(0);
    if (time_ptr == 0) {
        m.set_return(0);
        return;
    }
    const u64 time_100ns = mgr.query_unbiased_interrupt_time_precise();
    if (m.write_u64(time_ptr, time_100ns).is_err()) {
        m.set_return(0);
        return;
    }
    if (std::getenv("VITREO_WIN_DIAG")) {
        std::fprintf(stderr,
                     "[time] QueryUnbiasedInterruptTime -> %llu (100ns)\n",
                     static_cast<unsigned long long>(time_100ns));
    }
    VITREO_TRACE(
        "[time] QueryUnbiasedInterruptTime -> %llu (100ns)\n",
        static_cast<unsigned long long>(time_100ns));
    m.set_return(1);
}

void impl_IsProcessorFeaturePresent(Marshaler& m, SysInfoManager& mgr) {
    // DWORD ProcessorFeature: only the low 32 bits are defined on x64.
    const u32 feature = m.arg32(0);
    const bool present =
        mgr.is_processor_feature_present(feature, m.cpu_is_64bit());
    m.set_return(present ? 1u : 0u);
}

void impl_IsWow64Process(Marshaler& m) {
    // BOOL IsWow64Process(HANDLE hProcess, PBOOL Wow64Process)
    // TRUE when the target is a 32-bit process on 64-bit Windows (WOW64).
    // Learn: wow64apiset IsWow64Process. Target handle is currently the caller;
    // installers query GetCurrentProcess.
    (void)m.arg_ptr(0);
    GuestAddr out_ptr = m.arg_ptr(1);
    if (out_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const u32 wow64 = m.cpu_is_64bit() ? 0u : 1u;
    (void)m.write_u32(out_ptr, wow64);
    VITREO_TRACE("[sysinfo] IsWow64Process -> %u\n", wow64);
    m.set_return(1);
}

void impl_IsWow64Process2(Marshaler& m) {
    // BOOL IsWow64Process2(HANDLE hProcess, USHORT* pProcessMachine,
    //                      USHORT* pNativeMachine)
    // Non-WOW64: process=IMAGE_FILE_MACHINE_UNKNOWN. WOW64: process machine of
    // the guest. Native is always AMD64 on this runtime.
    (void)m.arg_ptr(0);
    GuestAddr proc_machine = m.arg_ptr(1);
    GuestAddr native_machine = m.arg_ptr(2);
    if (proc_machine == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const bool wow64 = !m.cpu_is_64bit();
    (void)m.write_u16(proc_machine,
                      wow64 ? kImageFileMachineI386 : kImageFileMachineUnknown);
    if (native_machine != 0) {
        (void)m.write_u16(native_machine, kImageFileMachineAmd64);
    }
    VITREO_TRACE("[sysinfo] IsWow64Process2 -> proc=%s native=AMD64\n",
                 wow64 ? "I386" : "UNKNOWN");
    m.set_return(1);
}

void impl_GetComputerNameA(Marshaler& m, SysInfoManager& mgr) {
    GuestAddr buffer = m.arg_ptr(0);
    GuestAddr size_ptr = m.arg_ptr(1);
    bool ok = mgr.get_computer_name_a(buffer, size_ptr, m);
    m.set_return(ok ? 1 : 0);
}

void impl_GetComputerNameW(Marshaler& m, SysInfoManager& mgr) {
    GuestAddr buffer = m.arg_ptr(0);
    GuestAddr size_ptr = m.arg_ptr(1);
    bool ok = mgr.get_computer_name_w(buffer, size_ptr, m);
    m.set_return(ok ? 1 : 0);
}

void impl_GlobalMemoryStatusEx(Marshaler& m, SysInfoManager& mgr) {
    GuestAddr buf = m.arg_ptr(0);
    bool ok = mgr.global_memory_status_ex(buf, m);
    m.set_return(ok ? 1u : 0u);
}

void impl_GetNativeSystemInfo(Marshaler& m, SysInfoManager& mgr) {
    GuestAddr info_ptr = m.arg_ptr(0);
    mgr.get_native_system_info(info_ptr, m);
}

void impl_GetDiskFreeSpaceExW(Marshaler& m, SysInfoManager& mgr) {
    GuestAddr dir_addr    = m.arg_ptr(0);
    GuestAddr free_caller = m.arg_ptr(1);
    GuestAddr total_bytes = m.arg_ptr(2);
    GuestAddr total_free  = m.arg_ptr(3);
    std::string dir = dir_addr ? m.read_wide_string(dir_addr).value_or("") : "";
    bool ok = mgr.get_disk_free_space_ex_w(dir, free_caller, total_bytes, total_free, m);
    m.set_return(ok ? 1u : 0u);
}

void impl_GetVolumeInformationW(Marshaler& m, SysInfoManager& mgr) {
    GuestAddr root         = m.arg_ptr(0);
    GuestAddr vol_name_buf = m.arg_ptr(1);
    u32 vol_name_size      = static_cast<u32>(m.arg(2));
    GuestAddr serial       = m.arg_ptr(3);
    GuestAddr max_comp_len = m.arg_ptr(4);
    GuestAddr flags        = m.arg_ptr(5);
    GuestAddr fs_name_buf  = m.arg_ptr(6);
    u32 fs_name_size       = static_cast<u32>(m.arg(7));
    std::string root_path = root ? m.read_wide_string(root).value_or("") : "";
    VITREO_TRACE("[sysinfo] GetVolumeInformationW(%s)\n", root_path.c_str());
    bool ok = mgr.get_volume_information_w(root, vol_name_buf, vol_name_size, serial,
                                            max_comp_len, flags, fs_name_buf, fs_name_size, m);
    m.set_return(ok ? 1u : 0u);
}

void impl_GetDriveTypeW(Marshaler& m, SysInfoManager& mgr) {
    GuestAddr path_addr = m.arg_ptr(0);
    std::string path = path_addr ? m.read_wide_string(path_addr).value_or("") : "";
    u32 dtype = mgr.get_drive_type_w(path);
    m.set_return(dtype);
}

void impl_GetLogicalDrives(Marshaler& m, SysInfoManager& mgr) {
    u32 mask = mgr.get_logical_drives();
    m.set_return(mask);
}

void impl_GetLogicalDriveStringsW(Marshaler& m, SysInfoManager& mgr) {
    u32 buf_len    = static_cast<u32>(m.arg(0));
    GuestAddr buf  = m.arg_ptr(1);
    u32 result = mgr.get_logical_drive_strings_w(buf_len, buf, m);
    m.set_return(result);
}

void impl_GetSystemFirmwareTable(Marshaler& m) {
    // UINT GetSystemFirmwareTable(DWORD FirmwareTableProviderSignature,
    //   DWORD FirmwareTableID, PVOID pFirmwareTableBuffer, DWORD BufferSize)
    // Measured (tools/oracle_probe/firmware_table_probe, x86 = x64):
    // - 'RSMB' ignores FirmwareTableID;
    // - a NULL or short buffer returns the required size with
    //   ERROR_INSUFFICIENT_BUFFER;
    // - success returns the size written with the last error 0;
    // - an unavailable provider returns 0 with ERROR_INVALID_FUNCTION.
    //   Vitreo has no ACPI tables yet, so 'ACPI' is one.
    constexpr u32 kRawSmbiosProvider = 0x52534D42;  // 'RSMB'
    constexpr u32 kRawSmbiosHeaderSize = 8;
    const u32 provider = static_cast<u32>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(2);
    const u32 buffer_size = static_cast<u32>(m.arg(3));

    if (provider != kRawSmbiosProvider) {
        set_last_error(error_codes::ERROR_INVALID_FUNCTION);
        m.set_return(0);
        return;
    }

    const auto table = virtual_firmware::smbios_table();
    const u32 required =
        kRawSmbiosHeaderSize + static_cast<u32>(table.size());
    if (buffer == 0 || buffer_size < required) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(required);
        return;
    }

    // RawSMBIOSData: Used20CallingMethod, SMBIOS major/minor version,
    // DmiRevision (0 as measured), table length.
    const u8 header[kRawSmbiosHeaderSize]{
        0,
        virtual_firmware::kSmbiosMajor, virtual_firmware::kSmbiosMinor,
        0,
        static_cast<u8>(table.size()),
        static_cast<u8>(table.size() >> 8),
        static_cast<u8>(table.size() >> 16),
        static_cast<u8>(table.size() >> 24),
    };
    const auto header_write = m.memory().write_bytes(
        buffer, header, sizeof(header));
    const auto table_write = m.memory().write_bytes(
        buffer + kRawSmbiosHeaderSize, table.data(), table.size());
    if (header_write.is_err() || table_write.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    VITREO_TRACE(
        "[sysinfo] GetSystemFirmwareTable(RSMB) -> %u bytes\n",
        required);
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(required);
}

void impl_EnumSystemFirmwareTables(Marshaler& m) {
    // UINT EnumSystemFirmwareTables(DWORD FirmwareTableProviderSignature,
    //   PVOID pFirmwareTableEnumBuffer, DWORD BufferSize)
    // Measured (firmware_table_probe "enum"): 'RSMB' lists one table, ID 0;
    // a NULL or short buffer returns the required size with
    // ERROR_INSUFFICIENT_BUFFER, success sets the last error to 0, and an
    // unavailable provider returns 0 with ERROR_INVALID_FUNCTION.
    constexpr u32 kRawSmbiosProvider = 0x52534D42;  // 'RSMB'
    const u32 provider = static_cast<u32>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 buffer_size = static_cast<u32>(m.arg(2));
    if (provider != kRawSmbiosProvider) {
        set_last_error(error_codes::ERROR_INVALID_FUNCTION);
        m.set_return(0);
        return;
    }
    constexpr u32 required = 4;
    if (buffer == 0 || buffer_size < required) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(required);
        return;
    }
    if (m.write_u32(buffer, 0).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(required);
}

// =============================================================================
// Export Table Factory
// =============================================================================

// =============================================================================
// WOW64 filesystem redirection
//
// A 32-bit process reaches %windir%\SysWOW64 through System32 (capsule::Vfs
// wow64_redirect); these three APIs switch that off and on for the calling
// thread. Measured (tests/winapi/kernel32/oracle/wow64_fsredir_probe.c):
// Disable stores the previous state (0 enabled, 1 disabled) in *OldValue and
// Revert restores the state it is given, both leaving the last error alone;
// Disable(NULL) is ERROR_NOACCESS. Enable(FALSE) disables and Enable(TRUE)
// re-enables, returning TRUE. A 64-bit process has nothing to redirect: all
// three return FALSE with ERROR_INVALID_FUNCTION.
// =============================================================================

namespace {

[[nodiscard]] bool refuse_in_64bit(Marshaler& m) {
    if (!m.cpu_is_64bit()) return false;
    set_last_error(error_codes::ERROR_INVALID_FUNCTION);
    m.set_return(0);
    return true;
}

}  // namespace

/// BOOL Wow64DisableWow64FsRedirection(PVOID *OldValue)
static void impl_Wow64DisableWow64FsRedirection(Marshaler& m) {
    const GuestAddr old_value = m.arg_ptr(0);
    if (refuse_in_64bit(m)) return;
    if (old_value == 0) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    (void)m.write_ptr(old_value, capsule::Vfs::thread_redirection_disabled() ? 1 : 0);
    capsule::Vfs::set_thread_redirection_disabled(true);
    VITREO_TRACE("[sysinfo] Wow64DisableWow64FsRedirection -> TRUE\n");
    m.set_return(1);
}

/// BOOL Wow64RevertWow64FsRedirection(PVOID OldValue)
static void impl_Wow64RevertWow64FsRedirection(Marshaler& m) {
    const u64 old_value = m.arg(0);
    if (refuse_in_64bit(m)) return;
    capsule::Vfs::set_thread_redirection_disabled(old_value != 0);
    VITREO_TRACE("[sysinfo] Wow64RevertWow64FsRedirection -> TRUE\n");
    m.set_return(1);
}

/// BOOLEAN Wow64EnableWow64FsRedirection(BOOLEAN Wow64FsEnableRedirection)
static void impl_Wow64EnableWow64FsRedirection(Marshaler& m) {
    const bool enable = (m.arg(0) & 0xFFu) != 0;
    if (refuse_in_64bit(m)) return;
    capsule::Vfs::set_thread_redirection_disabled(!enable);
    VITREO_TRACE("[sysinfo] Wow64EnableWow64FsRedirection(%d) -> TRUE\n", enable ? 1 : 0);
    m.set_return(1);
}

DllExports create_sysinfo_thunks(SysInfoManager& mgr) {
    DllExports exports;
    exports.entries.reserve(20);

    // GetSystemInfo (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GetSystemInfo", 1,
        [&mgr](Marshaler& m) { impl_GetSystemInfo(m, mgr); },
        4
    });
    // GetVersionExW (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GetVersionExW", 2,
        [&mgr](Marshaler& m) { impl_GetVersionExW(m, mgr); },
        4
    });
    // GetVersionExA (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GetVersionExA", 0,
        [&mgr](Marshaler& m) { impl_GetVersionExA(m, mgr); },
        4
    });
    // GetSystemTimeAsFileTime (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GetSystemTimeAsFileTime", 3,
        [&mgr](Marshaler& m) { impl_GetSystemTimeAsFileTime(m, mgr); },
        4
    });
    // QueryPerformanceCounter (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "QueryPerformanceCounter", 4,
        [&mgr](Marshaler& m) { impl_QueryPerformanceCounter(m, mgr); },
        4
    });
    // QueryPerformanceFrequency (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "QueryPerformanceFrequency", 5,
        [&mgr](Marshaler& m) { impl_QueryPerformanceFrequency(m, mgr); },
        4
    });
    // QueryUnbiasedInterruptTimePrecise (1 arg, VOID, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "QueryUnbiasedInterruptTimePrecise", 0,
        [&mgr](Marshaler& m) {
            impl_QueryUnbiasedInterruptTimePrecise(m, mgr);
        },
        4
    });
    // QueryInterruptTime / QueryInterruptTimePrecise (1 arg, VOID, 4 bytes)
    exports.entries.push_back(ExportEntry{
        "QueryInterruptTime", 0,
        [](Marshaler& m) { impl_QueryInterruptTime(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "QueryInterruptTimePrecise", 0,
        [](Marshaler& m) { impl_QueryInterruptTimePrecise(m); },
        4
    });
    // QueryUnbiasedInterruptTime (1 arg, BOOL, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "QueryUnbiasedInterruptTime", 0,
        [&mgr](Marshaler& m) { impl_QueryUnbiasedInterruptTime(m, mgr); },
        4
    });
    // IsProcessorFeaturePresent (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "IsProcessorFeaturePresent", 6,
        [&mgr](Marshaler& m) { impl_IsProcessorFeaturePresent(m, mgr); },
        4
    });
    // GetComputerNameA (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetComputerNameA", 7,
        [&mgr](Marshaler& m) { impl_GetComputerNameA(m, mgr); },
        8
    });
    // GetComputerNameW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetComputerNameW", 8,
        [&mgr](Marshaler& m) { impl_GetComputerNameW(m, mgr); },
        8
    });
    // GlobalMemoryStatusEx lives in kernel32_ext (memory_apis.cpp) — do not
    // register a second older copy here (audit 01 GMSEx #2).
    // GetNativeSystemInfo (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GetNativeSystemInfo", 10,
        [&mgr](Marshaler& m) { impl_GetNativeSystemInfo(m, mgr); },
        4
    });
    // GetDiskFreeSpaceExW (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "GetDiskFreeSpaceExW", 11,
        [&mgr](Marshaler& m) { impl_GetDiskFreeSpaceExW(m, mgr); },
        16
    });
    // GetVolumeInformationW (8 args, stdcall_bytes=32)
    exports.entries.push_back(ExportEntry{
        "GetVolumeInformationW", 12,
        [&mgr](Marshaler& m) { impl_GetVolumeInformationW(m, mgr); },
        32
    });
    // GetDriveTypeW (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GetDriveTypeW", 13,
        [&mgr](Marshaler& m) { impl_GetDriveTypeW(m, mgr); },
        4
    });
    // GetLogicalDrives (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetLogicalDrives", 14,
        [&mgr](Marshaler& m) { impl_GetLogicalDrives(m, mgr); },
        0
    });
    // GetLogicalDriveStringsW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetLogicalDriveStringsW", 15,
        [&mgr](Marshaler& m) { impl_GetLogicalDriveStringsW(m, mgr); },
        8
    });
    // IsWow64Process (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "IsWow64Process", 16,
        [](Marshaler& m) { impl_IsWow64Process(m); },
        8
    });
    // IsWow64Process2 (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "IsWow64Process2", 17,
        [](Marshaler& m) { impl_IsWow64Process2(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "GetSystemFirmwareTable", 18,
        [](Marshaler& m) { impl_GetSystemFirmwareTable(m); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "EnumSystemFirmwareTables", 0,
        [](Marshaler& m) { impl_EnumSystemFirmwareTables(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "Wow64DisableWow64FsRedirection", 19,
        [](Marshaler& m) { impl_Wow64DisableWow64FsRedirection(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "Wow64RevertWow64FsRedirection", 20,
        [](Marshaler& m) { impl_Wow64RevertWow64FsRedirection(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "Wow64EnableWow64FsRedirection", 21,
        [](Marshaler& m) { impl_Wow64EnableWow64FsRedirection(m); },
        4
    });

    return exports;
}

} // namespace vitreo::winapi
