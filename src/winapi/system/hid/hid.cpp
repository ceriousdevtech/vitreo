// SPDX-License-Identifier: LGPL-2.1-only

/// @file hid.cpp
/// @brief hid.dll exports (see hid.hpp for the contract).

#include "winapi/system/hid/hid.hpp"

#include "hardware/contracts/bluetooth_host.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 HIDP_STATUS_INVALID_PREPARSED_DATA = 0xC0110001u;

/// HidD_* take the device handle first. NULL, INVALID_HANDLE_VALUE or a
/// closed handle is ERROR_INVALID_HANDLE; an open handle that is not a HID
/// device (every handle here) is ERROR_INVALID_PARAMETER; outputs untouched
/// (hid_probe attrs_null/_invalid/_closed, attrs_file).
ApiHandler not_a_hid_device(u32 args, HandleTable* handles) {
    return [args, handles](Marshaler& m) {
        const u64 handle = m.arg(0);
        for (u32 i = 1; i < args; ++i) (void)m.arg(i);
        const u64 invalid = m.cpu_is_64bit() ? ~0ull : 0xFFFFFFFFull;
        const bool open = handle != 0 && handle != invalid &&
            (handles == nullptr ||
             (handle <= 0xFFFFFFFFull && handles->is_valid(static_cast<Handle>(handle))));
        set_last_error(open ? error_codes::ERROR_INVALID_PARAMETER
                            : error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
    };
}

/// HidP_* with preparsed data Vitreo never issued.
ApiHandler invalid_preparsed(u32 args) {
    return [args](Marshaler& m) {
        for (u32 i = 0; i < args; ++i) (void)m.arg(i);
        m.set_return(HIDP_STATUS_INVALID_PREPARSED_DATA);
    };
}

} // namespace

DllExports create_hid_thunks(HandleTable* handles) {
    DllExports hid;
    // void HidD_GetHidGuid(LPGUID): GUID_DEVINTERFACE_HID
    // {4D1E55B2-F16F-11CF-88CB-001111000030} (hidclass.h; hid_probe guid).
    hid.entries.push_back({"HidD_GetHidGuid", 0,
        [](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(0);
            static constexpr std::array<u8, 16> kHidGuid{
                0xB2, 0x55, 0x1E, 0x4D, 0x6F, 0xF1, 0xCF, 0x11,
                0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30};
            if (out != 0) (void)m.memory().write_bytes(out, kHidGuid.data(), kHidGuid.size());
            m.set_return(0);
        }, 4});
    // BOOLEAN HidD_FreePreparsedData(PHIDP_PREPARSED_DATA): nothing this DLL
    // issued, so FALSE with the last error untouched (hid_probe free_fake).
    hid.entries.push_back({"HidD_FreePreparsedData", 0,
        [](Marshaler& m) { (void)m.arg(0); m.set_return(0); }, 4});

    struct Row { const char* name; u16 args; };
    static constexpr Row kDevice[] = {
        {"HidD_FlushQueue", 1},          {"HidD_GetAttributes", 2},
        {"HidD_GetConfiguration", 3},    {"HidD_GetFeature", 3},
        {"HidD_GetIndexedString", 4},    {"HidD_GetInputReport", 3},
        {"HidD_GetManufacturerString", 3}, {"HidD_GetMsGenreDescriptor", 3},
        {"HidD_GetNumInputBuffers", 2},  {"HidD_GetPhysicalDescriptor", 3},
        {"HidD_GetPreparsedData", 2},    {"HidD_GetProductString", 3},
        {"HidD_GetSerialNumberString", 3}, {"HidD_SetConfiguration", 3},
        {"HidD_SetFeature", 3},          {"HidD_SetNumInputBuffers", 2},
        {"HidD_SetOutputReport", 3},
    };
    for (const Row& row : kDevice) {
        hid.entries.push_back({row.name, 0, not_a_hid_device(row.args, handles),
                               static_cast<u16>(row.args * 4)});
    }
    static constexpr Row kParser[] = {
        {"HidP_GetButtonCaps", 4},       {"HidP_GetCaps", 2},
        {"HidP_GetData", 6},             {"HidP_GetExtendedAttributes", 5},
        {"HidP_GetLinkCollectionNodes", 3}, {"HidP_GetScaledUsageValue", 8},
        {"HidP_GetSpecificButtonCaps", 7}, {"HidP_GetSpecificValueCaps", 7},
        {"HidP_GetUsageValue", 8},       {"HidP_GetUsageValueArray", 9},
        {"HidP_GetUsages", 8},           {"HidP_GetUsagesEx", 7},
        {"HidP_GetValueCaps", 4},        {"HidP_InitializeReportForID", 5},
        {"HidP_SetData", 6},             {"HidP_SetScaledUsageValue", 8},
        {"HidP_SetUsageValue", 8},       {"HidP_SetUsageValueArray", 9},
        {"HidP_SetUsages", 8},           {"HidP_UnsetUsages", 8},
    };
    for (const Row& row : kParser) {
        hid.entries.push_back({row.name, 0, invalid_preparsed(row.args), static_cast<u16>(row.args * 4)});
    }
    // ULONG HidP_MaxDataListLength(HIDP_REPORT_TYPE, PHIDP_PREPARSED_DATA)
    // and HidP_MaxUsageListLength(HIDP_REPORT_TYPE, USAGE, PHIDP_PREPARSED_DATA):
    // "zero if the preparsed data is not valid" (Learn; hid_probe maxdata_*).
    hid.entries.push_back({"HidP_MaxDataListLength", 0,
        [](Marshaler& m) { (void)m.arg(0); (void)m.arg(1); m.set_return(0); }, 8});
    hid.entries.push_back({"HidP_MaxUsageListLength", 0,
        [](Marshaler& m) { (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); m.set_return(0); }, 12});
    return hid;
}

} // namespace vitreo::winapi

