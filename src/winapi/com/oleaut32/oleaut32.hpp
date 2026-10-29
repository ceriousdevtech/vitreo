// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file oleaut32.hpp
/// @brief OLEAUT32.dll API stubs (OLE Automation)

#include <vitreo/common.hpp>
#include "capsule/registry/registry.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/com/oleaut32/typelib.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

class Marshaler;
class VirtualMemoryManager;
class ComTaskMemoryManager;

// Scratch region for typelib objects and other long-lived descriptors
// (bump-only, never recycled). BSTRs and SAFEARRAYs do NOT live here: like
// Windows they come from the COM task allocator (the process heap) so
// SysFreeString / SafeArrayDestroy really release them.
// Hole below the d3d9 lock region (0x50000000):
//   0x40000000 .. 0x50000000 (256 MB)
static constexpr GuestAddr OLEAUT32_SCRATCH_BASE = 0x40000000;
static constexpr u32       OLEAUT32_SCRATCH_SIZE = 0x10000000; // 256 MB
/// Refuse single scratch allocations larger than this.
static constexpr u32       OLEAUT32_SCRATCH_ALLOC_MAX = 0x01000000; // 16 MB

// =============================================================================
// OleAut32Manager
// =============================================================================

class OleAut32Manager {
public:
    OleAut32Manager() = default;
    ~OleAut32Manager() = default;

    OleAut32Manager(const OleAut32Manager&) = delete;
    OleAut32Manager& operator=(const OleAut32Manager&) = delete;

    /// Scratch commits go through the VMM (the range is reserved at registration).
    void set_virtual_memory_manager(VirtualMemoryManager* vmm) noexcept {
        m_vmm = vmm;
    }

    /// BSTR and SAFEARRAY storage comes from the COM task allocator, as on
    /// Windows (oleaut32 allocates through IMalloc). Without one (bare unit
    /// tests) allocations fall back to the scratch bump and are never freed.
    void set_task_memory(ComTaskMemoryManager* task_memory) noexcept {
        m_task_memory = task_memory;
    }

    /// Bump-allocate from the scratch region. Returns 0 on OOM.
    /// Used for typelib objects and descriptors — never recycled.
    [[nodiscard]] GuestAddr scratch_alloc(u32 size) noexcept;

    /// Allocate `size` zeroed bytes from the task allocator (16-byte aligned
    /// on x64, as HeapAlloc). Returns 0 on failure.
    [[nodiscard]] GuestAddr oa_alloc(Marshaler& m, u64 size);
    /// Free a block from oa_alloc. Blocks the allocator does not own are ignored.
    void oa_free(GuestAddr block);

    /// SysAllocStringByteLen semantics: allocate a BSTR of `byte_len` bytes,
    /// copying from `src` when non-NULL (else zero-filled), followed by a
    /// two-byte NUL. Returns the BSTR (first character) or 0.
    [[nodiscard]] GuestAddr bstr_alloc_bytes(Marshaler& m, GuestAddr src, u64 byte_len);
    /// SysFreeString on an explicit pointer.
    void bstr_free(Marshaler& m, GuestAddr bstr);

    /// Pointer-form VariantClear / VariantCopy (BSTRs duplicated, interfaces
    /// AddRef'd / Released, VT_ARRAY copied / destroyed).
    [[nodiscard]] u32 variant_clear_at(Marshaler& m, GuestAddr pvar);
    [[nodiscard]] u32 variant_copy_at(Marshaler& m, GuestAddr dst, GuestAddr src);

    /// IUnknown::AddRef / Release through the guest vtable (no-op on NULL).
    void com_add_ref(Marshaler& m, GuestAddr punk);
    void com_release(Marshaler& m, GuestAddr punk);

    /// Pointer-form SafeArrayCreate / CreateVector / Destroy / Copy.
    [[nodiscard]] GuestAddr sa_create(Marshaler& m, u16 vt, u32 c_dims,
                                      GuestAddr rgsabound);
    [[nodiscard]] GuestAddr sa_create_vector(Marshaler& m, u16 vt, i32 lbound,
                                             u32 count);
    [[nodiscard]] u32 sa_destroy(Marshaler& m, GuestAddr psa);
    [[nodiscard]] u32 sa_copy(Marshaler& m, GuestAddr psa, GuestAddr& out);

    /// SafeArrayPtrOfIndex(psa, rgIndices, ppvData).
    [[nodiscard]] u32 safe_array_ptr_of_index(Marshaler& m);

    /// SysAllocString — allocate BSTR from null-terminated UTF-16 string.
    [[nodiscard]] GuestAddr sys_alloc_string(Marshaler& m);
    [[nodiscard]] GuestAddr sys_alloc_string_from_z(Marshaler& m, GuestAddr psz);

