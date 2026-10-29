// SPDX-License-Identifier: LGPL-2.1-only
/// @file dshow_async_reader.cpp
/// @brief File Source (Async) (CLSID_AsyncReader): IBaseFilter +
/// IFileSourceFilter and its "Output" pin with IAsyncReader, reading the file
/// through the capsule VFS. Behaviour measured on Windows 11 x86 and x64
/// (tools/oracle_probe/dshow_asyncreader_probe{32,64}.expected).

#include "winapi/com/ole32/dshow_objects.hpp"

#include "capsule/vfs/vfs.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>

namespace vitreo::winapi::dshow {

namespace {

constexpr u32 HRESULT_FILE_NOT_FOUND = 0x80070002u;   // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
constexpr u32 HRESULT_HANDLE_EOF = 0x80070026u;       // HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)
constexpr i64 kUnits = 10'000'000;                    // byte positions travel as times * UNITS
// Measured: RequestAllocator returns cbAlign 512 whatever was asked for, and
// Request / SyncReadAligned refuse positions off that alignment.
constexpr u32 kReaderAlign = 512;

[[nodiscard]] MediaType stream_type() {
    // Measured: MEDIATYPE_Stream, subtype GUID_NULL for a WebM file (no
    // registered check bytes match), bFixedSizeSamples TRUE, lSampleSize 1,
    // no format.
    MediaType t;
    t.major = sdk::MEDIATYPE_Stream;
    t.fixed = true;
    t.sample_size = 1;
    return t;
}

class AsyncReaderFilter;

class AsyncOutputPin final : public PinBase {
public:
    AsyncReaderFilter* reader{nullptr};
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    struct Done {
        GuestAddr sample;
        u64 user;
    };
    std::deque<Done> done;
    bool reader_flushing{false};
    // Milestone counters ("[quartz] File Source" lines).
    std::atomic<u64> requests{0};
    std::atomic<u64> bytes_read{0};
    std::atomic<u64> sync_reads{0};
    std::atomic<u32> errors_logged{0};
    std::atomic<bool> eof_logged{false};
    void note_error(const char* what, u32 hr) {
        // One line per error kind (bit per HRESULT low byte) at most.
        const u32 bit = 1u << (hr & 31u);
        if ((errors_logged.fetch_or(bit) & bit) != 0) return;
        std::fprintf(stderr, "[quartz] File Source %s -> 0x%x (requests=%llu bytes=%llu)\n", what, hr,
                     static_cast<unsigned long long>(requests.load()),
                     static_cast<unsigned long long>(bytes_read.load()));
    }
    void note_read(i64 start, i64 got, i64 wanted, i64 length) {
        bytes_read.fetch_add(static_cast<u64>(std::max<i64>(got, 0)));
        if (start + got >= length && got < wanted && !eof_logged.exchange(true)) {
            std::fprintf(stderr,
                         "[quartz] File Source read reached the end of the file at %lld "
                         "(requests=%llu sync_reads=%llu bytes=%llu)\n",
                         static_cast<long long>(start + got),
                         static_cast<unsigned long long>(requests.load()),
                         static_cast<unsigned long long>(sync_reads.load()),
                         static_cast<unsigned long long>(bytes_read.load()));
        }
    }

    bool interface_for(const Guid& iid, Iface& out) const override {
        if (guid_eq(iid, sdk::IID_IAsyncReader)) {
            out = Iface::AsyncReader;
            return true;
        }
        return PinBase::interface_for(iid, out);
    }

protected:
    std::vector<MediaType> types() override;
    bool accept(Marshaler& m, const MediaType& type) override {
        (void)m;
        return guid_eq(type.major, sdk::MEDIATYPE_Stream);
    }
    u32 call_other(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) override;

private:
    /// Read [start, start + length) of the file into guest memory.
    i64 read_into(Marshaler& m, i64 start, i64 length, GuestAddr buffer);
    /// IMediaSample helpers (the sample may be a guest or a host object).
    bool sample_span(Marshaler& m, GuestAddr sample, i64& start, i64& stop, bool& has_time);
};

class AsyncReaderFilter final : public FilterBase {
public:
    std::mutex file_mutex;
    std::ifstream file;
    i64 length{0};
    bool loaded{false};
    std::u16string path;
    MediaType type;
    std::shared_ptr<AsyncOutputPin> output_pin;

