// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo -- avifil32.dll (AVIFile API).
//
// Function contracts: https://learn.microsoft.com/en-us/windows/win32/api/vfw/
// (AVIFileOpen, AVIFileInfo, AVIFileGetStream, AVIFileReadData,
// AVIStreamInfo, AVIStreamRead, AVIStreamReadFormat, AVIStreamFindSample,
// AVIStreamSampleToTime, AVIStreamTimeToSample, AVIStreamGetFrameOpen, ...).
// Everything the pages leave open -- the AVIFILEINFO / AVISTREAMINFO values
// for a given file, refcount returns, error codes, FindSample and Read edge
// cases, the GetFrame output for each lpbiWanted -- is measured by
// tools/oracle_probe/avifil32_probe.c (avifil32_probe{32,64}.expected).

#include "winapi/media/avifil32/avifil32.hpp"

#include "capsule/vfs/vfs.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/media/avifil32/avifil32_contract.hpp"
#include "winapi/media/msvfw32/msvfw32.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace vitreo::winapi {

using namespace avifil32;

namespace {

constexpr u32 kIcmDecompressGetFormat = 0x4000 + 10;
constexpr u32 kIcmDecompressQuery = 0x4000 + 11;
constexpr u32 kIcmDecompressBegin = 0x4000 + 12;
constexpr u32 kIcmDecompress = 0x4000 + 13;
constexpr u32 kIcmDecompressEnd = 0x4000 + 14;
constexpr u32 kIcmodeDecompress = 2;
constexpr u32 kIcdecompressNotKeyframe = 0x08000000;
constexpr u32 kIctypeVideo = fourcc('v', 'i', 'd', 'c');
constexpr u32 kDibHandler = fourcc('D', 'I', 'B', ' ');

void unimplemented(const char* api, const std::string& what) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    const std::string key = std::string(api) + ": " + what;
    {
        std::lock_guard lock(mutex);
        if (!seen.insert(key).second) return;
    }
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED avifil32!%s\n", key.c_str());
}

void put32(std::vector<u8>& b, usize at, u32 v) {
    b[at] = static_cast<u8>(v);
    b[at + 1] = static_cast<u8>(v >> 8);
    b[at + 2] = static_cast<u8>(v >> 16);
    b[at + 3] = static_cast<u8>(v >> 24);
}
u32 get32(const std::vector<u8>& b, usize at) {
    return static_cast<u32>(b[at]) | (static_cast<u32>(b[at + 1]) << 8) |
           (static_cast<u32>(b[at + 2]) << 16) | (static_cast<u32>(b[at + 3]) << 24);
}

std::u16string from_acp(std::string_view bytes) {
    if (bytes.empty()) return {};
    std::vector<u16> wide(bytes.size() + 1);
    auto r = codepage::multi_byte_to_wide(codepage::ansi_code_page(), 0, reinterpret_cast<const u8*>(bytes.data()),
                                bytes.size(), wide.data(), wide.size());
    return std::u16string(reinterpret_cast<const char16_t*>(wide.data()), r.count);
}

/// UTF-16 to CP_ACP bytes, NUL-terminated, at most @p cap bytes in all.
void to_acp(const std::u16string& s, u8* out, usize cap) {
    std::memset(out, 0, cap);
    if (s.empty() || cap == 0) return;
    std::vector<u8> tmp(s.size() * 4 + 4);
    auto r = codepage::wide_to_multi_byte(codepage::ansi_code_page(), 0, reinterpret_cast<const u16*>(s.data()),
                                s.size(), tmp.data(), tmp.size(), nullptr, false);
    std::memcpy(out, tmp.data(), std::min<usize>(r.count, cap - 1));
}

void put_wide(std::vector<u8>& b, usize at, const std::u16string& s, usize chars) {
    for (usize i = 0; i < chars; ++i) {
        const u16 c = i + 1 < chars && i < s.size() ? static_cast<u16>(s[i]) : 0;
        b[at + i * 2] = static_cast<u8>(c);
        b[at + i * 2 + 1] = static_cast<u8>(c >> 8);
    }
}

std::u16string read_wide_name(const std::vector<u8>& b, usize at, usize chars) {
    std::u16string s;
    for (usize i = 0; i < chars; ++i) {
        const u16 c = static_cast<u16>(b[at + i * 2] | (b[at + i * 2 + 1] << 8));
        if (c == 0) break;
        s.push_back(static_cast<char16_t>(c));
    }
    return s;
}

/// muldiv with the rounding AVIStreamSampleToTime / AVIStreamTimeToSample
/// use (measured): up when the stream has more than 1000 samples a second,
/// down otherwise.
i64 scale_round(i64 value, u64 mul, u64 div, bool up) {
    if (div == 0) return 0;
    const u64 n = static_cast<u64>(value) * mul;
    return static_cast<i64>(up ? (n + div - 1) / div : n / div);
}

std::u16string ascii16(const char* s) {
    std::u16string out;
    for (; *s; ++s) out.push_back(static_cast<char16_t>(static_cast<u8>(*s)));
    return out;
}

std::u16string type_label(u32 type) {
    if (type == streamtypeVIDEO) return u"Video";
    if (type == streamtypeAUDIO) return u"Audio";
    std::u16string s = u"'";
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char16_t>((type >> (8 * i)) & 0xFF));
    s.push_back(u'\'');
    return s;
}

std::string fourcc_text(u32 v) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((v >> (8 * i)) & 0xFF);
        s.push_back(c >= 32 && c < 127 ? c : '.');
    }
    return s;
}

/// Byte source over a capsule VFS handle (reads are positioned, so one lock
/// serializes seek+read).
class VfsSource final : public ByteSource {
public:
    VfsSource(capsule::Vfs& vfs, capsule::FileHandle h, u64 size) : vfs_(vfs), h_(h), size_(size) {}
    ~VfsSource() override { (void)vfs_.close(h_); }
    [[nodiscard]] u64 size() const override { return size_; }
    [[nodiscard]] bool read(u64 offset, void* dst, usize n) override {
        if (offset > size_ || n > size_ - offset) return false;
        std::lock_guard lock(mutex_);
        if (vfs_.seek(h_, static_cast<i64>(offset), capsule::SeekOrigin::Begin).is_err()) {
            return false;
        }
        usize done = 0;
        auto* out = static_cast<u8*>(dst);
        while (done < n) {
            auto r = vfs_.read(h_, std::span<u8>(out + done, n - done));
            if (r.is_err() || r.value() == 0) return false;
            done += r.value();
        }
        return true;
    }

private:
    capsule::Vfs& vfs_;
    capsule::FileHandle h_;
    u64 size_;
    std::mutex mutex_;
};

enum class Handler : u8 { None, Avi, Wave };

} // namespace

// What a stream method needs from a stream object of either handler.
struct Avifil32Manager::StreamView {
    bool wave{false};
    AviStreamHeader header;
    const AviStream* avi{nullptr};
    const WaveFileModel* wav{nullptr};
    ByteSource* source{nullptr};
    i64 start{0};
    i64 length{0};
};

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void Avifil32Manager::use_vfs(capsule::Vfs& vfs) {
    open_file_ = [&vfs](const std::string& path) -> OpenedFile {
        OpenedFile out;
        auto st = vfs.stat(path);
        if (st.is_err()) return out;
        out.exists = true;
        if ((st.value().attributes & capsule::FileAttributes::Directory) !=
            capsule::FileAttributes::None) {
            return out;
        }
        // AVIFileOpen's OF_SHARE_* flags do not exclude a second reader on
        // Windows (OF_SHARE_EXCLUSIVE twice succeeds, measured).
        auto h = vfs.open(path, capsule::AccessMode::Read, capsule::ShareMode::All,
                          capsule::CreationDisposition::OpenExisting);
        if (h.is_err()) return out;
        out.source = std::make_shared<VfsSource>(vfs, h.value(), st.value().size);
        return out;
    };
}

GuestAddr Avifil32Manager::alloc(u64 bytes) {
    return heap_.alloc ? heap_.alloc(bytes) : 0;
}

void Avifil32Manager::free_guest(GuestAddr at) {
    if (at != 0 && heap_.free) heap_.free(at);
}

u64 Avifil32Manager::call_guest(Marshaler& m, GuestAddr fn, std::initializer_list<u64> args) {
    if (!guest_call_ || fn == 0) return E_FAIL;
    std::vector<u64> a(args);
    return guest_call_(fn, a.data(), a.size(), m.cpu_is_64bit());
}

bool Avifil32Manager::ensure_vtables(Marshaler& m) {
    if (vtables_ready_) return true;
    if (thunks_ == nullptr) return false;
    x64_ = m.cpu_is_64bit();
    static const char* const kUnknown[] = {"QueryInterface", "AddRef", "Release"};
    struct Table {
        Iface iface;
        const char* prefix;
        std::vector<const char*> methods;
    };
    const Table tables[] = {
        {Iface::Unknown, "", {}},
        {Iface::File, "Vitreo_IAVIFile_",
         {"Info", "GetStream", "CreateStream", "WriteData", "ReadData", "EndRecord",
          "DeleteStream"}},
        {Iface::Persist, "Vitreo_IPersistFile_",
         {"GetClassID", "IsDirty", "Load", "Save", "SaveCompleted", "GetCurFile",
          "Reserved1"}},
        {Iface::Stream, "Vitreo_IAVIStream_",
         {"Create", "Info", "FindSample", "ReadFormat", "SetFormat", "Read", "Write", "Delete",
          "ReadData", "WriteData", "SetInfo"}},
        {Iface::Streaming, "Vitreo_IAVIStreaming_", {"Begin", "End"}},
        {Iface::GetFrame, "Vitreo_IGetFrame_", {"GetFrame", "Begin", "End", "SetFormat"}},
    };
    usize slots = 0;
    for (const auto& t : tables) slots += 3 + t.methods.size();
    const u32 p = ptr_size();
    const GuestAddr block = alloc(slots * p);
    if (block == 0) return false;
    GuestAddr cursor = block;
    for (const auto& t : tables) {
        vtables_[static_cast<usize>(t.iface)] = cursor;
        std::vector<std::string> names;
        for (const char* u : kUnknown) names.push_back(std::string("Vitreo_AVI_") + u);
        for (const char* meth : t.methods) names.push_back(std::string(t.prefix) + meth);
        for (const auto& name : names) {
            auto thunk = thunks_->get_thunk_address("avifil32.dll", name, true);
            if (thunk.is_err() || m.write_ptr(cursor, thunk.value()).is_err()) {
                free_guest(block);
                return false;
            }
            cursor += p;
        }
    }
    vtables_ready_ = true;
    return true;
}