    /// SysAllocStringLen — allocate BSTR with explicit char count.
    [[nodiscard]] GuestAddr sys_alloc_string_len(Marshaler& m, GuestAddr psz, u32 cch);

    /// SysAllocStringByteLen — allocate BSTR with explicit byte count.
    [[nodiscard]] GuestAddr sys_alloc_string_byte_len(Marshaler& m, GuestAddr psz, u32 len);

    /// Allocate a presented BSTR from an already-marshaled UTF-16 payload.
    /// The payload excludes the four-byte byte-length prefix and terminator.
    [[nodiscard]] GuestAddr allocate_bstr_bytes(
        Marshaler& m, std::span<const Byte> utf16_bytes);

    /// SysFreeString — free a BSTR back to the task allocator.
    void sys_free_string(Marshaler& m, GuestAddr bstr);

    /// SysStringLen — return char count of BSTR.
    [[nodiscard]] u32 sys_string_len(Marshaler& m, GuestAddr bstr);

    /// SysStringByteLen — return byte count of BSTR.
    [[nodiscard]] u32 sys_string_byte_len(Marshaler& m, GuestAddr bstr);

    /// SysReAllocStringLen — reallocate BSTR. Returns TRUE (1).
    [[nodiscard]] u32 sys_realloc_string_len(Marshaler& m);

    /// VariantInit — zero VARIANT (16 bytes x86 / 24 bytes x64).
    void variant_init(Marshaler& m);

    /// VariantClear — clear VARIANT, freeing BSTR if VT_BSTR. Returns S_OK.
    [[nodiscard]] u32 variant_clear(Marshaler& m);

    /// VariantCopy — copy VARIANT from src to dst. Returns S_OK.
    [[nodiscard]] u32 variant_copy(Marshaler& m);

    /// VariantChangeType — convert VARIANT between types.
    [[nodiscard]] u32 variant_change_type(Marshaler& m);

    /// Pointer-form VariantChangeType used by DispGetParam (same coerce rules).
    [[nodiscard]] u32 variant_change_type_ex(Marshaler& m,
                                             GuestAddr dest,
                                             GuestAddr src,
                                             u16 flags,
                                             u16 target_vt);

    /// SafeArrayCreate — allocate a SAFEARRAY from the task allocator.
    [[nodiscard]] GuestAddr safe_array_create(Marshaler& m);

    /// SafeArrayDestroy — free array data and descriptor.
    [[nodiscard]] u32 safe_array_destroy(Marshaler& m);

    /// SafeArrayGetDim — return number of dimensions (0 if psa is NULL).
    [[nodiscard]] u32 safe_array_get_dim(Marshaler& m);

    /// SafeArrayGetElemsize — return cbElements (0 if psa is NULL).
    [[nodiscard]] u32 safe_array_get_elemsize(Marshaler& m);

    /// SafeArrayGetUBound / SafeArrayGetLBound.
    [[nodiscard]] u32 safe_array_get_ubound(Marshaler& m);
    [[nodiscard]] u32 safe_array_get_lbound(Marshaler& m);

    /// SafeArrayLock / Unlock — bump the descriptor lock count.
    [[nodiscard]] u32 safe_array_lock(Marshaler& m);
    [[nodiscard]] u32 safe_array_unlock(Marshaler& m);

    /// SafeArrayAccessData / UnaccessData — lock and hand back pvData.
    [[nodiscard]] u32 safe_array_access_data(Marshaler& m);
    [[nodiscard]] u32 safe_array_unaccess_data(Marshaler& m);

    /// SafeArrayGetElement / PutElement — copy one cell (deep-copies BSTRs).
    [[nodiscard]] u32 safe_array_get_element(Marshaler& m);
    [[nodiscard]] u32 safe_array_put_element(Marshaler& m);

    /// SafeArrayCopy — allocate a new array and deep-copy contents.
    [[nodiscard]] u32 safe_array_copy(Marshaler& m);

    /// SafeArrayGetVartype — best-effort VARTYPE from fFeatures.
    [[nodiscard]] u32 safe_array_get_vartype(Marshaler& m);

    /// DispGetParam — pull one arg out of a DISPPARAMS and coerce it.
    /// oleaut32 ordinal 28. E_NOTIMPL aborts a script engine that imports
    /// this ordinal.
    [[nodiscard]] u32 disp_get_param(Marshaler& m);

    /// SafeArrayRedim — resize the last dimension (ordinal 40).
    [[nodiscard]] u32 safe_array_redim(Marshaler& m);

    /// OleCreateFontIndirect — stub, returns E_NOTIMPL.
    [[nodiscard]] u32 ole_create_font_indirect();

