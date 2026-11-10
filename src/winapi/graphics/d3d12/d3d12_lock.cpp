// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_lock.cpp
/// @brief FrontendRwLock (d3d12_lock.hpp).

#include "winapi/graphics/d3d12/d3d12_lock.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace vitreo::winapi::d3d12 {
namespace {

/// One thread's hold of one lock. A thread holds few frontend locks at once
/// (one per D3D12 frontend it is inside); a small table is enough.
struct Hold {
    const FrontendRwLock* lock{nullptr};
    u32 depth{0};
    bool recording_window{false};
    FrontendRwLock::Mode mode{FrontendRwLock::Mode::None};
};
constexpr usize kHolds = 8;
thread_local std::array<Hold, kHolds> t_holds{};

/// The reader counter this thread announces itself in.
std::atomic<u32> g_next_slot{0};
thread_local const u32 t_slot = g_next_slot.fetch_add(1, std::memory_order_relaxed);

Hold* find_hold(const FrontendRwLock* lock) {
    for (Hold& hold : t_holds) {
        if (hold.lock == lock) return &hold;
    }
    return nullptr;
}

Hold& claim_hold(const FrontendRwLock* lock) {
    if (Hold* hold = find_hold(lock)) return *hold;
    for (Hold& hold : t_holds) {
        if (hold.lock == nullptr) {
            hold.lock = lock;
            return hold;
        }
    }
    std::fprintf(stderr, "[vitreo] d3d12: a thread holds more than %zu frontend "
                         "locks\n", kHolds);
    std::abort();
}

}  // namespace

bool FrontendRwLock::drained() const {
    for (const Slot& slot : m_slots) {
        if (slot.readers.load(std::memory_order_seq_cst) != 0) return false;
    }
    return true;
}

void FrontendRwLock::notify_writer() {
    // The lock/unlock orders this wake after the writer's predicate check
    // or before its wait: it cannot be lost.
    { std::lock_guard<std::mutex> lock(m_drain_mutex); }
    m_drain_cv.notify_all();
}

void FrontendRwLock::notify_readers() {
    { std::lock_guard<std::mutex> lock(m_admission_mutex); }
    m_admission_cv.notify_all();
}

bool FrontendRwLock::acquire_shared(bool recording) {
    Slot& slot = m_slots[t_slot % kSlots];
    bool waited = false;
    for (;;) {
        slot.readers.fetch_add(1, std::memory_order_seq_cst);
        if (!m_writer.load(std::memory_order_seq_cst) ||
            (recording && m_recording_window.load(std::memory_order_seq_cst))) {
            return waited;
        }
        // A writer is in or draining: step back, then wait it out.
        slot.readers.fetch_sub(1, std::memory_order_seq_cst);
        notify_writer();
        waited = true;
        std::unique_lock<std::mutex> wait(m_admission_mutex);
        m_admission_cv.wait(wait, [&] {
            return !m_writer.load(std::memory_order_seq_cst) ||
                   (recording && m_recording_window.load(std::memory_order_seq_cst));
        });
    }
}

void FrontendRwLock::release_shared() {
    m_slots[t_slot % kSlots].readers.fetch_sub(1, std::memory_order_seq_cst);
    if (m_writer.load(std::memory_order_seq_cst)) notify_writer();
}

bool FrontendRwLock::acquire_exclusive() {
    bool waited = false;
    if (!m_writer_mutex.try_lock()) {
        m_writer_mutex.lock();
        waited = true;
    }
    m_writer.store(true, std::memory_order_seq_cst);
    if (!drained()) {
        waited = true;
        std::unique_lock<std::mutex> lock(m_drain_mutex);
        while (!drained()) {
            m_drain_cv.wait_for(lock, std::chrono::milliseconds(1));
        }
    }
    return waited;
}

void FrontendRwLock::release_exclusive() {
    m_writer.store(false, std::memory_order_seq_cst);
    m_writer_mutex.unlock();
    notify_readers();
}

bool FrontendRwLock::lock(Mode mode) {
    Hold& hold = claim_hold(this);
    if (hold.depth != 0) {
        // A callback into the frontend must regain full exclusion before it
        // can touch state outside the recording-only window.
        if (hold.recording_window) end_recording_window();
        if ((mode == Mode::Exclusive && hold.mode != Mode::Exclusive) ||
            (mode == Mode::Shared && hold.mode == Mode::Recording)) {
            // Upgrade: the outer shared frame continues under the stronger
            // hold until the outermost release.
            m_upgrades.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "[vitreo] d3d12: frontend lock upgraded from "
                                 "shared to exclusive\n");
            release_shared();
            (void)acquire_exclusive();
            hold.mode = Mode::Exclusive;
        }
        ++hold.depth;
        return false;
    }
    const bool waited =
        mode == Mode::Exclusive ? acquire_exclusive() : acquire_shared(mode == Mode::Recording);
    hold.mode = mode;
    hold.depth = 1;
    return waited;
}

void FrontendRwLock::unlock() {
    Hold* hold = find_hold(this);
    if (hold == nullptr || hold->depth == 0) return;
    if (hold->recording_window) end_recording_window();
    if (--hold->depth != 0) return;
    const Mode mode = hold->mode;
    hold->mode = Mode::None;
    hold->lock = nullptr;
    if (mode == Mode::Exclusive) {
        release_exclusive();
    } else {
        release_shared();
    }
}

bool FrontendRwLock::begin_recording_window() {
    Hold* hold = find_hold(this);
    if (hold == nullptr || hold->depth != 1 || hold->mode != Mode::Exclusive ||
        hold->recording_window) return false;
    hold->recording_window = true;
    m_recording_window.store(true, std::memory_order_seq_cst);
    notify_readers();
    return true;
}

void FrontendRwLock::end_recording_window() {
    Hold* hold = find_hold(this);
    if (hold == nullptr || !hold->recording_window) return;
    m_recording_window.store(false, std::memory_order_seq_cst);
    std::unique_lock<std::mutex> lock(m_drain_mutex);
    m_drain_cv.wait(lock, [&] { return drained(); });
    hold->recording_window = false;
}

u32 FrontendRwLock::depth() const {
    const Hold* hold = find_hold(this);
    return hold != nullptr ? hold->depth : 0u;
}

FrontendRwLock::Mode FrontendRwLock::mode() const {
    const Hold* hold = find_hold(this);
    return hold != nullptr && hold->depth != 0 ? hold->mode : Mode::None;
}

FrontendRwLock::Mode FrontendRwLock::release_if_outermost() {
    Hold* hold = find_hold(this);
    if (hold == nullptr || hold->depth != 1) return Mode::None;
    const Mode mode = hold->mode;
    unlock();
    return mode;
}

bool FrontendRwLock::reacquire(Mode mode) {
    if (mode == Mode::None) return false;
    return lock(mode);
}

}  // namespace vitreo::winapi::d3d12