// =============================================================================
// winusb.dll
// =============================================================================

namespace vitreo::winapi {

namespace {

void usb_fail(Marshaler& m, u32 error) {
    set_last_error(error);
    m.set_return(0);
}

/// USB_COMMON_DESCRIPTOR walk (USB 2.0 9.5): the first descriptor at or after
/// @p start of @p type, stopping at a zero bLength or the buffer end. The
/// type is compared before the length (s7_winusb_probe pd_zero_len).
[[nodiscard]] GuestAddr parse_usb_descriptors(Marshaler& m, GuestAddr buffer, u32 total,
                                              GuestAddr start, u8 type) {
    const GuestAddr end = buffer + total;
    GuestAddr p = start;
    while (p >= buffer && p + 2 <= end) {
        const auto length = m.memory().read_u8(p);
        const auto kind = m.memory().read_u8(p + 1);
        if (length.is_err() || kind.is_err()) return 0;
        if (kind.value() == type) return p;
        if (length.value() == 0) return 0;
        p += length.value();
    }
    return 0;
}

} // namespace

DllExports create_winusb_thunks() {
    DllExports usb;
    // Every call that takes a WINUSB_INTERFACE_HANDLE (or an isochronous
    // buffer handle registered on one) the process never received fails
    // ERROR_INVALID_HANDLE (s7_winusb_probe *_null). Byte counts: winusb.lib.
    struct Row { const char* name; u16 bytes; u32 error; };
    static constexpr Row kHandleCalls[] = {
        {"WinUsb_GetAssociatedInterface", 12, 6}, {"WinUsb_GetDescriptor", 28, 6},
        {"WinUsb_QueryInterfaceSettings", 12, 6}, {"WinUsb_QueryDeviceInformation", 16, 6},
        {"WinUsb_SetCurrentAlternateSetting", 8, 6}, {"WinUsb_GetCurrentAlternateSetting", 8, 6},
        {"WinUsb_QueryPipe", 16, 6}, {"WinUsb_QueryPipeEx", 16, 6},
        {"WinUsb_SetPipePolicy", 20, 6}, {"WinUsb_GetPipePolicy", 20, 6},
        {"WinUsb_ReadPipe", 24, 6}, {"WinUsb_WritePipe", 24, 6},
        // WINUSB_SETUP_PACKET is 8 bytes passed by value: _WinUsb_ControlTransfer@28.
        {"WinUsb_ControlTransfer", 28, 6},
        {"WinUsb_ResetPipe", 8, 6}, {"WinUsb_AbortPipe", 8, 6}, {"WinUsb_FlushPipe", 8, 6},
        {"WinUsb_SetPowerPolicy", 16, 6}, {"WinUsb_GetPowerPolicy", 16, 6},
        {"WinUsb_RegisterIsochBuffer", 20, 6}, {"WinUsb_UnregisterIsochBuffer", 4, 6},
        {"WinUsb_AbortPipeAsync", 12, 6}, {"WinUsb_ResetPipeAsync", 12, 6},
        {"WinUsb_SetCurrentAlternateSettingAsync", 12, 6},
        // Windows dereferences these handles without checking (a NULL one
        // faults there); an unknown handle is reported, not dereferenced.
        {"WinUsb_GetOverlappedResult", 16, 6}, {"WinUsb_GetCurrentFrameNumber", 12, 6},
        // Isochronous transfers on an unregistered buffer, and the time-sync
        // calls without an interface: ERROR_INVALID_PARAMETER.
        {"WinUsb_WriteIsochPipe", 20, 87}, {"WinUsb_ReadIsochPipe", 28, 87},
        {"WinUsb_WriteIsochPipeAsap", 20, 87}, {"WinUsb_ReadIsochPipeAsap", 28, 87},
        {"WinUsb_StartTrackingForTimeSync", 8, 87}, {"WinUsb_GetCurrentFrameNumberAndQpc", 8, 87},
        {"WinUsb_StopTrackingForTimeSync", 8, 87},
    };
    for (const Row& row : kHandleCalls) {
        const u32 error = row.error;
        const u32 args = row.bytes / 4u;
        usb.entries.push_back({row.name, 0, [error, args](Marshaler& m) {
            for (u32 i = 0; i < args; ++i) (void)m.arg(i);
            usb_fail(m, error);
        }, row.bytes});
    }
    // BOOL WinUsb_Initialize(HANDLE DeviceHandle, PWINUSB_INTERFACE_HANDLE):
    // no handle is a WinUSB device here, so ERROR_INVALID_HANDLE with the
    // output set to NULL (init_null / init_invalid / init_file).
    usb.entries.push_back({"WinUsb_Initialize", 0, [](Marshaler& m) {
        (void)m.arg(0);
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) (void)m.write_ptr(out, 0);
        usb_fail(m, error_codes::ERROR_INVALID_HANDLE);
    }, 8});
    // BOOL WinUsb_Free(WINUSB_INTERFACE_HANDLE): "TRUE" (Learn) and the last
    // error untouched (free_null).
    usb.entries.push_back({"WinUsb_Free", 0, [](Marshaler& m) {
        (void)m.arg(0);
        m.set_return(1);
    }, 4});
    // BOOL WinUsb_GetAdjustedFrameNumber(PULONG CurrentFrameNumber,
    // LARGE_INTEGER TimeStamp): no frame-number reference exists without a
    // USB bus (adjframe: ERROR_INVALID_PARAMETER, output untouched).
    usb.entries.push_back({"WinUsb_GetAdjustedFrameNumber", 0, [](Marshaler& m) {
        (void)m.arg(0);
        usb_fail(m, error_codes::ERROR_INVALID_PARAMETER);
    }, 12});
    // PUSB_COMMON_DESCRIPTOR WinUsb_ParseDescriptors(PVOID Buffer, ULONG
    // TotalLength, PVOID StartPosition, LONG DescriptorType).
    usb.entries.push_back({"WinUsb_ParseDescriptors", 0, [](Marshaler& m) {
        const GuestAddr buffer = m.arg_ptr(0);
        const u32 total = m.arg32(1);
        const GuestAddr start = m.arg_ptr(2);
        const u32 type = m.arg32(3);
        m.set_return(buffer == 0 ? 0 : parse_usb_descriptors(m, buffer, total, start,
                                                             static_cast<u8>(type)));
    }, 16});
    // PUSB_INTERFACE_DESCRIPTOR WinUsb_ParseConfigurationDescriptor(
    //     PUSB_CONFIGURATION_DESCRIPTOR, PVOID StartPosition, LONG
    //     InterfaceNumber, LONG AlternateSetting, LONG InterfaceClass,
    //     LONG InterfaceSubClass, LONG InterfaceProtocol)
    // The first interface descriptor (within wTotalLength) matching every
    // criterion that is not -1 (s7_winusb_probe pcd_*).
    usb.entries.push_back({"WinUsb_ParseConfigurationDescriptor", 0, [](Marshaler& m) {
        const GuestAddr config = m.arg_ptr(0);
        GuestAddr start = m.arg_ptr(1);
        const i32 want[5] = {static_cast<i32>(m.arg32(2)), static_cast<i32>(m.arg32(3)),
                             static_cast<i32>(m.arg32(4)), static_cast<i32>(m.arg32(5)),
                             static_cast<i32>(m.arg32(6))};
        const auto total = config != 0 ? m.read_u16(config + 2) : VResult<u16>::err(Error::InvalidArgument);
        if (total.is_err()) return m.set_return(0);
        // bInterfaceNumber @2, bAlternateSetting @3, bInterfaceClass @5,
        // bInterfaceSubClass @6, bInterfaceProtocol @7.
        static constexpr u32 kField[5] = {2, 3, 5, 6, 7};
        for (;;) {
            const GuestAddr found = parse_usb_descriptors(m, config, total.value(), start, 4);
            if (found == 0) return m.set_return(0);
            bool match = true;
            for (int i = 0; i < 5 && match; ++i) {
                if (want[i] == -1) continue;
                const auto value = m.memory().read_u8(found + kField[i]);
                match = value.is_ok() && value.value() == static_cast<u32>(want[i]);
            }
            if (match) return m.set_return(found);
            const auto length = m.memory().read_u8(found);
            if (length.is_err() || length.value() == 0) return m.set_return(0);
            start = found + length.value();
        }
    }, 28});
    return usb;
}