    bool interface_for(const Guid& iid, Iface& out) const override {
        if (guid_eq(iid, sdk::IID_IFileSourceFilter)) {
            out = Iface::FileSourceFilter;
            return true;
        }
        return FilterBase::interface_for(iid, out);
    }

    u32 load(Marshaler& m, const std::u16string& guest_path, const MediaType* given) {
        if (loaded) return hr::E_UNEXPECTED;  // measured: a second Load
        capsule::Vfs* vfs = host->mgr().vfs();
        std::string utf8;
        for (const char16_t c : guest_path) {
            if (c < 0x80) {
                utf8.push_back(static_cast<char>(c));
            } else if (c < 0x800) {
                utf8.push_back(static_cast<char>(0xC0 | (c >> 6)));
                utf8.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            } else {
                utf8.push_back(static_cast<char>(0xE0 | (c >> 12)));
                utf8.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                utf8.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
        }
        if (vfs == nullptr) return HRESULT_FILE_NOT_FOUND;
        auto host_path = vfs->resolve_host_path(utf8);
        std::error_code error;
        if (host_path.is_err() || !std::filesystem::is_regular_file(host_path.value(), error)) {
            return HRESULT_FILE_NOT_FOUND;
        }
        std::lock_guard<std::mutex> lock(file_mutex);
        file.open(host_path.value(), std::ios::binary);
        if (!file.is_open()) return HRESULT_FILE_NOT_FOUND;
        length = static_cast<i64>(std::filesystem::file_size(host_path.value(), error));
        if (given == nullptr) {
            // quartz picks the subtype from the registry's Media Type check
            // bytes (AVI, WAVE, MPEG, ...); a WebM file matches none and is
            // GUID_NULL (measured). The check-bytes table is not modelled.
            char magic[4] = {0, 0, 0, 0};
            file.read(magic, 4);
            file.clear();
            file.seekg(0);
            const bool registered = std::memcmp(magic, "RIFF", 4) == 0 ||
                                    (magic[0] == 0 && magic[1] == 0 && magic[2] == 1);
            if (registered) {
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true)) {
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED quartz!File Source (Async): "
                                         "Media Type check-bytes subtype (GUID_NULL used)\n");
                }
            }
        }
        type = given != nullptr ? *given : stream_type();
        path = guest_path;
        loaded = true;
        auto pin = std::make_shared<AsyncOutputPin>();
        pin->reader = this;
        pin->output = true;
        pin->name = u"Output";
        if (!publish_pin(*host, m, *this, pin, {Iface::Pin, Iface::AsyncReader})) {
            return hr::E_OUTOFMEMORY;
        }
        output_pin = pin;
        std::fprintf(stderr, "[quartz] File Source (Async) Load length=%lld\n",
                     static_cast<long long>(length));
        return hr::S_OK;
    }

    i64 read(i64 start, i64 count, std::vector<u8>& out) {
        std::lock_guard<std::mutex> lock(file_mutex);
        if (start >= length || count <= 0) return 0;
        const i64 n = std::min(count, length - start);
        out.resize(static_cast<usize>(n));
        file.clear();
        file.seekg(start);
        file.read(reinterpret_cast<char*>(out.data()), n);
        return static_cast<i64>(file.gcount());
    }

protected:
    std::vector<std::shared_ptr<PinBase>> visible_pins() override {
        // Measured: no pin until Load.
        return loaded ? pins : std::vector<std::shared_ptr<PinBase>>{};
    }

