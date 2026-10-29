// SPDX-License-Identifier: LGPL-2.1-only
/// @file dshow_allocator.cpp
/// @brief CLSID_MemoryAllocator: IMemAllocator and its IMediaSample pool,
/// with quartz CMemAllocator / CMediaSample behaviour measured on Windows 11
/// x86 and x64 (tools/oracle_probe/dshow_allocator_probe, dshow_sample_probe).

#include "winapi/com/ole32/dshow_host.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace vitreo::winapi::dshow {

namespace {

namespace sdk {
#include "winapi/com/ole32/dshow_guids.inc"
}

constexpr u32 S_OK_ = 0;
constexpr u32 S_FALSE_ = 1;
constexpr u32 E_POINTER_ = 0x80004003u;
constexpr u32 E_OUTOFMEMORY_ = 0x8007000Eu;
constexpr u32 E_NOTIMPL_ = 0x80004001u;
constexpr u32 VFW_E_BADALIGN = 0x8004020Eu;
constexpr u32 VFW_E_ALREADY_COMMITTED = 0x8004020Fu;
constexpr u32 VFW_E_BUFFERS_OUTSTANDING = 0x80040210u;
constexpr u32 VFW_E_NOT_COMMITTED = 0x80040211u;
constexpr u32 VFW_E_SIZENOTSET = 0x80040212u;
constexpr u32 VFW_E_TIMEOUT = 0x8004022Eu;
constexpr u32 VFW_E_BUFFER_OVERFLOW = 0x8004020Du;
constexpr u32 VFW_E_SAMPLE_TIME_NOT_SET = 0x80040249u;
constexpr u32 VFW_E_MEDIA_TIME_NOT_SET = 0x80040251u;
constexpr u32 VFW_S_NO_STOP_TIME = 0x00040270u;
constexpr u32 AM_GBF_NOWAIT = 4;
// CMemAllocator checks the alignment against the 64 KB allocation
// granularity: (granularity & (align - 1)) != 0 is VFW_E_BADALIGN (measured:
// 3 and 65536 are accepted, 0 and 131072 refused).
constexpr u32 kAllocationGranularity = 0x10000u;

[[nodiscard]] bool guid_eq(const Guid& a, const Guid& b) {
    return std::memcmp(&a, &b, sizeof(Guid)) == 0;
}

class MemAllocator;

class MediaSample final : public HostObject {
public:
    std::weak_ptr<MemAllocator> allocator;
    GuestAddr buffer{0};
    u32 size{0};
    // State (CMediaSample; reset each time the sample leaves the pool except
    // the data length, measured).
    std::mutex mutex;
    u32 actual{0};
    bool start_valid{false}, end_valid{false}, media_valid{false};
    i64 start{0}, end{0}, media_start{0}, media_end{0};
    bool sync{false}, preroll{false}, discontinuity{false};
    std::vector<u8> media_type;  // AM_MEDIA_TYPE bytes + format, empty = none

    void reset_for_reuse() {
        std::lock_guard<std::mutex> lock(mutex);
        start_valid = end_valid = media_valid = false;
        sync = preroll = discontinuity = false;
        media_type.clear();
    }

    bool interface_for(const Guid& iid, Iface& out) const override {
        // IMediaSample2 (which quartz's samples also expose) is not
        // implemented: callers such as CTransformFilter fall back to the
        // IMediaSample setters when QueryInterface refuses it.
        if (guid_eq(iid, sdk::IID_IMediaSample)) {
            out = Iface::MediaSample;
            return true;
        }
        return false;
    }
    bool recycle(Marshaler& m) override;
    u32 call(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) override;
};

class MemAllocator final : public HostObject {
public:
    std::mutex mutex;
    std::condition_variable cv;
    bool props_set{false};
    i32 count{0}, bytes{0}, align{0}, prefix{0};
    bool committed{false};
    GuestAddr block{0};
    std::vector<std::shared_ptr<MediaSample>> samples;
    std::vector<MediaSample*> free_list;
    std::atomic<bool> wake_logged{false};

    bool interface_for(const Guid& iid, Iface& out) const override {
        if (guid_eq(iid, sdk::IID_IMemAllocator)) {
            out = Iface::MemAllocator;
            return true;
        }
        return false;
    }

    void give_back(MediaSample* sample) {
        std::lock_guard<std::mutex> lock(mutex);
        for (MediaSample* f : free_list) {
            if (f == sample) return;
        }
        free_list.push_back(sample);
        cv.notify_all();
    }