    // -------------------------------------------------------------------------
    // Type libraries
    //
    // Self-registering COM servers load their .tlb, hand the ITypeLib to
    // RegisterTypeLib, and release it.
    // Vitreo reads the library-level attributes out of the file and hands back
    // a real ITypeLib whose supported methods answer from those attributes;
    // methods that would need decoded type descriptions fail honestly rather
    // than returning invented data.
    // -------------------------------------------------------------------------

    /// Dependencies the type-library path needs. Wired at DLL registration.
    void set_vfs(capsule::Vfs* vfs) noexcept { m_vfs = vfs; }
    void set_registry(capsule::Registry* registry) noexcept { m_registry = registry; }
    void set_thunk_registry(ThunkRegistry* thunks) noexcept { m_thunks = thunks; }

    /// Guest callback used by ITypeInfo::Invoke to call through an instance
    /// vtable (same contract as Kernel32ExtManager::GuestCall).
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;
    void set_guest_call(GuestCall call) { m_guest_call = std::move(call); }

    /// LoadTypeLib(szFile, ppTypeLib)
    [[nodiscard]] u32 load_type_lib(Marshaler& m);

    /// LoadTypeLibEx(szFile, regkind, pptlib)
    [[nodiscard]] u32 load_type_lib_ex(Marshaler& m);

    /// RegisterTypeLib(ptlib, szFullPath, szHelpDir)
    [[nodiscard]] u32 register_type_lib(Marshaler& m);

    /// LoadRegTypeLib(rguid, wVerMajor, wVerMinor, lcid, pptlib)
    /// Looks up HKCR\TypeLib\{guid}\ver\lcid\win32 written by RegisterTypeLib,
    /// then loads that path via LoadTypeLib.
    [[nodiscard]] u32 load_reg_type_lib(Marshaler& m);

    /// UnRegisterTypeLib(libID, wVerMajor, wVerMinor, lcid, syskind)
    [[nodiscard]] u32 unregister_type_lib(Marshaler& m);

    /// Resolve an Automation interface through
    /// HKCR\Interface\{IID}\TypeLib and return its parsed type information.
    /// This is the metadata source used by OLE32's universal marshaler; no
    /// guest ITypeLib object or guest pointer crosses the subsystem boundary.
    [[nodiscard]] std::optional<TypeInfoSummary>
    resolve_registered_type_info(const TypeLibGuid& iid);

    // ITypeLib vtable entry points (guest calls these through the object).
    [[nodiscard]] u32 typelib_query_interface(Marshaler& m);
    [[nodiscard]] u32 typelib_add_ref(Marshaler& m);
    [[nodiscard]] u32 typelib_release(Marshaler& m);
    [[nodiscard]] u32 typelib_get_type_info_count(Marshaler& m);
    [[nodiscard]] u32 typelib_get_type_info(Marshaler& m);
    [[nodiscard]] u32 typelib_get_type_info_type(Marshaler& m);
    [[nodiscard]] u32 typelib_get_type_info_of_guid(Marshaler& m);
    [[nodiscard]] u32 typelib_get_lib_attr(Marshaler& m);
    [[nodiscard]] u32 typelib_get_type_comp(Marshaler& m);
    [[nodiscard]] u32 typelib_get_documentation(Marshaler& m);
    [[nodiscard]] u32 typelib_is_name(Marshaler& m);
    [[nodiscard]] u32 typelib_find_name(Marshaler& m);
    [[nodiscard]] u32 typelib_release_tlib_attr(Marshaler& m);

    // ITypeInfo vtable entry points (guest calls through the object).
    [[nodiscard]] u32 typeinfo_query_interface(Marshaler& m);
    [[nodiscard]] u32 typeinfo_add_ref(Marshaler& m);
    [[nodiscard]] u32 typeinfo_release(Marshaler& m);
    [[nodiscard]] u32 typeinfo_get_type_attr(Marshaler& m);
    [[nodiscard]] u32 typeinfo_get_type_comp(Marshaler& m);
    [[nodiscard]] u32 typeinfo_get_func_desc(Marshaler& m);
    [[nodiscard]] u32 typeinfo_get_var_desc(Marshaler& m);
    [[nodiscard]] u32 typeinfo_get_names(Marshaler& m);
    [[nodiscard]] u32 typeinfo_get_ref_type_of_impl_type(Marshaler& m);
    [[nodiscard]] u32 typeinfo_get_impl_type_flags(Marshaler& m);
    [[nodiscard]] u32 typeinfo_get_ids_of_names(Marshaler& m);
    [[nodiscard]] u32 typeinfo_invoke(Marshaler& m);
    [[nodiscard]] u32 typeinfo_get_documentation(Marshaler& m);
    [[nodiscard]] u32 typeinfo_get_dll_entry(Marshaler& m);
    [[nodiscard]] u32 typeinfo_get_ref_type_info(Marshaler& m);
    [[nodiscard]] u32 typeinfo_address_of_member(Marshaler& m);
    [[nodiscard]] u32 typeinfo_create_instance(Marshaler& m);
    [[nodiscard]] u32 typeinfo_get_mops(Marshaler& m);
    [[nodiscard]] u32 typeinfo_get_containing_type_lib(Marshaler& m);
    [[nodiscard]] u32 typeinfo_release_type_attr(Marshaler& m);
    [[nodiscard]] u32 typeinfo_release_func_desc(Marshaler& m);
    [[nodiscard]] u32 typeinfo_release_var_desc(Marshaler& m);

private:
    /// One loaded type library, keyed by the guest ITypeLib pointer.
    struct LoadedTypeLib {
        TypeLibAttributes            attributes;
        std::vector<TypeInfoSummary> typeinfos;
        std::string                  path; ///< Guest path it was loaded from.
        u32                          ref_count{1};
        GuestAddr                    object{0}; ///< Guest ITypeLib* for this state.
    };