// Object layouts (pointer slots), measured with QueryInterface offsets.
GuestAddr Avifil32Manager::iface_ptr(const Object& o, Iface iface) const {
    const u32 p = ptr_size();
    switch (o.kind) {
    case Kind::AviFile:
        if (iface == Iface::Unknown) return o.base;
        if (iface == Iface::File) return o.base + 3 * p;
        if (iface == Iface::Persist) return o.base + 7 * p;
        return 0;
    case Kind::AviStream:
        if (iface == Iface::Unknown) return o.base;
        if (iface == Iface::Stream) return o.base + 3 * p;
        if (iface == Iface::Streaming) return o.base + 5 * p;
        return 0;
    case Kind::WaveFile:
        if (iface == Iface::Stream) return o.base;
        if (iface == Iface::File) return o.base + p;
        if (iface == Iface::Unknown) return o.base + 2 * p;
        if (iface == Iface::Persist) return o.base + 3 * p;
        return 0;
    case Kind::GetFrame:
        return iface == Iface::GetFrame || iface == Iface::Unknown ? o.base : 0;
    }
    return 0;
}

GuestAddr Avifil32Manager::new_object(Marshaler& m, Kind kind) {
    if (!ensure_vtables(m)) return 0;
    const u32 p = ptr_size();
    u32 slots = 1;
    std::vector<Iface> ifs;
    switch (kind) {
    case Kind::AviFile: slots = 8; ifs = {Iface::Unknown, Iface::File, Iface::Persist}; break;
    case Kind::AviStream:
        slots = 6;
        ifs = {Iface::Unknown, Iface::Stream, Iface::Streaming};
        break;
    case Kind::WaveFile:
        slots = 4;
        ifs = {Iface::Stream, Iface::File, Iface::Unknown, Iface::Persist};
        break;
    case Kind::GetFrame: slots = 1; ifs = {Iface::GetFrame}; break;
    }
    const GuestAddr base = alloc(static_cast<u64>(slots) * p);
    if (base == 0) return 0;
    std::vector<u8> zero(static_cast<usize>(slots) * p, 0);
    (void)m.memory().write_bytes(base, zero.data(), zero.size());
    Object o;
    o.kind = kind;
    o.base = base;
    o.bytes = slots * p;
    for (Iface i : ifs) {
        const GuestAddr at = iface_ptr(o, i);
        (void)m.write_ptr(at, vtables_[static_cast<usize>(i)]);
        ifaces_[at] = {base, i};
    }
    if (kind == Kind::GetFrame) ifaces_[base] = {base, Iface::GetFrame};
    objects_.emplace(base, std::move(o));
    return base;
}

Avifil32Manager::Object* Avifil32Manager::find(GuestAddr ptr, Iface* iface) {
    auto it = ifaces_.find(ptr);
    if (it == ifaces_.end()) return nullptr;
    if (iface) *iface = it->second.second;
    auto o = objects_.find(it->second.first);
    return o == objects_.end() ? nullptr : &o->second;
}

const Avifil32Manager::Object* Avifil32Manager::find(GuestAddr ptr, Iface* iface) const {
    return const_cast<Avifil32Manager*>(this)->find(ptr, iface);
}

GuestAddr Avifil32Manager::public_file_ptr(const Object& o) const {
    return iface_ptr(o, Iface::File);
}
GuestAddr Avifil32Manager::public_stream_ptr(const Object& o) const {
    return iface_ptr(o, Iface::Stream);
}

usize Avifil32Manager::live_objects() const {
    std::lock_guard lock(mutex_);
    return objects_.size();
}

bool Avifil32Manager::is_ours(GuestAddr ptr) const {
    std::lock_guard lock(mutex_);
    return find(ptr) != nullptr;
}

u32 Avifil32Manager::dll_can_unload_now() {
    std::lock_guard lock(mutex_);
    return objects_.empty() ? S_OK : S_FALSE;
}

// ---------------------------------------------------------------------------
// Reference counting
// ---------------------------------------------------------------------------

void Avifil32Manager::destroy_locked(GuestAddr base,
                                     std::vector<std::unique_ptr<GetFrameState>>& dead) {
    auto it = objects_.find(base);
    if (it == objects_.end()) return;
    Object o = std::move(it->second);
    objects_.erase(it);
    for (auto i = ifaces_.begin(); i != ifaces_.end();) {
        if (i->second.first == base) {
            i = ifaces_.erase(i);
        } else {
            ++i;
        }
    }
    free_guest(o.base);
    if (o.kind == Kind::AviStream) {
        auto f = objects_.find(o.file);
        if (f != objects_.end()) {
            if (o.index < f->second.streams.size()) f->second.streams[o.index] = 0;
            (void)release_locked(f->second, dead);
        }
    } else if (o.kind == Kind::GetFrame && o.frame) {
        Iface si{};
        if (Object* s = find(o.frame->stream, &si)) (void)release_locked(*s, dead);
        dead.push_back(std::move(o.frame));
    }
}

u32 Avifil32Manager::release_locked(Object& o, std::vector<std::unique_ptr<GetFrameState>>& dead) {
    if (o.refs == 0) return 0;
    const u32 left = --o.refs;
    if (left == 0) destroy_locked(o.base, dead);
    return left;
}

void Avifil32Manager::finish_frames(Marshaler& m, std::vector<std::unique_ptr<GetFrameState>>& dead) {
    for (auto& gf : dead) {
        if (gf->hic != 0 && icm_ != nullptr) {
            (void)icm_->send(m, gf->hic, kIcmDecompressEnd, 0, 0);
            (void)icm_->close(m, gf->hic);
        }
        free_guest(gf->buffer);
        free_guest(gf->in_format);
        free_guest(gf->in_data);
        free_guest(gf->icd);
    }
    dead.clear();
}

u32 Avifil32Manager::add_ref(Marshaler& m, GuestAddr object) {
    {
        std::lock_guard lock(mutex_);
        if (Object* o = find(object)) {
            // IGetFrame::AddRef answers the count before the increment
            // (measured: AddRef, AddRef, Release, Release = 1, 2, 2, 1).
            if (o->kind == Kind::GetFrame) return o->refs++;
            return ++o->refs;
        }
    }
    return static_cast<u32>(call_slot(m, object, kAddRef, {}));
}

u32 Avifil32Manager::release(Marshaler& m, GuestAddr object) {
    std::vector<std::unique_ptr<GetFrameState>> dead;
    u32 left = 0;
    bool ours = false;
    {
        std::lock_guard lock(mutex_);
        if (Object* o = find(object)) {
            ours = true;
            left = release_locked(*o, dead);
        }
    }
    if (!ours) return static_cast<u32>(call_slot(m, object, kRelease, {}));
    finish_frames(m, dead);
    return left;
}

u64 Avifil32Manager::dispatch(Marshaler& m, GuestAddr self, u32 iface_id, u32 slot,
                              const std::vector<u64>& a) {
    auto arg = [&](usize i) { return i < a.size() ? a[i] : 0; };
    auto ptr = [&](usize i) { return static_cast<GuestAddr>(arg(i)); };
    auto i32a = [&](usize i) { return static_cast<i32>(static_cast<u32>(arg(i))); };
    if (slot == kQueryInterface) return query_interface(m, self, ptr(0), ptr(1));
    if (slot == kAddRef) return add_ref(m, self);
    if (slot == kRelease) return release(m, self);
    switch (static_cast<Iface>(iface_id)) {
    case Iface::File:
        switch (slot) {
        case kFileInfo: return file_info(m, self, ptr(0), i32a(1), true);
        case kFileGetStream: return file_get_stream(m, self, ptr(0), static_cast<u32>(arg(1)), i32a(2));
        case kFileCreateStream: return file_create_stream(m, self, ptr(0));
        case kFileWriteData: return file_write_data(self);
        case kFileReadData:
            return file_read_data(m, self, static_cast<u32>(arg(0)), ptr(1), ptr(2));
        case kFileEndRecord: return file_end_record(self);
        case kFileDeleteStream: return file_delete_stream(self);
        default: break;
        }
        break;
    case Iface::Stream:
        switch (slot) {
        case kStreamCreate: return stream_create(self);
        case kStreamInfo: return stream_info(m, self, ptr(0), i32a(1), true);
        case kStreamFindSample:
            return static_cast<u32>(stream_find_sample(self, i32a(0), static_cast<u32>(arg(1))));
        case kStreamReadFormat: return stream_read_format(m, self, i32a(0), ptr(1), ptr(2));
        case kStreamRead:
            return stream_read(m, self, i32a(0), i32a(1), ptr(2), i32a(3), ptr(4), ptr(5));
        case kStreamReadData:
            return stream_read_data(m, self, static_cast<u32>(arg(0)), ptr(1), ptr(2));
        case kStreamSetFormat:
        case kStreamWrite:
        case kStreamDelete:
        case kStreamWriteData:
        case kStreamSetInfo: return stream_write_method(self, slot);
        default: break;
        }
        break;
    case Iface::Streaming:
        if (slot == kStreamingBegin || slot == kStreamingEnd) return AVIERR_OK;
        break;
    case Iface::GetFrame:
        switch (slot) {
        case kGetFrameGetFrame: return get_frame(m, self, i32a(0));
        case kGetFrameBegin:
        case kGetFrameEnd: return AVIERR_OK;
        case kGetFrameSetFormat: return get_frame_set_format(self);
        default: break;
        }
        break;
    case Iface::Persist:
        if (slot >= kPersistGetClassID && slot <= kPersistReserved1) {
            if (slot == kPersistGetCurFile && ptr(0) != 0) (void)m.write_ptr(ptr(0), 0);
            return persist_method(self, slot);
        }
        break;
    default: break;
    }
    return E_NOTIMPL;
}

