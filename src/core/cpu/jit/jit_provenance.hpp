// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <vitreo/types.hpp>
#include <cstdio>

namespace vitreo::cpu::jit::symbol_map {

// Diagnostic publication history. Caller serializes record/flush. No code is
// read, no cache is traversed, and retired/overlapping ranges are never erased.
// A query needs a successful flush watermark after its sample interval.
class ProvenanceWriter {
public:
    ProvenanceWriter(std::FILE* out, u64 capacity, u64 pid, u64 now)
        : out_(out), capacity_(capacity) {
        failed_ = out_ == nullptr || std::fprintf(out_,
            "V1 %llu %llu %llu\n", ull(pid), ull(now), ull(capacity_)) < 0;
    }

    void record(u64 now, u64 guest, u64 hot, u32 hot_size,
                u64 cold, u32 cold_size) {
        if (failed_) return;
        if (count_ == capacity_) {
            if (!lost_) {
                lost_ = true;
                failed_ = std::fprintf(out_, "LOSS cap %llu\n", ull(now)) < 0;
                if (std::fflush(out_) != 0) failed_ = true;
            }
            return;
        }
        ++count_;
        failed_ = std::fprintf(out_, "R %llu %llu %llx %llx %x %llx %x\n",
            ull(count_), ull(now), ull(guest), ull(hot), hot_size,
            ull(cold), cold_size) < 0;
    }

    bool flush(u64 now) {
        if (failed_) return false;
        failed_ = std::fprintf(out_, "F %llu %llu %u\n",
            ull(count_), ull(now), lost_ ? 1u : 0u) < 0;
        if (std::fflush(out_) != 0) failed_ = true;
        return !failed_;
    }
    bool lost() const { return lost_; }
    bool failed() const { return failed_; }
    u64 count() const { return count_; }
private:
    static unsigned long long ull(u64 value) { return static_cast<unsigned long long>(value); }
    std::FILE* out_;
    u64 capacity_;
    u64 count_{0};
    bool lost_{false};
    bool failed_{false};
};
} // namespace vitreo::cpu::jit::symbol_map