// =============================================================================
// bthprops.cpl
// =============================================================================

namespace {

constexpr u32 ERROR_REVISION_MISMATCH = 1306;
constexpr u32 BLUETOOTH_FIND_RADIO_PARAMS_SIZE = 4;
/// BLUETOOTH_RADIO_INFO: dwSize @0, address (BLUETOOTH_ADDRESS, a ULONGLONG
/// union) @8, szName[248] @16, ulClassofDevice @512, lmpSubversion @516,
/// manufacturer @518; 520 bytes on both architectures (s7_bluetooth_probe
/// sizes).
constexpr u32 BLUETOOTH_RADIO_INFO_SIZE = 520;
constexpr u32 BLUETOOTH_DEVICE_INFO_SIZE = 560;
constexpr char kRadioObject[] = "bthradio:0";

struct BthState {
    hardware::BluetoothHost* host{nullptr};
    HandleTable* handles{nullptr};
    GuestFunctionCaller caller;
    std::mutex mutex;
    std::unordered_set<u64> radio_finds;
    u32 next_find{0xB7F00000u};

    [[nodiscard]] hardware::BluetoothAdapterState adapter() {
        hardware::BluetoothAdapterState state{};
        if (host != nullptr) (void)host->query_default_adapter(state);
        return state;
    }
    [[nodiscard]] bool is_radio(u64 handle) const {
        if (handles == nullptr || handle == 0 || handle > 0xFFFFFFFFull) return false;
        const auto* object = static_cast<const std::string*>(
            handles->get(static_cast<Handle>(handle), HandleType::Device));
        return object != nullptr && *object == kRadioObject;
    }
};
using BthPtr = std::shared_ptr<BthState>;

void bt_fail(Marshaler& m, u32 error, u64 value = 0) {
    set_last_error(error);
    m.set_return(value);
}

// ---- SDP data elements (Bluetooth Core Vol 3 Part B 3) ----

enum class SdpParse { Ok, Invalid, Truncated };

struct SdpElement {
    u8 type{0};
    u8 size_index{0};
    usize header{0};
    usize data_len{0};
    [[nodiscard]] usize total() const { return header + data_len; }
};

/// SDP_TYPE_* (bthsdpdef.h): the descriptor's type field.
constexpr u8 SDP_NIL = 0, SDP_UINT = 1, SDP_INT = 2, SDP_UUID = 3, SDP_STRING = 4,
             SDP_BOOLEAN = 5, SDP_SEQUENCE = 6, SDP_ALTERNATIVE = 7, SDP_URL = 8;

/// One data element at @p pos of @p s (bounded by @p end). Invalid: a type
/// or size index the specification does not allow for that type (probe
/// elem desc=01/18/29/48/50/0d/1d/3c/2c); Truncated: it runs past the end.
[[nodiscard]] SdpParse parse_sdp(const std::vector<u8>& s, usize pos, usize end,
                                 SdpElement& e) {
    if (pos >= end) return SdpParse::Truncated;
    const u8 d = s[pos];
    e.type = d >> 3;
    e.size_index = d & 7;
    usize length_bytes = 0;
    switch (e.type) {
        case SDP_NIL:
            if (e.size_index != 0) return SdpParse::Invalid;
            e.data_len = 0;
            break;
        case SDP_UINT:
        case SDP_INT:
            if (e.size_index > 4) return SdpParse::Invalid;
            e.data_len = usize{1} << e.size_index;
            break;
        case SDP_UUID:
            if (e.size_index != 1 && e.size_index != 2 && e.size_index != 4) return SdpParse::Invalid;
            e.data_len = usize{1} << e.size_index;
            break;
        case SDP_BOOLEAN:
            if (e.size_index != 0) return SdpParse::Invalid;
            e.data_len = 1;
            break;
        case SDP_STRING:
        case SDP_SEQUENCE:
        case SDP_ALTERNATIVE:
        case SDP_URL:
            if (e.size_index < 5) return SdpParse::Invalid;
            length_bytes = usize{1} << (e.size_index - 5);
            break;
        default: return SdpParse::Invalid;
    }
    e.header = 1 + length_bytes;
    if (pos + e.header > end) return SdpParse::Truncated;
    if (length_bytes != 0) {
        usize length = 0;
        for (usize i = 0; i < length_bytes; ++i) length = (length << 8) | s[pos + 1 + i];
        e.data_len = length;
    }
    if (pos + e.total() > end) return SdpParse::Truncated;
    return SdpParse::Ok;
}

[[nodiscard]] u64 read_be(const std::vector<u8>& s, usize pos, usize bytes) {
    u64 value = 0;
    for (usize i = 0; i < bytes; ++i) value = (value << 8) | s[pos + i];
    return value;
}

/// SDP_ELEMENT_DATA (bluetoothapis.h): type @0, specificType @4, data union
/// @8 (16 bytes, zeroed first: probe attr lo/hi). Strings/URLs point at the
/// text, sequences/alternatives at their own descriptor with the whole
/// element's length. SDP_SPECIFICTYPE = (size index << 8) | (type << 4).
[[nodiscard]] bool write_sdp_element(Marshaler& m, GuestAddr out, GuestAddr base,
                                     const std::vector<u8>& s, usize pos,
                                     const SdpElement& e) {
    std::array<u8, 24> bytes{};
    const u32 type = e.type;
    u32 specific = 0;
    auto put_le = [&bytes](usize offset, u64 value, usize width) {
        for (usize i = 0; i < width; ++i) bytes[offset + i] = static_cast<u8>(value >> (8 * i));
    };
    const usize data = pos + e.header;
    switch (e.type) {
        case SDP_UINT:
        case SDP_INT:
        case SDP_UUID:
            specific = (static_cast<u32>(e.size_index) << 8) | (type << 4);
            // bthsdpdef.h defines SDP_ST_UUID32 as 0x0220 (the value of
            // SDP_ST_INT32), and Windows reports exactly that
            // (s7_bluetooth_probe attr id=203, elem desc=1a).
            if (e.type == SDP_UUID && e.data_len == 4) specific = 0x0220;
            if (e.type == SDP_UUID && e.data_len == 16) {
                // GUID: Data1/2/3 from big-endian fields, Data4 as bytes.
                put_le(8, read_be(s, data, 4), 4);
                put_le(12, read_be(s, data + 4, 2), 2);
                put_le(14, read_be(s, data + 6, 2), 2);
                std::memcpy(bytes.data() + 16, s.data() + data + 8, 8);
            } else if (e.data_len == 16) {
                // SDP_(U)LARGE_INTEGER_16: LowPart, then HighPart.
                put_le(8, read_be(s, data + 8, 8), 8);
                put_le(16, read_be(s, data, 8), 8);
            } else {
                put_le(8, read_be(s, data, e.data_len), e.data_len);
            }
            break;
        case SDP_BOOLEAN:
            bytes[8] = s[data];
            break;
        case SDP_STRING:
        case SDP_URL:
        case SDP_SEQUENCE:
        case SDP_ALTERNATIVE: {
            const bool text = e.type == SDP_STRING || e.type == SDP_URL;
            const u64 value = base + (text ? data : pos);
            const u32 length = static_cast<u32>(text ? e.data_len : e.total());
            if (m.cpu_is_64bit()) {
                put_le(8, value, 8);
                put_le(16, length, 4);
            } else {
                put_le(8, value, 4);
                put_le(12, length, 4);
            }
            break;
        }
        default: break;
    }
    put_le(0, type, 4);
    put_le(4, specific, 4);
    return m.memory().write_bytes(out, bytes.data(), bytes.size()).is_ok();
}

[[nodiscard]] std::optional<std::vector<u8>> read_stream(Marshaler& m, GuestAddr ptr, u32 length) {
    constexpr u32 kMax = 64u * 1024u * 1024u;
    if (ptr == 0 || length == 0 || length > kMax) return std::nullopt;
    std::vector<u8> bytes(length);
    if (m.memory().read_bytes(ptr, bytes.data(), bytes.size()).is_err()) return std::nullopt;
    return bytes;
}

struct SdpAttribute {
    u16 id;
    usize value_pos;
    SdpElement value;
};

/// Walk an SDP record — a SEQUENCE of (UINT16 id, value) pairs — calling
/// @p visit for each attribute until it returns false. The walk is bounded by
/// the buffer, not the sequence header (probe cbig/attr_trailing). Returns
/// false for a malformed record or pair (probe attr_notseq/altrec/id32/
/// novalue/badvalue).
template <typename Visit>
[[nodiscard]] bool walk_record(const std::vector<u8>& s, Visit&& visit) {
    SdpElement record;
    const SdpParse head = parse_sdp(s, 0, s.size(), record);
    if (head == SdpParse::Invalid || record.type != SDP_SEQUENCE || record.header > s.size()) {
        return false;
    }
    usize pos = record.header;
    while (pos < s.size()) {
        SdpElement id;
        if (parse_sdp(s, pos, s.size(), id) != SdpParse::Ok || s[pos] != 0x09) return false;
        SdpAttribute attribute{static_cast<u16>(read_be(s, pos + 1, 2)), pos + 3, {}};
        if (parse_sdp(s, attribute.value_pos, s.size(), attribute.value) != SdpParse::Ok) {
            return false;
        }
        if (!visit(attribute)) return true;
        pos = attribute.value_pos + attribute.value.total();
    }
    return true;
}

// ---- BluetoothSdpGetString ----

/// IANA MIBenum -> code page, for the MIBs BluetoothSdpGetString accepts
/// whose conversion Vitreo implements. Measured: s7_bluetooth_probe mib
/// lines fingerprint each MIB's decode of every high byte, and each matches
/// exactly one table in winapi/kernel32/locale/codepage_tables.inc (11 fits
/// 28598 and 38598, which decode identically).
struct MibCodePage { u16 mib; u32 code_page; };
constexpr MibCodePage kMibCodePages[] = {
    {3, 20127}, {4, 1252}, {5, 28592}, {6, 28593}, {7, 28594}, {8, 28595},
    {9, 28596}, {10, 28597}, {11, 28598}, {12, 1254}, {25, 20108}, {45, 28597},
    {103, 28591}, {106, 65001}, {109, 28603}, {111, 28605}, {1012, 28591},
    {2009, 850}, {2010, 852}, {2011, 437}, {2027, 10000}, {2028, 37}, {2047, 857},
    {2049, 861}, {2054, 869}, {2084, 20866}, {2086, 866}, {2087, 775}, {2088, 21866},
    {2250, 1250}, {2251, 1251}, {2252, 1252}, {2253, 1253}, {2254, 1254}, {2255, 1255},
    {2256, 1256}, {2257, 1257}, {2258, 1258}, {2259, 874},
};
/// MIBs Windows accepts that decode double-byte or stateful encodings
/// (Shift_JIS, EUC, ISO-2022, GB, Big5, ...), none of which Vitreo converts.
constexpr u16 kMibUnconverted[] = {17, 18, 19, 36, 37, 38, 39, 40, 57, 85, 114, 1010,
                                   2024, 2025, 2026, 2085};

/// The measured conversion: the longest prefix that converts without an
/// invalid sequence, or — when no non-empty prefix does — the whole string
/// with replacement characters (s7_bluetooth_probe str_bytes / str_badutf8).
[[nodiscard]] std::vector<u16> sdp_decode(u32 code_page, const u8* text, usize length) {
    constexpr u32 MB_ERR_INVALID_CHARS = 0x8;
    usize n = length;
    while (n > 0 && !codepage::multi_byte_to_wide(code_page, MB_ERR_INVALID_CHARS, text, n,
                                                  nullptr, 0).ok()) {
        --n;
    }
    const usize use = n == 0 ? length : n;
    const auto size = codepage::multi_byte_to_wide(code_page, 0, text, use, nullptr, 0);
    if (!size.ok() || size.count == 0) return {};
    std::vector<u16> units(size.count);
    (void)codepage::multi_byte_to_wide(code_page, 0, text, use, units.data(), units.size());
    return units;
}

void sdp_get_string(Marshaler& m) {
    const GuestAddr record_ptr = m.arg_ptr(0);
    const u32 record_len = m.arg32(1);
    const GuestAddr string_data = m.arg_ptr(2);
    const u16 offset = static_cast<u16>(m.arg32(3));
    const GuestAddr out = m.arg_ptr(4);
    const GuestAddr cch_ptr = m.arg_ptr(5);
    const auto record = read_stream(m, record_ptr, record_len);
    if (!record || cch_ptr == 0) return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
    u16 base = 0x0100;
    u16 mib = 106;
    if (string_data != 0) {
        // SDP_STRING_TYPE_DATA { USHORT encoding; USHORT mibeNum; USHORT attributeID; }
        mib = m.read_u16(string_data + 2).value_or(0);
        base = m.read_u16(string_data + 4).value_or(0);
    } else {
        // LanguageBaseAttributeIDList (0x0006): triplets of UINT16 language,
        // encoding, base. The thread's language is Vitreo's GetThreadLocale
        // (en-US, locale_string.hpp), ISO 639 "en"; without a match the
        // primary base 0x0100 in UTF-8 applies (str_locale_nomatch).
        bool malformed = false;
        const bool ok = walk_record(*record, [&](const SdpAttribute& a) {
            if (a.id != 0x0006) return true;
            if (a.value.type != SDP_SEQUENCE) { malformed = true; return false; }
            usize pos = a.value_pos + a.value.header;
            const usize end = a.value_pos + a.value.total();
            while (pos + 9 <= end) {
                if ((*record)[pos] != 0x09 || (*record)[pos + 3] != 0x09 || (*record)[pos + 6] != 0x09) break;
                const u16 language = static_cast<u16>(read_be(*record, pos + 1, 2));
                if (language == 0x656E) {
                    mib = static_cast<u16>(read_be(*record, pos + 4, 2));
                    base = static_cast<u16>(read_be(*record, pos + 7, 2));
                    break;
                }
                pos += 9;
            }
            return false;
        });
        if (!ok || malformed) return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
    }
    const u16 wanted = static_cast<u16>(base + offset);
    std::optional<SdpAttribute> found;
    if (!walk_record(*record, [&](const SdpAttribute& a) {
            if (a.id != wanted) return true;
            found = a;
            return false;
        })) {
        return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
    }
    if (!found) return m.set_return(error_codes::ERROR_FILE_NOT_FOUND);
    if (found->value.type != SDP_STRING) return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
    const u8* text = record->data() + found->value_pos + found->value.header;
    const usize length = found->value.data_len;
    std::vector<u16> units;
    if (mib == 1000 || mib == 1013 || mib == 1014 || mib == 1015) {
        // UCS-2 / UTF-16: 1013 is big-endian; an odd trailing byte is dropped.
        for (usize i = 0; i + 1 < length; i += 2) {
            units.push_back(mib == 1013 ? static_cast<u16>((text[i] << 8) | text[i + 1])
                                        : static_cast<u16>(text[i] | (text[i + 1] << 8)));
        }
    } else {
        u32 code_page = 0;
        for (const auto& row : kMibCodePages) {
            if (row.mib == mib) code_page = row.code_page;
        }
        if (code_page == 0) {
            if (std::find(std::begin(kMibUnconverted), std::end(kMibUnconverted), mib) !=
                std::end(kMibUnconverted)) {
                static std::mutex mutex;
                static std::unordered_set<u16> logged;
                std::lock_guard lock(mutex);
                if (logged.insert(mib).second) {
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED bthprops!BluetoothSdpGetString: "
                                         "MIBenum %u (a double-byte or stateful encoding)\n", mib);
                }
            }
            return m.set_return(error_codes::ERROR_INVALID_DATA);
        }
        units = sdp_decode(code_page, text, length);
    }
    const u32 needed = static_cast<u32>(units.size()) + 1;
    const auto capacity = m.read_u32(cch_ptr);
    if (out == 0 || capacity.is_err() || capacity.value() < needed) {
        (void)m.write_u32(cch_ptr, needed);
        return m.set_return(error_codes::ERROR_MORE_DATA);
    }
    units.push_back(0);
    if (m.memory().write_bytes(out, reinterpret_cast<const u8*>(units.data()),
                               units.size() * 2).is_err()) {
        return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
    }
    (void)m.write_u32(cch_ptr, needed);
    m.set_return(error_codes::ERROR_SUCCESS);
}

} // namespace

