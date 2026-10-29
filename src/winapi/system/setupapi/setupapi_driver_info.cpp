// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/setupapi/setupapi_internal.hpp"

#include "winapi/kernel32/locale/string_conv.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::winapi::setupapi_internal {

namespace {

/// The drivers SetupAPI would find for a list: only Vitreo's built-in Metal
/// display driver exists (there is no INF database). It matches the display
/// adapter's hardware ID and is the Display class's only class driver.
[[nodiscard]] std::vector<DriverEntry> vitreo_display_driver(const std::string& name) {
    DriverEntry entry;
    entry.token = next_token();
    entry.description = name;
    entry.hardware_id = display_hardware_id();
    return {entry};
}

[[nodiscard]] const std::vector<DriverEntry>* driver_list(DevInfoSet& set, SetElement* e,
                                                          u32 driver_type) {
    if (driver_type == SPDIT_COMPATDRIVER) {
        return e != nullptr && e->compat_drivers ? &*e->compat_drivers : nullptr;
    }
    if (driver_type != SPDIT_CLASSDRIVER) return nullptr;
    if (e != nullptr) return e->class_drivers ? &*e->class_drivers : nullptr;
    return set.class_drivers ? &*set.class_drivers : nullptr;
}

void write_fixed_string(std::vector<u8>& bytes, usize offset, std::string_view text,
                        bool wide, usize max_chars) {
    if (wide) {
        const auto units = utf8_to_utf16(text);
        const usize count = std::min(units.size(), max_chars - 1);
        std::memcpy(bytes.data() + offset, units.data(), count * 2);
    } else {
        const usize count = std::min(text.size(), max_chars - 1);
        std::memcpy(bytes.data() + offset, text.data(), count);
    }
}

} // namespace

/// BOOL SetupDiBuildDriverInfoList(HDEVINFO, PSP_DEVINFO_DATA, DWORD DriverType)
/// https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdibuilddriverinfolist
/// Searching INF files (a DriverPath or DI_ENUMSINGLEINF, or a device with
/// hardware IDs other than Vitreo's display) is not implemented and fails.
void build_driver_info_list(Marshaler& m, const StatePtr& state) {
    const u64 handle = m.arg(0);
    const GuestAddr did = m.arg_ptr(1);
    const u32 driver_type = m.arg32(2);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    SetElement* element = nullptr;
    if (did != 0) {
        const auto index = resolve_element(m, *set, did);
        if (!index) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
        element = &set->elements[*index];
    }
    if ((driver_type != SPDIT_CLASSDRIVER && driver_type != SPDIT_COMPATDRIVER) ||
        (driver_type == SPDIT_COMPATDRIVER && element == nullptr)) {
        return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    }
    const u32 size = devinstall_size(m);
    const std::vector<u8>& params = install_params_of(*set, element, size);
    u32 flags = 0;
    std::memcpy(&flags, params.data() + 4, 4);
    // DriverPath follows cbSize, Flags, FlagsEx, hwndParent, InstallMsgHandler,
    // InstallMsgHandlerContext, FileQueue, ClassInstallReserved, Reserved.
    const usize driver_path = m.cpu_is_64bit() ? 56u : 32u;
    const bool single_inf = (flags & DI_ENUMSINGLEINF) != 0 ||
        (params.size() > driver_path + 1 && (params[driver_path] | params[driver_path + 1]) != 0);
    const auto nodes = state->nodes();
    std::vector<DriverEntry> drivers;
    const std::string name = state->display_name();
    const Guid list_class = element != nullptr ? element->class_guid : set->class_guid;
    if (driver_type == SPDIT_COMPATDRIVER) {
        const auto ids = element_property(nodes, *element, Prop::HardwareIds);
        if (single_inf || (element->created && ids)) {
            log_unimplemented_once("setupapi!SetupDiBuildDriverInfoList",
                                   "searching INF files for compatible drivers");
            return fail(m, error_codes::ERROR_NOT_SUPPORTED);
        }
        if (ids && !name.empty() && ids->list.front() == display_hardware_id()) {
            drivers = vitreo_display_driver(name);
        }
        element->compat_drivers = drivers;
    } else {
        if (single_inf || (list_class != GUID_ZERO && list_class != GUID_DEVCLASS_DISPLAY)) {
            log_unimplemented_once("setupapi!SetupDiBuildDriverInfoList",
                                   "searching INF files for class drivers");
            return fail(m, error_codes::ERROR_NOT_SUPPORTED);
        }
        if (list_class == GUID_DEVCLASS_DISPLAY && !name.empty()) {
            drivers = vitreo_display_driver(name);
        }
        (element != nullptr ? element->class_drivers : set->class_drivers) = drivers;
    }
    succeed(m);
}

