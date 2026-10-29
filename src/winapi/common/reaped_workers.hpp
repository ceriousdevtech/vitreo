#pragma once

/// @file reaped_workers.hpp
/// @brief Per-operation host worker threads that are joined once finished.
///
/// A joinable thread that exits but is never joined keeps its pthread record
/// (one 32 KiB VM region on macOS) until it is joined. Workers spawned per
/// overlapped I/O operation therefore must not be held until teardown: a
/// guest that pends millions of pipe writes over its lifetime would keep one
/// region per write. Finished workers are handed back on every spawn so the
/// owner joins them outside its own lock (workers take that lock).

#include <atomic>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace vitreo::winapi {

class ReapedWorkers {
public:
    /// Owner-lock-free join target: destroying it joins each finished worker,
    /// which has already passed its last statement.
    using Finished = std::vector<std::jthread>;

    /// Start @p fn(std::stop_token) on a new worker. Caller holds the owner's
    /// lock; workers that already finished are moved into @p finished, which
    /// the caller must destroy only after releasing that lock.
    template <class Fn>
    void spawn(Fn&& fn, Finished& finished) {
        take_finished(finished);
        auto done = std::make_shared<std::atomic<bool>>(false);
        m_workers.push_back(Worker{done, std::jthread(
            [done, fn = std::forward<Fn>(fn)](std::stop_token stop) mutable {
                struct MarkDone {
                    std::atomic<bool>& flag;
                    ~MarkDone() { flag.store(true, std::memory_order_release); }
                } mark{*done};
                fn(std::move(stop));
            })});
    }

    /// Move finished workers into @p finished (caller holds the owner's lock).
    void take_finished(Finished& finished) {
        for (auto it = m_workers.begin(); it != m_workers.end();) {
            if (it->done->load(std::memory_order_acquire)) {
                finished.push_back(std::move(it->thread));
                it = m_workers.erase(it);
            } else {
                ++it;
            }
        }
    }

    /// Request stop on every worker and move all of them into @p all for
    /// teardown (caller holds the owner's lock).
    void take_all(Finished& all) {
        for (auto& w : m_workers) w.thread.request_stop();
        for (auto& w : m_workers) all.push_back(std::move(w.thread));
        m_workers.clear();
    }

    /// Workers not yet taken (running, or finished since the last reap).
    [[nodiscard]] std::size_t size() const { return m_workers.size(); }

private:
    struct Worker {
        std::shared_ptr<std::atomic<bool>> done;
        std::jthread thread;
    };
    std::vector<Worker> m_workers;
};

}  // namespace vitreo::winapi