u64 Avifil32Manager::call_slot(Marshaler& m, GuestAddr object, u32 slot,
                               std::initializer_list<u64> args) {
    if (object == 0) return E_POINTER;
    {
        Iface iface{};
        bool ours = false;
        {
            std::lock_guard lock(mutex_);
            ours = find(object, &iface) != nullptr;
        }
        if (ours) return dispatch(m, object, static_cast<u32>(iface), slot, std::vector<u64>(args));
    }
    auto vtbl = m.read_ptr(object);
    if (vtbl.is_err() || vtbl.value() == 0) return E_POINTER;
    auto fn = m.read_ptr(vtbl.value() + static_cast<GuestAddr>(slot) * (m.cpu_is_64bit() ? 8 : 4));
    if (fn.is_err()) return E_POINTER;
    std::vector<u64> a;
    a.push_back(object);
    a.insert(a.end(), args.begin(), args.end());
    if (!guest_call_ || fn.value() == 0) return E_FAIL;
    return guest_call_(fn.value(), a.data(), a.size(), m.cpu_is_64bit());
}

bool Avifil32Manager::read_guid(Marshaler& m, GuestAddr at, std::array<u8, 16>& out) const {
    return at != 0 && m.memory().read_bytes(at, reinterpret_cast<Byte*>(out.data()), 16).is_ok();
}

u32 Avifil32Manager::query_interface(Marshaler& m, GuestAddr self, GuestAddr iid,
                                     GuestAddr out) {
    if (out == 0) return E_POINTER;
    std::array<u8, 16> g{};
    if (!read_guid(m, iid, g)) {
        (void)m.write_ptr(out, 0);
        return E_NOINTERFACE;
    }
    auto is = [&](u32 data1) { return g == ole_guid(data1); };
    std::lock_guard lock(mutex_);
    Object* o = find(self);
    if (o == nullptr) {
        (void)m.write_ptr(out, 0);
        return E_NOINTERFACE;
    }
    GuestAddr result = 0;
    switch (o->kind) {
    case Kind::AviFile:
        if (is(IID_IUnknown_Data1)) result = iface_ptr(*o, Iface::Unknown);
        else if (is(IID_IAVIFile_Data1)) result = iface_ptr(*o, Iface::File);
        else if (is(IID_IPersistFile_Data1) || is(IID_IAVIPersistFile_Data1))
            result = iface_ptr(*o, Iface::Persist);
        break;
    case Kind::AviStream:
        if (is(IID_IUnknown_Data1)) result = iface_ptr(*o, Iface::Unknown);
        else if (is(IID_IAVIStream_Data1)) result = iface_ptr(*o, Iface::Stream);
        else if (is(IID_IAVIStreaming_Data1)) result = iface_ptr(*o, Iface::Streaming);
        break;
    case Kind::WaveFile:
        if (is(IID_IUnknown_Data1)) result = iface_ptr(*o, Iface::Unknown);
        else if (is(IID_IAVIFile_Data1)) result = iface_ptr(*o, Iface::File);
        else if (is(IID_IAVIStream_Data1)) result = iface_ptr(*o, Iface::Stream);
        else if (is(IID_IPersistFile_Data1)) result = iface_ptr(*o, Iface::Persist);
        break;
    case Kind::GetFrame:
        if (is(IID_IUnknown_Data1) || is(IID_IGetFrame_Data1)) {
            result = o->base;
        } else if (is(IID_IAVIStream_Data1) && o->frame) {
            // The frame object hands out the stream it reads (measured).
            if (Object* s = find(o->frame->stream)) {
                ++s->refs;
                (void)m.write_ptr(out, o->frame->stream);
                return S_OK;
            }
        }
        break;
    }
    if (result == 0) {
        (void)m.write_ptr(out, 0);
        return E_NOINTERFACE;
    }
    ++o->refs;
    (void)m.write_ptr(out, result);
    return S_OK;
}

// ---------------------------------------------------------------------------
// AVIFileInit / AVIFileExit / AVIFileOpen
// ---------------------------------------------------------------------------

void Avifil32Manager::file_init(Marshaler& m) {
    if (com_.ole_initialize) com_.ole_initialize(m);
}

void Avifil32Manager::file_exit(Marshaler& m) {
    // Objects stay usable after AVIFileExit (measured); only the OLE
    // initialization AVIFileInit took is undone.
    if (com_.ole_uninitialize) com_.ole_uninitialize(m);
}

u32 Avifil32Manager::open_handler(Marshaler& m, const std::u16string& path, u32 mode,
                                  GuestAddr handler_clsid, GuestAddr& out) {
    (void)mode;
    out = 0;
    Handler handler = Handler::None;
    bool au = false;
    if (handler_clsid != 0) {
        std::array<u8, 16> g{};
        if (!read_guid(m, handler_clsid, g)) return REGDB_E_CLASSNOTREG;
        if (g == ole_guid(CLSID_AVIFile_Data1)) handler = Handler::Avi;
        else if (g == ole_guid(CLSID_WaveFile_Data1)) handler = Handler::Wave;
        else return REGDB_E_CLASSNOTREG;
    }
    const std::string utf8 = utf16_to_utf8(reinterpret_cast<const u16*>(path.c_str()),
                                           static_cast<int>(path.size()));
    OpenedFile opened;
    if (open_file_) opened = open_file_(utf8);

    // HKCR\AVIFile\RIFFHandlers (AVI, WAVE) by content, then
    // HKCR\AVIFile\Extensions (AVI, WAV, AU) by name; the BMP/DIB/FLC/FLI/
    // TGA entries name classes Windows no longer registers.
    if (handler == Handler::None && opened.source) {
        const u32 form = riff_form_type(*opened.source);
        if (form == fourcc('A', 'V', 'I', ' ')) handler = Handler::Avi;
        else if (form == fourcc('W', 'A', 'V', 'E')) handler = Handler::Wave;
    }
    const usize slash = path.find_last_of(u"\\/");
    const std::u16string leaf = slash == std::u16string::npos ? path : path.substr(slash + 1);
    if (handler == Handler::None) {
        const usize dot = leaf.rfind(u'.');
        std::string ext;
        if (dot != std::u16string::npos) {
            for (char16_t c : leaf.substr(dot + 1)) {
                ext.push_back(static_cast<char>(c < 128 ? std::toupper(static_cast<int>(c)) : '?'));
            }
        }
        if (ext == "AVI") handler = Handler::Avi;
        else if (ext == "WAV") handler = Handler::Wave;
        else if (ext == "AU") {
            handler = Handler::Wave;
            au = true;
        }
    }
    if (handler == Handler::None) return REGDB_E_CLASSNOTREG;
    if (!opened.source) return AVIERR_FILEOPEN;

    if (handler == Handler::Avi) {
        auto model = std::make_shared<AviFileModel>();
        const u32 hr = parse_avi(*opened.source, *model);
        if (hr != AVIERR_OK) return hr;
        std::lock_guard lock(mutex_);
        const GuestAddr base = new_object(m, Kind::AviFile);
        if (base == 0) return AVIERR_MEMORY;
        Object& o = objects_.at(base);
        o.source = opened.source;
        o.avi = std::move(model);
        o.file_name = leaf;
        o.streams.assign(o.avi->streams.size(), 0);
        out = public_file_ptr(o);
        return AVIERR_OK;
    }
    auto wave = std::make_shared<WaveFileModel>();
    if (parse_wave(*opened.source, *wave) != AVIERR_OK) {
        if (au && riff_form_type(*opened.source) == 0) {
            unimplemented("AVIFileOpen", "Sun .au audio files (WAVE file handler)");
        }
        return AVIERR_BADFORMAT;
    }
    std::lock_guard lock(mutex_);
    const GuestAddr base = new_object(m, Kind::WaveFile);
    if (base == 0) return AVIERR_MEMORY;
    Object& o = objects_.at(base);
    o.source = opened.source;
    o.wave = std::move(wave);
    o.file_name = leaf;
    out = public_file_ptr(o);
    return AVIERR_OK;
}

u32 Avifil32Manager::file_open(Marshaler& m, GuestAddr ppfile, GuestAddr name, bool wide,
                               u32 mode, GuestAddr handler) {
    if (ppfile == 0) return AVIERR_BADPARAM;
    (void)m.write_ptr(ppfile, 0);
    std::u16string path;
    if (name != 0) {
        if (wide) {
            auto s = m.read_wide_string(name, 32768);
            if (s.is_ok()) {
                auto w = utf8_to_utf16(s.value());
                path.assign(reinterpret_cast<const char16_t*>(w.data()), w.size());
            }
        } else {
            auto s = m.read_ansi_string(name, 32768);
            if (s.is_ok()) path = from_acp(s.value());
        }
    }
    if (com_.thread_in_apartment && !com_.thread_in_apartment(m)) return CO_E_NOTINITIALIZED;
    if ((mode & (OF_WRITE | OF_READWRITE | OF_CREATE)) != 0) {
        unimplemented(wide ? "AVIFileOpenW" : "AVIFileOpenA",
                      "OF_WRITE / OF_READWRITE / OF_CREATE (writing AVI files)");
        return AVIERR_UNSUPPORTED;
    }
    GuestAddr out = 0;
    const u32 hr = open_handler(m, path, mode, handler, out);
    if (hr == AVIERR_OK) (void)m.write_ptr(ppfile, out);
    return hr;
}