    u32 call_other(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) override {
        if (iface != Iface::FileSourceFilter) return hr::E_NOTIMPL;
        const auto arg = [&](usize i) { return i < args.size() ? args[i] : 0; };
        if (slot == 3) {  // Load(LPCOLESTR pszFileName, const AM_MEDIA_TYPE* pmt)
            const GuestAddr name_ptr = static_cast<GuestAddr>(arg(0));
            if (name_ptr == 0) return hr::E_POINTER;
            const GuestAddr pmt = static_cast<GuestAddr>(arg(1));
            const MediaType given = read_media_type(m, pmt);
            return load(m, read_wide(m, name_ptr), pmt != 0 ? &given : nullptr);
        }
        if (slot == 4) {  // GetCurFile(LPOLESTR* ppszFileName, AM_MEDIA_TYPE* pmt)
            const GuestAddr out = static_cast<GuestAddr>(arg(0));
            const GuestAddr pmt = static_cast<GuestAddr>(arg(1));
            if (out == 0) return hr::E_POINTER;
            if (!loaded) {  // measured: S_OK, *ppszFileName NULL
                (void)m.write_ptr(out, 0);
                return hr::S_OK;
            }
            const GuestAddr text = host->mgr().co_task_mem_alloc_guest((path.size() + 1) * 2);
            if (text == 0) return hr::E_OUTOFMEMORY;
            write_wide(m, text, path);
            (void)m.write_ptr(out, text);
            if (pmt != 0) (void)write_media_type(*host, m, pmt, type);
            return hr::S_OK;
        }
        return hr::E_NOTIMPL;
    }
};

std::vector<MediaType> AsyncOutputPin::types() {
    // Measured: the pin enumerates its type twice.
    return {reader->type, reader->type};
}

i64 AsyncOutputPin::read_into(Marshaler& m, i64 start, i64 length, GuestAddr buffer) {
    std::vector<u8> bytes;
    const i64 n = reader->read(start, length, bytes);
    if (n > 0) (void)m.memory().write_bytes(buffer, bytes.data(), static_cast<usize>(n));
    return n;
}

bool AsyncOutputPin::sample_span(Marshaler& m, GuestAddr sample, i64& start, i64& stop,
                                 bool& has_time) {
    const GuestAddr times = host->scratch(16);
    if (times == 0) return false;
    const u32 got = host->call(m, sample, Iface::MediaSample, 5, {times, times + 8});  // GetTime
    has_time = got == hr::S_OK || got == 0x00040270u;  // VFW_S_NO_STOP_TIME
    start = static_cast<i64>(m.memory().read_u64(times).value_or(0)) / kUnits;
    stop = static_cast<i64>(m.memory().read_u64(times + 8).value_or(0)) / kUnits;
    host->free_scratch(times);
    return true;
}