    /// One ITypeInfo handed out from a loaded library.
    struct LoadedTypeInfo {
        std::shared_ptr<LoadedTypeLib> parent;
        u32                            index{0};
        u32                            ref_count{1};
    };

    /// Read, parse and publish a type library as a guest ITypeLib.
    /// Returns an HRESULT; writes the object pointer to `out_ptr` on success.
    [[nodiscard]] u32 load_type_lib_from_path(Marshaler& m,
                                              const std::string& path,
                                              GuestAddr out_ptr);

    /// Read a standalone or PE-resource type library, including LoadTypeLib's
    /// documented "module\\resource-id" path syntax.
    [[nodiscard]] std::optional<TypeLibParseResult>
    parse_type_lib_from_path(const std::string& path);

    /// Build the guest ITypeLib object (vtable + 13 method slots).
    /// Returns 0 when the thunks or scratch memory are unavailable.
    [[nodiscard]] GuestAddr create_typelib_object(Marshaler& m);

    /// Build a guest ITypeInfo for `index` inside `parent`.
    [[nodiscard]] GuestAddr create_typeinfo_object(
        Marshaler& m, const std::shared_ptr<LoadedTypeLib>& parent, u32 index);

    /// Look up the state behind a guest ITypeLib pointer, or nullptr.
    [[nodiscard]] std::shared_ptr<LoadedTypeLib> find_typelib(GuestAddr object);

    /// Look up the state behind a guest ITypeInfo pointer, or nullptr.
    [[nodiscard]] std::shared_ptr<LoadedTypeInfo> find_typeinfo(GuestAddr object);

    /// Allocate a BSTR holding the UTF-16 widening of a host ANSI string.
    /// Returns 0 when the scratch region is exhausted.
    [[nodiscard]] GuestAddr alloc_bstr_from_ansi(Marshaler& m,
                                                 std::string_view text);

    /// Write the HKCR\TypeLib entries RegisterTypeLib is defined to create.
    [[nodiscard]] bool write_typelib_registration(const TypeLibAttributes& attributes,
                                                  const std::string& path,
                                                  const std::string& help_dir);

    /// Resolve a registered type-library path from HKCR\TypeLib. On success
    /// writes the guest path into `out_path` and returns S_OK.
    [[nodiscard]] u32 resolve_reg_typelib_path(const TypeLibGuid& libid,
                                              u16 major, u16 minor, u32 lcid,
                                              std::string& out_path);

    u32 m_scratch_offset{0}; ///< Bump allocator offset into scratch region
    std::mutex m_scratch_mutex; ///< Guards the scratch bump (any guest thread).
    ComTaskMemoryManager* m_task_memory{nullptr};

    VirtualMemoryManager* m_vmm{nullptr};
    /// Bytes of the scratch region actually committed so far. The range is
    /// RESERVED at registration and committed in 1 MB steps as the bump
    /// advances, so a guest that never calls oleaut32 pays nothing for it.
    u64 m_scratch_committed{0};

    capsule::Vfs*      m_vfs{nullptr};
    capsule::Registry* m_registry{nullptr};
    ThunkRegistry*     m_thunks{nullptr};
    GuestCall          m_guest_call;

    std::mutex m_typelibs_mutex;
    std::unordered_map<GuestAddr, std::shared_ptr<LoadedTypeLib>> m_typelibs;
    std::unordered_map<GuestAddr, std::shared_ptr<LoadedTypeInfo>> m_typeinfos;
};

/// Create OLEAUT32.dll export thunks
DllExports create_oleaut32_thunks(OleAut32Manager& mgr);

} // namespace vitreo::winapi