    /// Free the buffers once decommitted and every sample is back.
    void free_if_idle(Marshaler& m) {
        std::vector<std::shared_ptr<MediaSample>> drop;
        GuestAddr memory = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (committed || free_list.size() != samples.size()) return;
            drop.swap(samples);
            free_list.clear();
            memory = block;
            block = 0;
        }
        for (auto& sample : drop) host->discard(sample);
        if (memory != 0) host->mgr().co_task_mem_free_guest(memory);
        (void)m;
    }

    void final_release(Marshaler& m) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            committed = false;
        }
        cv.notify_all();
        free_if_idle(m);
    }

    u32 call(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) override {
        (void)iface;
        const auto arg = [&](usize i) { return i < args.size() ? args[i] : 0; };
        switch (slot) {
        case 3: {  // SetProperties(ALLOCATOR_PROPERTIES* pRequest, ALLOCATOR_PROPERTIES* pActual)
            const GuestAddr request = static_cast<GuestAddr>(arg(0));
            const GuestAddr actual = static_cast<GuestAddr>(arg(1));
            if (request == 0 || actual == 0) return E_POINTER_;
            for (u32 i = 0; i < 16; i += 4) (void)m.write_u32(actual + i, 0);
            std::lock_guard<std::mutex> lock(mutex);
            if (committed) return VFW_E_ALREADY_COMMITTED;
            if (free_list.size() != samples.size()) return VFW_E_BUFFERS_OUTSTANDING;
            const i32 want_count = static_cast<i32>(m.read_u32(request).value_or(0));
            const i32 want_bytes = static_cast<i32>(m.read_u32(request + 4).value_or(0));
            const i32 want_align = static_cast<i32>(m.read_u32(request + 8).value_or(0));
            const i32 want_prefix = static_cast<i32>(m.read_u32(request + 12).value_or(0));
            if (want_align == 0 ||
                (kAllocationGranularity & (static_cast<u32>(want_align) - 1u)) != 0) {
                return VFW_E_BADALIGN;
            }
            i64 total = static_cast<i64>(want_bytes) + want_prefix;
            const i64 remainder = total % want_align;
            if (remainder != 0) total += want_align - remainder;
            count = want_count;
            bytes = static_cast<i32>(total - want_prefix);
            align = want_align;
            prefix = want_prefix;
            props_set = true;
            (void)m.write_u32(actual + 0, static_cast<u32>(count));
            (void)m.write_u32(actual + 4, static_cast<u32>(bytes));
            (void)m.write_u32(actual + 8, static_cast<u32>(align));
            (void)m.write_u32(actual + 12, static_cast<u32>(prefix));
            return S_OK_;
        }
        case 4: {  // GetProperties(ALLOCATOR_PROPERTIES*)
            const GuestAddr out = static_cast<GuestAddr>(arg(0));
            if (out == 0) return E_POINTER_;
            std::lock_guard<std::mutex> lock(mutex);
            // Measured: before SetProperties the 64-bit quartz writes zeros
            // and the 32-bit one leaves the caller's structure alone.
            if (!props_set && !m.cpu_is_64bit()) return S_OK_;
            (void)m.write_u32(out + 0, static_cast<u32>(count));
            (void)m.write_u32(out + 4, static_cast<u32>(bytes));
            (void)m.write_u32(out + 8, static_cast<u32>(align));
            (void)m.write_u32(out + 12, static_cast<u32>(prefix));
            return S_OK_;
        }
        case 5: {  // Commit
            std::unique_lock<std::mutex> lock(mutex);
            if (!props_set) return VFW_E_SIZENOTSET;
            if (committed) return S_OK_;
            if (samples.empty() && count > 0) {
                const u32 stride = static_cast<u32>(bytes + prefix);
                const u64 total = static_cast<u64>(stride) * static_cast<u32>(count) +
                                  static_cast<u32>(align);
                lock.unlock();
                const GuestAddr memory = host->mgr().co_task_mem_alloc_guest(total);
                if (memory == 0) return E_OUTOFMEMORY_;
                const GuestAddr base = static_cast<GuestAddr>(
                    (memory + static_cast<u32>(align) - 1u) / static_cast<u32>(align) *
                    static_cast<u32>(align));
                std::vector<std::shared_ptr<MediaSample>> made;
                for (i32 i = 0; i < count; ++i) {
                    auto sample = std::make_shared<MediaSample>();
                    sample->allocator = std::static_pointer_cast<MemAllocator>(shared_from_this());
                    sample->buffer = base + static_cast<GuestAddr>(i) * stride +
                                     static_cast<u32>(prefix);
                    sample->size = static_cast<u32>(bytes);
                    sample->actual = sample->size;
                    sample->set_refs(0);
                    if (!host->publish(m, sample, {Iface::MediaSample})) {
                        host->mgr().co_task_mem_free_guest(memory);
                        return E_OUTOFMEMORY_;
                    }
                    made.push_back(std::move(sample));
                }
                lock.lock();
                block = memory;
                samples = std::move(made);
                free_list.clear();
                for (auto& sample : samples) free_list.push_back(sample.get());
            }
            committed = true;
            return S_OK_;
        }
        case 6: {  // Decommit: outstanding samples stay usable (measured)
            {
                std::lock_guard<std::mutex> lock(mutex);
                committed = false;
            }
            cv.notify_all();
            free_if_idle(m);
            return S_OK_;
        }
        case 7: {  // GetBuffer(IMediaSample**, REFERENCE_TIME*, REFERENCE_TIME*, DWORD dwFlags)
            const GuestAddr out = static_cast<GuestAddr>(arg(0));
            const u32 flags = static_cast<u32>(arg(3));
            if (out == 0) return E_POINTER_;
            MediaSample* sample = nullptr;
            bool waited = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                for (;;) {
                    if (!committed) {
                        lock.unlock();
                        if (waited && !wake_logged.exchange(true)) {
                            std::fprintf(stderr,
                                         "[quartz] allocator 0x%llx: a GetBuffer blocked on a full "
                                         "pool was released by Decommit (VFW_E_NOT_COMMITTED)\n",
                                         static_cast<unsigned long long>(pointer(Iface::MemAllocator)));
                        }
                        (void)m.write_ptr(out, 0);
                        return VFW_E_NOT_COMMITTED;
                    }
                    if (!free_list.empty()) {
                        sample = free_list.front();
                        free_list.erase(free_list.begin());
                        break;
                    }
                    if ((flags & AM_GBF_NOWAIT) != 0) {
                        lock.unlock();
                        (void)m.write_ptr(out, 0);
                        return VFW_E_TIMEOUT;
                    }
                    waited = true;
                    cv.wait(lock);
                }
            }
            sample->reset_for_reuse();
            sample->set_refs(1);
            (void)m.write_ptr(out, sample->pointer(Iface::MediaSample));
            return S_OK_;
        }
        case 8: {  // ReleaseBuffer(IMediaSample*)
            Iface kind{};
            auto object = host->find(static_cast<GuestAddr>(arg(0)), &kind);
            if (object == nullptr || kind != Iface::MediaSample) return E_POINTER_;
            auto* sample = static_cast<MediaSample*>(object.get());
            if (sample->allocator.lock().get() != this) return E_POINTER_;
            give_back(sample);
            free_if_idle(m);
            return S_OK_;
        }
        default:
            return E_NOTIMPL_;
        }
    }
};

