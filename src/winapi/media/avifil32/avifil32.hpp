#pragma once

/// @file avifil32.hpp
/// @brief avifil32.dll -- the AVIFile API (vfw.h) over avifil32's two
/// built-in file handlers: the AVI file handler (CLSID_AVIFile) and the WAVE
/// file handler ({00020003-0000-0000-C000-000000000046}).
///
/// PAVIFILE, PAVISTREAM and PGETFRAME are guest COM objects: blocks of guest
/// memory whose vtable slots are internal avifil32 thunks, laid out as
/// Windows lays them out (measured: the AVI file's IUnknown sits three
/// pointers before its IAVIFile and its IPersistFile four after; a stream's
/// IUnknown three before IAVIStream and IAVIStreaming two after; the WAVE
/// handler is one object whose IAVIStream, IAVIFile, IUnknown and
/// IPersistFile are consecutive). The flat exports call the same host
/// methods for these objects and go through the object's own vtable for
/// interfaces the guest implemented itself.
///
/// What is measured rather than documented (tools/oracle_probe/
/// avifil32_probe.c, .expected for x86 and x64) is noted where it is used.
/// Write access is not implemented: AVIFileOpen with OF_WRITE, OF_READWRITE
/// or OF_CREATE fails with AVIERR_UNSUPPORTED and an UNIMPLEMENTED line;
/// objects opened for reading answer the write methods as Windows does for a
/// read-only file. See .docs/contracts/avifil32.md.
///
/// Threading: one mutex guards the object tables. Guest code (foreign
/// vtables, guest codecs through msvfw32) is never called with it held.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/media/avifil32/avifil32_contract.hpp"
#include "winapi/media/avifil32/avifil32_dib.hpp"
#include "winapi/media/avifil32/avifil32_riff.hpp"

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::capsule {
class Vfs;
}

namespace vitreo::winapi {

class Marshaler;
class Msvfw32Manager;

class Avifil32Manager {
public:
    Avifil32Manager() = default;

    void set_thunk_registry(ThunkRegistry* registry) noexcept { thunks_ = registry; }
    void set_msvfw32(Msvfw32Manager* icm) noexcept { icm_ = icm; }

    /// Invoke a guest function (foreign vtable slots).
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;
    void set_guest_call(GuestCall call) { guest_call_ = std::move(call); }

    /// Guest memory for objects, vtables and frame buffers (process heap).
    struct Heap {
        std::function<GuestAddr(u64)> alloc;
        std::function<void(GuestAddr)> free;
    };
    void set_heap(Heap heap) { heap_ = std::move(heap); }

    /// Open a file for reading. @p exists reports whether the path names
    /// anything at all (a directory included).
    struct OpenedFile {
        std::shared_ptr<avifil32::ByteSource> source;
        bool exists{false};
    };
    using FileOpener = std::function<OpenedFile(const std::string& utf8_path)>;
    void set_file_opener(FileOpener opener) { open_file_ = std::move(opener); }
    /// The capsule VFS as the file opener.
    void use_vfs(capsule::Vfs& vfs);

    /// COM state the handlers depend on: AVIFileOpen creates its handler with
    /// CoCreateInstance (CO_E_NOTINITIALIZED outside an apartment, measured),
    /// and AVIFileInit / AVIFileExit are OleInitialize / OleUninitialize.
    struct Com {
        std::function<bool(Marshaler&)> thread_in_apartment;
        std::function<void(Marshaler&)> ole_initialize;
        std::function<void(Marshaler&)> ole_uninitialize;
    };
    void set_com(Com com) { com_ = std::move(com); }

    // ---- flat exports ----------------------------------------------------
    void file_init(Marshaler& m);
    void file_exit(Marshaler& m);
    [[nodiscard]] u32 file_open(Marshaler& m, GuestAddr ppfile, GuestAddr name, bool wide,
                                u32 mode, GuestAddr handler);
    [[nodiscard]] u32 stream_open_from_file(Marshaler& m, GuestAddr ppavi, GuestAddr name,
                                            bool wide, u32 type, i32 lparam, u32 mode,
                                            GuestAddr handler);
    [[nodiscard]] u32 add_ref(Marshaler& m, GuestAddr object);
    [[nodiscard]] u32 release(Marshaler& m, GuestAddr object);
    [[nodiscard]] u32 flat_file_info(Marshaler& m, GuestAddr file, GuestAddr info, i32 size,
                                     bool wide);
    [[nodiscard]] u32 flat_stream_info(Marshaler& m, GuestAddr stream, GuestAddr info,
                                       i32 size, bool wide);
    [[nodiscard]] u32 flat_file_create_stream(Marshaler& m, GuestAddr file, GuestAddr pp,
                                              GuestAddr info, bool wide);
    [[nodiscard]] i32 stream_start(Marshaler& m, GuestAddr stream);
    [[nodiscard]] i32 stream_length(Marshaler& m, GuestAddr stream);
    [[nodiscard]] i32 sample_to_time(Marshaler& m, GuestAddr stream, i32 sample);
    [[nodiscard]] i32 time_to_sample(Marshaler& m, GuestAddr stream, i32 time);
    [[nodiscard]] u32 begin_streaming(Marshaler& m, GuestAddr stream, i32 start, i32 end,
                                      i32 rate);
    [[nodiscard]] u32 end_streaming(Marshaler& m, GuestAddr stream);
    [[nodiscard]] GuestAddr get_frame_open(Marshaler& m, GuestAddr stream, GuestAddr wanted);
    [[nodiscard]] GuestAddr flat_get_frame(Marshaler& m, GuestAddr getframe, i32 pos);
    [[nodiscard]] u32 get_frame_close(Marshaler& m, GuestAddr getframe);
    /// Flat export that calls vtable slot @p slot of @p object (ours or the
    /// guest's) with @p args after `this`.
    [[nodiscard]] u64 call_slot(Marshaler& m, GuestAddr object, u32 slot,
                                std::initializer_list<u64> args);
    [[nodiscard]] u32 dll_can_unload_now();

