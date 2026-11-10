// SPDX-License-Identifier: LGPL-2.1-only

/// @file hid.cpp
/// @brief hid.dll exports (see hid.hpp for the contract).

#include "winapi/system/hid/hid.hpp"
#include "winapi/common/gamepad_hid_report.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/global_local.hpp"

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

bool gamepad_preparsed(Marshaler& m, GuestAddr address) {
    std::array<u8, gamepad_hid::kPreparsedData.size()> bytes{};
    return m.memory().read_bytes(address, bytes.data(), bytes.size()).is_ok() &&
           gamepad_hid::recognizes(bytes);
}

// HID parser capabilities: full selection/filtering, required counts and
// partial writes follow hidpi.h plus hid_caps_{x86,x64}.expected.
void gamepad_caps_query(Marshaler& m, bool value, bool specific) {
    const u32 type = m.arg32(0);
    const u16 page = specific ? static_cast<u16>(m.arg32(1)) : 0;
    const u16 link = specific ? static_cast<u16>(m.arg32(2)) : 0;
    const u16 usage = specific ? static_cast<u16>(m.arg32(3)) : 0;
    const GuestAddr out = m.arg_ptr(specific ? 4 : 1);
    const GuestAddr length = m.arg_ptr(specific ? 5 : 2);
    if (!gamepad_preparsed(m, m.arg_ptr(specific ? 6 : 3))) {
        m.set_return(hid_sdk::kHIDP_STATUS_INVALID_PREPARSED_DATA); return;
    }
    if (type > hid_sdk::kHidP_Feature) {
        m.set_return(hid_sdk::kHIDP_STATUS_INVALID_REPORT_TYPE); return;
    }
    const auto capacity = m.read_u16(length);
    if (capacity.is_err()) { (void)m.raise_access_violation(false,length); return; }
    std::vector<gamepad_hid::Bytes> rows;
    // All controls of the normalized collection live directly in its root.
    const bool root = link == 0 || link == hid_sdk::kHIDP_LINK_COLLECTION_ROOT;
    if (type == hid_sdk::kHidP_Input && root) {
        if (value && (page == 0 || page == hid_sdk::kHID_USAGE_PAGE_GENERIC)) {
            for (const auto& field : gamepad_hid::kValues)
                if (usage == 0 || usage == field.usage)
                    rows.push_back(gamepad_hid::value_capabilities(m.cpu_is_64bit(),field));
        } else if (!value && (page == 0 || page == hid_sdk::kHID_USAGE_PAGE_BUTTON) &&
                   (usage == 0 || usage <= gamepad_hid::kButtonMasks.size())) {
            rows.push_back(gamepad_hid::button_capabilities(m.cpu_is_64bit()));
        }
    }
    const auto* mask = value ? hid_sdk::kValueCapsWriteMask : hid_sdk::kButtonCapsWriteMask;
    for (usize i = 0; i < std::min<usize>(capacity.value(),rows.size()); ++i) {
        const auto& row = rows[i];
        for (usize start=0; start<row.size();) {
            if (!mask[start]) { ++start; continue; }
            usize end = start+1;
            while (end<row.size() && mask[end]) ++end;
            const GuestAddr at = out+i*row.size()+start;
            if (m.memory().write_bytes(at,row.data()+start,end-start).is_err()) {
                (void)m.raise_access_violation(true,at); return;
            }
            start=end;
        }
    }
    if (m.memory().write_u16(length,static_cast<u16>(rows.size())).is_err()) {
        (void)m.raise_access_violation(true,length); return;
    }
    m.set_return(rows.empty() ? hid_sdk::kHIDP_STATUS_USAGE_NOT_FOUND :
        capacity.value()<rows.size() ? hid_sdk::kHIDP_STATUS_BUFFER_TOO_SMALL :
        hid_sdk::kHIDP_STATUS_SUCCESS);
}