u32 AsyncOutputPin::call_other(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) {
    if (iface != Iface::AsyncReader) return hr::E_NOTIMPL;
    const auto arg = [&](usize i) { return i < args.size() ? args[i] : 0; };
    switch (slot) {
    case 3: {  // RequestAllocator(IMemAllocator* pPreferred, ALLOCATOR_PROPERTIES* pProps, IMemAllocator** ppActual)
        const GuestAddr preferred = static_cast<GuestAddr>(arg(0));
        const GuestAddr props = static_cast<GuestAddr>(arg(1));
        const GuestAddr out = static_cast<GuestAddr>(arg(2));
        if (props == 0 || out == 0) return hr::E_POINTER;
        const GuestAddr p = host->scratch(32);
        if (p == 0) return hr::E_OUTOFMEMORY;
        for (u32 i = 0; i < 16; i += 4) (void)m.write_u32(p + i, m.read_u32(props + i).value_or(0));
        if (m.read_u32(p + 8).value_or(0) < kReaderAlign) (void)m.write_u32(p + 8, kReaderAlign);
        const auto use = [&](GuestAddr allocator) {
            return host->call(m, allocator, Iface::MemAllocator, 3, {p, p + 16}) == hr::S_OK;
        };
        GuestAddr chosen = 0;
        if (preferred != 0 && use(preferred)) {
            (void)host->add_ref(m, preferred);
            chosen = preferred;
        } else {
            const GuestAddr own = create_memory_allocator(*host, m);
            if (own != 0 && use(own)) {
                chosen = own;
            } else if (own != 0) {
                (void)host->release(m, own);
            }
        }
        host->free_scratch(p);
        (void)m.write_ptr(out, chosen);
        return chosen != 0 ? hr::S_OK : hr::E_FAIL;
    }
    case 4: {  // Request(IMediaSample* pSample, DWORD_PTR dwUser)
        const GuestAddr sample = static_cast<GuestAddr>(arg(0));
        if (sample == 0) return hr::E_POINTER;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (reader_flushing) return hr::VFW_E_WRONG_STATE;
        }
        i64 start = 0, stop = 0;
        bool has_time = false;
        if (!sample_span(m, sample, start, stop, has_time)) return hr::E_OUTOFMEMORY;
        const GuestAddr buf = host->scratch(8);
        (void)m.write_ptr(buf, 0);
        (void)host->call(m, sample, Iface::MediaSample, 3, {buf});  // GetPointer
        const GuestAddr data = static_cast<GuestAddr>(m.read_ptr(buf).value_or(0));
        host->free_scratch(buf);
        const u32 size = host->call(m, sample, Iface::MediaSample, 4, {});  // GetSize
        i64 actual = 0;
        if (requests.fetch_add(1) == 0) {
            std::fprintf(stderr, "[quartz] File Source first Request start=%lld stop=%lld size=%u\n",
                         static_cast<long long>(start), static_cast<long long>(stop), size);
        }
        if (has_time) {
            if (start % kReaderAlign != 0 || stop % kReaderAlign != 0) {
                note_error("Request misaligned", hr::VFW_E_BADALIGN);
                return hr::VFW_E_BADALIGN;
            }
            if (stop - start > static_cast<i64>(size)) {
                note_error("Request longer than the sample", hr::E_INVALIDARG);
                return hr::E_INVALIDARG;
            }
            if (start >= reader->length) {
                note_error("Request past the end", HRESULT_HANDLE_EOF);
                return HRESULT_HANDLE_EOF;
            }
            actual = read_into(m, start, stop - start, data);
            note_read(start, actual, stop - start, reader->length);
            if (start + actual < stop) {
                // Measured: a short read ends at the aligned-up end of the data.
                const i64 end = start + (actual + kReaderAlign - 1) / kReaderAlign * kReaderAlign;
                const GuestAddr t = host->scratch(16);
                (void)m.write_u64(t, static_cast<u64>(start * kUnits));
                (void)m.write_u64(t + 8, static_cast<u64>(end * kUnits));
                (void)host->call(m, sample, Iface::MediaSample, 6, {t, t + 8});
                host->free_scratch(t);
            }
        }
        // Measured: a sample without times completes with no data.
        (void)host->call(m, sample, Iface::MediaSample, 12, {static_cast<u64>(actual)});
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            done.push_back(Done{sample, arg(1)});
        }
        queue_cv.notify_all();
        return hr::S_OK;
    }
    case 5: {  // WaitForNext(DWORD dwTimeout, IMediaSample** ppSample, DWORD_PTR* pdwUser)
        const u32 timeout = static_cast<u32>(arg(0));
        const GuestAddr out = static_cast<GuestAddr>(arg(1));
        const GuestAddr user = static_cast<GuestAddr>(arg(2));
        if (out == 0 || user == 0) return hr::E_POINTER;
        std::unique_lock<std::mutex> lock(queue_mutex);
        const auto ready = [&] { return !done.empty() || reader_flushing; };
        if (timeout == 0xFFFFFFFFu) {
            queue_cv.wait(lock, ready);
        } else {
            queue_cv.wait_for(lock, std::chrono::milliseconds(timeout), ready);
        }
        if (!done.empty()) {
            const Done d = done.front();
            done.pop_front();
            const bool flush = reader_flushing;
            lock.unlock();
            (void)m.write_ptr(out, d.sample);
            (void)m.write_ptr(user, d.user);
            // Measured: a request pending at BeginFlush comes back with
            // VFW_E_TIMEOUT.
            return flush ? hr::VFW_E_TIMEOUT : hr::S_OK;
        }
        const bool flush = reader_flushing;
        lock.unlock();
        (void)m.write_ptr(out, 0);
        if (flush) return hr::VFW_E_WRONG_STATE;  // *pdwUser untouched (measured)
        (void)m.write_ptr(user, 0);
        return hr::VFW_E_TIMEOUT;
    }
    case 6: {  // SyncReadAligned(IMediaSample*)
        const GuestAddr sample = static_cast<GuestAddr>(arg(0));
        if (sample == 0) return hr::E_POINTER;
        i64 start = 0, stop = 0;
        bool has_time = false;
        if (!sample_span(m, sample, start, stop, has_time)) return hr::E_OUTOFMEMORY;
        if (start % kReaderAlign != 0 || stop % kReaderAlign != 0) return hr::VFW_E_BADALIGN;
        const GuestAddr buf = host->scratch(8);
        (void)m.write_ptr(buf, 0);
        (void)host->call(m, sample, Iface::MediaSample, 3, {buf});
        const GuestAddr data = static_cast<GuestAddr>(m.read_ptr(buf).value_or(0));
        host->free_scratch(buf);
        const u32 size = host->call(m, sample, Iface::MediaSample, 4, {});
        if (stop - start > static_cast<i64>(size)) return hr::E_INVALIDARG;
        sync_reads.fetch_add(1);
        const i64 actual = read_into(m, start, stop - start, data);
        note_read(start, actual, stop - start, reader->length);
        (void)host->call(m, sample, Iface::MediaSample, 12, {static_cast<u64>(actual)});
        return actual == stop - start ? hr::S_OK : hr::S_FALSE;
    }
    case 7: {  // SyncRead(LONGLONG llPosition, LONG lLength, BYTE* pBuffer)
        const i64 position = static_cast<i64>(arg(0));
        const i64 count = static_cast<i32>(static_cast<u32>(arg(1)));
        const GuestAddr buffer = static_cast<GuestAddr>(arg(2));
        if (buffer == 0) return hr::E_POINTER;
        if (position < 0 || count < 0) return hr::E_INVALIDARG;
        sync_reads.fetch_add(1);
        const i64 got = read_into(m, position, count, buffer);
        note_read(position, got, count, reader->length);
        // Measured: a short read (or none, past the end) is S_FALSE.
        return got == count ? hr::S_OK : hr::S_FALSE;
    }
    case 8: {  // Length(LONGLONG* pTotal, LONGLONG* pAvailable)
        const GuestAddr total = static_cast<GuestAddr>(arg(0));
        const GuestAddr avail = static_cast<GuestAddr>(arg(1));
        if (total == 0 || avail == 0) return hr::E_POINTER;
        (void)m.write_u64(total, static_cast<u64>(reader->length));
        (void)m.write_u64(avail, static_cast<u64>(reader->length));
        return hr::S_OK;
    }
    case 9: {  // BeginFlush
        std::fprintf(stderr, "[quartz] File Source BeginFlush (requests=%llu sync_reads=%llu bytes=%llu)\n",
                     static_cast<unsigned long long>(requests.load()),
                     static_cast<unsigned long long>(sync_reads.load()),
                     static_cast<unsigned long long>(bytes_read.load()));
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            reader_flushing = true;
        }
        queue_cv.notify_all();
        return hr::S_OK;
    }
    case 10: {  // EndFlush
        std::lock_guard<std::mutex> lock(queue_mutex);
        reader_flushing = false;
        return hr::S_OK;
    }
    default:
        return hr::E_NOTIMPL;
    }
}

} // namespace

GuestAddr create_async_reader(DShowHost& host, Marshaler& m) {
    auto filter = std::make_shared<AsyncReaderFilter>();
    filter->clsid = sdk::CLSID_AsyncReader;
    if (!host.publish(m, filter, {Iface::BaseFilter, Iface::FileSourceFilter})) return 0;
    return filter->pointer(Iface::BaseFilter);
}

u32 async_reader_load(DShowHost& host, Marshaler& m, GuestAddr filter,
                      const std::u16string& guest_path) {
    auto found = host.find(filter);
    // Only File Source (Async) objects expose IFileSourceFilter here.
    if (found == nullptr || found->pointer(Iface::FileSourceFilter) == 0) return hr::E_POINTER;
    auto object = std::static_pointer_cast<AsyncReaderFilter>(found);
    return object->load(m, guest_path, nullptr);
}

} // namespace vitreo::winapi::dshow