bool MediaSample::recycle(Marshaler& m) {
    auto owner = allocator.lock();
    if (owner == nullptr) return false;
    owner->give_back(this);
    owner->free_if_idle(m);
    return true;
}

u32 MediaSample::call(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) {
    (void)iface;
    const auto arg = [&](usize i) { return i < args.size() ? args[i] : 0; };
    std::lock_guard<std::mutex> lock(mutex);
    switch (slot) {
    case 3: {  // GetPointer(BYTE**)
        const GuestAddr out = static_cast<GuestAddr>(arg(0));
        if (out == 0) return E_POINTER_;
        (void)m.write_ptr(out, buffer);
        return S_OK_;
    }
    case 4:  // GetSize
        return size;
    case 5: {  // GetTime(REFERENCE_TIME* pTimeStart, REFERENCE_TIME* pTimeEnd)
        const GuestAddr a = static_cast<GuestAddr>(arg(0));
        const GuestAddr b = static_cast<GuestAddr>(arg(1));
        if (a == 0 || b == 0) return E_POINTER_;
        if (!start_valid) return VFW_E_SAMPLE_TIME_NOT_SET;
        (void)m.write_u64(a, static_cast<u64>(start));
        if (!end_valid) {
            (void)m.write_u64(b, static_cast<u64>(start + 1));
            return VFW_S_NO_STOP_TIME;
        }
        (void)m.write_u64(b, static_cast<u64>(end));
        return S_OK_;
    }
    case 6: {  // SetTime(REFERENCE_TIME*, REFERENCE_TIME*): NULL start clears both
        const GuestAddr a = static_cast<GuestAddr>(arg(0));
        const GuestAddr b = static_cast<GuestAddr>(arg(1));
        start_valid = a != 0;
        end_valid = a != 0 && b != 0;
        if (a != 0) start = static_cast<i64>(m.memory().read_u64(a).value_or(0));
        if (end_valid) end = static_cast<i64>(m.memory().read_u64(b).value_or(0));
        return S_OK_;
    }
    case 7: return sync ? S_OK_ : S_FALSE_;                       // IsSyncPoint
    case 8: sync = static_cast<u32>(arg(0)) != 0; return S_OK_;   // SetSyncPoint
    case 9: return preroll ? S_OK_ : S_FALSE_;                    // IsPreroll
    case 10: preroll = static_cast<u32>(arg(0)) != 0; return S_OK_;
    case 11: return actual;                                       // GetActualDataLength
    case 12: {                                                    // SetActualDataLength
        const u32 length = static_cast<u32>(arg(0));
        if (length > size) return VFW_E_BUFFER_OVERFLOW;
        actual = length;
        return S_OK_;
    }
    case 13: {  // GetMediaType(AM_MEDIA_TYPE**): S_FALSE and NULL when unchanged
        const GuestAddr out = static_cast<GuestAddr>(arg(0));
        if (out == 0) return E_POINTER_;
        if (media_type.empty()) {
            (void)m.write_ptr(out, 0);
            return S_FALSE_;
        }
        const bool x64 = m.cpu_is_64bit();
        const u32 mt_size = x64 ? 88u : 72u;
        const u32 cb_off = x64 ? 72u : 64u, pb_off = x64 ? 80u : 68u;
        const GuestAddr copy = host->mgr().co_task_mem_alloc_guest(mt_size);
        if (copy == 0) return E_OUTOFMEMORY_;
        (void)m.memory().write_bytes(copy, media_type.data(), mt_size);
        const u32 cb = m.read_u32(copy + cb_off).value_or(0);
        GuestAddr format = 0;
        if (cb != 0) {
            format = host->mgr().co_task_mem_alloc_guest(cb);
            if (format != 0) (void)m.memory().write_bytes(format, media_type.data() + mt_size, cb);
        }
        (void)m.write_ptr(copy + pb_off, format);
        (void)m.write_ptr(out, copy);
        return S_OK_;
    }
    case 14: {  // SetMediaType(AM_MEDIA_TYPE*): a copy travels with the sample
        const GuestAddr mt = static_cast<GuestAddr>(arg(0));
        media_type.clear();
        if (mt == 0) return S_OK_;
        const bool x64 = m.cpu_is_64bit();
        const u32 mt_size = x64 ? 88u : 72u;
        const u32 cb_off = x64 ? 72u : 64u, pb_off = x64 ? 80u : 68u;
        std::vector<u8> bytes(mt_size);
        (void)m.memory().read_bytes(mt, bytes.data(), mt_size);
        const u32 cb = m.read_u32(mt + cb_off).value_or(0);
        const GuestAddr pb = static_cast<GuestAddr>(m.read_ptr(mt + pb_off).value_or(0));
        bytes.resize(mt_size + cb);
        if (cb != 0 && pb != 0) (void)m.memory().read_bytes(pb, bytes.data() + mt_size, cb);
        media_type = std::move(bytes);
        return S_OK_;
    }
    case 15: return discontinuity ? S_OK_ : S_FALSE_;                     // IsDiscontinuity
    case 16: discontinuity = static_cast<u32>(arg(0)) != 0; return S_OK_; // SetDiscontinuity
    case 17: {  // GetMediaTime
        const GuestAddr a = static_cast<GuestAddr>(arg(0));
        const GuestAddr b = static_cast<GuestAddr>(arg(1));
        if (a == 0 || b == 0) return E_POINTER_;
        if (!media_valid) return VFW_E_MEDIA_TIME_NOT_SET;
        (void)m.write_u64(a, static_cast<u64>(media_start));
        (void)m.write_u64(b, static_cast<u64>(media_end));
        return S_OK_;
    }
    case 18: {  // SetMediaTime
        const GuestAddr a = static_cast<GuestAddr>(arg(0));
        const GuestAddr b = static_cast<GuestAddr>(arg(1));
        media_valid = a != 0;
        if (a != 0) {
            media_start = static_cast<i64>(m.memory().read_u64(a).value_or(0));
            media_end = b != 0 ? static_cast<i64>(m.memory().read_u64(b).value_or(0)) : media_start;
        }
        return S_OK_;
    }
    default:
        return E_NOTIMPL_;
    }
}

} // namespace

GuestAddr create_memory_allocator(DShowHost& host, Marshaler& m) {
    auto allocator = std::make_shared<MemAllocator>();
    if (!host.publish(m, allocator, {Iface::MemAllocator})) return 0;
    return allocator->pointer(Iface::MemAllocator);
}

} // namespace vitreo::winapi::dshow