// Shared report validation order measured on Windows: invalid enum first,
// then exact length (including zero lengths for absent report types), then
// report presence. A report ID unmatched by GetData is a successful empty set.
u32 gamepad_report_status(u32 type, u32 length) {
    if (type > hid_sdk::kHidP_Feature) return hid_sdk::kHIDP_STATUS_INVALID_REPORT_TYPE;
    const u32 expected=type==hid_sdk::kHidP_Input ? gamepad_hid::kReportBytes : 0;
    if (length!=expected) return hid_sdk::kHIDP_STATUS_INVALID_REPORT_LENGTH;
    return type==hid_sdk::kHidP_Input ? hid_sdk::kHIDP_STATUS_SUCCESS :
                                      hid_sdk::kHIDP_STATUS_REPORT_DOES_NOT_EXIST;
}
void gamepad_get_data(Marshaler& m) {
    if (!gamepad_preparsed(m,m.arg_ptr(3))) {
        m.set_return(hid_sdk::kHIDP_STATUS_INVALID_PREPARSED_DATA);return;
    }
    const u32 status=gamepad_report_status(m.arg32(0),m.arg32(5));
    if (status!=hid_sdk::kHIDP_STATUS_SUCCESS) {m.set_return(status);return;}
    const GuestAddr report_ptr=m.arg_ptr(4),length_ptr=m.arg_ptr(2),out=m.arg_ptr(1);
    gamepad_hid::Report report{};
    if (m.memory().read_bytes(report_ptr,report.data(),report.size()).is_err()) {
        (void)m.raise_access_violation(false,report_ptr);return;
    }
    const auto capacity=m.read_u32(length_ptr);
    if (capacity.is_err()) {(void)m.raise_access_violation(false,length_ptr);return;}
    struct Data {u16 index;u32 value;bool button;};
    std::array<Data,gamepad_hid::kHatDataIndex+1> entries{};
    usize count=0;
    if (report[0]==gamepad_hid::kReportId) {
        // HID data index order follows the published normalized collection.
        for (const auto& field : gamepad_hid::kValues)
            if (field.data_index<gamepad_hid::kFirstButtonDataIndex)
                entries[count++]={field.data_index,gamepad_hid::read_value(report,field),false};
        for (u16 usage=1;usage<=gamepad_hid::kButtonMasks.size();++usage)
            if (gamepad_hid::button_pressed(report,usage))
                entries[count++]={static_cast<u16>(gamepad_hid::kFirstButtonDataIndex+usage-1),1,true};
        const auto& hat=gamepad_hid::kValues[std::size(gamepad_hid::kValues)-1];
        entries[count++]={hat.data_index,gamepad_hid::read_value(report,hat),false};
    }
    const auto& layout=gamepad_hid::layout(m.cpu_is_64bit());
    for (usize i=0;i<std::min<usize>(capacity.value(),count);++i) {
        const GuestAddr entry=out+i*layout.sizeof_HIDP_DATA;
        const GuestAddr index=entry+layout.offsetof_HIDP_DATA_DataIndex;
        const GuestAddr value=entry+layout.offsetof_HIDP_DATA_RawValue;
        if (m.memory().write_u16(index,entries[i].index).is_err()) {
            (void)m.raise_access_violation(true,index);return;
        }
        // A button writes BOOLEAN On only, not the other bytes of the
        // RawValue union. Reserved bytes remain caller-owned (Windows oracle).
        const bool written=entries[i].button ? m.write_u8(value,1).is_ok() :
                                              m.write_u32(value,entries[i].value).is_ok();
        if (!written) {(void)m.raise_access_violation(true,value);return;}
    }
    if (m.write_u32(length_ptr,static_cast<u32>(count)).is_err()) {
        (void)m.raise_access_violation(true,length_ptr);return;
    }
    m.set_return(capacity.value()<count ? hid_sdk::kHIDP_STATUS_BUFFER_TOO_SMALL :
                                        hid_sdk::kHIDP_STATUS_SUCCESS);
}