u32 Avifil32Manager::stream_open_from_file(Marshaler& m, GuestAddr ppavi, GuestAddr name,
                                           bool wide, u32 type, i32 lparam, u32 mode,
                                           GuestAddr handler) {
    if (ppavi == 0) return AVIERR_BADPARAM;
    // AVIFileOpen into a temporary: on its failure *ppavi is untouched
    // (measured), on AVIFileGetStream's failure it is NULL.
    const GuestAddr slot = alloc(8);
    if (slot == 0) return AVIERR_MEMORY;
    u32 hr = file_open(m, slot, name, wide, mode, handler);
    if (hr == AVIERR_OK) {
        auto file = m.read_ptr(slot);
        const GuestAddr f = file.is_ok() ? file.value() : 0;
        hr = static_cast<u32>(call_slot(m, f, kFileGetStream, {ppavi, type, static_cast<u32>(lparam)}));
        (void)release(m, f);
    }
    free_guest(slot);
    return hr;
}

// ---------------------------------------------------------------------------
// IAVIFile
// ---------------------------------------------------------------------------

u32 Avifil32Manager::file_info(Marshaler& m, GuestAddr self, GuestAddr info, i32 size,
                               bool wide) {
    const u32 needed = wide ? kAviFileInfoWSize : kAviFileInfoASize;
    if (size < 0 || static_cast<u32>(size) < needed) return AVIERR_BADSIZE;
    std::vector<u8> b(needed, 0);
    std::u16string type;
    {
        std::lock_guard lock(mutex_);
        const Object* o = find(self);
        if (o == nullptr) return E_POINTER;
        if (o->kind == Kind::AviFile) {
            const AviMainHeader& h = o->avi->header;
            const bool interleaved = (h.flags & AVIFILEINFO_ISINTERLEAVED) != 0;
            put32(b, 0, h.max_bytes_per_sec);
            put32(b, 4, h.flags & AVIFILEINFO_ISINTERLEAVED);
            put32(b, 8, AVIFILECAPS_CANREAD | AVIFILECAPS_CANWRITE);
            put32(b, 12, static_cast<u32>(o->avi->streams.size()));
            put32(b, 16, interleaved ? 0 : h.suggested_buffer);
            put32(b, 20, h.width);
            put32(b, 24, h.height);
            put32(b, 28, h.usec_per_frame);
            put32(b, 32, 1000000);
            put32(b, 36, h.total_frames);
            type = u"AVI Default File Handler";
        } else if (o->kind == Kind::WaveFile) {
            const WaveFileModel& w = *o->wave;
            put32(b, 12, 1);
            put32(b, 28, 1);
            put32(b, 32, w.samples_per_sec);
            put32(b, 36, w.data_size / w.block_align);
            type = u"Microsoft Waveform";
        } else {
            return E_POINTER;
        }
    }
    if (wide) put_wide(b, kAviFileInfoTypeOffset, type, kAviNameChars);
    else to_acp(type, b.data() + kAviFileInfoTypeOffset, kAviNameChars);
    if (m.memory().write_bytes(info, b.data(), b.size()).is_err()) return AVIERR_BADPARAM;
    return AVIERR_OK;
}

u32 Avifil32Manager::flat_file_info(Marshaler& m, GuestAddr file, GuestAddr info, i32 size,
                                    bool wide) {
    if (is_ours(file)) return file_info(m, file, info, size, wide);
    if (wide) return static_cast<u32>(call_slot(m, file, kFileInfo, {info, static_cast<u32>(size)}));
    if (size < 0 || static_cast<u32>(size) < kAviFileInfoASize) return AVIERR_BADSIZE;
    const GuestAddr tmp = alloc(kAviFileInfoWSize);
    if (tmp == 0) return AVIERR_MEMORY;
    const u32 hr = static_cast<u32>(call_slot(m, file, kFileInfo, {tmp, kAviFileInfoWSize}));
    if (hr == AVIERR_OK) {
        std::vector<u8> w(kAviFileInfoWSize), a(kAviFileInfoASize, 0);
        (void)m.memory().read_bytes(tmp, reinterpret_cast<Byte*>(w.data()), w.size());
        std::memcpy(a.data(), w.data(), kAviFileInfoTypeOffset);
        to_acp(read_wide_name(w, kAviFileInfoTypeOffset, kAviNameChars),
               a.data() + kAviFileInfoTypeOffset, kAviNameChars);
        (void)m.memory().write_bytes(info, a.data(), a.size());
    }
    free_guest(tmp);
    return hr;
}

u32 Avifil32Manager::file_get_stream(Marshaler& m, GuestAddr self, GuestAddr pp, u32 type,
                                     i32 lparam) {
    if (pp == 0) return AVIERR_BADPARAM;
    std::lock_guard lock(mutex_);
    Object* o = find(self);
    if (o == nullptr) return E_POINTER;
    if (o->kind == Kind::WaveFile) {
        // The WAVE handler is its own single stream; anything else is
        // AVIERR_BADPARAM with *ppStream untouched (measured).
        if ((type != 0 && type != streamtypeAUDIO) || lparam != 0) return AVIERR_BADPARAM;
        ++o->refs;
        (void)m.write_ptr(pp, iface_ptr(*o, Iface::Stream));
        return AVIERR_OK;
    }
    if (o->kind != Kind::AviFile) return E_POINTER;
    i64 index = -1;
    if (lparam >= 0) {
        i32 seen = 0;
        for (usize i = 0; i < o->avi->streams.size(); ++i) {
            if (type != 0 && o->avi->streams[i].header.fcc_type != type) continue;
            if (seen++ == lparam) {
                index = static_cast<i64>(i);
                break;
            }
        }
    }
    if (index < 0) {
        (void)m.write_ptr(pp, 0);
        return AVIERR_NODATA;
    }
    const usize i = static_cast<usize>(index);
    if (o->streams[i] != 0) {
        Object& s = objects_.at(o->streams[i]);
        ++s.refs;
        (void)m.write_ptr(pp, public_stream_ptr(s));
        return AVIERR_OK;
    }
    const GuestAddr file_base = o->base;
    const GuestAddr base = new_object(m, Kind::AviStream);
    if (base == 0) {
        (void)m.write_ptr(pp, 0);
        return AVIERR_MEMORY;
    }
    Object& file = objects_.at(file_base);
    Object& s = objects_.at(base);
    s.file = file_base;
    s.index = static_cast<u32>(i);
    file.streams[i] = base;
    ++file.refs; // a stream keeps its file alive (measured)
    (void)m.write_ptr(pp, public_stream_ptr(s));
    return AVIERR_OK;
}

u32 Avifil32Manager::file_create_stream(Marshaler& m, GuestAddr self, GuestAddr pp) {
    (void)self;
    // A file opened for reading: AVIERR_UNSUPPORTED, *ppStream NULL (both
    // handlers, measured).
    if (pp != 0) (void)m.write_ptr(pp, 0);
    return AVIERR_UNSUPPORTED;
}

u32 Avifil32Manager::flat_file_create_stream(Marshaler& m, GuestAddr file, GuestAddr pp,
                                             GuestAddr info, bool wide) {
    if (is_ours(file)) return file_create_stream(m, file, pp);
    if (wide) return static_cast<u32>(call_slot(m, file, kFileCreateStream, {pp, info}));
    unimplemented("AVIFileCreateStreamA", "an IAVIFile the application implements");
    if (pp != 0) (void)m.write_ptr(pp, 0);
    return AVIERR_UNSUPPORTED;
}

u32 Avifil32Manager::file_write_data(GuestAddr self) {
    std::lock_guard lock(mutex_);
    const Object* o = find(self);
    // The WAVE handler accepts and drops it; a read-only AVI file refuses.
    return o != nullptr && o->kind == Kind::WaveFile ? AVIERR_OK : AVIERR_READONLY;
}

u32 Avifil32Manager::file_end_record(GuestAddr self) { return file_write_data(self); }

u32 Avifil32Manager::file_delete_stream(GuestAddr self) {
    std::lock_guard lock(mutex_);
    const Object* o = find(self);
    return o != nullptr && o->kind == Kind::WaveFile ? AVIERR_BADPARAM : AVIERR_UNSUPPORTED;
}

namespace {
/// IAVIFile::ReadData / IAVIStream::ReadData over a chunk list (measured:
/// a NULL buffer asks the size; a short buffer gets the first *lpcb bytes
/// and S_OK; *lpcb always becomes the chunk size; AVIERR_NODATA sets 0).
u32 read_data_chunk(Marshaler& m, const std::vector<DataChunk>& chunks, u32 ckid,
                    GuestAddr data, GuestAddr pcb) {
    if (pcb == 0) return AVIERR_BADPARAM;
    auto cb = m.read_u32(pcb);
    if (cb.is_err()) return AVIERR_BADPARAM;
    for (const auto& c : chunks) {
        if (c.ckid != ckid) continue;
        if (data != 0) {
            const usize n = std::min<usize>(static_cast<i32>(cb.value()) < 0 ? 0 : cb.value(),
                                            c.data.size());
            if (n != 0 && m.memory().write_bytes(data, c.data.data(), n).is_err()) {
                return AVIERR_BADPARAM;
            }
        }
        (void)m.write_u32(pcb, static_cast<u32>(c.data.size()));
        return AVIERR_OK;
    }
    (void)m.write_u32(pcb, 0);
    return AVIERR_NODATA;
}
} // namespace

u32 Avifil32Manager::file_read_data(Marshaler& m, GuestAddr self, u32 ckid, GuestAddr data,
                                    GuestAddr pcb) {
    std::vector<DataChunk> chunks;
    {
        std::lock_guard lock(mutex_);
        const Object* o = find(self);
        if (o == nullptr) return E_POINTER;
        if (o->kind == Kind::AviFile) chunks = o->avi->extra;
    }
    return read_data_chunk(m, chunks, ckid, data, pcb);
}

