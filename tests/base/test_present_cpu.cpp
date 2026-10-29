// SPDX-License-Identifier: LGPL-2.1-only
//
// [present-cpu] meter (vitreo/present_cpu.hpp). The reference clock is the
// thread's own CLOCK_THREAD_CPUTIME_ID (libc), never the meter: a spin is
// driven until THAT clock has advanced N ms, and the meter must report at
// least N. The negative controls are a thread that sleeps for the same wall
// time, and a meter read of a different thread while this one spins.

#include <vitreo/present_cpu.hpp>
#include <vitreo/ab_cycle.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

namespace {

using namespace vitreo;
namespace pc = vitreo::diagnostics::present_cpu;

constexpr u64 kSpinNs = 30'000'000;      // 30 ms of this thread's own CPU
/// THREAD_BASIC_INFO counts whole microseconds; two truncated readings can
/// lose up to one each.
constexpr u64 kResolutionNs = 2'000;

u64 own_cpu_ns() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<u64>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<u64>(ts.tv_nsec);
}

/// Busy-wait until this thread's own CPU clock has advanced @p ns.
void spin_own_cpu(u64 ns) {
    const u64 start = own_cpu_ns();
    volatile u64 sink = 0;
    while (own_cpu_ns() - start < ns) {
        for (int i = 0; i < 1000; ++i) sink = sink + static_cast<u64>(i);
    }
}

TEST(PresentCpuMeter, SpinOnOwnThreadReadsAtLeastTheSpin) {
    const unsigned int port = pc::current_thread_port();
    u64 before = 0;
    u64 after = 0;
    ASSERT_TRUE(pc::thread_cpu_ns(port, before));
    spin_own_cpu(kSpinNs);
    ASSERT_TRUE(pc::thread_cpu_ns(port, after));
    EXPECT_GE(after - before + kResolutionNs, kSpinNs);
}

TEST(PresentCpuMeter, SleepOnOwnThreadReadsFarLessThanTheWallTime) {
    // Negative control: the same wall time, blocked, is not CPU time.
    const unsigned int port = pc::current_thread_port();
    u64 before = 0;
    u64 after = 0;
    ASSERT_TRUE(pc::thread_cpu_ns(port, before));
    std::this_thread::sleep_for(std::chrono::nanoseconds(kSpinNs));
    ASSERT_TRUE(pc::thread_cpu_ns(port, after));
    EXPECT_LT(after - before, kSpinNs / 10);
}

TEST(PresentCpuMeter, ReadsAnotherThreadsSpinAndNotItsSleep) {
    // The named-thread path reads a port that is not the caller's.
    for (const bool spin : {true, false}) {
        std::atomic<unsigned int> port{0};
        std::atomic<int> phase{0};
        std::thread worker([&] {
            port.store(pc::current_thread_port());
            phase.store(1);
            while (phase.load() != 2) std::this_thread::yield();
            if (spin) {
                spin_own_cpu(kSpinNs);
            } else {
                std::this_thread::sleep_for(std::chrono::nanoseconds(kSpinNs));
            }
            phase.store(3);
            while (phase.load() != 4) std::this_thread::yield();
        });
        while (phase.load() != 1) std::this_thread::yield();
        u64 before = 0;
        u64 after = 0;
        ASSERT_TRUE(pc::thread_cpu_ns(port.load(), before));
        phase.store(2);
        while (phase.load() != 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_TRUE(pc::thread_cpu_ns(port.load(), after));
        phase.store(4);
        worker.join();
        if (spin) {
            EXPECT_GE(after - before + kResolutionNs, kSpinNs);
        } else {
            // The worker yields while it waits for phase 2/4, which costs a
            // little CPU; nowhere near the 30 ms of wall time it slept.
            EXPECT_LT(after - before, kSpinNs / 10);
        }
    }
}

TEST(PresentCpuMeter, DeadPortIsRefused) {
    u64 ignored = 0;
    EXPECT_FALSE(pc::thread_cpu_ns(0, ignored));
}

std::string read_all(std::FILE* f) {
    std::fflush(f);
    std::rewind(f);
    std::string text;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), f) != nullptr) text += buf;
    return text;
}