bool gamepad_root_link(u16 link) {
    return link==0 || link==hid_sdk::kHIDP_LINK_COLLECTION_ROOT;
}
void gamepad_get_usage_value(Marshaler& m, bool scaled) {
    if (!gamepad_preparsed(m,m.arg_ptr(5))) {
        m.set_return(hid_sdk::kHIDP_STATUS_INVALID_PREPARSED_DATA);return;
    }
    u32 status=gamepad_report_status(m.arg32(0),m.arg32(7));
    if(status!=hid_sdk::kHIDP_STATUS_SUCCESS) {m.set_return(status);return;}
    const auto* field=gamepad_hid::find_value(static_cast<u16>(m.arg32(3)));
    if(static_cast<u16>(m.arg32(1))!=hid_sdk::kHID_USAGE_PAGE_GENERIC ||
       !gamepad_root_link(static_cast<u16>(m.arg32(2))) || !field) {
        m.set_return(hid_sdk::kHIDP_STATUS_USAGE_NOT_FOUND);return;
    }
    gamepad_hid::Report report{};
    if(m.memory().read_bytes(m.arg_ptr(6),report.data(),report.size()).is_err()) {
        (void)m.raise_access_violation(false,m.arg_ptr(6));return;
    }
    if(report[0]!=gamepad_hid::kReportId) {
        m.set_return(hid_sdk::kHIDP_STATUS_INCOMPATIBLE_REPORT_ID);return;
    }
    u32 value=gamepad_hid::read_value(report,*field);
    // Every normalized field has unsigned logical/physical minimum zero
    // and equal logical/physical maxima, so scaling is the identity. Null
    // handling still applies; Windows writes zero for scaled range failures.
    if(scaled && value>field->logical_max) {
        value=0;
        status=field->has_null ? hid_sdk::kHIDP_STATUS_NULL : hid_sdk::kHIDP_STATUS_VALUE_OUT_OF_RANGE;
    }
    if(m.write_u32(m.arg_ptr(4),value).is_err()) {
        (void)m.raise_access_violation(true,m.arg_ptr(4));return;
    }
    m.set_return(status);
}
void gamepad_get_usages(Marshaler& m, bool extended) {
    const unsigned shift=extended?1:0;
    if(!gamepad_preparsed(m,m.arg_ptr(5-shift))) {
        m.set_return(hid_sdk::kHIDP_STATUS_INVALID_PREPARSED_DATA);return;
    }
    const GuestAddr count_ptr=m.arg_ptr(4-shift),out=m.arg_ptr(3-shift);
    const auto capacity=m.read_u32(count_ptr);
    if(capacity.is_err()) {(void)m.raise_access_violation(false,count_ptr);return;}
    // Windows clears a USAGE-sized prefix for BOTH variants, even though
    // GetUsagesEx subsequently emits larger usage/page pairs. This happens
    // before type/length validation, but after preparsed-data validation.
    const u64 bytes=m.cpu_is_64bit() ? u64(capacity.value())*sizeof(u16) :
        static_cast<u32>(capacity.value()*sizeof(u16));
    static constexpr std::array<u8,256> zeros{};
    for(u64 offset=0;offset<bytes;) {
        const usize chunk=static_cast<usize>(std::min<u64>(zeros.size(),bytes-offset));
        if(m.memory().write_bytes(out+offset,zeros.data(),chunk).is_err()) {
            (void)m.raise_access_violation(true,out+offset);return;
        }
        offset+=chunk;
    }
    const u32 validation=gamepad_report_status(m.arg32(0),m.arg32(7-shift));
    if(validation!=hid_sdk::kHIDP_STATUS_SUCCESS) {m.set_return(validation);return;}
    const u16 page=extended ? hid_sdk::kHID_USAGE_PAGE_BUTTON : static_cast<u16>(m.arg32(1));
    u32 status=hid_sdk::kHIDP_STATUS_SUCCESS;
    std::array<u16,gamepad_hid::kButtonMasks.size()> usages{};
    usize count=0;
    if(page!=hid_sdk::kHID_USAGE_PAGE_BUTTON ||
       !gamepad_root_link(static_cast<u16>(m.arg32(2-shift)))) {
        status=hid_sdk::kHIDP_STATUS_USAGE_NOT_FOUND;
    } else {
        gamepad_hid::Report report{};
        if(m.memory().read_bytes(m.arg_ptr(6-shift),report.data(),report.size()).is_err()) {
            (void)m.raise_access_violation(false,m.arg_ptr(6-shift));return;
        }
        if(report[0]!=gamepad_hid::kReportId) status=hid_sdk::kHIDP_STATUS_INCOMPATIBLE_REPORT_ID;
        else for(u16 usage=1;usage<=gamepad_hid::kButtonMasks.size();++usage)
            if(gamepad_hid::button_pressed(report,usage)) usages[count++]=usage;
    }
    for(usize i=0;i<std::min<usize>(capacity.value(),count);++i) {
        const GuestAddr at=out+i*sizeof(u16)*(extended?2:1);
        if(m.memory().write_u16(at,usages[i]).is_err()) {
            (void)m.raise_access_violation(true,at);return;
        }
        if(extended && m.memory().write_u16(at+sizeof(u16),page).is_err()) {
            (void)m.raise_access_violation(true,at+sizeof(u16));return;
        }
    }
    if(m.write_u32(count_ptr,static_cast<u32>(count)).is_err()) {
        (void)m.raise_access_violation(true,count_ptr);return;
    }
    m.set_return(status!=hid_sdk::kHIDP_STATUS_SUCCESS ? status :
        capacity.value()<count ? hid_sdk::kHIDP_STATUS_BUFFER_TOO_SMALL : hid_sdk::kHIDP_STATUS_SUCCESS);
}

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
ApiHandler unsupported_gamepad_parser(u32 args, u32 preparsed_arg, const char* name) {
    const auto logged = std::make_shared<std::once_flag>();
    return [args, preparsed_arg, name, logged](Marshaler& m) {
        for (u32 i = 0; i < args; ++i) (void)m.arg(i);
        if (gamepad_preparsed(m,m.arg_ptr(preparsed_arg))) {
            std::call_once(*logged,[name] {
                std::fprintf(stderr,"[vitreo] UNIMPLEMENTED hid.dll!%s: normalized gamepad collection\n",name);
            });
            m.set_return(hid_sdk::kHIDP_STATUS_NOT_IMPLEMENTED);
        } else m.set_return(HIDP_STATUS_INVALID_PREPARSED_DATA);
    };
}

} // namespace