u32 Avifil32Manager::persist_method(GuestAddr self, u32 slot) {
    Kind kind = Kind::AviFile;
    {
        std::lock_guard lock(mutex_);
        const Object* o = find(self);
        if (o != nullptr) kind = o->kind;
    }
    switch (slot) {
    case kPersistGetClassID: return S_OK; // returns S_OK and leaves *pClassID (measured)
    case kPersistIsDirty: return S_FALSE;
    case kPersistGetCurFile: return kind == Kind::WaveFile ? E_FAIL : E_NOTIMPL;
    case kPersistLoad: unimplemented("IPersistFile::Load", "reloading an open AVIFile"); break;
    case kPersistSave: unimplemented("IPersistFile::Save", "writing AVI files"); break;
    case kPersistSaveCompleted:
        unimplemented("IPersistFile::SaveCompleted", "writing AVI files");
        break;
    default: unimplemented("IAVIPersistFile::Reserved1", "undocumented method"); break;
    }
    return E_NOTIMPL;
}

// ---------------------------------------------------------------------------
// IAVIStream
// ---------------------------------------------------------------------------

bool Avifil32Manager::view_of(GuestAddr stream, StreamView& v) {
    Iface iface{};
    Object* o = find(stream, &iface);
    if (o == nullptr) return false;
    if (o->kind == Kind::WaveFile) {
        const WaveFileModel& w = *o->wave;
        v.wave = true;
        v.wav = o->wave.get();
        v.source = o->source.get();
        v.header = {};
        v.header.fcc_type = streamtypeAUDIO;
        v.header.scale = w.block_align;
        v.header.rate = w.avg_bytes_per_sec;
        v.header.length = w.data_size / w.block_align;
        v.header.sample_size = w.block_align;
        v.start = 0;
        v.length = v.header.length;
        return true;
    }
    if (o->kind != Kind::AviStream) return false;
    const Object& file = objects_.at(o->file);
    v.wave = false;
    v.avi = &file.avi->streams[o->index];
    v.source = file.source.get();
    v.header = v.avi->header;
    v.start = v.header.start;
    v.length = v.header.length;
    return true;
}

bool Avifil32Manager::fill_stream_info(const Object& o, std::vector<u8>& b) {
    b.assign(kAviStreamInfoWSize, 0);
    std::u16string name;
    AviStreamHeader h;
    if (o.kind == Kind::WaveFile) {
        const WaveFileModel& w = *o.wave;
        h.fcc_type = streamtypeAUDIO;
        h.scale = w.block_align;
        h.rate = w.avg_bytes_per_sec;
        h.length = w.data_size / w.block_align;
        h.sample_size = w.block_align;
        name = u"Microsoft Waveform: " + o.file_name;
    } else if (o.kind == Kind::AviStream) {
        const Object& file = objects_.at(o.file);
        const AviStream& s = file.avi->streams[o.index];
        h = s.header;
        BitmapInfoHeader bih;
        if (h.fcc_type == streamtypeVIDEO && h.fcc_handler == 0 && read_bih(s.format, bih) &&
            bih.compression == BI_RGB) {
            h.fcc_handler = kDibHandler; // measured: 0 reads back as 'DIB '
        }
        // dwSuggestedBufferSize is the largest chunk in the index (measured:
        // a strh value of 16 over a single 14-byte chunk reads back 14).
        if (!s.chunks.empty()) {
            u32 largest = 0;
            for (const auto& c : s.chunks) largest = std::max(largest, c.size);
            h.suggested_buffer = largest;
        }
        if (s.has_name) {
            name = from_acp(s.name);
        } else {
            u32 ordinal = 0;
            for (u32 i = 0; i <= o.index; ++i) {
                if (file.avi->streams[i].header.fcc_type == s.header.fcc_type) ++ordinal;
            }
            name = file.file_name + u" " + type_label(s.header.fcc_type) + u" #" +
                   ascii16(std::to_string(ordinal).c_str());
        }
    } else {
        return false;
    }
    put32(b, 0, h.fcc_type);
    put32(b, 4, h.fcc_handler);
    put32(b, 8, h.flags);
    put32(b, 12, 0); // dwCaps
    b[16] = static_cast<u8>(h.priority);
    b[17] = static_cast<u8>(h.priority >> 8);
    b[18] = static_cast<u8>(h.language);
    b[19] = static_cast<u8>(h.language >> 8);
    put32(b, 20, h.scale);
    put32(b, 24, h.rate);
    put32(b, 28, h.start);
    put32(b, 32, h.length);
    put32(b, 36, h.initial_frames);
    put32(b, 40, h.suggested_buffer);
    put32(b, 44, h.quality);
    put32(b, 48, h.sample_size);
    for (usize i = 0; i < 4; ++i) put32(b, 52 + i * 4, static_cast<u32>(static_cast<i32>(h.frame[i])));
    put32(b, 68, 0); // dwEditCount
    put32(b, 72, 0); // dwFormatChangeCount
    put_wide(b, kAviStreamInfoNameOffset, name, kAviNameChars);
    return true;
}

u32 Avifil32Manager::stream_info(Marshaler& m, GuestAddr self, GuestAddr info, i32 size,
                                 bool wide) {
    const u32 needed = wide ? kAviStreamInfoWSize : kAviStreamInfoASize;
    if (size < 0 || static_cast<u32>(size) < needed) return AVIERR_BADSIZE;
    std::vector<u8> w;
    {
        std::lock_guard lock(mutex_);
        const Object* o = find(self);
        if (o == nullptr || !fill_stream_info(*o, w)) return E_POINTER;
    }
    std::vector<u8> out = w;
    if (!wide) {
        out.assign(kAviStreamInfoASize, 0);
        std::memcpy(out.data(), w.data(), kAviStreamInfoNameOffset);
        to_acp(read_wide_name(w, kAviStreamInfoNameOffset, kAviNameChars),
               out.data() + kAviStreamInfoNameOffset, kAviNameChars);
    }
    if (m.memory().write_bytes(info, out.data(), out.size()).is_err()) return AVIERR_BADPARAM;
    return AVIERR_OK;
}

bool Avifil32Manager::foreign_info_w(Marshaler& m, GuestAddr stream, std::vector<u8>& w) {
    const GuestAddr tmp = alloc(kAviStreamInfoWSize);
    if (tmp == 0) return false;
    const u32 hr = static_cast<u32>(call_slot(m, stream, kStreamInfo, {tmp, kAviStreamInfoWSize}));
    w.assign(kAviStreamInfoWSize, 0);
    const bool ok = hr == AVIERR_OK &&
                    m.memory().read_bytes(tmp, reinterpret_cast<Byte*>(w.data()), w.size()).is_ok();
    free_guest(tmp);
    return ok;
}

u32 Avifil32Manager::flat_stream_info(Marshaler& m, GuestAddr stream, GuestAddr info, i32 size,
                                      bool wide) {
    if (is_ours(stream)) return stream_info(m, stream, info, size, wide);
    if (wide) return static_cast<u32>(call_slot(m, stream, kStreamInfo, {info, static_cast<u32>(size)}));
    if (size < 0 || static_cast<u32>(size) < kAviStreamInfoASize) return AVIERR_BADSIZE;
    std::vector<u8> w;
    if (!foreign_info_w(m, stream, w)) return AVIERR_ERROR;
    std::vector<u8> a(kAviStreamInfoASize, 0);
    std::memcpy(a.data(), w.data(), kAviStreamInfoNameOffset);
    to_acp(read_wide_name(w, kAviStreamInfoNameOffset, kAviNameChars),
           a.data() + kAviStreamInfoNameOffset, kAviNameChars);
    return m.memory().write_bytes(info, a.data(), a.size()).is_ok() ? AVIERR_OK : AVIERR_BADPARAM;
}

u32 Avifil32Manager::stream_create(GuestAddr self) {
    (void)self;
    return AVIERR_UNSUPPORTED; // both handlers (measured)
}

namespace {

/// The chunk holding stream position @p pos, or -1 (outside
/// [dwStart, dwStart + dwLength) or not in the index).
i64 chunk_at(const AviStream& s, i64 pos) {
    const i64 start = s.header.start;
    if (pos < start || pos >= start + static_cast<i64>(s.header.length)) return -1;
    if (s.header.sample_size == 0) {
        const i64 k = pos - start;
        return k < static_cast<i64>(s.chunks.size()) ? k : -1;
    }
    auto it = std::upper_bound(s.chunks.begin(), s.chunks.end(), pos,
                               [](i64 p, const StreamChunk& c) { return p < c.first; });
    if (it == s.chunks.begin()) return -1;
    --it;
    if (pos >= it->first + static_cast<i64>(it->samples)) return -1;
    return it - s.chunks.begin();
}

/// The stream's format at chunk @p k: strf with every palette change the
/// index places before that chunk applied.
std::vector<u8> format_at(const AviStream& s, i64 k) {
    std::vector<u8> f = s.format;
    if (k < 0 || s.palette_changes.empty()) return f;
    const u32 ordinal = s.chunks[static_cast<usize>(k)].ordinal;
    for (const auto& pc : s.palette_changes) {
        if (pc.ordinal < ordinal) apply_palette_change(f, pc.data);
    }
    return f;
}

} // namespace