    // ---- COM methods (vtable thunks) ---------------------------------------
    [[nodiscard]] u32 query_interface(Marshaler& m, GuestAddr self, GuestAddr iid,
                                      GuestAddr out);
    [[nodiscard]] u32 file_info(Marshaler& m, GuestAddr self, GuestAddr info, i32 size,
                                bool wide);
    [[nodiscard]] u32 file_get_stream(Marshaler& m, GuestAddr self, GuestAddr pp, u32 type,
                                      i32 lparam);
    [[nodiscard]] u32 file_create_stream(Marshaler& m, GuestAddr self, GuestAddr pp);
    [[nodiscard]] u32 file_write_data(GuestAddr self);
    [[nodiscard]] u32 file_read_data(Marshaler& m, GuestAddr self, u32 ckid, GuestAddr data,
                                     GuestAddr pcb);
    [[nodiscard]] u32 file_end_record(GuestAddr self);
    [[nodiscard]] u32 file_delete_stream(GuestAddr self);
    [[nodiscard]] u32 persist_method(GuestAddr self, u32 slot);
    [[nodiscard]] u32 stream_create(GuestAddr self);
    [[nodiscard]] u32 stream_info(Marshaler& m, GuestAddr self, GuestAddr info, i32 size,
                                  bool wide);
    [[nodiscard]] i32 stream_find_sample(GuestAddr self, i32 pos, u32 flags);
    [[nodiscard]] u32 stream_read_format(Marshaler& m, GuestAddr self, i32 pos,
                                         GuestAddr format, GuestAddr pcb);
    [[nodiscard]] u32 stream_read(Marshaler& m, GuestAddr self, i32 start, i32 samples,
                                  GuestAddr buffer, i32 cb, GuestAddr pbytes,
                                  GuestAddr psamples);
    [[nodiscard]] u32 stream_read_data(Marshaler& m, GuestAddr self, u32 ckid, GuestAddr data,
                                       GuestAddr pcb);
    /// SetFormat / Write / Delete / WriteData / SetInfo on a read-only stream.
    [[nodiscard]] u32 stream_write_method(GuestAddr self, u32 slot);
    [[nodiscard]] GuestAddr get_frame(Marshaler& m, GuestAddr self, i32 pos);
    [[nodiscard]] u32 get_frame_set_format(GuestAddr self);

    /// Live object count (tests).
    [[nodiscard]] usize live_objects() const;

    // Vtable slots (vfw.h interface order).
    enum Slot : u32 {
        kQueryInterface = 0, kAddRef = 1, kRelease = 2,
        // IAVIFile
        kFileInfo = 3, kFileGetStream, kFileCreateStream, kFileWriteData, kFileReadData,
        kFileEndRecord, kFileDeleteStream,
        // IAVIStream
        kStreamCreate = 3, kStreamInfo, kStreamFindSample, kStreamReadFormat,
        kStreamSetFormat, kStreamRead, kStreamWrite, kStreamDelete, kStreamReadData,
        kStreamWriteData, kStreamSetInfo,
        // IAVIStreaming
        kStreamingBegin = 3, kStreamingEnd,
        // IGetFrame
        kGetFrameGetFrame = 3, kGetFrameBegin, kGetFrameEnd, kGetFrameSetFormat,
        // IPersistFile (+ IAVIPersistFile::Reserved1)
        kPersistGetClassID = 3, kPersistIsDirty, kPersistLoad, kPersistSave,
        kPersistSaveCompleted, kPersistGetCurFile, kPersistReserved1,
    };

private:
    enum class Kind : u8 { AviFile, AviStream, WaveFile, GetFrame };
    enum class Iface : u8 { Unknown, File, Persist, Stream, Streaming, GetFrame, Count };