/// Parse `cpu_ms_per_frame=` from the first line holding @p needle.
double field_after(const std::string& text, const std::string& needle,
                   const char* field) {
    const auto at = text.find(needle);
    if (at == std::string::npos) return -1.0;
    const auto eol = text.find('\n', at);
    const std::string line = text.substr(at, eol - at);
    const auto f = line.find(field);
    if (f == std::string::npos) return -1.0;
    return std::strtod(line.c_str() + f + std::strlen(field), nullptr);
}

TEST(PresentCpuMeter, WindowReportsPresentingAndNamedThreadsPerFrame) {
    std::FILE* out = std::tmpfile();
    ASSERT_NE(out, nullptr);
    pc::reset_for_test();
    pc::configure_for_test(out, 20'000'000);  // 20 ms windows
    diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::On);

    // A named thread that sleeps throughout (it must read ~0 per frame), and
    // this thread named as the presenter.
    std::atomic<bool> stop{false};
    std::atomic<bool> named{false};
    std::thread idle([&] {
        pc::note_thread_name(77, u"IdleWorker", pc::current_thread_port());
        named.store(true);
        while (!stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    while (!named.load()) std::this_thread::yield();
    pc::note_thread_name(42, u"Presenter\"X", pc::current_thread_port());

    // Window 0 opens at the first present. Two frames of 15 ms of CPU each
    // close it (30 ms > 20 ms window).
    pc::note_present();
    spin_own_cpu(15'000'000);
    pc::note_present();
    spin_own_cpu(15'000'000);
    pc::note_present();

    stop.store(true);
    idle.join();
    diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::None);

    const std::string text = read_all(out);
    std::fclose(out);
    pc::reset_for_test();

    EXPECT_NE(text.find("[present-cpu] w=0 arm=on"), std::string::npos) << text;
    EXPECT_NE(text.find("frames=2"), std::string::npos) << text;
    // The quote in the name cannot end the field.
    const double present = field_after(
        text, "thread=\"Presenter'X\" T42 role=present", "cpu_ms_per_frame=");
    EXPECT_GE(present + 0.002, 15.0) << text;
    EXPECT_LT(present, 15.0 * 3) << text;
    const double idle_ms =
        field_after(text, "thread=\"IdleWorker\" T77", "cpu_ms_per_frame=");
    EXPECT_GE(idle_ms, 0.0) << text;
    EXPECT_LT(idle_ms, 1.5) << text;
}

TEST(PresentCpuMeter, WindowClosesAtAnArmChange) {
    std::FILE* out = std::tmpfile();
    ASSERT_NE(out, nullptr);
    pc::reset_for_test();
    pc::configure_for_test(out, 10'000'000'000ull);  // never by time
    diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::On);
    pc::note_present();
    pc::note_present();
    diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::Off);
    pc::note_present();   // first present of the new arm closes window 0
    pc::note_present();
    diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::On);
    pc::note_present();   // closes window 1
    diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::None);
    const std::string text = read_all(out);
    std::fclose(out);
    pc::reset_for_test();
    EXPECT_NE(text.find("[present-cpu] w=0 arm=on"), std::string::npos) << text;
    EXPECT_NE(text.find("[present-cpu] w=1 arm=off"), std::string::npos)
        << text;
    EXPECT_EQ(text.find("w=2"), std::string::npos) << text;
}

TEST(AbCycleOnly, CommaListNamesExactFeatures) {
    using diagnostics::ab_detail::listed;
    EXPECT_TRUE(listed("com-direct", "com-direct"));
    EXPECT_TRUE(listed("com-direct,d3d11-percall", "com-direct"));
    EXPECT_TRUE(listed("com-direct,d3d11-percall", "d3d11-percall"));
    EXPECT_TRUE(listed("a,com-direct,b", "com-direct"));
    EXPECT_FALSE(listed("com-direct,d3d11-percall", "recycler"));
    EXPECT_FALSE(listed("com-direct", "com"));
    EXPECT_FALSE(listed("com", "com-direct"));
    EXPECT_FALSE(listed("com-direct,", "recycler"));
    EXPECT_FALSE(listed("", "recycler"));
}

}  // namespace