i32 Avifil32Manager::stream_find_sample(GuestAddr self, i32 pos, u32 flags) {
    std::lock_guard lock(mutex_);
    StreamView v;
    if (!view_of(self, v)) return -1;
    if (v.wave) {
        // The WAVE handler answers the position itself; FIND_FORMAT finds
        // the single format at 0 (measured).
        if ((flags & FIND_FORMAT) != 0) {
            if ((flags & FIND_NEXT) != 0) return pos <= 0 ? 0 : -1;
            return 0;
        }
        return pos;
    }
    const AviStream& s = *v.avi;
    i64 p = pos;
    // Measured: FIND_FROM_START with position 0 searches from dwStart.
    if ((flags & FIND_FROM_START) != 0 && p == 0) p = v.start;
    const i64 k = chunk_at(s, p);
    if (k < 0) return -1;
    const bool next = (flags & FIND_NEXT) != 0;
    if ((flags & FIND_FORMAT) != 0) {
        if (s.palette_changes.empty()) {
            if (!next) return static_cast<i32>(v.start);
            return p <= v.start ? static_cast<i32>(v.start) : -1;
        }
        const u32 ordinal = s.chunks[static_cast<usize>(k)].ordinal;
        if (next) {
            for (const auto& pc : s.palette_changes) {
                if (pc.ordinal > ordinal) return static_cast<i32>(pc.pos);
            }
        } else {
            for (auto it = s.palette_changes.rbegin(); it != s.palette_changes.rend(); ++it) {
                if (it->ordinal < ordinal) return static_cast<i32>(it->pos);
            }
        }
        return -1;
    }
    i64 found_chunk = k;
    i64 found = p;
    const bool any = (flags & FIND_ANY) != 0;
    if (!any && s.header.sample_size == 0) {
        found_chunk = -1;
        if (next) {
            for (usize i = static_cast<usize>(k); i < s.chunks.size(); ++i) {
                if (s.chunks[i].key) {
                    found_chunk = static_cast<i64>(i);
                    break;
                }
            }
        } else {
            for (i64 i = k; i >= 0; --i) {
                if (s.chunks[static_cast<usize>(i)].key) {
                    found_chunk = i;
                    break;
                }
            }
        }
        if (found_chunk < 0) return -1;
        found = s.chunks[static_cast<usize>(found_chunk)].first;
        if (found >= v.start + v.length) return -1;
    }
    const StreamChunk& c = s.chunks[static_cast<usize>(found_chunk)];
    switch (flags & FIND_RET) {
    case FIND_LENGTH: return static_cast<i32>(c.size);
    case FIND_OFFSET: return static_cast<i32>(c.offset);
    case FIND_SIZE: return static_cast<i32>(c.samples);
    case FIND_INDEX: return static_cast<i32>(c.ordinal);
    default: return static_cast<i32>(found);
    }
}

u32 Avifil32Manager::stream_read_format(Marshaler& m, GuestAddr self, i32 pos, GuestAddr format,
                                        GuestAddr pcb) {
    if (pcb == 0) return AVIERR_BADPARAM;
    std::vector<u8> f;
    bool wave = false;
    {
        std::lock_guard lock(mutex_);
        StreamView v;
        if (!view_of(self, v)) return E_POINTER;
        wave = v.wave;
        f = v.wave ? v.wav->format : format_at(*v.avi, chunk_at(*v.avi, pos));
    }
    auto cb = m.read_u32(pcb);
    if (cb.is_err()) return AVIERR_BADPARAM;
    const i32 have = static_cast<i32>(cb.value());
    const u32 size = static_cast<u32>(f.size());
    if (format == 0) {
        (void)m.write_u32(pcb, size);
        return AVIERR_OK;
    }
    if (have < 0 || static_cast<u32>(have) < size) {
        // The first *lpcbFormat bytes are copied either way; the AVI handler
        // then reports the full size, the WAVE handler leaves it (measured).
        const usize n = have < 0 ? 0 : static_cast<usize>(have);
        if (n != 0) (void)m.memory().write_bytes(format, f.data(), n);
        if (!wave) (void)m.write_u32(pcb, size);
        return AVIERR_BUFFERTOOSMALL;
    }
    if (size != 0 && m.memory().write_bytes(format, f.data(), size).is_err()) {
        return AVIERR_BADPARAM;
    }
    (void)m.write_u32(pcb, size);
    return AVIERR_OK;
}

u32 Avifil32Manager::stream_read(Marshaler& m, GuestAddr self, i32 start, i32 samples,
                                 GuestAddr buffer, i32 cb, GuestAddr pbytes, GuestAddr psamples) {
    auto outs = [&](i64 bytes, i64 n) {
        if (pbytes != 0) (void)m.write_u32(pbytes, static_cast<u32>(bytes));
        if (psamples != 0) (void)m.write_u32(psamples, static_cast<u32>(n));
    };
    std::lock_guard lock(mutex_);
    StreamView v;
    if (!view_of(self, v)) return E_POINTER;
    const i64 cap = cb < 0 ? 0 : cb;

    if (v.wave) {
        // Measured: positions outside the data read nothing with S_OK;
        // lSamples 0 reads one sample and AVISTREAMREAD_CONVENIENT none; a
        // buffer limits the count to whole blocks it can hold.
        const u32 block = v.wav->block_align;
        i64 n = samples == 0 ? 1 : (samples < 0 ? 0 : samples);
        if (start < 0 || start >= v.length) n = 0;
        n = std::min<i64>(n, v.length - std::max<i64>(start, 0));
        if (n < 0) n = 0;
        if (buffer != 0) n = std::min<i64>(n, cap / block);
        const i64 bytes = n * block;
        if (buffer != 0 && bytes != 0) {
            std::vector<u8> data(static_cast<usize>(bytes));
            if (!v.source->read(v.wav->data_offset + static_cast<u64>(start) * block, data.data(),
                                data.size())) {
                return AVIERR_FILEREAD;
            }
            if (m.memory().write_bytes(buffer, data.data(), data.size()).is_err()) {
                return AVIERR_BADPARAM;
            }
        }
        outs(bytes, n);
        return AVIERR_OK;
    }

    const AviStream& s = *v.avi;
    if (start < v.start) return AVIERR_BADPARAM; // outputs untouched (measured)
    const i64 k = chunk_at(s, start);
    if (k < 0) {
        // Past the end, or a position the index does not reach: a size query
        // fails with AVIERR_ERROR, a read with AVIERR_FILEREAD (measured).
        outs(0, 0);
        return buffer == 0 ? AVIERR_ERROR : AVIERR_FILEREAD;
    }
    const StreamChunk& c = s.chunks[static_cast<usize>(k)];
    if (s.header.sample_size == 0) {
        // One variable-size sample per call whatever lSamples asks.
        if (buffer == 0) {
            outs(c.size, 1);
            return AVIERR_OK;
        }
        if (cap < c.size) {
            outs(c.size, 0);
            return AVIERR_BUFFERTOOSMALL;
        }
        std::vector<u8> data(c.size);
        if (c.size != 0 && !v.source->read(c.offset, data.data(), data.size())) {
            outs(0, 0);
            return AVIERR_FILEREAD;
        }
        if (c.size != 0 && m.memory().write_bytes(buffer, data.data(), data.size()).is_err()) {
            return AVIERR_BADPARAM;
        }
        outs(c.size, 1);
        return AVIERR_OK;
    }
    // Fixed-size samples: AVISTREAMREAD_CONVENIENT reads to the end of the
    // chunk; a count crosses chunks; a buffer holds whole samples.
    const u32 ss = s.header.sample_size;
    const i64 end = std::min<i64>(v.start + v.length, v.start + static_cast<i64>(s.total_samples));
    i64 n = samples == AVISTREAMREAD_CONVENIENT ? c.first + c.samples - start
                                                : (samples <= 0 ? 1 : samples);
    n = std::min<i64>(n, end - start);
    if (buffer != 0) {
        n = std::min<i64>(n, cap / ss);
        if (n == 0) {
            outs(ss, 0);
            return AVIERR_BUFFERTOOSMALL;
        }
    }
    if (buffer != 0) {
        std::vector<u8> data(static_cast<usize>(n) * ss);
        usize done = 0;
        i64 pos = start;
        for (usize i = static_cast<usize>(k); i < s.chunks.size() && done < data.size(); ++i) {
            const StreamChunk& ch = s.chunks[i];
            const u64 skip = static_cast<u64>(pos - ch.first) * ss;
            const usize take = std::min<usize>(data.size() - done, ch.size - skip);
            if (take != 0 && !v.source->read(ch.offset + skip, data.data() + done, take)) {
                outs(0, 0);
                return AVIERR_FILEREAD;
            }
            done += take;
            pos = ch.first + ch.samples;
        }
        if (!data.empty() && m.memory().write_bytes(buffer, data.data(), data.size()).is_err()) {
            return AVIERR_BADPARAM;
        }
    }
    outs(n * ss, n);
    return AVIERR_OK;
}

u32 Avifil32Manager::stream_read_data(Marshaler& m, GuestAddr self, u32 ckid, GuestAddr data,
                                      GuestAddr pcb) {
    std::vector<DataChunk> chunks;
    {
        std::lock_guard lock(mutex_);
        StreamView v;
        if (!view_of(self, v)) return E_POINTER;
        // The WAVE handler has no stream data: AVIERR_UNSUPPORTED, *lpcb
        // untouched (measured).
        if (v.wave) return AVIERR_UNSUPPORTED;
        chunks = v.avi->extra;
    }
    return read_data_chunk(m, chunks, ckid, data, pcb);
}

u32 Avifil32Manager::stream_write_method(GuestAddr self, u32 slot) {
    std::lock_guard lock(mutex_);
    StreamView v;
    const bool wave = view_of(self, v) && v.wave;
    // A stream of a file opened for reading (measured): the AVI handler
    // answers AVIERR_READONLY, the WAVE handler AVIERR_UNSUPPORTED except
    // for Write, which is AVIERR_READONLY. Outputs are left untouched.
    if (slot == kStreamWrite) return AVIERR_READONLY;
    return wave ? AVIERR_UNSUPPORTED : AVIERR_READONLY;
}

// ---------------------------------------------------------------------------
// Flat stream helpers (AVIStreamStart, Length, SampleToTime, TimeToSample)
// ---------------------------------------------------------------------------

namespace {
struct InfoNumbers {
    u32 scale{0}, rate{0}, start{0}, length{0};
};
InfoNumbers numbers_of(const std::vector<u8>& w) {
    return {get32(w, 20), get32(w, 24), get32(w, 28), get32(w, 32)};
}
} // namespace

i32 Avifil32Manager::stream_start(Marshaler& m, GuestAddr stream) {
    std::vector<u8> w;
    {
        std::lock_guard lock(mutex_);
        const Object* o = find(stream);
        if (o != nullptr) {
            if (!fill_stream_info(*o, w)) return -1;
            return static_cast<i32>(numbers_of(w).start);
        }
    }
    return foreign_info_w(m, stream, w) ? static_cast<i32>(numbers_of(w).start) : -1;
}

