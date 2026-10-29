// SPDX-License-Identifier: LGPL-2.1-only

#include "com_core.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "filter_graph.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "winapi/com/rpcrt4/rpcrt4.hpp"
#include "capsule/registry/registry.hpp"
#include "capsule/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

/// @file additional_ole32_stubs.cpp
/// @brief Additional OLE32 stubs
///
/// Closed cut from com_core.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// ----- Additional OLE32 stubs

void ComCoreManager::impl_IsAccelerator(Marshaler& m) {
    (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
    VITREO_TRACE("[ole32] IsAccelerator -> FALSE\n");
    m.set_return(0);
}

void ComCoreManager::impl_OleTranslateAccelerator(Marshaler& m) {
    (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
    VITREO_TRACE("[ole32] OleTranslateAccelerator -> S_FALSE\n");
    m.set_return(com::S_FALSE);
}

void ComCoreManager::impl_OleDestroyMenuDescriptor(Marshaler& m) {
    if (m.arg(0) == 0) { m.set_return(0x80070006u /* E_HANDLE */); return; }
    VITREO_TRACE("[ole32] OleDestroyMenuDescriptor -> S_OK\n");
    m.set_return(com::S_OK);
}

void ComCoreManager::impl_OleCreateMenuDescriptor(Marshaler& m) {
    (void)m.arg(0); (void)m.arg(1);
    VITREO_TRACE("[ole32] OleCreateMenuDescriptor -> 0xFADE0001\n");
    m.set_return(0xFADE0001u);
}

void ComCoreManager::impl_OleLockRunning(Marshaler& m) {
    if (m.arg(0) == 0) { m.set_return(0x80070057u /* E_INVALIDARG */); return; }
    (void)m.arg(1); (void)m.arg(2);
    VITREO_TRACE("[ole32] OleLockRunning -> S_OK\n");
    m.set_return(com::S_OK);
}

void ComCoreManager::impl_OleGetClipboard(Marshaler& m) {
    (void)m.arg(0);
    VITREO_TRACE("[ole32] OleGetClipboard -> E_FAIL\n");
    m.set_return(0x80004005u);
}

// ---- OLE clipboard ------------------------------------------------------
//
// Learn: https://learn.microsoft.com/en-us/windows/win32/api/ole2/nf-ole2-olesetclipboard
// Placing an IDataObject on the clipboard means rendering its formats through
// the object's own IDataObject::GetData. Vitreo has no bridge from a guest
// data object to the host pasteboard, so the data never reaches the
// clipboard; CLIPBRD_E_CANT_SET is the documented failure for exactly that,
// and it keeps OleGetClipboard's answer consistent.
void ComCoreManager::impl_OleSetClipboard(Marshaler& m) {
    constexpr u32 CLIPBRD_E_CANT_SET = 0x800401D0u;
    const GuestAddr data_object = m.arg_ptr(0);
    VITREO_TRACE("[ole32] OleSetClipboard(0x%llx) -> CLIPBRD_E_CANT_SET\n",
                 static_cast<unsigned long long>(data_object));
    m.set_return(CLIPBRD_E_CANT_SET);
}

// S_OK only when the object *is* the current clipboard owner. Nothing ever
// becomes the owner here, so the answer is always S_FALSE — which is what a
// caller checks before deciding whether it still has to flush.
void ComCoreManager::impl_OleIsCurrentClipboard(Marshaler& m) {
    (void)m.arg(0);
    VITREO_TRACE("[ole32] OleIsCurrentClipboard -> S_FALSE\n");
    m.set_return(com::S_FALSE);
}

// Renders the owned object's data so it outlives the owner and releases
// ownership. With no OLE clipboard owner there is nothing to render, and
// Windows returns S_OK for that case too.
void ComCoreManager::impl_OleFlushClipboard(Marshaler& m) {
    VITREO_TRACE("[ole32] OleFlushClipboard -> S_OK\n");
    m.set_return(com::S_OK);
}

// RegisterDragDrop / RevokeDragDrop (ole2.h). Measured
// (tools/oracle_probe/s7_ole_probe.expected):
//  - Register needs OleInitialize on the thread: CoInitialize alone gives
//    E_OUTOFMEMORY and no AddRef.
//  - A NULL or non-window hwnd is DRAGDROP_E_INVALIDHWND, a NULL target
//    E_INVALIDARG, a second registration DRAGDROP_E_ALREADYREGISTERED.
//  - Success AddRefs the target and sets the "OleDropTargetInterface" window
//    property; Revoke Releases it, and an unregistered window is
//    DRAGDROP_E_NOTREGISTERED.
namespace {
constexpr u32 kDragDropENotRegistered = 0x80040100u;
constexpr u32 kDragDropEAlreadyRegistered = 0x80040101u;
constexpr u32 kDragDropEInvalidHwnd = 0x80040102u;
constexpr char16_t kDropTargetProp[] = u"OleDropTargetInterface";
} // namespace

void ComCoreManager::impl_RevokeDragDrop(Marshaler& m) {
    const u64 hwnd = m.arg_ptr(0);
    if (hwnd == 0 || call_guest_export(m, "user32.dll", "IsWindow", {hwnd}) == 0) {
        m.set_return(kDragDropEInvalidHwnd);
        return;
    }
    GuestAddr target = 0;
    {
        std::lock_guard lock(m_drop_targets_mutex);
        auto it = m_drop_targets.find(hwnd);
        if (it == m_drop_targets.end()) {
            m.set_return(kDragDropENotRegistered);
            return;
        }
        target = it->second;
        m_drop_targets.erase(it);
    }
    const GuestAddr name = co_task_mem_alloc_guest(sizeof(kDropTargetProp));
    if (name != 0) {
        for (usize i = 0; i < std::size(kDropTargetProp); ++i) {
            (void)m.write_u16(name + i * 2, static_cast<u16>(kDropTargetProp[i]));
        }
        (void)call_guest_export(m, "user32.dll", "RemovePropW", {hwnd, name});
        co_task_mem_free_guest(name);
    }
    guest_iunknown_release(m, target);
    m.set_return(com::S_OK);
}

void ComCoreManager::impl_CoLockObjectExternal(Marshaler& m) {
    if (m.arg(0) == 0) { m.set_return(0x80070057u /* E_INVALIDARG */); return; }
    (void)m.arg(1); (void)m.arg(2);
    VITREO_TRACE("[ole32] CoLockObjectExternal -> S_OK\n");
    m.set_return(com::S_OK);
}

void ComCoreManager::impl_RegisterDragDrop(Marshaler& m) {
    const u64 hwnd = m.arg_ptr(0);
    const GuestAddr target = m.arg_ptr(1);
    if (!thread_ole_initialized(m)) {
        m.set_return(0x8007000Eu);  // E_OUTOFMEMORY (measured)
        return;
    }
    if (hwnd == 0 || call_guest_export(m, "user32.dll", "IsWindow", {hwnd}) == 0) {
        m.set_return(kDragDropEInvalidHwnd);
        return;
    }
    if (target == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    {
        std::lock_guard lock(m_drop_targets_mutex);
        if (m_drop_targets.contains(hwnd)) {
            m.set_return(kDragDropEAlreadyRegistered);
            return;
        }
        m_drop_targets.emplace(hwnd, target);
    }
    (void)guest_iunknown_addref(m, target);
    const GuestAddr name = co_task_mem_alloc_guest(sizeof(kDropTargetProp));
    if (name != 0) {
        for (usize i = 0; i < std::size(kDropTargetProp); ++i) {
            (void)m.write_u16(name + i * 2, static_cast<u16>(kDropTargetProp[i]));
        }
        (void)call_guest_export(m, "user32.dll", "SetPropW", {hwnd, name, target});
        co_task_mem_free_guest(name);
    }
    m.set_return(com::S_OK);
}

void ComCoreManager::impl_DoDragDrop(Marshaler& m) {
    // HRESULT DoDragDrop(IDataObject*, IDropSource*, DWORD, DWORD* pdwEffect)
    // The modal drag loop (IDropSource::QueryContinueDrag/GiveFeedback over
    // registered IDropTargets) is not built; report E_UNEXPECTED, the
    // documented failure, rather than inventing a user cancel.
    if (m.arg_ptr(0) == 0 || m.arg_ptr(1) == 0 || m.arg_ptr(3) == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    static std::once_flag once;
    std::call_once(once, [] {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ole32!DoDragDrop: modal drag loop\n");
    });
    m.set_return(0x8000FFFFu);  // E_UNEXPECTED
}

std::shared_ptr<ComCoreManager::MemoryStreamState>
ComCoreManager::find_memory_stream(GuestAddr object) {
    std::lock_guard lock(m_memory_streams_mutex);
    auto it = m_memory_streams.find(object);
    return it == m_memory_streams.end() ? nullptr : it->second;
}

// ---- HGLOBAL-backed stream storage (CreateStreamOnHGlobal) -----------------

ComCoreManager::MemoryStreamBacking::~MemoryStreamBacking() {
    // Learn CreateStreamOnHGlobal: with fDeleteOnRelease the HGLOBAL is freed
    // on the final release of the stream (clones share this backing).
    if (hglobal != 0 && delete_on_release && globals != nullptr) {
        (void)globals->global_free(hglobal);
    }
}

bool ComCoreManager::MemoryStreamBacking::read_at(GuestMemory& memory, u64 pos,
                                                  u8* out, u64 n) {
    if (n == 0) return true;
    if (!is_hglobal()) {
        std::memcpy(out, bytes.data() + pos, static_cast<usize>(n));
        return true;
    }
    const u64 data = globals->global_lock(hglobal);
    if (data == 0) return false;
    const bool ok = memory.read_bytes(data + pos, reinterpret_cast<Byte*>(out),
                                      static_cast<usize>(n)).is_ok();
    (void)globals->global_unlock(hglobal);
    return ok;
}

bool ComCoreManager::MemoryStreamBacking::write_at(GuestMemory& memory, u64 pos,
                                                   const u8* in, u64 n) {
    if (n == 0) return true;
    if (!is_hglobal()) {
        std::memcpy(bytes.data() + pos, in, static_cast<usize>(n));
        return true;
    }
    const u64 data = globals->global_lock(hglobal);
    if (data == 0) return false;
    const bool ok = memory.write_bytes(data + pos, reinterpret_cast<const Byte*>(in),
                                       static_cast<usize>(n)).is_ok();
    (void)globals->global_unlock(hglobal);
    return ok;
}

bool ComCoreManager::MemoryStreamBacking::resize(u64 new_size) {
    if (!is_hglobal()) {
        if (new_size > std::numeric_limits<usize>::max()) return false;
        bytes.resize(static_cast<usize>(new_size), 0);
        return true;
    }
    if (new_size > globals->global_size(hglobal)) {
        constexpr u32 GMEM_MOVEABLE = 0x2;
        const u64 grown = globals->global_realloc(hglobal, new_size, GMEM_MOVEABLE);
        if (grown == 0) return false;
        hglobal = grown;
    }
    size = new_size;
    return true;
}

GuestAddr ComCoreManager::create_memory_stream_object(
    Marshaler& m,
    std::shared_ptr<MemoryStreamBacking> backing,
    u64 position) {
    if (m_thunks == nullptr || backing == nullptr) return 0;

    static constexpr std::array<std::string_view, 14> method_names{{
        "Vitreo_MemoryStream_QueryInterface",
        "Vitreo_MemoryStream_AddRef",
        "Vitreo_MemoryStream_Release",
        "Vitreo_MemoryStream_Read",
        "Vitreo_MemoryStream_Write",
        "Vitreo_MemoryStream_Seek",
        "Vitreo_MemoryStream_SetSize",
        "Vitreo_MemoryStream_CopyTo",
        "Vitreo_MemoryStream_Commit",
        "Vitreo_MemoryStream_Revert",
        "Vitreo_MemoryStream_LockRegion",
        "Vitreo_MemoryStream_UnlockRegion",
        "Vitreo_MemoryStream_Stat",
        "Vitreo_MemoryStream_Clone",
    }};

    std::array<GuestAddr, method_names.size()> methods{};
    for (usize i = 0; i < method_names.size(); ++i) {
        auto thunk = m_thunks->get_thunk_address("ole32.dll", method_names[i]);
        if (thunk.is_err()) return 0;
        methods[i] = thunk.value();
    }

    const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const u32 allocation_size =
        pointer_size * static_cast<u32>(1 + methods.size());
    const GuestAddr object = co_task_mem_alloc_guest(allocation_size);
    if (object == 0 ||
        !m.memory().is_mapped(object, allocation_size)) {
        return 0;
    }
    const GuestAddr vtable = object + pointer_size;
    if (m.write_ptr(object, vtable).is_err()) return 0;
    for (usize i = 0; i < methods.size(); ++i) {
        if (m.write_ptr(vtable + i * pointer_size, methods[i]).is_err()) {
            return 0;
        }
    }

    auto state = std::make_shared<MemoryStreamState>();
    state->position = position;
    state->backing = std::move(backing);
    {
        std::lock_guard lock(m_memory_streams_mutex);
        m_memory_streams[object] = std::move(state);
    }
    return object;
}

GuestAddr ComCoreManager::create_memory_stream(
    Marshaler& m, GuestAddr initial_data, u32 initial_size) {
    if (initial_data == 0 && initial_size != 0) return 0;

    auto backing = std::make_shared<MemoryStreamBacking>();
    backing->bytes.resize(initial_size);
    if (initial_size != 0 &&
        m.memory().read_bytes(initial_data, backing->bytes.data(),
                              initial_size).is_err()) {
        return 0;
    }
    return create_memory_stream_object(m, std::move(backing), 0);
}

void ComCoreManager::memory_stream_query_interface(Marshaler& m) {
    constexpr u32 E_NOINTERFACE_HR = 0x80004002u;
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    auto state = find_memory_stream(object);
    if (state == nullptr || iid_ptr == 0 || out_ptr == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }

    constexpr Guid IID_ISTREAM{
        0x0000000Cu, 0x0000u, 0x0000u,
        {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
    constexpr Guid IID_ISEQUENTIALSTREAM{
        0x0C733A30u, 0x2A1Cu, 0x11CEu,
        {0xAD,0xE5,0x00,0xAA,0x00,0x44,0x77,0x3D}};

    auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    const bool supported =
        guid_equal(iid.value(), IID_IUNKNOWN) ||
        guid_equal(iid.value(), IID_ISTREAM) ||
        guid_equal(iid.value(), IID_ISEQUENTIALSTREAM);
    if (!supported) {
        (void)m.write_ptr(out_ptr, 0);
        m.set_return(E_NOINTERFACE_HR);
        return;
    }

    {
        std::lock_guard lock(state->mutex);
        ++state->ref_count;
    }
    if (m.write_ptr(out_ptr, object).is_err()) {
        std::lock_guard lock(state->mutex);
        --state->ref_count;
        m.set_return(com::E_INVALIDARG);
        return;
    }
    m.set_return(com::S_OK);
}

void ComCoreManager::memory_stream_add_ref(Marshaler& m) {
    auto state = find_memory_stream(m.arg_ptr(0));
    if (state == nullptr) {
        m.set_return(0);
        return;
    }
    std::lock_guard lock(state->mutex);
    m.set_return(++state->ref_count);
}

void ComCoreManager::memory_stream_release(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    auto state = find_memory_stream(object);
    if (state == nullptr) {
        m.set_return(0);
        return;
    }

    u32 refs = 0;
    {
        std::lock_guard lock(state->mutex);
        if (state->ref_count != 0) --state->ref_count;
        refs = state->ref_count;
    }
    if (refs == 0) {
        std::lock_guard lock(m_memory_streams_mutex);
        auto it = m_memory_streams.find(object);
        if (it != m_memory_streams.end() && it->second == state) {
            m_memory_streams.erase(it);
            co_task_mem_free_guest(object);
        }
    }
    m.set_return(refs);
}

void ComCoreManager::memory_stream_read(Marshaler& m) {
    constexpr u32 STG_E_INVALIDPOINTER = 0x80030009u;
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 requested = static_cast<u32>(m.arg(2));
    const GuestAddr bytes_read_ptr = m.arg_ptr(3);
    if (bytes_read_ptr != 0) (void)m.write_u32(bytes_read_ptr, 0);

    auto state = find_memory_stream(object);
    if (state == nullptr || (buffer == 0 && requested != 0)) {
        m.set_return(STG_E_INVALIDPOINTER);
        return;
    }

    u32 count = 0;
    bool hglobal_kind = false;
    {
        std::lock_guard state_lock(state->mutex);
        std::lock_guard backing_lock(state->backing->mutex);
        hglobal_kind = state->backing->is_hglobal();
        const u64 size = state->backing->stream_size();
        const u64 remaining =
            state->position < size ? size - state->position : 0;
        count = static_cast<u32>(
            std::min<u64>(remaining, static_cast<u64>(requested)));
        std::vector<u8> chunk(count);
        if (count != 0 &&
            (!state->backing->read_at(m.memory(), state->position, chunk.data(), count) ||
             m.memory().write_bytes(buffer, chunk.data(), count).is_err())) {
            m.set_return(STG_E_INVALIDPOINTER);
            return;
        }
        state->position += count;
    }
    if (bytes_read_ptr != 0) (void)m.write_u32(bytes_read_ptr, count);
    // Measured: the HGLOBAL stream answers a short read with S_OK
    // (s7_ole_probe "Read 16 of 5"); SHCreateMemStream's reports S_FALSE.
    m.set_return(count == requested || hglobal_kind ? com::S_OK : com::S_FALSE);
}

void ComCoreManager::memory_stream_write(Marshaler& m) {
    constexpr u32 STG_E_INVALIDPOINTER = 0x80030009u;
    constexpr u32 STG_E_MEDIUMFULL = 0x80030070u;
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 requested = static_cast<u32>(m.arg(2));
    const GuestAddr bytes_written_ptr = m.arg_ptr(3);
    if (bytes_written_ptr != 0) (void)m.write_u32(bytes_written_ptr, 0);

    auto state = find_memory_stream(object);
    if (state == nullptr || (buffer == 0 && requested != 0)) {
        m.set_return(STG_E_INVALIDPOINTER);
        return;
    }

    std::vector<u8> source(requested);
    if (requested != 0 &&
        m.memory().read_bytes(buffer, source.data(), requested).is_err()) {
        m.set_return(STG_E_INVALIDPOINTER);
        return;
    }

    std::lock_guard state_lock(state->mutex);
    std::lock_guard backing_lock(state->backing->mutex);
    if (state->position >
        std::numeric_limits<u64>::max() - requested) {
        m.set_return(STG_E_MEDIUMFULL);
        return;
    }
    const u64 end = state->position + requested;
    if (end > std::numeric_limits<usize>::max()) {
        m.set_return(STG_E_MEDIUMFULL);
        return;
    }
    if (end > state->backing->stream_size() && !state->backing->resize(end)) {
        m.set_return(STG_E_MEDIUMFULL);
        return;
    }
    if (!state->backing->write_at(m.memory(), state->position, source.data(),
                                  requested)) {
        m.set_return(STG_E_INVALIDPOINTER);
        return;
    }
    state->position = end;
    if (bytes_written_ptr != 0) {
        (void)m.write_u32(bytes_written_ptr, requested);
    }
    m.set_return(com::S_OK);
}

void ComCoreManager::memory_stream_seek(Marshaler& m) {
    constexpr u32 STG_E_INVALIDFUNCTION = 0x80030001u;
    const GuestAddr object = m.arg_ptr(0);
    const bool is_64bit = m.cpu_is_64bit();
    const u64 raw_move = is_64bit
        ? m.arg(1)
        : (static_cast<u64>(static_cast<u32>(m.arg(2))) << 32) |
              static_cast<u32>(m.arg(1));
    const i64 move = static_cast<i64>(raw_move);
    const u32 origin =
        static_cast<u32>(m.arg(is_64bit ? 2 : 3));
    const GuestAddr new_position_ptr =
        m.arg_ptr(is_64bit ? 3 : 4);

    auto state = find_memory_stream(object);
    if (state == nullptr) {
        m.set_return(STG_E_INVALIDFUNCTION);
        return;
    }
    // Measured: the HGLOBAL stream reports every bad seek (before the start,
    // unknown origin) as STG_E_SEEKERROR.
    constexpr u32 STG_E_SEEKERROR = 0x80030019u;
    const u32 seek_error =
        state->backing->is_hglobal() ? STG_E_SEEKERROR : STG_E_INVALIDFUNCTION;
    if (origin > 2) {
        m.set_return(seek_error);
        return;
    }

    std::lock_guard state_lock(state->mutex);
    u64 base = 0;
    if (origin == 1) {
        base = state->position;
    } else if (origin == 2) {
        std::lock_guard backing_lock(state->backing->mutex);
        base = state->backing->stream_size();
    }
    u64 next = 0;
    if (move < 0) {
        const u64 magnitude = static_cast<u64>(-(move + 1)) + 1;
        if (magnitude > base) {
            m.set_return(seek_error);
            return;
        }
        next = base - magnitude;
    } else {
        const u64 magnitude = static_cast<u64>(move);
        if (base > std::numeric_limits<u64>::max() - magnitude) {
            m.set_return(STG_E_INVALIDFUNCTION);
            return;
        }
        next = base + magnitude;
    }
    state->position = next;
    if (new_position_ptr != 0 &&
        m.write_u64(new_position_ptr, next).is_err()) {
        m.set_return(STG_E_INVALIDFUNCTION);
        return;
    }
    m.set_return(com::S_OK);
}

void ComCoreManager::memory_stream_set_size(Marshaler& m) {
    constexpr u32 STG_E_INVALIDPOINTER = 0x80030009u;
    constexpr u32 STG_E_MEDIUMFULL = 0x80030070u;
    const bool is_64bit = m.cpu_is_64bit();
    const u64 new_size = is_64bit
        ? m.arg(1)
        : (static_cast<u64>(static_cast<u32>(m.arg(2))) << 32) |
              static_cast<u32>(m.arg(1));
    auto state = find_memory_stream(m.arg_ptr(0));
    if (state == nullptr) {
        m.set_return(STG_E_INVALIDPOINTER);
        return;
    }
    if (new_size > std::numeric_limits<usize>::max()) {
        m.set_return(STG_E_MEDIUMFULL);
        return;
    }
    std::lock_guard backing_lock(state->backing->mutex);
    m.set_return(state->backing->resize(new_size) ? com::S_OK : STG_E_MEDIUMFULL);
}

void ComCoreManager::memory_stream_copy_to(Marshaler& m) {
    constexpr u32 E_NOTIMPL = 0x80004001u;
    const bool is_64bit = m.cpu_is_64bit();
    const GuestAddr read_ptr = m.arg_ptr(is_64bit ? 3 : 4);
    const GuestAddr written_ptr = m.arg_ptr(is_64bit ? 4 : 5);
    if (read_ptr != 0) (void)m.write_u64(read_ptr, 0);
    if (written_ptr != 0) (void)m.write_u64(written_ptr, 0);
    // Invoking an arbitrary guest IStream::Write from inside a host thunk
    // requires nested guest callback dispatch. Until that facility is exposed
    // here, fail explicitly rather than claiming data was copied.
    m.set_return(E_NOTIMPL);
}

void ComCoreManager::memory_stream_commit(Marshaler& m) {
    auto state = find_memory_stream(m.arg_ptr(0));
    m.set_return(state != nullptr ? com::S_OK : com::E_INVALIDARG);
}

void ComCoreManager::memory_stream_revert(Marshaler& m) {
    // A direct-mode HGLOBAL stream has nothing to revert (measured S_OK).
    constexpr u32 STG_E_INVALIDFUNCTION = 0x80030001u;
    auto state = find_memory_stream(m.arg_ptr(0));
    m.set_return(state != nullptr && state->backing->is_hglobal()
                     ? com::S_OK : STG_E_INVALIDFUNCTION);
}

void ComCoreManager::memory_stream_lock_region(Marshaler& m) {
    constexpr u32 STG_E_INVALIDFUNCTION = 0x80030001u;
    (void)m.arg_ptr(0);
    m.set_return(STG_E_INVALIDFUNCTION);
}

void ComCoreManager::memory_stream_unlock_region(Marshaler& m) {
    constexpr u32 STG_E_INVALIDFUNCTION = 0x80030001u;
    (void)m.arg_ptr(0);
    m.set_return(STG_E_INVALIDFUNCTION);
}

void ComCoreManager::memory_stream_stat(Marshaler& m) {
    constexpr u32 STG_E_INVALIDPOINTER = 0x80030009u;
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr stat_ptr = m.arg_ptr(1);
    auto state = find_memory_stream(object);
    if (state == nullptr || stat_ptr == 0) {
        m.set_return(STG_E_INVALIDPOINTER);
        return;
    }

    const usize stat_size = m.cpu_is_64bit() ? 80 : 72;
    std::array<u8, 80> stat{};
    if (m.memory().write_bytes(stat_ptr, stat.data(), stat_size).is_err()) {
        m.set_return(STG_E_INVALIDPOINTER);
        return;
    }
    const GuestAddr type_offset = m.cpu_is_64bit() ? 8 : 4;
    const GuestAddr size_offset = m.cpu_is_64bit() ? 16 : 8;
    const GuestAddr mode_offset = m.cpu_is_64bit() ? 48 : 40;
    u64 size = 0;
    {
        std::lock_guard backing_lock(state->backing->mutex);
        size = state->backing->stream_size();
    }
    (void)m.write_u32(stat_ptr + type_offset, 2); // STGTY_STREAM
    (void)m.write_u64(stat_ptr + size_offset, size);
    (void)m.write_u32(stat_ptr + mode_offset, 2); // STGM_READWRITE
    m.set_return(com::S_OK);
}

void ComCoreManager::memory_stream_clone(Marshaler& m) {
    constexpr u32 E_OUTOFMEMORY = 0x8007000Eu;
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr out_ptr = m.arg_ptr(1);
    auto state = find_memory_stream(object);
    if (state == nullptr || out_ptr == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }

    u64 position = 0;
    std::shared_ptr<MemoryStreamBacking> backing;
    {
        std::lock_guard lock(state->mutex);
        position = state->position;
        backing = state->backing;
    }
    const GuestAddr clone =
        create_memory_stream_object(m, std::move(backing), position);
    if (clone == 0 || m.write_ptr(out_ptr, clone).is_err()) {
        (void)m.write_ptr(out_ptr, 0);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(com::S_OK);
}

void ComCoreManager::impl_CreateStreamOnHGlobal(Marshaler& m) {
    // HRESULT CreateStreamOnHGlobal(HGLOBAL hGlobal, BOOL fDeleteOnRelease,
    //                               LPSTREAM *ppstm)
    // https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-createstreamonhglobal
    // "The initial contents of the stream are the current contents of the
    // memory block"; hGlobal NULL allocates a new (moveable) block; the
    // stream's bytes live in the HGLOBAL, so GetHGlobalFromStream + GlobalLock
    // see them. Measured: ppstm NULL is E_INVALIDARG.
    const GuestAddr hglobal = m.arg_ptr(0);
    const u32 delete_on_release = m.arg32(1);
    const GuestAddr out_ptr = m.arg_ptr(2);

    if (out_ptr == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(out_ptr, 0);
    if (m_globals == nullptr) {
        m.set_return(0x8007000Eu); // E_OUTOFMEMORY
        return;
    }
    m_globals->set_guest_64bit(m.cpu_is_64bit());

    auto backing = std::make_shared<MemoryStreamBacking>();
    backing->globals = m_globals;
    backing->delete_on_release = delete_on_release != 0;
    if (hglobal != 0) {
        backing->hglobal = hglobal;
        backing->size = m_globals->global_size(hglobal);
    } else {
        constexpr u32 GMEM_MOVEABLE = 0x2, GMEM_NODISCARD = 0x20;
        backing->hglobal = m_globals->global_alloc(GMEM_MOVEABLE | GMEM_NODISCARD, 0);
        if (backing->hglobal == 0) {
            m.set_return(0x8007000Eu);
            return;
        }
    }

    const GuestAddr stream = create_memory_stream_object(m, std::move(backing), 0);
    if (stream == 0) {
        VITREO_TRACE("[ole32] CreateStreamOnHGlobal -> E_OUTOFMEMORY\n");
        m.set_return(0x8007000Eu); // E_OUTOFMEMORY
        return;
    }
    if (m.write_ptr(out_ptr, stream).is_err()) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    VITREO_TRACE("[ole32] CreateStreamOnHGlobal(hGlobal=0x%llx, delete=%u) "
                 "-> IStream=0x%llx\n",
                 static_cast<unsigned long long>(hglobal), delete_on_release,
                 static_cast<unsigned long long>(stream));
    m.set_return(com::S_OK);
}

} // namespace vitreo::winapi