    struct GetFrameState {
        enum class Mode : u8 { Copy, Rle8, Icm } mode{Mode::Copy};
        GuestAddr stream{0};      ///< IAVIStream it holds a reference on
        GuestAddr buffer{0};      ///< packed DIB handed to the caller
        u32 buffer_bytes{0};
        std::vector<u8> header;   ///< BITMAPINFOHEADER + colour table
        bool palette_from_stream{false};
        std::vector<u8> pixels;   ///< RLE decode surface
        i64 decoded{-1};          ///< chunk index the surface holds
        // ICM
        u32 hic{0};
        GuestAddr in_format{0};
        GuestAddr in_data{0};
        u32 in_data_bytes{0};
        GuestAddr icd{0};
    };

    struct Object {
        Kind kind{Kind::AviFile};
        GuestAddr base{0};
        u32 bytes{0};
        u32 refs{1};
        // AVI file / WAVE file
        std::shared_ptr<avifil32::ByteSource> source;
        std::shared_ptr<avifil32::AviFileModel> avi;
        std::shared_ptr<avifil32::WaveFileModel> wave;
        std::u16string file_name; ///< last path component, as the caller named it
        std::vector<GuestAddr> streams; ///< AVI: live stream object per index
        // AVI stream
        GuestAddr file{0}; ///< owning file object base
        u32 index{0};
        // GetFrame
        std::unique_ptr<GetFrameState> frame;
    };

    struct StreamView;

    [[nodiscard]] u32 ptr_size() const noexcept { return x64_ ? 8u : 4u; }
    [[nodiscard]] bool ensure_vtables(Marshaler& m);
    [[nodiscard]] GuestAddr new_object(Marshaler& m, Kind kind);
    [[nodiscard]] GuestAddr iface_ptr(const Object& o, Iface iface) const;
    [[nodiscard]] bool is_ours(GuestAddr ptr) const;
    [[nodiscard]] Object* find(GuestAddr iface_ptr, Iface* iface = nullptr);
    [[nodiscard]] const Object* find(GuestAddr iface_ptr, Iface* iface = nullptr) const;
    [[nodiscard]] GuestAddr public_file_ptr(const Object& o) const;
    [[nodiscard]] GuestAddr public_stream_ptr(const Object& o) const;
    /// Drop one reference; returns the new count and appends objects whose
    /// teardown needs guest calls to @p dead_frames.
    u32 release_locked(Object& o, std::vector<std::unique_ptr<GetFrameState>>& dead_frames);
    void destroy_locked(GuestAddr base,
                        std::vector<std::unique_ptr<GetFrameState>>& dead_frames);
    void finish_frames(Marshaler& m, std::vector<std::unique_ptr<GetFrameState>>& dead);
    [[nodiscard]] bool view_of(GuestAddr stream, StreamView& view);
    [[nodiscard]] u32 open_handler(Marshaler& m, const std::u16string& path, u32 mode,
                                   GuestAddr handler, GuestAddr& out);
    [[nodiscard]] bool read_guid(Marshaler& m, GuestAddr at, std::array<u8, 16>& out) const;
    [[nodiscard]] bool fill_stream_info(const Object& stream, std::vector<u8>& info_w);
    [[nodiscard]] bool foreign_info_w(Marshaler& m, GuestAddr stream, std::vector<u8>& info_w);
    [[nodiscard]] GuestAddr alloc(u64 bytes);
    void free_guest(GuestAddr at);
    [[nodiscard]] u64 call_guest(Marshaler& m, GuestAddr fn, std::initializer_list<u64> args);
    /// One of our vtable slots, called on interface @p iface of our object.
    [[nodiscard]] u64 dispatch(Marshaler& m, GuestAddr self, u32 iface, u32 slot,
                               const std::vector<u64>& args);

    ThunkRegistry* thunks_{nullptr};
    Msvfw32Manager* icm_{nullptr};
    GuestCall guest_call_;
    Heap heap_;
    FileOpener open_file_;
    Com com_;

    mutable std::mutex mutex_;
    bool x64_{false};
    bool vtables_ready_{false};
    std::array<GuestAddr, static_cast<usize>(Iface::Count)> vtables_{};
    std::unordered_map<GuestAddr, Object> objects_;           ///< by base
    std::unordered_map<GuestAddr, std::pair<GuestAddr, Iface>> ifaces_; ///< ptr -> base
};

DllExports create_avifil32_thunks(Avifil32Manager& mgr);

/// The four IID data exports (IID_IAVIEditStream, IID_IAVIFile,
/// IID_IAVIStream, IID_IGetFrame) with their ordinals; @p write stores 16
/// GUID bytes at a guest address and returns it.
struct Avifil32DataExport {
    const char* name;
    u16 ordinal;
    u32 data1;
};
[[nodiscard]] const std::vector<Avifil32DataExport>& avifil32_data_exports();

} // namespace vitreo::winapi