i32 Avifil32Manager::stream_length(Marshaler& m, GuestAddr stream) {
    std::vector<u8> w;
    {
        std::lock_guard lock(mutex_);
        const Object* o = find(stream);
        if (o != nullptr) {
            if (!fill_stream_info(*o, w)) return -1;
            return static_cast<i32>(numbers_of(w).length);
        }
    }
    return foreign_info_w(m, stream, w) ? static_cast<i32>(numbers_of(w).length) : -1;
}

i32 Avifil32Manager::sample_to_time(Marshaler& m, GuestAddr stream, i32 sample) {
    std::vector<u8> w;
    bool ok = false;
    {
        std::lock_guard lock(mutex_);
        if (const Object* o = find(stream)) ok = fill_stream_info(*o, w);
    }
    if (w.empty()) ok = foreign_info_w(m, stream, w);
    if (!ok) return -1;
    const InfoNumbers n = numbers_of(w);
    if (n.rate == 0) return -1;
    // Clamped to [dwStart, dwStart + dwLength] (measured).
    i64 s = std::clamp<i64>(sample, n.start, static_cast<i64>(n.start) + n.length);
    const bool up = static_cast<u64>(n.rate) > static_cast<u64>(n.scale) * 1000;
    return static_cast<i32>(scale_round(s, static_cast<u64>(n.scale) * 1000, n.rate, up));
}

i32 Avifil32Manager::time_to_sample(Marshaler& m, GuestAddr stream, i32 time) {
    std::vector<u8> w;
    bool ok = false;
    {
        std::lock_guard lock(mutex_);
        if (const Object* o = find(stream)) ok = fill_stream_info(*o, w);
    }
    if (w.empty()) ok = foreign_info_w(m, stream, w);
    if (!ok) return -1;
    const InfoNumbers n = numbers_of(w);
    if (time < 0) return -1; // measured
    if (n.scale == 0) return -1;
    const bool up = static_cast<u64>(n.rate) > static_cast<u64>(n.scale) * 1000;
    const i64 s = scale_round(time, n.rate, static_cast<u64>(n.scale) * 1000, up);
    return static_cast<i32>(std::clamp<i64>(s, n.start, static_cast<i64>(n.start) + n.length));
}

u32 Avifil32Manager::begin_streaming(Marshaler& m, GuestAddr stream, i32 start, i32 end,
                                     i32 rate) {
    {
        std::lock_guard lock(mutex_);
        if (find(stream) != nullptr) return AVIERR_OK; // IAVIStreaming::Begin is S_OK
    }
    // A foreign stream: its IAVIStreaming when it has one, else nothing to do.
    const GuestAddr slot = alloc(8);
    if (slot == 0) return AVIERR_MEMORY;
    u32 hr = AVIERR_OK;
    const auto iid = ole_guid(IID_IAVIStreaming_Data1);
    const GuestAddr iid_at = alloc(16);
    if (iid_at != 0 && m.memory().write_bytes(iid_at, iid.data(), 16).is_ok() &&
        static_cast<u32>(call_slot(m, stream, kQueryInterface, {iid_at, slot})) == S_OK) {
        auto p = m.read_ptr(slot);
        if (p.is_ok() && p.value() != 0) {
            hr = static_cast<u32>(call_slot(m, p.value(), kStreamingBegin,
                                            {static_cast<u32>(start), static_cast<u32>(end),
                                             static_cast<u32>(rate)}));
            (void)call_slot(m, p.value(), kRelease, {});
        }
    }
    free_guest(iid_at);
    free_guest(slot);
    return hr;
}

u32 Avifil32Manager::end_streaming(Marshaler& m, GuestAddr stream) {
    {
        std::lock_guard lock(mutex_);
        if (find(stream) != nullptr) return AVIERR_OK;
    }
    const GuestAddr slot = alloc(8);
    if (slot == 0) return AVIERR_MEMORY;
    u32 hr = AVIERR_OK;
    const auto iid = ole_guid(IID_IAVIStreaming_Data1);
    const GuestAddr iid_at = alloc(16);
    if (iid_at != 0 && m.memory().write_bytes(iid_at, iid.data(), 16).is_ok() &&
        static_cast<u32>(call_slot(m, stream, kQueryInterface, {iid_at, slot})) == S_OK) {
        auto p = m.read_ptr(slot);
        if (p.is_ok() && p.value() != 0) {
            hr = static_cast<u32>(call_slot(m, p.value(), kStreamingEnd, {}));
            (void)call_slot(m, p.value(), kRelease, {});
        }
    }
    free_guest(iid_at);
    free_guest(slot);
    return hr;
}

// ---------------------------------------------------------------------------
// GetFrame
// ---------------------------------------------------------------------------

GuestAddr Avifil32Manager::get_frame_open(Marshaler& m, GuestAddr stream, GuestAddr wanted) {
    std::vector<u8> format;
    u32 handler = 0;
    {
        std::lock_guard lock(mutex_);
        Object* o = find(stream);
        if (o == nullptr) {
            unimplemented("AVIStreamGetFrameOpen", "an IAVIStream the application implements");
            return 0;
        }
        StreamView v;
        if (!view_of(stream, v) || v.wave || v.header.fcc_type != streamtypeVIDEO) return 0;
        format = format_at(*v.avi, -1);
        handler = v.header.fcc_handler;
    }
    BitmapInfoHeader src;
    if (!read_bih(format, src)) return 0;
    const bool by_caller = wanted != 0 && wanted != AVIGETFRAMEF_BESTDISPLAYFMT;
    BitmapInfoHeader want{};
    std::vector<u8> want_bytes;
    if (by_caller) {
        want_bytes.resize(kBitmapInfoHeaderSize);
        if (m.memory().read_bytes(wanted, reinterpret_cast<Byte*>(want_bytes.data()),
                                  want_bytes.size()).is_err() ||
            !read_bih(want_bytes, want)) {
            return 0;
        }
    }

    auto gf = std::make_unique<GetFrameState>();
    gf->stream = stream;
    const u32 height = static_cast<u32>(src.height < 0 ? -src.height : src.height);
    if (src.compression == BI_RGB) {
        // Uncompressed frames are handed out as stored: an lpbiWanted that
        // differs in depth, size or orientation is refused (measured).
        if (by_caller && (want.bit_count != src.bit_count || want.width != src.width ||
                          want.height != src.height || want.compression != BI_RGB)) {
            return 0;
        }
        gf->mode = GetFrameState::Mode::Copy;
        const usize table = static_cast<usize>(dib_color_entries(src)) * 4;
        gf->header.assign(format.begin(),
                          format.begin() + static_cast<std::ptrdiff_t>(std::min(
                                               format.size(), kBitmapInfoHeaderSize + table)));
        gf->header.resize(kBitmapInfoHeaderSize + table, 0);
        put32(gf->header, 0, kBitmapInfoHeaderSize);
        if (src.size_image == 0) put32(gf->header, 20, dib_stride(src.width, src.bit_count) * height);
        gf->palette_from_stream = src.bit_count <= 8;
    } else if (src.compression == BI_RLE8) {
        // msrle32 on Windows: 8-bit output only, the source palette unless
        // the caller supplies one (measured).
        BitmapInfoHeader out = src;
        out.size = kBitmapInfoHeaderSize;
        out.compression = BI_RGB;
        out.size_image = dib_stride(src.width, 8) * height;
        std::vector<u8> table;
        if (by_caller) {
            if (want.bit_count != 8 || want.width != src.width || want.height != src.height ||
                want.compression != BI_RGB) {
                return 0;
            }
            out = want;
            out.size = kBitmapInfoHeaderSize;
            out.clr_used = want.clr_used != 0 ? want.clr_used : 256;
            out.size_image = dib_stride(src.width, 8) * height;
            table.resize(static_cast<usize>(out.clr_used) * 4);
            (void)m.memory().read_bytes(wanted + (want.size ? want.size : kBitmapInfoHeaderSize),
                                        reinterpret_cast<Byte*>(table.data()), table.size());
        } else {
            if (out.clr_used == 0) out.clr_used = 256;
            table.assign(static_cast<usize>(out.clr_used) * 4, 0);
            const usize have = format.size() > kBitmapInfoHeaderSize
                                   ? format.size() - kBitmapInfoHeaderSize
                                   : 0;
            std::memcpy(table.data(), format.data() + kBitmapInfoHeaderSize,
                        std::min(have, table.size()));
            gf->palette_from_stream = true;
        }
        gf->mode = GetFrameState::Mode::Rle8;
        gf->header.assign(kBitmapInfoHeaderSize, 0);
        write_bih(gf->header.data(), out);
        gf->header.insert(gf->header.end(), table.begin(), table.end());
        gf->pixels.assign(static_cast<usize>(out.size_image), 0);
    } else if (src.compression == BI_RLE4) {
        // Windows' msrle32 does not decode BI_RLE4 for AVIFile: NULL for
        // every lpbiWanted (measured).
        return 0;
    } else {
        // Any other format goes to a Drivers32 codec through msvfw32.
        if (icm_ == nullptr) {
            unimplemented("AVIStreamGetFrameOpen",
                          "no decompressor for '" + fourcc_text(src.compression) + "'");
            return 0;
        }
        gf->in_format = alloc(format.size());
        if (gf->in_format == 0 ||
            m.memory().write_bytes(gf->in_format, format.data(), format.size()).is_err()) {
            free_guest(gf->in_format);
            return 0;
        }
        const u32 hic = icm_->locate(m, kIctypeVideo, handler != kDibHandler ? handler : 0,
                                     gf->in_format, by_caller ? wanted : 0, kIcmodeDecompress);
        if (hic == 0) {
            unimplemented("AVIStreamGetFrameOpen",
                          "no decompressor for '" + fourcc_text(src.compression) +
                              "' (Windows in-box codecs are not provided)");
            free_guest(gf->in_format);
            return 0;
        }
        gf->hic = hic;
        std::vector<u8> out;
        if (by_caller) {
            const usize table = static_cast<usize>(dib_color_entries(want)) * 4;
            out.resize(kBitmapInfoHeaderSize + table);
            (void)m.memory().read_bytes(wanted, reinterpret_cast<Byte*>(out.data()), out.size());
        } else {
            const i64 size = static_cast<i32>(icm_->send(m, hic, kIcmDecompressGetFormat,
                                                         gf->in_format, 0));
            const GuestAddr tmp = size > 0 ? alloc(static_cast<u64>(size)) : 0;
            if (tmp != 0 && static_cast<i32>(icm_->send(m, hic, kIcmDecompressGetFormat,
                                                         gf->in_format, tmp)) == 0) {
                out.resize(static_cast<usize>(size));
                (void)m.memory().read_bytes(tmp, reinterpret_cast<Byte*>(out.data()), out.size());
            }
            free_guest(tmp);
        }
        BitmapInfoHeader ob;
        if (!read_bih(out, ob)) {
            std::vector<std::unique_ptr<GetFrameState>> dead;
            dead.push_back(std::move(gf));
            finish_frames(m, dead);
            return 0;
        }
        const u32 ob_height = static_cast<u32>(ob.height < 0 ? -ob.height : ob.height);
        if (ob.size_image == 0) {
            ob.size_image = dib_stride(ob.width, ob.bit_count) * ob_height;
            write_bih(out.data(), ob);
        }
        gf->header = out;
        gf->mode = GetFrameState::Mode::Icm;
    }

    const u32 image = gf->mode == GetFrameState::Mode::Copy
                          ? dib_stride(src.width, src.bit_count) * height
                          : get32(gf->header, 20);
    gf->buffer_bytes = static_cast<u32>(gf->header.size()) + image;
    gf->buffer = alloc(gf->buffer_bytes);
    if (gf->buffer == 0 ||
        m.memory().write_bytes(gf->buffer, gf->header.data(), gf->header.size()).is_err()) {
        std::vector<std::unique_ptr<GetFrameState>> dead;
        dead.push_back(std::move(gf));
        finish_frames(m, dead);
        return 0;
    }
    if (gf->mode == GetFrameState::Mode::Icm) {
        gf->icd = alloc(48);
        const u32 hr = static_cast<u32>(
            icm_->send(m, gf->hic, kIcmDecompressQuery, gf->in_format, gf->buffer));
        if (gf->icd == 0 || hr != 0 ||
            static_cast<u32>(icm_->send(m, gf->hic, kIcmDecompressBegin, gf->in_format,
                                        gf->buffer)) != 0) {
            std::vector<std::unique_ptr<GetFrameState>> dead;
            dead.push_back(std::move(gf));
            finish_frames(m, dead);
            return 0;
        }
    }

    {
        std::lock_guard lock(mutex_);
        Object* s = find(stream);
        const GuestAddr base = s != nullptr ? new_object(m, Kind::GetFrame) : 0;
        if (base != 0) {
            ++find(stream)->refs; // the frame object holds its stream (measured)
            objects_.at(base).frame = std::move(gf);
            return base;
        }
    }
    // ICM teardown runs guest code: without the lock.
    std::vector<std::unique_ptr<GetFrameState>> dead;
    dead.push_back(std::move(gf));
    finish_frames(m, dead);
    return 0;
}