/// BOOL SetupDiEnumDriverInfo{A,W}(HDEVINFO, PSP_DEVINFO_DATA, DWORD
///     DriverType, DWORD MemberIndex, PSP_DRVINFO_DATA)
void enum_driver_info(Marshaler& m, const StatePtr& state, bool wide) {
    const u64 handle = m.arg(0);
    const GuestAddr did = m.arg_ptr(1);
    const u32 driver_type = m.arg32(2);
    const u32 member_index = m.arg32(3);
    const GuestAddr driver_info = m.arg_ptr(4);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    SetElement* element = nullptr;
    if (did != 0) {
        const auto index = resolve_element(m, *set, did);
        if (!index) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
        element = &set->elements[*index];
    }
    const auto* list = driver_list(*set, element, driver_type);
    if (list == nullptr) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    if (member_index >= list->size()) return fail(m, error_codes::ERROR_NO_MORE_ITEMS);
    // SP_DRVINFO_DATA_V2_{W,A} or the V1 layout (no DriverDate /
    // DriverVersion); sizes from setupapi.h (x64 pack 8, x86 pack 1):
    // V2_W 1568/1564, V1_W 1552/1548, V2_A 800/796, V1_A 784/780.
    const bool is64 = m.cpu_is_64bit();
    const u32 v2_size = wide ? (is64 ? 1568u : 1564u) : (is64 ? 800u : 796u);
    const u32 v1_size = wide ? (is64 ? 1552u : 1548u) : (is64 ? 784u : 780u);
    const auto cb = driver_info != 0 ? m.read_u32(driver_info)
                                     : VResult<u32>::err(Error::InvalidArgument);
    if (cb.is_err() || (cb.value() != v2_size && cb.value() != v1_size)) {
        return fail(m, ERROR_INVALID_USER_BUFFER);
    }
    const DriverEntry& entry = (*list)[member_index];
    const u32 size = cb.value();
    const usize strings = is64 ? 16u : 12u;
    const usize line = wide ? 512u : 256u;
    std::vector<u8> bytes(size, 0);
    std::memcpy(bytes.data(), &size, 4);
    std::memcpy(bytes.data() + 4, &driver_type, 4);
    std::memcpy(bytes.data() + 8, &entry.token, is64 ? 8 : 4);
    write_fixed_string(bytes, strings, entry.description, wide, 256);
    write_fixed_string(bytes, strings + line, "Apple Inc.", wide, 256);
    write_fixed_string(bytes, strings + 2 * line, "Vitreo", wide, 256);
    if (m.memory().write_bytes(driver_info, bytes.data(), bytes.size()).is_err()) {
        return fail(m, ERROR_INVALID_USER_BUFFER);
    }
    succeed(m);
}

/// BOOL SetupDiDestroyDriverInfoList(HDEVINFO, PSP_DEVINFO_DATA, DWORD)
/// s7_setupapi_probe ddil_*: bad set 6; a type other than CLASS/COMPAT, or
/// COMPAT without a device, 87; destroying a list that was never built
/// succeeds. The DIDINFOLIST / DIDCOMPATINFO state goes with the list.
void destroy_driver_info_list(Marshaler& m, const StatePtr& state) {
    const u64 handle = m.arg(0);
    const GuestAddr did = m.arg_ptr(1);
    const u32 driver_type = m.arg32(2);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    SetElement* element = nullptr;
    if (did != 0) {
        const auto index = resolve_element(m, *set, did);
        if (!index) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
        element = &set->elements[*index];
    }
    if ((driver_type != SPDIT_CLASSDRIVER && driver_type != SPDIT_COMPATDRIVER) ||
        (driver_type == SPDIT_COMPATDRIVER && element == nullptr)) {
        return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    }
    if (driver_type == SPDIT_COMPATDRIVER) {
        element->compat_drivers.reset();
    } else {
        (element != nullptr ? element->class_drivers : set->class_drivers).reset();
    }
    succeed(m);
}

