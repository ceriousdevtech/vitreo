// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo -- avifil32.dll export table and COM method thunks.
//
// Names and ordinals: avifil32.dll's export table (x86 and x64 agree; the
// "avi export" lines of tools/oracle_probe/avifil32_probe*.expected). x86
// argument bytes: libavifil32.a (tools/oracle_probe/avifil32_importlib32.txt);
// AVISave / AVISaveA / AVISaveW are CDECL (variadic).

#include "winapi/media/avifil32/avifil32.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/media/avifil32/avifil32_contract.hpp"

#include <cstdio>
#include <mutex>
#include <set>
#include <string>

namespace vitreo::winapi {

using namespace avifil32;

namespace {

void unimplemented_once(const char* api, const char* what) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    {
        std::lock_guard lock(mutex);
        if (!seen.insert(api).second) return;
    }
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED avifil32!%s: %s\n", api, what);
}

struct ExportSpec {
    const char* name;
    u16 ordinal;
    u16 stdcall_bytes;
};

// avifil32.dll exports 1..76 (73..76 are the IID data exports, registered
// by the DLL registry as data).
constexpr ExportSpec kExports[] = {
    {"AVIBuildFilter", 1, 12},          {"AVIBuildFilterA", 2, 12},
    {"AVIBuildFilterW", 3, 12},         {"AVIClearClipboard", 4, 0},
    {"AVIFileAddRef", 5, 4},            {"AVIFileCreateStream", 6, 12},
    {"AVIFileCreateStreamA", 7, 12},    {"AVIFileCreateStreamW", 8, 12},
    {"AVIFileEndRecord", 9, 4},         {"AVIFileExit", 10, 0},
    {"AVIFileGetStream", 11, 16},       {"AVIFileInfo", 12, 12},
    {"AVIFileInfoA", 13, 12},           {"AVIFileInfoW", 14, 12},
    {"AVIFileInit", 15, 0},             {"AVIFileOpen", 16, 16},
    {"AVIFileOpenA", 17, 16},           {"AVIFileOpenW", 18, 16},
    {"AVIFileReadData", 19, 16},        {"AVIFileRelease", 20, 4},
    {"AVIFileWriteData", 21, 16},       {"AVIGetFromClipboard", 22, 4},
    {"AVIMakeCompressedStream", 23, 16}, {"AVIMakeFileFromStreams", 24, 12},
    {"AVIMakeStreamFromClipboard", 25, 12}, {"AVIPutFileOnClipboard", 26, 4},
    {"AVISave", 27, 0},                 {"AVISaveA", 28, 0},
    {"AVISaveOptions", 29, 20},         {"AVISaveOptionsFree", 30, 8},
    {"AVISaveV", 31, 24},               {"AVISaveVA", 32, 24},
    {"AVISaveVW", 33, 24},              {"AVISaveW", 34, 0},
    {"AVIStreamAddRef", 35, 4},         {"AVIStreamBeginStreaming", 36, 16},
    {"AVIStreamCreate", 37, 16},        {"AVIStreamEndStreaming", 38, 4},
    {"AVIStreamFindSample", 39, 12},    {"AVIStreamGetFrame", 40, 8},
    {"AVIStreamGetFrameClose", 41, 4},  {"AVIStreamGetFrameOpen", 42, 8},
    {"AVIStreamInfo", 43, 12},          {"AVIStreamInfoA", 44, 12},
    {"AVIStreamInfoW", 45, 12},         {"AVIStreamLength", 46, 4},
    {"AVIStreamOpenFromFile", 47, 24},  {"AVIStreamOpenFromFileA", 48, 24},
    {"AVIStreamOpenFromFileW", 49, 24}, {"AVIStreamRead", 50, 28},
    {"AVIStreamReadData", 51, 16},      {"AVIStreamReadFormat", 52, 16},
    {"AVIStreamRelease", 53, 4},        {"AVIStreamSampleToTime", 54, 8},
    {"AVIStreamSetFormat", 55, 16},     {"AVIStreamStart", 56, 4},
    {"AVIStreamTimeToSample", 57, 8},   {"AVIStreamWrite", 58, 32},
    {"AVIStreamWriteData", 59, 16},     {"CreateEditableStream", 60, 8},
    {"DllCanUnloadNow", 61, 0},         {"DllGetClassObject", 62, 12},
    {"EditStreamClone", 63, 8},         {"EditStreamCopy", 64, 16},
    {"EditStreamCut", 65, 16},          {"EditStreamPaste", 66, 24},
    {"EditStreamSetInfo", 67, 12},      {"EditStreamSetInfoA", 68, 12},
    {"EditStreamSetInfoW", 69, 12},     {"EditStreamSetName", 70, 8},
    {"EditStreamSetNameA", 71, 8},      {"EditStreamSetNameW", 72, 8},
};

const ExportSpec* spec_of(const char* name) {
    for (const auto& e : kExports) {
        if (std::string_view(e.name) == name) return &e;
    }
    return nullptr;
}

} // namespace