GuestAddr Avifil32Manager::get_frame(Marshaler& m, GuestAddr self, i32 pos) {
    struct Work {
        u64 offset;
        u32 size;
        bool key;
    };
    std::vector<Work> work;
    GetFrameState* gf = nullptr;
    std::vector<u8> header;
    std::shared_ptr<ByteSource> source;
    i64 target = -1;
    {
        std::lock_guard lock(mutex_);
        Object* o = find(self);
        if (o == nullptr || o->kind != Kind::GetFrame || !o->frame) return 0;
        gf = o->frame.get();
        Iface si{};
        Object* so = find(gf->stream, &si);
        if (so == nullptr || so->kind != Kind::AviStream) return 0;
        const Object& file = objects_.at(so->file);
        const AviStream& s = file.avi->streams[so->index];
        source = file.source;
        const i64 k = chunk_at(s, pos);
        if (k < 0) return 0;
        i64 k0 = -1;
        for (i64 i = k; i >= 0; --i) {
            if (s.chunks[static_cast<usize>(i)].key) {
                k0 = i;
                break;
            }
        }
        if (k0 < 0) return 0;
        header = gf->header;
        if (gf->palette_from_stream) {
            const std::vector<u8> f = format_at(s, k);
            const usize n = std::min(header.size(), f.size()) - kBitmapInfoHeaderSize;
            if (header.size() > kBitmapInfoHeaderSize && f.size() > kBitmapInfoHeaderSize) {
                std::memcpy(header.data() + kBitmapInfoHeaderSize, f.data() + kBitmapInfoHeaderSize,
                            n);
            }
        }
        i64 from = k0;
        if (gf->mode == GetFrameState::Mode::Copy) {
            // Uncompressed: the frame's own bits; a zero-length (dropped)
            // frame repeats the last stored one.
            from = k;
            while (from > 0 && s.chunks[static_cast<usize>(from)].size == 0) --from;
            const StreamChunk& c = s.chunks[static_cast<usize>(from)];
            work.push_back({c.offset, c.size, true});
        } else {
            if (gf->decoded >= k0 && gf->decoded <= k) from = gf->decoded + 1;
            for (i64 i = from; i <= k; ++i) {
                const StreamChunk& c = s.chunks[static_cast<usize>(i)];
                work.push_back({c.offset, c.size, c.key});
            }
            if (from == k0) gf->decoded = -1;
        }
        target = k;
    }

    const usize head = header.size();
    if (m.memory().write_bytes(gf->buffer, header.data(), head).is_err()) return 0;
    const usize image = gf->buffer_bytes - head;

    if (gf->mode == GetFrameState::Mode::Copy) {
        std::vector<u8> bits(image, 0);
        const Work& w = work.front();
        std::vector<u8> data(w.size);
        if (w.size != 0 && !source->read(w.offset, data.data(), data.size())) return 0;
        std::memcpy(bits.data(), data.data(), std::min(bits.size(), data.size()));
        if (m.memory().write_bytes(gf->buffer + head, bits.data(), bits.size()).is_err()) return 0;
        return gf->buffer;
    }

    BitmapInfoHeader ob;
    (void)read_bih(header, ob);
    const u32 height = static_cast<u32>(ob.height < 0 ? -ob.height : ob.height);
    if (gf->mode == GetFrameState::Mode::Rle8) {
        if (gf->decoded < 0) std::fill(gf->pixels.begin(), gf->pixels.end(), 0);
        for (const Work& w : work) {
            std::vector<u8> data(w.size);
            if (w.size != 0 && !source->read(w.offset, data.data(), data.size())) return 0;
            decode_rle8(data.data(), data.size(), gf->pixels.data(), dib_stride(ob.width, 8),
                        static_cast<u32>(ob.width < 0 ? -ob.width : ob.width), height);
        }
        gf->decoded = target;
        if (m.memory().write_bytes(gf->buffer + head, gf->pixels.data(),
                                   std::min(image, gf->pixels.size())).is_err()) {
            return 0;
        }
        return gf->buffer;
    }

    // ICM: ICM_DECOMPRESS each frame from the key frame on.
    const bool is64 = m.cpu_is_64bit();
    const u32 p = is64 ? 8u : 4u;
    for (const Work& w : work) {
        std::vector<u8> data(w.size);
        if (w.size != 0 && !source->read(w.offset, data.data(), data.size())) return 0;
        if (w.size > gf->in_data_bytes) {
            free_guest(gf->in_data);
            gf->in_data = alloc(w.size);
            gf->in_data_bytes = gf->in_data != 0 ? w.size : 0;
            if (gf->in_data == 0) return 0;
        }
        if (w.size != 0) (void)m.memory().write_bytes(gf->in_data, data.data(), data.size());
        (void)m.write_u32(gf->in_format + 20, w.size); // biSizeImage of this frame
        const u32 flags = w.key ? 0u : kIcdecompressNotKeyframe;
        std::vector<u8> icd(is64 ? 48 : 24, 0);
        auto put_ptr = [&](usize at, u64 v) {
            for (u32 i = 0; i < p; ++i) icd[at + i] = static_cast<u8>(v >> (8 * i));
        };
        put32(icd, 0, flags);
        put_ptr(p, gf->in_format);
        put_ptr(2 * p, gf->in_data);
        put_ptr(3 * p, gf->buffer);
        put_ptr(4 * p, gf->buffer + head);
        (void)m.memory().write_bytes(gf->icd, icd.data(), icd.size());
        const i32 r = static_cast<i32>(icm_->send(m, gf->hic, kIcmDecompress, gf->icd, icd.size()));
        if (r < 0) {
            gf->decoded = -1;
            return 0;
        }
    }
    gf->decoded = target;
    return gf->buffer;
}

GuestAddr Avifil32Manager::flat_get_frame(Marshaler& m, GuestAddr getframe, i32 pos) {
    if (is_ours(getframe)) return get_frame(m, getframe, pos);
    return static_cast<GuestAddr>(
        call_slot(m, getframe, kGetFrameGetFrame, {static_cast<u32>(pos)}));
}

u32 Avifil32Manager::get_frame_close(Marshaler& m, GuestAddr getframe) {
    (void)release(m, getframe);
    return AVIERR_OK;
}

u32 Avifil32Manager::get_frame_set_format(GuestAddr self) {
    (void)self;
    unimplemented("IGetFrame::SetFormat", "renegotiating the output format of an open IGetFrame");
    return E_NOTIMPL;
}

// ---------------------------------------------------------------------------
// Data exports
// ---------------------------------------------------------------------------

const std::vector<Avifil32DataExport>& avifil32_data_exports() {
    // Ordinals: the export table of avifil32.dll (x86 and x64 agree).
    static const std::vector<Avifil32DataExport> kExports = {
        {"IID_IAVIEditStream", 73, IID_IAVIEditStream_Data1},
        {"IID_IAVIFile", 74, IID_IAVIFile_Data1},
        {"IID_IAVIStream", 75, IID_IAVIStream_Data1},
        {"IID_IGetFrame", 76, IID_IGetFrame_Data1},
    };
    return kExports;
}

} // namespace vitreo::winapi