DllExports create_bthprops_thunks(hardware::BluetoothHost* host, HandleTable* handles,
                                  GuestFunctionCaller caller) {
    auto state = std::make_shared<BthState>();
    state->host = host;
    state->handles = handles;
    state->caller = std::move(caller);
    DllExports bt;

    // HBLUETOOTH_RADIO_FIND BluetoothFindFirstRadio(const
    //     BLUETOOTH_FIND_RADIO_PARAMS*, HANDLE* phRadio)
    // s7_bluetooth_probe ffr_*: NULL params 87, dwSize != 4 1306 (phRadio
    // untouched); no radio NULL + ERROR_NO_MORE_ITEMS. The radio handle is a
    // real kernel handle the caller closes with CloseHandle.
    bt.entries.push_back({"BluetoothFindFirstRadio", 0, [state](Marshaler& m) {
        const GuestAddr params = m.arg_ptr(0);
        const GuestAddr radio_out = m.arg_ptr(1);
        if (params == 0) return bt_fail(m, error_codes::ERROR_INVALID_PARAMETER);
        if (m.read_u32(params).value_or(0) != BLUETOOTH_FIND_RADIO_PARAMS_SIZE) {
            return bt_fail(m, ERROR_REVISION_MISMATCH);
        }
        if (!state->adapter().present) return bt_fail(m, error_codes::ERROR_NO_MORE_ITEMS);
        if (radio_out == 0 || state->handles == nullptr) {
            return bt_fail(m, error_codes::ERROR_INVALID_PARAMETER);
        }
        const Handle radio = state->handles->allocate(HandleType::Device,
                                                      new std::string(kRadioObject));
        if (m.write_ptr(radio_out, radio).is_err()) {
            (void)state->handles->close(radio);
            return bt_fail(m, error_codes::ERROR_INVALID_PARAMETER);
        }
        std::lock_guard lock(state->mutex);
        const u64 find = state->next_find += 4;
        state->radio_finds.insert(find);
        m.set_return(find);
    }, 8});
    // BOOL BluetoothFindNextRadio(HBLUETOOTH_RADIO_FIND, HANDLE*): a host has
    // one radio, so a live find is exhausted (ERROR_NO_MORE_ITEMS); NULL or
    // unknown ERROR_INVALID_HANDLE with phRadio untouched (fnr_null).
    bt.entries.push_back({"BluetoothFindNextRadio", 0, [state](Marshaler& m) {
        const u64 find = m.arg(0);
        (void)m.arg_ptr(1);
        std::lock_guard lock(state->mutex);
        if (state->radio_finds.count(find) == 0) return bt_fail(m, error_codes::ERROR_INVALID_HANDLE);
        bt_fail(m, error_codes::ERROR_NO_MORE_ITEMS);
    }, 8});
    bt.entries.push_back({"BluetoothFindRadioClose", 0, [state](Marshaler& m) {
        const u64 find = m.arg(0);
        std::lock_guard lock(state->mutex);
        if (state->radio_finds.erase(find) == 0) return bt_fail(m, error_codes::ERROR_INVALID_HANDLE);
        m.set_return(1);
    }, 4});
    // DWORD BluetoothGetRadioInfo(HANDLE hRadio, PBLUETOOTH_RADIO_INFO)
    // s7_bluetooth_probe gri_*: NULL info or NULL radio 87; dwSize != 520
    // 1306; a handle that is not a radio ERROR_INVALID_HANDLE. The host
    // contract cannot supply the BD_ADDR, local name, class of device, LMP
    // subversion or manufacturer (reading them needs CoreBluetooth, whose TCC
    // denial kills the process: hardware/macos/bluetooth_host_iobluetooth.mm),
    // so the radio's own information is not reported.
    bt.entries.push_back({"BluetoothGetRadioInfo", 0, [state](Marshaler& m) {
        const u64 radio = m.arg(0);
        const GuestAddr info = m.arg_ptr(1);
        if (info == 0 || radio == 0) return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        if (m.read_u32(info).value_or(0) != BLUETOOTH_RADIO_INFO_SIZE) {
            return m.set_return(ERROR_REVISION_MISMATCH);
        }
        if (!state->is_radio(radio)) return m.set_return(error_codes::ERROR_INVALID_HANDLE);
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED bthprops!BluetoothGetRadioInfo: "
                                 "host radio address/name/class/LMP version are unavailable\n");
        });
        m.set_return(error_codes::ERROR_NOT_SUPPORTED);
    }, 8});
    // BOOL BluetoothIsConnectable(HANDLE hRadio): NULL asks about every radio;
    // a handle that is not a radio is FALSE + ERROR_INVALID_HANDLE
    // (isconn_event). The host radio accepts connections while powered.
    bt.entries.push_back({"BluetoothIsConnectable", 0, [state](Marshaler& m) {
        const u64 radio = m.arg(0);
        if (radio != 0 && !state->is_radio(radio)) {
            return bt_fail(m, error_codes::ERROR_INVALID_HANDLE);
        }
        const auto adapter = state->adapter();
        m.set_return(adapter.present && adapter.powered ? 1u : 0u);
    }, 4});
    for (const char* name : {"BluetoothEnableDiscovery", "BluetoothEnableIncomingConnections"}) {
        bt.entries.push_back({name, 0, [name](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg(1);
            // Discoverability and incoming connections are the user's macOS
            // setting; Vitreo does not change them.
            VITREO_TRACE("[bthprops] %s -> FALSE (host radio policy is the user's)\n", name);
            bt_fail(m, error_codes::ERROR_NOT_SUPPORTED);
        }, 8});
    }
    // HBLUETOOTH_DEVICE_FIND BluetoothFindFirstDevice(const
    //     BLUETOOTH_DEVICE_SEARCH_PARAMS*, BLUETOOTH_DEVICE_INFO*)
    // s7_bluetooth_probe ffd_*: NULL arguments 87, a wrong dwSize in either
    // structure 1306. Remembered/connected devices are known only to
    // CoreBluetooth, so the search finds nothing.
    bt.entries.push_back({"BluetoothFindFirstDevice", 0, [state](Marshaler& m) {
        const GuestAddr params = m.arg_ptr(0);
        const GuestAddr info = m.arg_ptr(1);
        if (params == 0 || info == 0) return bt_fail(m, error_codes::ERROR_INVALID_PARAMETER);
        const u32 search_size = m.cpu_is_64bit() ? 40u : 32u;
        if (m.read_u32(params).value_or(0) != search_size ||
            m.read_u32(info).value_or(0) != BLUETOOTH_DEVICE_INFO_SIZE) {
            return bt_fail(m, ERROR_REVISION_MISMATCH);
        }
        // hRadio: the last field of BLUETOOTH_DEVICE_SEARCH_PARAMS.
        const u64 radio = m.read_ptr(params + (m.cpu_is_64bit() ? 32u : 28u)).value_or(0);
        if (radio != 0 && !state->is_radio(radio)) return bt_fail(m, error_codes::ERROR_INVALID_HANDLE);
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED bthprops!BluetoothFindFirstDevice: "
                                 "paired/connected device enumeration needs CoreBluetooth\n");
        });
        bt_fail(m, error_codes::ERROR_NO_MORE_ITEMS);
    }, 8});
    // No device-find handle is ever issued: ERROR_INVALID_HANDLE (fnd_null,
    // fdc_null).
    bt.entries.push_back({"BluetoothFindNextDevice", 0, [](Marshaler& m) {
        (void)m.arg(0);
        (void)m.arg_ptr(1);
        bt_fail(m, error_codes::ERROR_INVALID_HANDLE);
    }, 8});
    bt.entries.push_back({"BluetoothFindDeviceClose", 0, [](Marshaler& m) {
        (void)m.arg(0);
        bt_fail(m, error_codes::ERROR_INVALID_HANDLE);
    }, 4});

    // DWORD BluetoothSdpGetElementData(LPBYTE, ULONG, PSDP_ELEMENT_DATA)
    bt.entries.push_back({"BluetoothSdpGetElementData", 0, [](Marshaler& m) {
        const GuestAddr stream_ptr = m.arg_ptr(0);
        const u32 length = m.arg32(1);
        const GuestAddr out = m.arg_ptr(2);
        const auto stream = read_stream(m, stream_ptr, length);
        SdpElement e;
        if (!stream || out == 0 || parse_sdp(*stream, 0, stream->size(), e) != SdpParse::Ok ||
            !write_sdp_element(m, out, stream_ptr, *stream, 0, e)) {
            return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        }
        m.set_return(error_codes::ERROR_SUCCESS);
    }, 12});
    // DWORD BluetoothSdpGetContainerElementData(LPBYTE, ULONG,
    //     HBLUETOOTH_CONTAINER_ELEMENT* pElement, PSDP_ELEMENT_DATA)
    // *pElement is the element last returned (NULL to start); the walk is
    // bounded by the buffer, a truncated element ends it (259) and an invalid
    // descriptor is 87 (s7_bluetooth_probe cont_* / cw* / ctr / cbig).
    bt.entries.push_back({"BluetoothSdpGetContainerElementData", 0, [](Marshaler& m) {
        const GuestAddr stream_ptr = m.arg_ptr(0);
        const u32 length = m.arg32(1);
        const GuestAddr element_ptr = m.arg_ptr(2);
        const GuestAddr out = m.arg_ptr(3);
        const auto stream = read_stream(m, stream_ptr, length);
        if (!stream || element_ptr == 0 || out == 0) {
            return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        }
        SdpElement container;
        const SdpParse head = parse_sdp(*stream, 0, stream->size(), container);
        if (head == SdpParse::Invalid ||
            (container.type != SDP_SEQUENCE && container.type != SDP_ALTERNATIVE) ||
            container.header > stream->size()) {
            return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        }
        const auto current = m.read_ptr(element_ptr);
        if (current.is_err()) return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        usize pos = container.header;
        if (current.value() != 0) {
            if (current.value() < stream_ptr + container.header ||
                current.value() >= stream_ptr + stream->size()) {
                return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
            }
            const usize at = static_cast<usize>(current.value() - stream_ptr);
            SdpElement previous;
            const SdpParse r = parse_sdp(*stream, at, stream->size(), previous);
            if (r == SdpParse::Invalid) return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
            pos = r == SdpParse::Ok ? at + previous.total() : stream->size();
        }
        SdpElement e;
        const SdpParse r = parse_sdp(*stream, pos, stream->size(), e);
        if (r == SdpParse::Invalid) return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        if (r == SdpParse::Truncated) {
            (void)m.write_ptr(element_ptr, 0);
            return m.set_return(error_codes::ERROR_NO_MORE_ITEMS);
        }
        if (!write_sdp_element(m, out, stream_ptr, *stream, pos, e) ||
            m.write_ptr(element_ptr, stream_ptr + pos).is_err()) {
            return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        }
        m.set_return(error_codes::ERROR_SUCCESS);
    }, 16});
    // DWORD BluetoothSdpGetAttributeValue(LPBYTE, ULONG, USHORT, PSDP_ELEMENT_DATA)
    // s7_bluetooth_probe attr_*: the first pair with the id (a linear walk:
    // order is not required); missing ERROR_FILE_NOT_FOUND; a malformed
    // record or a malformed pair before the match 87.
    bt.entries.push_back({"BluetoothSdpGetAttributeValue", 0, [](Marshaler& m) {
        const GuestAddr record_ptr = m.arg_ptr(0);
        const u32 length = m.arg32(1);
        const u16 wanted = static_cast<u16>(m.arg32(2));
        const GuestAddr out = m.arg_ptr(3);
        const auto record = read_stream(m, record_ptr, length);
        if (!record || out == 0) return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        std::optional<SdpAttribute> found;
        if (!walk_record(*record, [&](const SdpAttribute& a) {
                if (a.id != wanted) return true;
                found = a;
                return false;
            })) {
            return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        }
        if (!found) return m.set_return(error_codes::ERROR_FILE_NOT_FOUND);
        if (!write_sdp_element(m, out, record_ptr, *record, found->value_pos, found->value)) {
            return m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        }
        m.set_return(error_codes::ERROR_SUCCESS);
    }, 16});
    // BOOL BluetoothSdpEnumAttributes(LPBYTE, ULONG,
    //     PFN_BLUETOOTH_ENUM_ATTRIBUTES_CALLBACK, LPVOID)
    // The callback gets (id, value element, its size, pvParam); FALSE stops.
    // s7_bluetooth_probe enum_*: NULL stream/callback or a zero size 87; a
    // record that is not a sequence, or a malformed pair, FALSE +
    // ERROR_INVALID_DATA; a completed or stopped walk TRUE, and the last
    // error is ERROR_INVALID_DATA either way.
    bt.entries.push_back({"BluetoothSdpEnumAttributes", 0, [state](Marshaler& m) {
        const GuestAddr record_ptr = m.arg_ptr(0);
        const u32 length = m.arg32(1);
        const GuestAddr callback = m.arg_ptr(2);
        const u64 param = m.arg(3);
        if (record_ptr == 0 || callback == 0 || length == 0) {
            return bt_fail(m, error_codes::ERROR_INVALID_PARAMETER);
        }
        const auto record = read_stream(m, record_ptr, length);
        if (!record) return bt_fail(m, error_codes::ERROR_INVALID_DATA);
        if (!state->caller) {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED bthprops!BluetoothSdpEnumAttributes: "
                                 "no guest callback path\n");
            return bt_fail(m, error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        }
        // Collect first: the callback runs guest code and may re-enter.
        std::vector<SdpAttribute> attributes;
        bool complete = true;
        const bool ok = walk_record(*record, [&](const SdpAttribute& a) {
            attributes.push_back(a);
            return true;
        });
        complete = ok;
        for (const SdpAttribute& a : attributes) {
            const u64 args[4] = {a.id, record_ptr + a.value_pos, a.value.total(), param};
            if ((state->caller(callback, args, 4) & 0xFFFFFFFFu) == 0) {
                complete = true;
                break;
            }
        }
        if (!complete) return bt_fail(m, error_codes::ERROR_INVALID_DATA);
        set_last_error(error_codes::ERROR_INVALID_DATA);
        m.set_return(1);
    }, 16});
    bt.entries.push_back({"BluetoothSdpGetString", 0, [](Marshaler& m) { sdp_get_string(m); }, 24});
    return bt;
}

} // namespace vitreo::winapi
