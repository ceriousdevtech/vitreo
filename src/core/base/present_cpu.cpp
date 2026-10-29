// SPDX-License-Identifier: LGPL-2.1-only

/// @file present_cpu.cpp
/// @brief `[present-cpu]` meter. See vitreo/present_cpu.hpp.

#include <vitreo/present_cpu.hpp>

#include <vitreo/ab_cycle.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <mach/mach.h>
#include <pthread.h>

namespace vitreo::diagnostics::present_cpu {

namespace {

constexpr u64 kDefaultWindowNs = 1'000'000'000ull;
/// Above the dozens of named threads the guest starts. Names past this
/// cap are counted and dropped, never allocated.
constexpr usize kMaxThreads = 128;
constexpr usize kNameChars = 40;

struct NamedThread {
    u32 tid{0};
    mach_port_t port{MACH_PORT_NULL};  ///< our own send right, or NULL
    char name[kNameChars]{};
    u64 base_cpu_ns{0};                ///< reading at the window's start
    bool have_base{false};
};

struct Meter {
    std::mutex mu;
    NamedThread threads[kMaxThreads];
    usize count{0};
    u64 dropped{0};

    // The open window.
    bool open{false};
    const char* arm{"none"};
    u64 start_ns{0};
    u64 frames{0};
    u64 index{0};
    u64 first_ns{0};

    // The presenting thread, read at every present.
    mach_port_t present_port{MACH_PORT_NULL};
    u64 present_last_ns{0};
    bool present_have_last{false};
    u64 present_sum_ns{0};
    u64 present_max_ns{0};
    u64 present_frames{0};   ///< frames whose delta was measured

