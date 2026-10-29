// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/setupapi/setupapi_internal.hpp"

#include <cstring>

namespace vitreo::winapi::setupapi_internal {

// ---- install parameters and driver lists -----------------------------------------

[[nodiscard]] u32 devinstall_size(const Marshaler& m) {
    // SP_DEVINSTALL_PARAMS_W: 584 bytes on x64 (pack 8), 556 on x86 (pack 1).
    return m.cpu_is_64bit() ? 584u : 556u;
}

/// The install-parameter block of the set (NULL DeviceInfoData) or element,
/// sized for this bitness.
[[nodiscard]] std::vector<u8>& install_params_of(DevInfoSet& set, SetElement* e,
                                                 u32 size) {
    std::vector<u8>& params = e != nullptr ? e->install_params : set.install_params;
    if (params.size() != size) params.assign(size, 0);
    return params;
}

void update_u32(std::vector<u8>& bytes, usize offset, u32 set_bits, u32 clear_bits) {
    u32 value = 0;
    std::memcpy(&value, bytes.data() + offset, 4);
    value = (value & ~clear_bits) | set_bits;
    std::memcpy(bytes.data() + offset, &value, 4);
}

/// BOOL SetupDiGetDeviceInstallParamsW(HDEVINFO, PSP_DEVINFO_DATA,
///     PSP_DEVINSTALL_PARAMS_W): cbSize must be exactly 584/556 (1784).
/// DI_FLAGSEX_DIDINFOLIST / DIDCOMPATINFO report the built driver lists and
/// DI_CLASSINSTALLPARAMS the class install parameters.
void get_device_install_params(Marshaler& m, const StatePtr& state) {
    const u64 handle = m.arg(0);
    const GuestAddr did = m.arg_ptr(1);
    const GuestAddr params = m.arg_ptr(2);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    SetElement* element = nullptr;
    if (did != 0) {
        const auto index = resolve_element(m, *set, did);
        if (!index) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
        element = &set->elements[*index];
    }
    const u32 size = devinstall_size(m);
    const auto cb = params != 0 ? m.read_u32(params) : VResult<u32>::err(Error::InvalidArgument);
    if (cb.is_err() || cb.value() != size) return fail(m, ERROR_INVALID_USER_BUFFER);
    std::vector<u8> bytes = install_params_of(*set, element, size);
    std::memcpy(bytes.data(), &size, 4);
    const bool class_list = element != nullptr ? element->class_drivers.has_value()
                                               : set->class_drivers.has_value();
    const bool compat_list = element != nullptr && element->compat_drivers.has_value();
    update_u32(bytes, 8,
               (class_list ? DI_FLAGSEX_DIDINFOLIST : 0) |
                   (compat_list ? DI_FLAGSEX_DIDCOMPATINFO : 0),
               DI_FLAGSEX_DIDINFOLIST | DI_FLAGSEX_DIDCOMPATINFO);
    const bool has_class_params = element != nullptr ? !element->class_install_params.empty()
                                                     : !set->class_install_params.empty();
    u32 flags = 0;
    std::memcpy(&flags, bytes.data() + 4, 4);
    if (!has_class_params) update_u32(bytes, 4, 0, DI_CLASSINSTALLPARAMS);
    if (m.memory().write_bytes(params, bytes.data(), bytes.size()).is_err()) {
        return fail(m, ERROR_INVALID_USER_BUFFER);
    }
    succeed(m);
}

/// BOOL SetupDiSetDeviceInstallParamsW: the same block; SetupAPI owns the
/// DIDINFOLIST / DIDCOMPATINFO bits, so a caller cannot set them.
void set_device_install_params(Marshaler& m, const StatePtr& state) {
    const u64 handle = m.arg(0);
    const GuestAddr did = m.arg_ptr(1);
    const GuestAddr params = m.arg_ptr(2);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    SetElement* element = nullptr;
    if (did != 0) {
        const auto index = resolve_element(m, *set, did);
        if (!index) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
        element = &set->elements[*index];
    }
    const u32 size = devinstall_size(m);
    if (params == 0) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    const auto cb = m.read_u32(params);
    std::vector<u8> bytes(size);
    if (cb.is_err() || cb.value() != size ||
        m.memory().read_bytes(params, bytes.data(), bytes.size()).is_err()) {
        return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    }
    update_u32(bytes, 8, 0, DI_FLAGSEX_DIDINFOLIST | DI_FLAGSEX_DIDCOMPATINFO);
    install_params_of(*set, element, size) = std::move(bytes);
    succeed(m);
}

// ---- class install parameters -------------------------------------------------------

namespace {

/// Byte size of the class-install parameter block each DIF code takes, from
/// s7_setupapi_probe scip_fn / scip_content (x64, x86). 0: the DIF takes no
/// parameters (87); SIZE_ANY: any size >= the header (codes SetupAPI does
/// not know, and DIF_DESTROYWIZARDDATA).
constexpr u32 SIZE_ANY = 0xFFFFFFFFu;

[[nodiscard]] u32 class_install_params_size(u32 dif, bool is64) {
    switch (dif) {
        case DIF_SELECTDEVICE: return 1212;
        case DIF_REMOVE: return 16;
        case DIF_DETECT: return is64 ? 24 : 16;
        case DIF_INSTALLWIZARD: return is64 ? 208 : 112;
        case DIF_DESTROYWIZARDDATA: return SIZE_ANY;
        case DIF_PROPERTYCHANGE: return 20;
        case DIF_ENABLECLASS: return 28;
        case DIF_UNREMOVE: return 16;
        case DIF_ADDPROPERTYPAGE_ADVANCED:
        case DIF_ADDPROPERTYPAGE_BASIC:
        case DIF_ADDREMOTEPROPERTYPAGE_ADVANCED: return is64 ? 192 : 100;
        case DIF_RESERVED1: return is64 ? 536 : 532;
        case DIF_TROUBLESHOOTER: return 1048;
        case DIF_POWERMESSAGEWAKE: return 1032;
        default: break;
    }
    if (dif >= DIF_NEWDEVICEWIZARD_PRESELECT && dif <= DIF_NEWDEVICEWIZARD_FINISHINSTALL) {
        return is64 ? 192 : 100;
    }
    // Every other defined code takes no parameter block.
    if (dif == 0 || dif <= DIF_FINISHINSTALL_ACTION || dif == DIF_RESERVED2) return 0;
    return SIZE_ANY;
}

/// Field checks SetupAPI applies to the parameter blocks it interprets
/// (s7_setupapi_probe scip_content).
[[nodiscard]] bool class_install_params_valid(u32 dif, const std::vector<u8>& bytes,
                                              bool is64) {
    auto field = [&bytes](usize offset) {
        u32 value = 0;
        std::memcpy(&value, bytes.data() + offset, 4);
        return value;
    };
    switch (dif) {
        case DIF_PROPERTYCHANGE: {
            const u32 change = field(8);
            const u32 scope = field(12);
            if (change >= 1 && change <= 3) {   // DICS_ENABLE / DISABLE / PROPCHANGE
                return scope == DICS_FLAG_GLOBAL || scope == DICS_FLAG_CONFIGSPECIFIC ||
                       scope == DICS_FLAG_CONFIGGENERAL;
            }
            // DICS_START / DICS_STOP apply to a hardware profile only.
            return (change == 4 || change == 5) && scope == DICS_FLAG_CONFIGSPECIFIC;
        }
        case DIF_REMOVE: {
            const u32 scope = field(8);
            return scope == 1 || scope == 2;   // DI_REMOVEDEVICE_GLOBAL / CONFIGSPECIFIC
        }
        case DIF_UNREMOVE: return field(8) == 2;   // DI_UNREMOVEDEVICE_CONFIGSPECIFIC
        case DIF_DETECT: {
            u64 callback = 0;
            std::memcpy(&callback, bytes.data() + 8, is64 ? 8 : 4);
            return callback != 0;
        }
        default: return true;
    }
}

} // namespace

/// BOOL SetupDiSetClassInstallParamsW(HDEVINFO, PSP_DEVINFO_DATA,
///     PSP_CLASSINSTALL_HEADER, DWORD Size)
/// https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdisetclassinstallparamsw
/// s7_setupapi_probe scip_*: NULL clears (a size with NULL is 1784); a size
/// below the header or a header cbSize other than 8 is 1784; a DIF-specific
/// size or field mismatch 87. Setting raises DI_CLASSINSTALLPARAMS, clearing
/// drops it.
void set_class_install_params(Marshaler& m, const StatePtr& state) {
    const u64 handle = m.arg(0);
    const GuestAddr did = m.arg_ptr(1);
    const GuestAddr params = m.arg_ptr(2);
    const u32 size = m.arg32(3);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    SetElement* element = nullptr;
    if (did != 0) {
        const auto index = resolve_element(m, *set, did);
        if (!index) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
        element = &set->elements[*index];
    }
    std::vector<u8>& stored = element != nullptr ? element->class_install_params
                                                 : set->class_install_params;
    const u32 install_size = devinstall_size(m);
    if (params == 0) {
        if (size != 0) return fail(m, ERROR_INVALID_USER_BUFFER);
        stored.clear();
        update_u32(install_params_of(*set, element, install_size), 4, 0, DI_CLASSINSTALLPARAMS);
        return succeed(m);
    }
    const auto header_cb = m.read_u32(params);
    if (size < 8 || header_cb.is_err() || header_cb.value() != 8) {
        return fail(m, ERROR_INVALID_USER_BUFFER);
    }
    std::vector<u8> bytes(size);
    if (m.memory().read_bytes(params, bytes.data(), bytes.size()).is_err()) {
        return fail(m, ERROR_INVALID_USER_BUFFER);
    }
    u32 dif = 0;
    std::memcpy(&dif, bytes.data() + 4, 4);
    const u32 expected = class_install_params_size(dif, m.cpu_is_64bit());
    if (expected == 0 || (expected != SIZE_ANY && size != expected) ||
        !class_install_params_valid(dif, bytes, m.cpu_is_64bit())) {
        return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    }
    stored = std::move(bytes);
    update_u32(install_params_of(*set, element, install_size), 4, DI_CLASSINSTALLPARAMS, 0);
    succeed(m);
}

/// BOOL SetupDiGetClassInstallParamsW(HDEVINFO, PSP_DEVINFO_DATA,
///     PSP_CLASSINSTALL_HEADER, DWORD Size, PDWORD RequiredSize)
/// Returns the block SetupDiSetClassInstallParams stored (scip_set: size 20,
/// DIF_PROPERTYCHANGE), or ERROR_NO_CLASSINSTALL_PARAMS once cleared with
/// RequiredSize untouched (get2).
void get_class_install_params(Marshaler& m, const StatePtr& state) {
    constexpr u32 ERROR_NO_CLASSINSTALL_PARAMS = 0xE0000215u;
    const u64 handle = m.arg(0);
    const GuestAddr did = m.arg_ptr(1);
    const GuestAddr params = m.arg_ptr(2);
    const u32 size = m.arg32(3);
    const GuestAddr required_out = m.arg_ptr(4);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    SetElement* element = nullptr;
    if (did != 0) {
        const auto index = resolve_element(m, *set, did);
        if (!index) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
        element = &set->elements[*index];
    }
    if (params == 0 && size != 0) return fail(m, ERROR_INVALID_USER_BUFFER);
    if (params != 0) {
        const auto cb = m.read_u32(params);
        if (size < 8 || cb.is_err() || cb.value() != 8) return fail(m, ERROR_INVALID_USER_BUFFER);
    }
    const std::vector<u8>& stored = element != nullptr ? element->class_install_params
                                                       : set->class_install_params;
    if (stored.empty()) return fail(m, ERROR_NO_CLASSINSTALL_PARAMS);
    if (required_out != 0) (void)m.write_u32(required_out, static_cast<u32>(stored.size()));
    if (params == 0 || size < stored.size()) return fail(m, error_codes::ERROR_INSUFFICIENT_BUFFER);
    if (m.memory().write_bytes(params, stored.data(), stored.size()).is_err()) {
        return fail(m, ERROR_INVALID_USER_BUFFER);
    }
    succeed(m);
}

} // namespace vitreo::winapi::setupapi_internal