DllExports create_hid_thunks(HandleTable* handles, FileManager* files, GlobalLocalManager* allocator) {
    struct Allocations {std::mutex mutex;std::unordered_set<GuestAddr> pointers;};
    auto allocations=std::make_shared<Allocations>();
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
        [allocations,allocator](Marshaler& m) {
            std::lock_guard lock(allocations->mutex);
            const auto at=m.arg_ptr(0);
            if(!allocator || !allocations->pointers.erase(at)) {m.set_return(0);return;}
            const auto saved=get_last_error();const bool ok=allocator->global_free(at)==0;
            set_last_error(saved);m.set_return(ok ? 1:0);
        }, 4});

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
        auto fallback=not_a_hid_device(row.args,handles);
        const auto once=std::make_shared<std::once_flag>();
        hid.entries.push_back({row.name,0,[handles,files,allocator,allocations,row,fallback,once](Marshaler& m) {
            auto file=handles ? handles->get_shared<HidOpenFile>(static_cast<Handle>(m.arg(0)),HandleType::HidDevice) : HidOpenFileHolder{};
            if(m.arg(0)>0xFFFFFFFFull || !file) {fallback(m);return;}
            const std::string_view name=row.name;
            auto fail=[&](u32 error){set_last_error(error);m.set_return(0);};
            auto completed=[&] {
                // All measured HidD metadata routines use an explicit private
                // event. It completes the synchronous file, not an async file.
                if(file->completion_signal) {
                    file->completion_signal->reset();
                    if(!file->overlapped) file->completion_signal->signal();
                }
            };
            auto write=[&](GuestAddr out,const auto& bytes) {
                if(m.memory().write_bytes(out,bytes.data(),bytes.size()).is_err()) {fail(error_codes::ERROR_NOACCESS);return false;}
                completed();m.set_return(1);return true;
            };
            if(name=="HidD_GetPreparsedData") {
                if(!m.arg_ptr(1)) {(void)m.raise_access_violation(true,0);return;}
                if(!allocator) {fail(error_codes::ERROR_NOT_SUPPORTED);return;}
                const auto saved=get_last_error();
                const GuestAddr pointer=allocator->global_alloc(GMEM_FIXED,gamepad_hid::kPreparsedData.size());
                if(!pointer) {fail(error_codes::ERROR_NOT_ENOUGH_MEMORY);return;}
                if(m.memory().write_bytes(pointer,gamepad_hid::kPreparsedData.data(),gamepad_hid::kPreparsedData.size()).is_err() ||
                    (m.cpu_is_64bit() ? m.write_u64(m.arg_ptr(1),pointer).is_err() : m.write_u32(m.arg_ptr(1),static_cast<u32>(pointer)).is_err())) {
                    (void)allocator->global_free(pointer);set_last_error(saved);(void)m.raise_access_violation(true,m.arg_ptr(1));return;
                }
                {std::lock_guard lock(allocations->mutex);allocations->pointers.insert(pointer);}
                completed();set_last_error(saved);m.set_return(1);return;
            }
            if(name=="HidD_GetAttributes") {
                const auto& l=gamepad_hid::layout(m.cpu_is_64bit());
                gamepad_hid::Bytes bytes(l.sizeof_HIDD_ATTRIBUTES);
                gamepad_hid::store32(bytes,l.offsetof_HIDD_ATTRIBUTES_Size,l.sizeof_HIDD_ATTRIBUTES);
                gamepad_hid::store16(bytes,l.offsetof_HIDD_ATTRIBUTES_VersionNumber,1);
                // Oracle: HIDD_ATTRIBUTES tail padding is caller-owned.
                bytes.resize(l.offsetof_HIDD_ATTRIBUTES_VersionNumber+sizeof(u16));
                if(m.memory().write_bytes(m.arg_ptr(1),bytes.data(),bytes.size()).is_err()) {(void)m.raise_access_violation(true,m.arg_ptr(1));return;}
                completed();m.set_return(1);return;
            }
            if(name=="HidD_GetProductString" || name=="HidD_GetManufacturerString" || name=="HidD_GetSerialNumberString") {
                const std::string text=name=="HidD_GetProductString" ? "Vitreo Virtual Gamepad" : name=="HidD_GetManufacturerString" ? "Vitreo" : std::to_string(file->slot)+"&"+std::to_string(file->generation);
                gamepad_hid::Bytes bytes((text.size()+1)*2);
                for(usize i=0;i<text.size();++i) bytes[i*2]=static_cast<u8>(text[i]);
                if(m.arg32(2)<bytes.size()) {fail(error_codes::ERROR_INVALID_USER_BUFFER);return;}
                (void)write(m.arg_ptr(1),bytes);return;
            }
            if(name=="HidD_GetNumInputBuffers") {
                if(!m.arg_ptr(1)) {fail(error_codes::ERROR_INVALID_PARAMETER);return;}
                std::lock_guard lock(file->mutex);
                if(m.write_u32(m.arg_ptr(1),file->capacity).is_err()) fail(error_codes::ERROR_NOACCESS);else {completed();m.set_return(1);}
                return;
            }
            if(name=="HidD_SetNumInputBuffers") {
                const u32 count=m.arg32(1);
                if(count<hid_sdk::kMinimumInputBuffers || count>hid_sdk::kMaximumInputBuffers) {fail(error_codes::ERROR_INVALID_PARAMETER);return;}
                std::lock_guard lock(file->mutex);file->capacity=count;
                while(file->reports.size()>count) file->reports.pop_front();completed();m.set_return(1);return;
            }
            if(name=="HidD_FlushQueue") {
                std::lock_guard lock(file->mutex);file->reports.clear();completed();m.set_return(1);return;
            }
            if(name=="HidD_GetInputReport") {
                if(m.arg32(2)<gamepad_hid::kReportBytes) {
                    std::call_once(*once,[]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED hid.dll!HidD_GetInputReport: undersized snapshot error contract\n");});
                    fail(error_codes::ERROR_NOT_SUPPORTED);return;
                }
                const auto id=m.read_u8(m.arg_ptr(1));
                if(id.is_err()) {fail(error_codes::ERROR_NOACCESS);return;}
                if(id.value()!=gamepad_hid::kReportId) {
                    std::call_once(*once,[]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED hid.dll!HidD_GetInputReport: unmatched report ID\n");});
                    fail(error_codes::ERROR_NOT_SUPPORTED);return;
                }
                const auto catalog=files ? files->gamepad_catalog() : nullptr;
                bool host_failed=false,unsupported=false;
                const auto report=catalog ? catalog->snapshot(file,&host_failed,&unsupported) : std::optional<gamepad_hid::Report>{};
                if(unsupported) {
                    std::call_once(*once,[]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED hid.dll!HidD_GetInputReport: native background monitoring unavailable\n");});
                    fail(error_codes::ERROR_NOT_SUPPORTED);return;
                }
                if(!report) {fail(host_failed ? error_codes::ERROR_GEN_FAILURE : error_codes::ERROR_DEVICE_NOT_CONNECTED);return;}
                (void)write(m.arg_ptr(1),*report);return;
            }
            std::call_once(*once,[row]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED hid.dll!%s: normalized gamepad device operation\n",row.name);});
            fail(error_codes::ERROR_NOT_SUPPORTED);
        },static_cast<u16>(row.args*4)});
    }
    // The original, copyable preparsed collection is shared with Raw Input.
    // Microsoft hidpi.h / HidP_GetCaps: the only status failure is invalid
    // preparsed data; successful output is the complete HIDP_CAPS structure.
    hid.entries.push_back({"HidP_GetCaps", 0, [](Marshaler& m) {
        if (!gamepad_preparsed(m, m.arg_ptr(0))) {
            m.set_return(hid_sdk::kHIDP_STATUS_INVALID_PREPARSED_DATA);
            return;
        }
        const auto caps = gamepad_hid::capabilities(m.cpu_is_64bit());
        const GuestAddr out = m.arg_ptr(1);
        if (m.memory().write_bytes(out, caps.data(), caps.size()).is_err()) {
            (void)m.raise_access_violation(true, out);
            return;
        }
        m.set_return(hid_sdk::kHIDP_STATUS_SUCCESS);
    }, 8});
    hid.entries.push_back({"HidP_GetButtonCaps",0,
        [](Marshaler& m) { gamepad_caps_query(m,false,false); },16});
    hid.entries.push_back({"HidP_GetSpecificButtonCaps",0,
        [](Marshaler& m) { gamepad_caps_query(m,false,true); },28});
    hid.entries.push_back({"HidP_GetValueCaps",0,
        [](Marshaler& m) { gamepad_caps_query(m,true,false); },16});
    hid.entries.push_back({"HidP_GetSpecificValueCaps",0,
        [](Marshaler& m) { gamepad_caps_query(m,true,true); },28});
    hid.entries.push_back({"HidP_GetData",0,gamepad_get_data,24});
    hid.entries.push_back({"HidP_GetUsageValue",0,
        [](Marshaler& m){gamepad_get_usage_value(m,false);},32});
    hid.entries.push_back({"HidP_GetScaledUsageValue",0,
        [](Marshaler& m){gamepad_get_usage_value(m,true);},32});
    hid.entries.push_back({"HidP_GetUsages",0,
        [](Marshaler& m){gamepad_get_usages(m,false);},32});
    hid.entries.push_back({"HidP_GetUsagesEx",0,
        [](Marshaler& m){gamepad_get_usages(m,true);},28});
    struct ParserRow { const char* name; u16 args; u16 preparsed_arg; };
    static constexpr ParserRow kParser[] = {
        {"HidP_GetExtendedAttributes",5,2},
        {"HidP_GetLinkCollectionNodes",3,2}, {"HidP_GetUsageValueArray",9,6},
        {"HidP_InitializeReportForID",5,2},
        {"HidP_SetData",6,3}, {"HidP_SetScaledUsageValue",8,5},
        {"HidP_SetUsageValue",8,5}, {"HidP_SetUsageValueArray",9,6},
        {"HidP_SetUsages",8,5}, {"HidP_UnsetUsages",8,5},
    };
    for (const ParserRow& row : kParser) {
        hid.entries.push_back({row.name,0,
            unsupported_gamepad_parser(row.args,row.preparsed_arg,row.name),static_cast<u16>(row.args*4)});
    }
    // ULONG HidP_MaxDataListLength(HIDP_REPORT_TYPE, PHIDP_PREPARSED_DATA)
    // and HidP_MaxUsageListLength(HIDP_REPORT_TYPE, USAGE, PHIDP_PREPARSED_DATA):
    // "zero if the preparsed data is not valid" (Learn; hid_probe maxdata_*).
    hid.entries.push_back({"HidP_MaxDataListLength", 0,
        [](Marshaler& m) {
            m.set_return(m.arg32(0) == hid_sdk::kHidP_Input &&
                gamepad_preparsed(m, m.arg_ptr(1)) ? gamepad_hid::kHatDataIndex + 1 : 0);
        }, 8});
    hid.entries.push_back({"HidP_MaxUsageListLength", 0,
        [](Marshaler& m) {
            const u16 page = static_cast<u16>(m.arg32(1));
            m.set_return(m.arg32(0) == hid_sdk::kHidP_Input &&
                (page == 0 || page == hid_sdk::kHID_USAGE_PAGE_BUTTON) &&
                gamepad_preparsed(m, m.arg_ptr(2)) ? gamepad_hid::kButtonMasks.size() : 0);
        }, 12});
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