    std::FILE* out{nullptr};
    u64 window_ns{kDefaultWindowNs};
};

Meter& meter() {
    // Immortal: presents and thread names can arrive during static
    // destruction (a guest still running while exit() tears down).
    static Meter* m = new Meter();
    return *m;
}

std::atomic<int>& forced_enabled() {
    static std::atomic<int> value{0};
    return value;
}

u64 mono_ns() noexcept {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void copy_name(char (&dst)[kNameChars], std::u16string_view name) noexcept {
    usize n = 0;
    for (char16_t c : name) {
        if (n + 1 >= kNameChars) break;
        // ASCII only, so the line stays one parseable token; quotes would
        // end the field.
        char ch = (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
        if (ch == '"') ch = '\'';
        dst[n++] = ch;
    }
    dst[n] = '\0';
}

NamedThread* find_tid(Meter& m, u32 tid) noexcept {
    for (usize i = 0; i < m.count; ++i) {
        if (m.threads[i].tid == tid) return &m.threads[i];
    }
    return nullptr;
}

/// Replace @p entry's port with a new send right to @p port (or NULL).
void adopt_port(NamedThread& entry, mach_port_t port) noexcept {
    if (entry.port == port && port != MACH_PORT_NULL) return;
    if (entry.port != MACH_PORT_NULL) {
        (void)mach_port_deallocate(mach_task_self(), entry.port);
        entry.port = MACH_PORT_NULL;
    }
    entry.have_base = false;
    if (port == MACH_PORT_NULL) return;
    // Our own reference keeps the name from being recycled for another
    // thread after this one exits (it becomes a dead name instead).
    if (mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1) ==
        KERN_SUCCESS) {
        entry.port = port;
    }
}

const char* name_of_port(const Meter& m, mach_port_t port) noexcept {
    for (usize i = 0; i < m.count; ++i) {
        if (m.threads[i].port == port && port != MACH_PORT_NULL) {
            return m.threads[i].name;
        }
    }
    return "-";
}

u32 tid_of_port(const Meter& m, mach_port_t port) noexcept {
    for (usize i = 0; i < m.count; ++i) {
        if (m.threads[i].port == port && port != MACH_PORT_NULL) {
            return m.threads[i].tid;
        }
    }
    return 0;
}

/// Close the open window at @p now: one line for the window, one for the
/// presenting thread, one per named thread with a baseline. Then rebase every
/// named thread so the next window starts here.
void emit_and_rebase(Meter& m, u64 now) noexcept {
    std::FILE* out = m.out != nullptr ? m.out : stderr;
    const double dt_s = static_cast<double>(now - m.start_ns) / 1e9;
    const double t_s = static_cast<double>(now - m.first_ns) / 1e9;
    const double frames = static_cast<double>(m.frames);
    std::fprintf(out,
                 "[present-cpu] w=%llu arm=%s t=%.1fs dt=%.3fs frames=%llu "
                 "fps=%.1f t_ms=%.1f\n",
                 static_cast<unsigned long long>(m.index), m.arm, t_s, dt_s,
                 static_cast<unsigned long long>(m.frames),
                 dt_s > 0.0 ? frames / dt_s : 0.0,
                 static_cast<double>(now) / 1e6);
    if (m.present_frames != 0) {
        const double per_frame =
            static_cast<double>(m.present_sum_ns) / 1e6 /
            static_cast<double>(m.present_frames);
        std::fprintf(out,
                     "[present-cpu] w=%llu arm=%s thread=\"%s\" T%u "
                     "role=present cpu_ms_per_frame=%.2f max_frame_ms=%.2f "
                     "cpu_ms_per_s=%.1f\n",
                     static_cast<unsigned long long>(m.index), m.arm,
                     name_of_port(m, m.present_port),
                     tid_of_port(m, m.present_port), per_frame,
                     static_cast<double>(m.present_max_ns) / 1e6,
                     dt_s > 0.0 ? static_cast<double>(m.present_sum_ns) / 1e6 /
                                      dt_s
                                : 0.0);
    }
    for (usize i = 0; i < m.count; ++i) {
        NamedThread& t = m.threads[i];
        if (t.port == MACH_PORT_NULL) continue;
        u64 cpu = 0;
        if (!thread_cpu_ns(t.port, cpu)) {
            // The thread exited: the name is dead. Drop our right.
            adopt_port(t, MACH_PORT_NULL);
            continue;
        }
        if (t.have_base && frames > 0.0 && cpu >= t.base_cpu_ns) {
            const double ms = static_cast<double>(cpu - t.base_cpu_ns) / 1e6;
            std::fprintf(out,
                         "[present-cpu] w=%llu arm=%s thread=\"%s\" T%u "
                         "cpu_ms_per_frame=%.2f cpu_ms_per_s=%.1f\n",
                         static_cast<unsigned long long>(m.index), m.arm,
                         t.name, t.tid, ms / frames,
                         dt_s > 0.0 ? ms / dt_s : 0.0);
        }
        t.base_cpu_ns = cpu;
        t.have_base = true;
    }
}

void open_window(Meter& m, u64 now, const char* arm) noexcept {
    m.open = true;
    m.arm = arm;
    m.start_ns = now;
    m.frames = 0;
    m.present_sum_ns = 0;
    m.present_max_ns = 0;
    m.present_frames = 0;
}

}  // namespace

bool enabled() noexcept {
    if (forced_enabled().load(std::memory_order_relaxed) != 0) return true;
    static const bool on = std::getenv("VITREO_PRESENT_RATE") != nullptr;
    return on;
}

bool thread_cpu_ns(unsigned int mach_thread, u64& out) noexcept {
    if (mach_thread == MACH_PORT_NULL) return false;
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(static_cast<mach_port_t>(mach_thread), THREAD_BASIC_INFO,
                    reinterpret_cast<thread_info_t>(&info),
                    &count) != KERN_SUCCESS) {
        return false;
    }
    const u64 us =
        (static_cast<u64>(info.user_time.seconds) +
         static_cast<u64>(info.system_time.seconds)) * 1'000'000ull +
        static_cast<u64>(info.user_time.microseconds) +
        static_cast<u64>(info.system_time.microseconds);
    out = us * 1000ull;
    return true;
}

unsigned int current_thread_port() noexcept {
    return static_cast<unsigned int>(pthread_mach_thread_np(pthread_self()));
}

void note_present() noexcept {
    if (!enabled()) return;
    static thread_local const mach_port_t own =
        static_cast<mach_port_t>(current_thread_port());
    const auto steady_now = std::chrono::steady_clock::now();
    const char* arm = ab_cycle_arm_name(steady_now);
    Meter& m = meter();
    std::lock_guard<std::mutex> lock(m.mu);
    const u64 now = mono_ns();
    u64 cpu = 0;
    const bool have_cpu = thread_cpu_ns(own, cpu);
    if (!m.open) {
        if (m.first_ns == 0) m.first_ns = now;
        open_window(m, now, arm);
        // Baseline every named thread at the window's start.
        for (usize i = 0; i < m.count; ++i) {
            NamedThread& t = m.threads[i];
            u64 c = 0;
            t.have_base = t.port != MACH_PORT_NULL && thread_cpu_ns(t.port, c);
            t.base_cpu_ns = c;
        }
    } else {
        ++m.frames;
        if (have_cpu && m.present_have_last && m.present_port == own &&
            cpu >= m.present_last_ns) {
            const u64 d = cpu - m.present_last_ns;
            m.present_sum_ns += d;
            if (d > m.present_max_ns) m.present_max_ns = d;
            ++m.present_frames;
        }
    }
    m.present_port = own;
    m.present_last_ns = cpu;
    m.present_have_last = have_cpu;
    if (now - m.start_ns >= m.window_ns || std::strcmp(arm, m.arm) != 0) {
        if (m.frames != 0) {
            emit_and_rebase(m, now);
            ++m.index;
        }
        open_window(m, now, arm);
    }
}

void note_thread_name(u32 guest_tid, std::u16string_view name,
                      unsigned int mach_thread) noexcept {
    if (!enabled()) return;
    Meter& m = meter();
    std::lock_guard<std::mutex> lock(m.mu);
    NamedThread* entry = find_tid(m, guest_tid);
    if (entry == nullptr) {
        if (m.count == kMaxThreads) {
            if (m.dropped++ == 0) {
                std::fprintf(m.out != nullptr ? m.out : stderr,
                             "[present-cpu] %zu named threads: further names "
                             "are not metered\n",
                             kMaxThreads);
            }
            return;
        }
        entry = &m.threads[m.count++];
        *entry = NamedThread{};
        entry->tid = guest_tid;
    }
    copy_name(entry->name, name);
    if (mach_thread != MACH_PORT_NULL) {
        adopt_port(*entry, static_cast<mach_port_t>(mach_thread));
    }
}

bool thread_name(u32 guest_tid, char* out, usize capacity) noexcept {
    if (capacity == 0) return false;
    out[0] = '\0';
    if (!enabled()) return false;
    Meter& m = meter();
    std::lock_guard<std::mutex> lock(m.mu);
    const NamedThread* entry = find_tid(m, guest_tid);
    if (entry == nullptr) return false;
    std::snprintf(out, capacity, "%s", entry->name);
    return true;
}

void note_thread_port(u32 guest_tid) noexcept {
    if (!enabled()) return;
    Meter& m = meter();
    std::lock_guard<std::mutex> lock(m.mu);
    NamedThread* entry = find_tid(m, guest_tid);
    if (entry == nullptr) return;
    adopt_port(*entry, static_cast<mach_port_t>(current_thread_port()));
}

void configure_for_test(std::FILE* out, u64 window_ns) noexcept {
    forced_enabled().store(1, std::memory_order_relaxed);
    Meter& m = meter();
    std::lock_guard<std::mutex> lock(m.mu);
    m.out = out;
    m.window_ns = window_ns != 0 ? window_ns : kDefaultWindowNs;
}

void reset_for_test() noexcept {
    Meter& m = meter();
    std::lock_guard<std::mutex> lock(m.mu);
    for (usize i = 0; i < m.count; ++i) adopt_port(m.threads[i], MACH_PORT_NULL);
    m.count = 0;
    m.dropped = 0;
    m.open = false;
    m.frames = 0;
    m.index = 0;
    m.first_ns = 0;
    m.present_port = MACH_PORT_NULL;
    m.present_have_last = false;
    m.present_sum_ns = 0;
    m.present_max_ns = 0;
    m.present_frames = 0;
}

}  // namespace vitreo::diagnostics::present_cpu