/// BOOL SetupDiGetDriverInfoDetailW(HDEVINFO, PSP_DEVINFO_DATA,
///     PSP_DRVINFO_DATA_W, PSP_DRVINFO_DETAIL_DATA_W, DWORD, PDWORD)
/// s7_setupapi_probe gdid_* / bdil_disp: bad set 6; bad DeviceInfoData or a
/// driver not in the list 87; the detail buffer must be NULL with size 0, or
/// have cbSize == sizeof (1584/1570) and size >= offsetof(HardwareID)+2
/// (1576/1568 + 2), else 1784; otherwise the fixed part is filled, as many
/// IDs as fit while staying MULTI_SZ, RequiredSize written, and 122 if short.
/// Vitreo's display driver has no INF: InfFileName and SectionName are empty.
void get_driver_info_detail(Marshaler& m, const StatePtr& state) {
    const u64 handle = m.arg(0);
    const GuestAddr did = m.arg_ptr(1);
    const GuestAddr driver_info = m.arg_ptr(2);
    const GuestAddr detail = m.arg_ptr(3);
    const u32 detail_size = m.arg32(4);
    const GuestAddr required_out = m.arg_ptr(5);
    const bool is64 = m.cpu_is_64bit();
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    SetElement* element = nullptr;
    if (did != 0) {
        const auto index = resolve_element(m, *set, did);
        if (!index) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
        element = &set->elements[*index];
    }
    if (driver_info == 0) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    const auto driver_type = m.read_u32(driver_info + 4);
    const auto token = m.read_ptr(driver_info + 8);
    const auto* list = driver_type.is_ok() ? driver_list(*set, element, driver_type.value())
                                           : nullptr;
    const DriverEntry* entry = nullptr;
    if (list != nullptr && token.is_ok()) {
        for (const auto& candidate : *list) {
            if (candidate.token == token.value()) entry = &candidate;
        }
    }
    if (entry == nullptr) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    // SP_DRVINFO_DETAIL_DATA_W: cbSize, InfDate, CompatIDsOffset,
    // CompatIDsLength, Reserved, SectionName[256], InfFileName[260],
    // DrvDescription[256], HardwareID[1].
    const u32 sizeof_detail = is64 ? 1584u : 1570u;
    const u32 hardware_offset = is64 ? 1576u : 1568u;
    const u32 description_offset = hardware_offset - 512u;
    if (detail == 0) {
        if (detail_size != 0) return fail(m, ERROR_INVALID_USER_BUFFER);
    } else {
        const auto cb = m.read_u32(detail);
        if (cb.is_err() || cb.value() != sizeof_detail || detail_size < hardware_offset + 2) {
            return fail(m, ERROR_INVALID_USER_BUFFER);
        }
    }
    // HardwareID MULTI_SZ: the hardware ID, then (none) compatible IDs.
    const std::vector<u8> id_bytes = utf16_bytes(entry->hardware_id);
    const u32 required = hardware_offset + static_cast<u32>(id_bytes.size()) + 2;
    if (required_out != 0) (void)m.write_u32(required_out, required);
    if (detail != 0) {
        std::vector<u8> fixed(hardware_offset + 2, 0);
        std::memcpy(fixed.data(), &sizeof_detail, 4);
        const u32 compat_offset = static_cast<u32>(id_bytes.size() / 2);
        std::memcpy(fixed.data() + 12, &compat_offset, 4);
        write_fixed_string(fixed, description_offset, entry->description, true, 256);
        std::vector<u8> ids;
        if (detail_size >= required) {
            ids = id_bytes;
            ids.push_back(0);
            ids.push_back(0);
        }
        if (m.memory().write_bytes(detail, fixed.data(), hardware_offset).is_err() ||
            (ids.empty() ? m.write_u16(detail + hardware_offset, 0).is_err()
                         : m.memory().write_bytes(detail + hardware_offset, ids.data(),
                                                  ids.size()).is_err())) {
            return fail(m, ERROR_INVALID_USER_BUFFER);
        }
    }
    if (detail == 0 || detail_size < required) {
        return fail(m, error_codes::ERROR_INSUFFICIENT_BUFFER);
    }
    succeed(m);
}

} // namespace vitreo::winapi::setupapi_internal