DllExports create_avifil32_thunks(Avifil32Manager& mgr) {
    DllExports exports;
    auto add = [&](const char* name, ApiHandler handler) {
        const ExportSpec* spec = spec_of(name);
        exports.entries.push_back(ExportEntry{std::string(name), spec ? spec->ordinal : u16{0},
                                              std::move(handler),
                                              spec ? spec->stdcall_bytes : u16{0}});
    };
    auto i32_arg = [](Marshaler& m, usize i) { return static_cast<i32>(m.arg32(i)); };

    add("AVIFileInit", [&mgr](Marshaler& m) { mgr.file_init(m); });
    add("AVIFileExit", [&mgr](Marshaler& m) { mgr.file_exit(m); });
    for (const char* name : {"AVIFileOpen", "AVIFileOpenA", "AVIFileOpenW"}) {
        const bool wide = std::string_view(name) == "AVIFileOpenW";
        add(name, [&mgr, wide](Marshaler& m) {
            m.set_return(mgr.file_open(m, m.arg_ptr(0), m.arg_ptr(1), wide, m.arg32(2),
                                       m.arg_ptr(3)));
        });
    }
    for (const char* name :
         {"AVIStreamOpenFromFile", "AVIStreamOpenFromFileA", "AVIStreamOpenFromFileW"}) {
        const bool wide = std::string_view(name) == "AVIStreamOpenFromFileW";
        add(name, [&mgr, wide, i32_arg](Marshaler& m) {
            m.set_return(mgr.stream_open_from_file(m, m.arg_ptr(0), m.arg_ptr(1), wide,
                                                   m.arg32(2), i32_arg(m, 3), m.arg32(4),
                                                   m.arg_ptr(5)));
        });
    }
    for (const char* name : {"AVIFileAddRef", "AVIStreamAddRef"}) {
        add(name, [&mgr](Marshaler& m) { m.set_return(mgr.add_ref(m, m.arg_ptr(0))); });
    }
    for (const char* name : {"AVIFileRelease", "AVIStreamRelease"}) {
        add(name, [&mgr](Marshaler& m) { m.set_return(mgr.release(m, m.arg_ptr(0))); });
    }
    for (const char* name : {"AVIFileInfo", "AVIFileInfoA", "AVIFileInfoW"}) {
        const bool wide = std::string_view(name) == "AVIFileInfoW";
        add(name, [&mgr, wide, i32_arg](Marshaler& m) {
            m.set_return(mgr.flat_file_info(m, m.arg_ptr(0), m.arg_ptr(1), i32_arg(m, 2), wide));
        });
    }
    for (const char* name : {"AVIStreamInfo", "AVIStreamInfoA", "AVIStreamInfoW"}) {
        const bool wide = std::string_view(name) == "AVIStreamInfoW";
        add(name, [&mgr, wide, i32_arg](Marshaler& m) {
            m.set_return(mgr.flat_stream_info(m, m.arg_ptr(0), m.arg_ptr(1), i32_arg(m, 2), wide));
        });
    }
    for (const char* name : {"AVIFileCreateStream", "AVIFileCreateStreamA", "AVIFileCreateStreamW"}) {
        const bool wide = std::string_view(name) == "AVIFileCreateStreamW";
        add(name, [&mgr, wide](Marshaler& m) {
            m.set_return(mgr.flat_file_create_stream(m, m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2),
                                                     wide));
        });
    }
    // Flat exports that are one vtable call (Windows shares their code: the
    // RVA of AVIFileReadData is AVIStreamSetFormat's, and so on).
    auto slot_call = [&](const char* name, u32 slot, usize nargs) {
        add(name, [&mgr, slot, nargs](Marshaler& m) {
            u64 a[6] = {};
            for (usize i = 0; i < nargs; ++i) a[i] = m.arg(1 + i);
            const GuestAddr self = m.arg_ptr(0);
            u64 r = 0;
            switch (nargs) {
            case 0: r = mgr.call_slot(m, self, slot, {}); break;
            case 1: r = mgr.call_slot(m, self, slot, {a[0]}); break;
            case 2: r = mgr.call_slot(m, self, slot, {a[0], a[1]}); break;
            case 3: r = mgr.call_slot(m, self, slot, {a[0], a[1], a[2]}); break;
            case 6: r = mgr.call_slot(m, self, slot, {a[0], a[1], a[2], a[3], a[4], a[5]}); break;
            case 7: break;
            }
            m.set_return(r);
        });
    };
    slot_call("AVIFileGetStream", Avifil32Manager::kFileGetStream, 3);
    slot_call("AVIFileReadData", Avifil32Manager::kFileReadData, 3);
    slot_call("AVIFileWriteData", Avifil32Manager::kFileWriteData, 3);
    slot_call("AVIFileEndRecord", Avifil32Manager::kFileEndRecord, 0);
    slot_call("AVIStreamFindSample", Avifil32Manager::kStreamFindSample, 2);
    slot_call("AVIStreamReadFormat", Avifil32Manager::kStreamReadFormat, 3);
    slot_call("AVIStreamSetFormat", Avifil32Manager::kStreamSetFormat, 3);
    slot_call("AVIStreamReadData", Avifil32Manager::kStreamReadData, 3);
    slot_call("AVIStreamWriteData", Avifil32Manager::kStreamWriteData, 3);
    slot_call("AVIStreamRead", Avifil32Manager::kStreamRead, 6);
    add("AVIStreamWrite", [&mgr](Marshaler& m) {
        m.set_return(mgr.call_slot(m, m.arg_ptr(0), Avifil32Manager::kStreamWrite,
                                   {m.arg(1), m.arg(2), m.arg(3), m.arg(4), m.arg(5), m.arg(6),
                                    m.arg(7)}));
    });
    add("AVIStreamStart", [&mgr](Marshaler& m) {
        m.set_return(static_cast<u32>(mgr.stream_start(m, m.arg_ptr(0))));
    });
    add("AVIStreamLength", [&mgr](Marshaler& m) {
        m.set_return(static_cast<u32>(mgr.stream_length(m, m.arg_ptr(0))));
    });
    add("AVIStreamSampleToTime", [&mgr, i32_arg](Marshaler& m) {
        m.set_return(static_cast<u32>(mgr.sample_to_time(m, m.arg_ptr(0), i32_arg(m, 1))));
    });
    add("AVIStreamTimeToSample", [&mgr, i32_arg](Marshaler& m) {
        m.set_return(static_cast<u32>(mgr.time_to_sample(m, m.arg_ptr(0), i32_arg(m, 1))));
    });
    add("AVIStreamBeginStreaming", [&mgr, i32_arg](Marshaler& m) {
        m.set_return(mgr.begin_streaming(m, m.arg_ptr(0), i32_arg(m, 1), i32_arg(m, 2),
                                         i32_arg(m, 3)));
    });
    add("AVIStreamEndStreaming", [&mgr](Marshaler& m) {
        m.set_return(mgr.end_streaming(m, m.arg_ptr(0)));
    });
    add("AVIStreamGetFrameOpen", [&mgr](Marshaler& m) {
        m.set_return(mgr.get_frame_open(m, m.arg_ptr(0), m.arg_ptr(1)));
    });
    add("AVIStreamGetFrame", [&mgr, i32_arg](Marshaler& m) {
        m.set_return(mgr.flat_get_frame(m, m.arg_ptr(0), i32_arg(m, 1)));
    });
    add("AVIStreamGetFrameClose", [&mgr](Marshaler& m) {
        m.set_return(mgr.get_frame_close(m, m.arg_ptr(0)));
    });
    // AVIStreamCreate creates the handler class named by pclsidHandler; with
    // none there is no class (REGDB_E_CLASSNOTREG, *ppavi untouched, measured).
    add("AVIStreamCreate", [](Marshaler& m) {
        if (m.arg_ptr(3) != 0) {
            unimplemented_once("AVIStreamCreate", "creating a stream handler class by CLSID");
        }
        m.set_return(REGDB_E_CLASSNOTREG);
    });
    add("DllCanUnloadNow", [&mgr](Marshaler& m) { m.set_return(mgr.dll_can_unload_now()); });
    add("DllGetClassObject", [](Marshaler& m) {
        unimplemented_once("DllGetClassObject", "AVIFile handler class factories");
        if (m.arg_ptr(2) != 0) (void)m.write_ptr(m.arg_ptr(2), 0);
        m.set_return(CLASS_E_CLASSNOTAVAILABLE);
    });

    // Not implemented: write side, editing, clipboard, compression options.
    struct Refused {
        const char* name;
        u32 result;
        int out_arg; ///< pointer argument to NULL on failure, or -1
        const char* what;
    };
    static const Refused kRefused[] = {
        {"AVIBuildFilter", AVIERR_ERROR, -1, "file-type filter strings"},
        {"AVIBuildFilterA", AVIERR_ERROR, -1, "file-type filter strings"},
        {"AVIBuildFilterW", AVIERR_ERROR, -1, "file-type filter strings"},
        {"AVIClearClipboard", AVIERR_UNSUPPORTED, -1, "the AVI clipboard"},
        {"AVIGetFromClipboard", AVIERR_UNSUPPORTED, 0, "the AVI clipboard"},
        {"AVIMakeStreamFromClipboard", AVIERR_UNSUPPORTED, 2, "the AVI clipboard"},
        {"AVIPutFileOnClipboard", AVIERR_UNSUPPORTED, -1, "the AVI clipboard"},
        {"AVIMakeCompressedStream", AVIERR_UNSUPPORTED, 0, "compressing streams"},
        {"AVIMakeFileFromStreams", AVIERR_UNSUPPORTED, 0, "files built from streams"},
        {"AVISave", AVIERR_UNSUPPORTED, -1, "writing AVI files"},
        {"AVISaveA", AVIERR_UNSUPPORTED, -1, "writing AVI files"},
        {"AVISaveW", AVIERR_UNSUPPORTED, -1, "writing AVI files"},
        {"AVISaveV", AVIERR_UNSUPPORTED, -1, "writing AVI files"},
        {"AVISaveVA", AVIERR_UNSUPPORTED, -1, "writing AVI files"},
        {"AVISaveVW", AVIERR_UNSUPPORTED, -1, "writing AVI files"},
        {"AVISaveOptions", 0, -1, "the compression options dialog"},
        {"AVISaveOptionsFree", AVIERR_UNSUPPORTED, -1, "compression options"},
        {"CreateEditableStream", AVIERR_UNSUPPORTED, 0, "editable streams"},
        {"EditStreamClone", AVIERR_UNSUPPORTED, 1, "editable streams"},
        {"EditStreamCopy", AVIERR_UNSUPPORTED, 3, "editable streams"},
        {"EditStreamCut", AVIERR_UNSUPPORTED, 3, "editable streams"},
        {"EditStreamPaste", AVIERR_UNSUPPORTED, -1, "editable streams"},
        {"EditStreamSetInfo", AVIERR_UNSUPPORTED, -1, "editable streams"},
        {"EditStreamSetInfoA", AVIERR_UNSUPPORTED, -1, "editable streams"},
        {"EditStreamSetInfoW", AVIERR_UNSUPPORTED, -1, "editable streams"},
        {"EditStreamSetName", AVIERR_UNSUPPORTED, -1, "editable streams"},
        {"EditStreamSetNameA", AVIERR_UNSUPPORTED, -1, "editable streams"},
        {"EditStreamSetNameW", AVIERR_UNSUPPORTED, -1, "editable streams"},
    };
    for (const auto& r : kRefused) {
        add(r.name, [r](Marshaler& m) {
            unimplemented_once(r.name, r.what);
            if (r.out_arg >= 0 && m.arg_ptr(static_cast<usize>(r.out_arg)) != 0) {
                (void)m.write_ptr(m.arg_ptr(static_cast<usize>(r.out_arg)), 0);
            }
            m.set_return(r.result);
        });
    }

    // ---- COM method thunks (internal: reached only through vtables) ----------
    auto method = [&](std::string name, u16 stdcall_bytes, ApiHandler handler) {
        ExportEntry e{std::move(name), 0, std::move(handler), stdcall_bytes};
        e.internal = true;
        exports.entries.push_back(std::move(e));
    };
    method("Vitreo_AVI_QueryInterface", 12, [&mgr](Marshaler& m) {
        m.set_return(mgr.query_interface(m, m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2)));
    });
    method("Vitreo_AVI_AddRef", 4, [&mgr](Marshaler& m) {
        m.set_return(mgr.add_ref(m, m.arg_ptr(0)));
    });
    method("Vitreo_AVI_Release", 4, [&mgr](Marshaler& m) {
        m.set_return(mgr.release(m, m.arg_ptr(0)));
    });
    // IAVIFile
    method("Vitreo_IAVIFile_Info", 12, [&mgr, i32_arg](Marshaler& m) {
        m.set_return(mgr.file_info(m, m.arg_ptr(0), m.arg_ptr(1), i32_arg(m, 2), true));
    });
    method("Vitreo_IAVIFile_GetStream", 16, [&mgr, i32_arg](Marshaler& m) {
        m.set_return(mgr.file_get_stream(m, m.arg_ptr(0), m.arg_ptr(1), m.arg32(2), i32_arg(m, 3)));
    });
    method("Vitreo_IAVIFile_CreateStream", 12, [&mgr](Marshaler& m) {
        m.set_return(mgr.file_create_stream(m, m.arg_ptr(0), m.arg_ptr(1)));
    });
    method("Vitreo_IAVIFile_WriteData", 16, [&mgr](Marshaler& m) {
        m.set_return(mgr.file_write_data(m.arg_ptr(0)));
    });
    method("Vitreo_IAVIFile_ReadData", 16, [&mgr](Marshaler& m) {
        m.set_return(mgr.file_read_data(m, m.arg_ptr(0), m.arg32(1), m.arg_ptr(2), m.arg_ptr(3)));
    });
    method("Vitreo_IAVIFile_EndRecord", 4, [&mgr](Marshaler& m) {
        m.set_return(mgr.file_end_record(m.arg_ptr(0)));
    });
    method("Vitreo_IAVIFile_DeleteStream", 12, [&mgr](Marshaler& m) {
        m.set_return(mgr.file_delete_stream(m.arg_ptr(0)));
    });
    // IPersistFile (+ IAVIPersistFile::Reserved1)
    const std::pair<const char*, std::pair<u32, u16>> persist[] = {
        {"GetClassID", {Avifil32Manager::kPersistGetClassID, 8}},
        {"IsDirty", {Avifil32Manager::kPersistIsDirty, 4}},
        {"Load", {Avifil32Manager::kPersistLoad, 12}},
        {"Save", {Avifil32Manager::kPersistSave, 12}},
        {"SaveCompleted", {Avifil32Manager::kPersistSaveCompleted, 8}},
        {"GetCurFile", {Avifil32Manager::kPersistGetCurFile, 8}},
        {"Reserved1", {Avifil32Manager::kPersistReserved1, 4}},
    };
    for (const auto& [name, info] : persist) {
        const u32 slot = info.first;
        method(std::string("Vitreo_IPersistFile_") + name, info.second, [&mgr, slot](Marshaler& m) {
            if (slot == Avifil32Manager::kPersistGetCurFile && m.arg_ptr(1) != 0) {
                (void)m.write_ptr(m.arg_ptr(1), 0);
            }
            m.set_return(mgr.persist_method(m.arg_ptr(0), slot));
        });
    }
    // IAVIStream
    method("Vitreo_IAVIStream_Create", 12, [&mgr](Marshaler& m) {
        m.set_return(mgr.stream_create(m.arg_ptr(0)));
    });
    method("Vitreo_IAVIStream_Info", 12, [&mgr, i32_arg](Marshaler& m) {
        m.set_return(mgr.stream_info(m, m.arg_ptr(0), m.arg_ptr(1), i32_arg(m, 2), true));
    });
    method("Vitreo_IAVIStream_FindSample", 12, [&mgr, i32_arg](Marshaler& m) {
        m.set_return(static_cast<u32>(mgr.stream_find_sample(m.arg_ptr(0), i32_arg(m, 1),
                                                             m.arg32(2))));
    });
    method("Vitreo_IAVIStream_ReadFormat", 16, [&mgr, i32_arg](Marshaler& m) {
        m.set_return(mgr.stream_read_format(m, m.arg_ptr(0), i32_arg(m, 1), m.arg_ptr(2),
                                            m.arg_ptr(3)));
    });
    method("Vitreo_IAVIStream_Read", 28, [&mgr, i32_arg](Marshaler& m) {
        m.set_return(mgr.stream_read(m, m.arg_ptr(0), i32_arg(m, 1), i32_arg(m, 2), m.arg_ptr(3),
                                     i32_arg(m, 4), m.arg_ptr(5), m.arg_ptr(6)));
    });
    method("Vitreo_IAVIStream_ReadData", 16, [&mgr](Marshaler& m) {
        m.set_return(mgr.stream_read_data(m, m.arg_ptr(0), m.arg32(1), m.arg_ptr(2), m.arg_ptr(3)));
    });
    const std::pair<const char*, std::pair<u32, u16>> writes[] = {
        {"SetFormat", {Avifil32Manager::kStreamSetFormat, 16}},
        {"Write", {Avifil32Manager::kStreamWrite, 32}},
        {"Delete", {Avifil32Manager::kStreamDelete, 12}},
        {"WriteData", {Avifil32Manager::kStreamWriteData, 16}},
        {"SetInfo", {Avifil32Manager::kStreamSetInfo, 12}},
    };
    for (const auto& [name, info] : writes) {
        const u32 slot = info.first;
        method(std::string("Vitreo_IAVIStream_") + name, info.second, [&mgr, slot](Marshaler& m) {
            m.set_return(mgr.stream_write_method(m.arg_ptr(0), slot));
        });
    }
    // IAVIStreaming
    method("Vitreo_IAVIStreaming_Begin", 16, [](Marshaler& m) { m.set_return(AVIERR_OK); });
    method("Vitreo_IAVIStreaming_End", 4, [](Marshaler& m) { m.set_return(AVIERR_OK); });
    // IGetFrame
    method("Vitreo_IGetFrame_GetFrame", 8, [&mgr, i32_arg](Marshaler& m) {
        m.set_return(mgr.get_frame(m, m.arg_ptr(0), i32_arg(m, 1)));
    });
    method("Vitreo_IGetFrame_Begin", 16, [](Marshaler& m) { m.set_return(AVIERR_OK); });
    method("Vitreo_IGetFrame_End", 4, [](Marshaler& m) { m.set_return(AVIERR_OK); });
    method("Vitreo_IGetFrame_SetFormat", 28, [&mgr](Marshaler& m) {
        m.set_return(mgr.get_frame_set_format(m.arg_ptr(0)));
    });
    return exports;
}

} // namespace vitreo::winapi
