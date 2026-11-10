/// @file file.cpp
/// @brief CreateFileW / CreateFileA / ReadFile / WriteFile implementation (WIN32-010/011)

#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/filesystem/file_pipe_internal.hpp"
#include "winapi/kernel32/filesystem/file_thunks_internal.hpp"
#include "winapi/kernel32/filesystem/file_profile_internal.hpp"
#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/filepos.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/system/virtual_firmware.hpp"
#include "platform/host_hardware.hpp"
#include "winapi/core/thunk_registry.hpp"

#include "capsule/vfs/filesystem.hpp"
#include "winapi/common/write_sha1_debug.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/ipc_name.hpp"
#include "ipc/unix_pipe.hpp"

#include <algorithm>
#include <vector>
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <cstdio>
#include <mutex>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/file.h>
#include <unistd.h>

/// @file device_io_control.cpp
/// @brief FileManager::device_io_control
///
/// Closed method cut from file.cpp.

namespace vitreo::winapi {

namespace {

/// Strip the NT object-namespace prefix a mount-point substitute name carries.
/// FSCTL_SET_REPARSE_POINT names its target as "\\??\\C:\\dir"; the Win32 path
/// is what the VFS resolves.
std::string_view strip_nt_prefix(std::string_view path) {
    if (path.starts_with("\\??\\")) path.remove_prefix(4);
    if (path.starts_with("\\\\?\\")) path.remove_prefix(4);
    return path;
}

} // namespace

bool FileManager::set_reparse_point(Handle handle, GuestAddr input,
                                    u32 input_size) {
    // REPARSE_DATA_BUFFER: ReparseTag[4] ReparseDataLength[2] Reserved[2],
    // then for a mount point SubstituteNameOffset[2] SubstituteNameLength[2]
    // PrintNameOffset[2] PrintNameLength[2] and the WCHAR PathBuffer.
    constexpr u32 kMountPointHeader = 16;
    if (m_memory == nullptr || input == 0 || input_size < kMountPointHeader) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    const u32 tag = m_memory->read_u32(input).value_or(0);
    constexpr u32 kIoReparseTagMountPoint = 0xA0000003u;
    if (tag != kIoReparseTagMountPoint) {
        // Only directory junctions are realized. A symbolic-link reparse point
        // carries different flags and is a separate contract.
        VITREO_TRACE("[device] FSCTL_SET_REPARSE_POINT unsupported tag=0x%x\n",
                     tag);
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        return false;
    }
    const u32 subst_off = m_memory->read_u16(input + 8).value_or(0);
    const u32 subst_len = m_memory->read_u16(input + 10).value_or(0);
    if (subst_len == 0 || (kMountPointHeader + subst_off + subst_len) > input_size) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    std::string target;
    target.reserve(subst_len / 2);
    for (u32 i = 0; i + 1 < subst_len; i += 2) {
        const u32 unit =
            m_memory->read_u16(input + kMountPointHeader + subst_off + i)
                .value_or(0);
        if (unit == 0) break;
        // Junction targets are filesystem paths; anything outside Latin-1 does
        // not survive, which matches the rest of the VFS path handling.
        target.push_back(static_cast<char>(unit & 0xFF));
    }
    const std::string_view target_win = strip_nt_prefix(target);
    const std::string link_win = path_for_handle(handle);
    if (link_win.empty() || target_win.empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    auto target_host = m_vfs.resolve_host_path(target_win);
    auto link_host = m_vfs.resolve_host_path(link_win);
    if (target_host.is_err() || link_host.is_err()) {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        return false;
    }

    // The caller created the directory first (that is how a junction is made
    // on Windows: CreateDirectory, open it, then set the reparse point on it).
    // A host symlink cannot replace a live directory, so drop the empty one.
    // rmdir fails if it is not empty, which is the right refusal.
    if (::rmdir(link_host.value().c_str()) != 0 && errno != ENOENT) {
        VITREO_TRACE("[device] junction: rmdir(%s) failed errno=%d\n",
                     link_host.value().c_str(), errno);
        set_last_error(error_codes::ERROR_DIR_NOT_EMPTY);
        return false;
    }
    if (::symlink(target_host.value().c_str(), link_host.value().c_str()) != 0) {
        VITREO_TRACE("[device] junction: symlink(%s -> %s) errno=%d\n",
                     link_host.value().c_str(), target_host.value().c_str(),
                     errno);
        set_last_error(errno == EEXIST ? error_codes::ERROR_ALREADY_EXISTS
                                       : error_codes::ERROR_ACCESS_DENIED);
        return false;
    }
    VITREO_TRACE("[device] junction '%s' -> '%.*s'\n", link_win.c_str(),
                 static_cast<int>(target_win.size()), target_win.data());
    return true;
}

bool FileManager::device_io_control(
    Handle handle, u32 control_code, GuestAddr input, u32 input_size,
    GuestAddr output, u32 output_size, u32& bytes_returned) {
    bytes_returned = 0;
    if(m_handles.get_type(handle)==HandleType::HidDevice) {
        auto file=m_handles.get_shared<HidOpenFile>(handle,HandleType::HidDevice);
        if(!file || !m_memory) {set_last_error(error_codes::ERROR_INVALID_HANDLE);return false;}
        auto completed=[&] {
            if(file->completion_signal) {
                file->completion_signal->reset();
                if((file->notification_modes.load() & hid_sdk::kFILE_SKIP_SET_EVENT_ON_HANDLE)==0) file->completion_signal->signal();
            }
            return true;
        };
        const auto& l=gamepad_hid::layout(false); // identical public HIDClass layout on x86/x64
        gamepad_hid::Bytes bytes;
        switch(control_code) {
            case hid_sdk::kIOCTL_HID_GET_INPUT_REPORT: {
                if(output_size<gamepad_hid::kReportBytes) {
                    static std::once_flag once;std::call_once(once,[]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED kernel32!DeviceIoControl: undersized HID input snapshot error contract\n");});
                    set_last_error(error_codes::ERROR_NOT_SUPPORTED);return false;
                }
                const auto id=m_memory->read_u8(output);
                if(id.is_err()) {set_last_error(error_codes::ERROR_NOACCESS);return false;}
                if(id.value()!=gamepad_hid::kReportId) {
                    static std::once_flag once;std::call_once(once,[]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED kernel32!DeviceIoControl: unmatched HID snapshot report ID\n");});
                    set_last_error(error_codes::ERROR_NOT_SUPPORTED);return false;
                }
                bool failed=false,unsupported=false;const auto report=m_gamepad_catalog ? m_gamepad_catalog->snapshot(file,&failed,&unsupported) : std::optional<gamepad_hid::Report>{};
                if(unsupported) {
                    static std::once_flag once;std::call_once(once,[]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED kernel32!DeviceIoControl: native background HID monitoring unavailable\n");});
                    set_last_error(error_codes::ERROR_NOT_SUPPORTED);return false;
                }
                if(!report) {set_last_error(failed ? error_codes::ERROR_GEN_FAILURE : error_codes::ERROR_DEVICE_NOT_CONNECTED);return false;}
                if(m_memory->write_bytes(output+1,report->data()+1,report->size()-1).is_err()) {set_last_error(error_codes::ERROR_NOACCESS);return false;}
                // HIDClass (not the minidriver) reports Information=0.
                return completed();
            }
            case hid_sdk::kIOCTL_HID_GET_COLLECTION_INFORMATION:
                bytes.resize(l.sizeof_HID_COLLECTION_INFORMATION);
                gamepad_hid::store32(bytes,l.offsetof_HID_COLLECTION_INFORMATION_DescriptorSize,gamepad_hid::kPreparsedData.size());
                bytes[l.offsetof_HID_COLLECTION_INFORMATION_Polled]=1;
                gamepad_hid::store16(bytes,l.offsetof_HID_COLLECTION_INFORMATION_VersionNumber,1);
                break;
            case hid_sdk::kIOCTL_HID_GET_COLLECTION_DESCRIPTOR:
                bytes.assign(gamepad_hid::kPreparsedData.begin(),gamepad_hid::kPreparsedData.end());break;
            case hid_sdk::kIOCTL_GET_NUM_DEVICE_INPUT_BUFFERS: {
                std::lock_guard lock(file->mutex);bytes.resize(sizeof(u32));gamepad_hid::store32(bytes,0,file->capacity);break;
            }
            case hid_sdk::kIOCTL_SET_NUM_DEVICE_INPUT_BUFFERS: {
                if(input_size<sizeof(u32)) {set_last_error(error_codes::ERROR_INVALID_PARAMETER);return false;}
                const auto count=m_memory->read_u32(input);
                if(count.is_err() || count.value()<hid_sdk::kMinimumInputBuffers || count.value()>hid_sdk::kMaximumInputBuffers) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);return false;
                }
                std::lock_guard lock(file->mutex);file->capacity=count.value();
                while(file->reports.size()>file->capacity) file->reports.pop_front();return completed();
            }
            case hid_sdk::kIOCTL_HID_FLUSH_QUEUE: {
                std::lock_guard lock(file->mutex);file->reports.clear();return completed();
            }
            default: {
                static std::mutex mutex;static std::unordered_set<u32> logged;
                std::lock_guard lock(mutex);
                if(logged.insert(control_code).second) std::fprintf(stderr,"[vitreo] UNIMPLEMENTED kernel32!DeviceIoControl: HID code=0x%x\n",control_code);
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);return false;
            }
        }
        if(control_code==hid_sdk::kIOCTL_HID_GET_COLLECTION_INFORMATION && (!output || !output_size)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);return false;
        }
        if(control_code==hid_sdk::kIOCTL_HID_GET_COLLECTION_DESCRIPTOR) {
            // Windows copies the entire available prefix before returning
            // ERROR_INVALID_USER_BUFFER for an undersized descriptor buffer.
            const usize copied=std::min<usize>(output_size,bytes.size());
            if(!output || (copied && m_memory->write_bytes(output,bytes.data(),copied).is_err())) {
                set_last_error(error_codes::ERROR_INVALID_USER_BUFFER);return false;
            }
            if(output_size<bytes.size()) {set_last_error(error_codes::ERROR_INVALID_USER_BUFFER);return false;}
            bytes_returned=static_cast<u32>(bytes.size());return completed();
        }
        if(output_size<bytes.size()) {set_last_error(error_codes::ERROR_INVALID_USER_BUFFER);return false;}
        if(m_memory->write_bytes(output,bytes.data(),bytes.size()).is_err()) {set_last_error(error_codes::ERROR_NOACCESS);return false;}
        bytes_returned=static_cast<u32>(bytes.size());return completed();
    }
    // An FSCTL is issued on a FILE handle, not a device one: Windows routes it
    // to the filesystem that owns the file. Rejecting every non-Device handle
    // made libuv's junction path fail with ERROR_INVALID_HANDLE, which Node
    // reports as "EBADF: bad file descriptor, symlink ...".
    constexpr u32 kFsctlSetReparsePoint = 0x000900A4u;
    if (control_code == kFsctlSetReparsePoint) {
        const HandleType type = m_handles.get_type(handle);
        if (type != HandleType::File && type != HandleType::Device) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return false;
        }
        return set_reparse_point(handle, input, input_size);
    }
    if (m_handles.get_type(handle) != HandleType::Device) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    const auto* device = static_cast<const std::string*>(
        m_handles.get(handle, HandleType::Device));
    if (device == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }

    // Volume devices opened via CreateFile("\\.\X:"). Media-present optical
    // mounts answer CHECK_VERIFY; other IOCTLs are logged so bring-up can name
    // the next contract without guessing SecuROM's sequence.
    if (device->size() == 5 && device->compare(0, 4, "vol:") == 0) {
        const char letter = (*device)[4];
        const std::string root =
            std::string(1, static_cast<char>(std::toupper(
                                 static_cast<unsigned char>(letter)))) +
            ":\\";
        auto resolved = m_vfs.resolve_components(root);
        if (resolved.is_err() || resolved.value().first == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return false;
        }
        capsule::IFileSystem* fs = resolved.value().first;
        const bool optical = fs->is_optical();

        constexpr u32 IOCTL_STORAGE_CHECK_VERIFY = 0x002D4800;
        constexpr u32 IOCTL_STORAGE_CHECK_VERIFY2 = 0x002D4400;
        constexpr u32 IOCTL_CDROM_CHECK_VERIFY = 0x00024800;
        constexpr u32 IOCTL_STORAGE_QUERY_PROPERTY = 0x002D1400;
        constexpr u32 IOCTL_CDROM_READ_TOC = 0x00024000;

        switch (control_code) {
            case IOCTL_STORAGE_CHECK_VERIFY:
            case IOCTL_STORAGE_CHECK_VERIFY2:
            case IOCTL_CDROM_CHECK_VERIFY:
                // Media is present whenever the volume is mounted in the VFS.
                bytes_returned = 0;
                VITREO_TRACE("[device] volume %c: CHECK_VERIFY -> TRUE "
                             "optical=%d\n",
                             std::toupper(static_cast<unsigned char>(letter)),
                             optical ? 1 : 0);
                return true;

            case IOCTL_CDROM_READ_TOC: {
                // CDROM_TOC: Length[2] (BE) + FirstTrack + LastTrack +
                // TRACK_DATA[N]. SecuROM requests an 804-byte buffer
                // (header + 100 tracks); we return one data track + lead-out.
                if (!optical) {
                    set_last_error(error_codes::ERROR_INVALID_FUNCTION);
                    return false;
                }
                if (output == 0 || output_size < 20 || m_memory == nullptr) {
                    set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                    return false;
                }
                capsule::VolumeInfo info{};
                u32 leadout_lba = 0;
                if (fs->volume_info(info) && info.total_clusters > 0) {
                    leadout_lba = info.total_clusters;
                } else {
                    leadout_lba = 1; // minimal non-empty disc
                }
                auto lba_to_msf = [](u32 lba, u8 addr[4]) {
                    // CD absolute MSF includes the 150-frame (2s) pregap.
                    const u32 abs = lba + 150u;
                    addr[0] = 0;
                    addr[1] = static_cast<u8>(abs / (75u * 60u));
                    addr[2] = static_cast<u8>((abs / 75u) % 60u);
                    addr[3] = static_cast<u8>(abs % 75u);
                };
                std::vector<u8> toc(20, 0);
                // Length excludes the Length field itself: First/Last + 2*TRACK_DATA.
                toc[0] = 0;
                toc[1] = 18;
                toc[2] = 1;   // FirstTrack
                toc[3] = 1;   // LastTrack
                // Track 1 — data track (Control=4, Adr=1 → packed byte 0x14).
                toc[4] = 0;
                toc[5] = 0x14;
                toc[6] = 1;
                toc[7] = 0;
                lba_to_msf(0, toc.data() + 8);
                // Lead-out — track number 0xAA.
                toc[12] = 0;
                toc[13] = 0x14;
                toc[14] = 0xAA;
                toc[15] = 0;
                lba_to_msf(leadout_lba, toc.data() + 16);

                const u32 copy_n =
                    std::min(output_size, static_cast<u32>(toc.size()));
                if (m_memory->write_bytes(output, toc.data(), copy_n).is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    return false;
                }
                bytes_returned = copy_n;
                VITREO_TRACE("[device] volume %c: READ_TOC -> %u bytes "
                             "leadout_lba=%u\n",
                             std::toupper(static_cast<unsigned char>(letter)),
                             bytes_returned, leadout_lba);
                return true;
            }

            case 0x002D4804: {
                // IOCTL_STORAGE_MEDIA_REMOVAL — lock/unlock tray. Input is a
                // BOOLEAN PreventMediaRemoval; no output. Accept for optical.
                (void)input;
                bytes_returned = 0;
                VITREO_TRACE("[device] volume %c: MEDIA_REMOVAL in=%u -> TRUE\n",
                             std::toupper(static_cast<unsigned char>(letter)),
                             input_size);
                return true;
            }

            case 0x00041018: {
                // IOCTL_SCSI_GET_ADDRESS → SCSI_ADDRESS (8 bytes).
                if (output == 0 || output_size < 8 || m_memory == nullptr) {
                    set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                    return false;
                }
                // SCSI_ADDRESS: ULONG Length; UCHAR Port/Path/Target/Lun.
                u8 addr[8] = {};
                addr[0] = 8; // Length (little-endian ULONG)
                // PortNumber/PathId/TargetId/Lun remain 0.
                if (m_memory->write_bytes(output, addr, 8).is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    return false;
                }
                bytes_returned = 8;
                VITREO_TRACE("[device] volume %c: SCSI_GET_ADDRESS -> 8 bytes\n",
                             std::toupper(static_cast<unsigned char>(letter)));
                return true;
            }

            case IOCTL_STORAGE_QUERY_PROPERTY: {
                // STORAGE_PROPERTY_QUERY: PropertyId (u32) + QueryType (u32) + ...
                if (input_size < 8 || input == 0 || m_memory == nullptr) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    return false;
                }
                const auto prop_id = m_memory->read_u32(input);
                const auto query_type = m_memory->read_u32(input + 4);
                if (prop_id.is_err() || query_type.is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    return false;
                }
                constexpr u32 StorageDeviceProperty = 0;
                constexpr u32 StorageAdapterProperty = 1;
                constexpr u32 PropertyStandardQuery = 0;
                constexpr u32 PropertyExistsQuery = 1;

                if (query_type.value() == PropertyExistsQuery) {
                    const bool exists =
                        prop_id.value() == StorageDeviceProperty ||
                        prop_id.value() == StorageAdapterProperty;
                    bytes_returned = 0;
                    if (!exists) {
                        set_last_error(error_codes::ERROR_INVALID_FUNCTION);
                        return false;
                    }
                    return true;
                }
                if (query_type.value() != PropertyStandardQuery) {
                    set_last_error(error_codes::ERROR_INVALID_FUNCTION);
                    return false;
                }
                if (output == 0 || output_size < 8) {
                    set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                    return false;
                }

                std::vector<u8> out;
                if (prop_id.value() == StorageDeviceProperty) {
                    // STORAGE_DEVICE_DESCRIPTOR + NUL-terminated VendorId /
                    // ProductId / ProductRevision strings (offsets from start).
                    const char vendor[] = "VITREO";
                    const char product[] = "CD-ROM";
                    const char revision[] = "1.0";
                    constexpr u32 header = 40; // up through RawPropertiesLength
                    const u32 vendor_off = header;
                    const u32 product_off =
                        vendor_off + static_cast<u32>(sizeof(vendor));
                    const u32 revision_off =
                        product_off + static_cast<u32>(sizeof(product));
                    const u32 total =
                        revision_off + static_cast<u32>(sizeof(revision));
                    out.assign(total, 0);
                    auto put_u32 = [&](u32 off, u32 v) {
                        out[off] = static_cast<u8>(v);
                        out[off + 1] = static_cast<u8>(v >> 8);
                        out[off + 2] = static_cast<u8>(v >> 16);
                        out[off + 3] = static_cast<u8>(v >> 24);
                    };
                    put_u32(0, header);   // Version
                    put_u32(4, total);    // Size
                    out[8] = optical ? 0x05u : 0x00u; // SCSI CD-ROM / disk
                    out[9] = 0;                       // DeviceTypeModifier
                    out[10] = optical ? 1u : 0u;      // RemovableMedia
                    out[11] = 0;                      // CommandQueueing
                    put_u32(12, vendor_off);
                    put_u32(16, product_off);
                    put_u32(20, revision_off);
                    put_u32(24, 0); // SerialNumberOffset
                    out[28] = optical ? 0x03u : 0x07u; // BusTypeAtapi / BusTypeUsb
                    put_u32(32, 0); // RawPropertiesLength
                    std::memcpy(out.data() + vendor_off, vendor, sizeof(vendor));
                    std::memcpy(out.data() + product_off, product, sizeof(product));
                    std::memcpy(out.data() + revision_off, revision,
                                sizeof(revision));
                } else if (prop_id.value() == StorageAdapterProperty) {
                    // STORAGE_ADAPTER_DESCRIPTOR (fixed 32-byte header form).
                    out.assign(32, 0);
                    auto put_u32 = [&](u32 off, u32 v) {
                        out[off] = static_cast<u8>(v);
                        out[off + 1] = static_cast<u8>(v >> 8);
                        out[off + 2] = static_cast<u8>(v >> 16);
                        out[off + 3] = static_cast<u8>(v >> 24);
                    };
                    put_u32(0, 32);   // Version
                    put_u32(4, 32);   // Size
                    put_u32(8, 1);    // MaximumTransferLength (pages) — unused
                    put_u32(12, 1);   // MaximumPhysicalPages
                    put_u32(16, 0);   // AlignmentMask
                    out[20] = 0;      // AdapterUsesPio
                    out[21] = 0;      // AdapterScansDown
                    out[22] = 1;      // CommandQueueing
                    out[23] = 1;      // AcceleratedTransfer
                    out[24] = optical ? 0x03u : 0x07u; // BusType
                    out[25] = 0;      // BusMajorVersion
                    out[26] = 0;      // BusMinorVersion
                    out[27] = 0;      // SrbType
                    out[28] = 0;      // AddressType
                } else {
                    VITREO_TRACE("[device] volume %c: QUERY_PROPERTY "
                                 "PropertyId=%u unsupported\n",
                                 std::toupper(static_cast<unsigned char>(letter)),
                                 prop_id.value());
                    set_last_error(error_codes::ERROR_INVALID_FUNCTION);
                    return false;
                }

                const u32 copy_n =
                    std::min(output_size, static_cast<u32>(out.size()));
                if (m_memory->write_bytes(output, out.data(), copy_n).is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    return false;
                }
                bytes_returned = copy_n;
                VITREO_TRACE("[device] volume %c: QUERY_PROPERTY id=%u -> "
                             "%u bytes optical=%d\n",
                             std::toupper(static_cast<unsigned char>(letter)),
                             prop_id.value(), bytes_returned,
                             optical ? 1 : 0);
                return true;
            }

            default:
                VITREO_TRACE("[device] volume %c: unsupported IOCTL=0x%08x "
                             "in=%u out=%u optical=%d\n",
                             std::toupper(static_cast<unsigned char>(letter)),
                             control_code, input_size, output_size,
                             optical ? 1 : 0);
                set_last_error(error_codes::ERROR_INVALID_FUNCTION);
                return false;
        }
    }

    if (*device == "disk:0") {
        // \\.\PhysicalDrive0 = the host's physical disk (HostDisk). Learn
        // IOCTL_DISK_GET_DRIVE_GEOMETRY / DISK_GEOMETRY; values and failures
        // measured by tools/oracle_probe/disk_geometry_probe.
        constexpr u32 IOCTL_DISK_GET_DRIVE_GEOMETRY = 0x00070000;
        constexpr u32 IOCTL_DISK_GET_LENGTH_INFO = 0x0007405C;
        const platform::HostDisk& disk = platform::host_hardware().disk;
        if (control_code == IOCTL_DISK_GET_DRIVE_GEOMETRY) {
            constexpr u32 kDiskGeometryBytes = 24;
            if (output == 0 || output_size < kDiskGeometryBytes || m_memory == nullptr) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                return false;
            }
            // The BIOS-style translation Windows reports for fixed disks:
            // 255 heads x 63 sectors, Cylinders = DiskSize / (255*63*BytesPerSector).
            constexpr u32 kTracksPerCylinder = 255;
            constexpr u32 kSectorsPerTrack = 63;
            constexpr u32 kFixedMedia = 12;
            const u64 cylinder_bytes =
                u64{kTracksPerCylinder} * kSectorsPerTrack * disk.block_bytes;
            u8 geometry[kDiskGeometryBytes] = {};
            const u64 cylinders = cylinder_bytes ? disk.size_bytes / cylinder_bytes : 0;
            std::memcpy(geometry + 0, &cylinders, 8);
            std::memcpy(geometry + 8, &kFixedMedia, 4);
            std::memcpy(geometry + 12, &kTracksPerCylinder, 4);
            std::memcpy(geometry + 16, &kSectorsPerTrack, 4);
            std::memcpy(geometry + 20, &disk.block_bytes, 4);
            if (m_memory->write_bytes(output, geometry, kDiskGeometryBytes).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return false;
            }
            bytes_returned = kDiskGeometryBytes;
            return true;
        }
        constexpr u32 IOCTL_STORAGE_QUERY_PROPERTY_DISK = 0x002D1400;
        if (control_code == IOCTL_STORAGE_QUERY_PROPERTY_DISK) {
            // Learn IOCTL_STORAGE_QUERY_PROPERTY / STORAGE_PROPERTY_QUERY;
            // every failure and truncation rule below is measured
            // (tools/oracle_probe/storage_property_probe{64,32}.expected).
            constexpr u32 kQueryBytes = 12;  // PropertyId, QueryType, AdditionalParameters[1]
            if (input == 0 || input_size < kQueryBytes || m_memory == nullptr) {
                set_last_error(error_codes::ERROR_BAD_LENGTH);
                return false;
            }
            const auto property_id = m_memory->read_u32(input);
            const auto query_type = m_memory->read_u32(input + 4);
            if (property_id.is_err() || query_type.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return false;
            }
            constexpr u32 kPropertyStandardQuery = 0;
            constexpr u32 kPropertyExistsQuery = 1;
            constexpr u32 kStorageDeviceProperty = 0;
            if (query_type.value() != kPropertyStandardQuery &&
                query_type.value() != kPropertyExistsQuery) {
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);
                return false;
            }
            if (property_id.value() != kStorageDeviceProperty) {
                // Windows also answers StorageAdapterProperty (1) and
                // StorageDeviceIdProperty (2) here; Vitreo has no adapter or
                // device-identification data for them.
                static std::mutex logged_mutex;
                static std::unordered_set<u32> logged;
                std::lock_guard<std::mutex> lock(logged_mutex);
                if (logged.insert(property_id.value()).second) {
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!DeviceIoControl: "
                                         "IOCTL_STORAGE_QUERY_PROPERTY PropertyId %u on a disk\n",
                                 property_id.value());
                }
                set_last_error(error_codes::ERROR_INVALID_FUNCTION);
                return false;
            }
            bytes_returned = 0;
            if (query_type.value() == kPropertyExistsQuery) return true;

            // STORAGE_DEVICE_DESCRIPTOR (Version 40 = the fixed part) followed
            // by the NUL-terminated strings its offsets name; an empty string
            // has offset 0 (not present).
            constexpr u32 kDescriptorBytes = 40;
            std::vector<u8> out(kDescriptorBytes, 0);
            const auto put_u32 = [&](u32 off, u32 v) { std::memcpy(out.data() + off, &v, 4); };
            const auto append = [&](u32 offset_field, const std::string& text) {
                if (text.empty()) return;
                put_u32(offset_field, static_cast<u32>(out.size()));
                out.insert(out.end(), text.begin(), text.end());
                out.push_back(0);
            };
            put_u32(0, kDescriptorBytes);       // Version
            out[8] = 0;                         // DeviceType: DIRECT_ACCESS_DEVICE
            out[9] = 0;                         // DeviceTypeModifier
            out[10] = 0;                        // RemovableMedia: the internal disk
            out[11] = 1;                        // CommandQueueing
            append(12, disk.vendor);
            append(16, disk.product);
            append(20, disk.revision);
            append(24, disk.serial);
            // STORAGE_BUS_TYPE (winioctl.h).
            u32 bus_type = 0;                   // BusTypeUnknown
            switch (disk.bus) {
                case platform::HostDiskBus::Nvme: bus_type = 0x11; break;
                case platform::HostDiskBus::Sata: bus_type = 0x0B; break;
                case platform::HostDiskBus::Usb: bus_type = 0x07; break;
                case platform::HostDiskBus::SecureDigital: bus_type = 0x0C; break;
                case platform::HostDiskBus::Unknown: break;
            }
            put_u32(28, bus_type);
            put_u32(32, 0);                     // RawPropertiesLength
            put_u32(4, static_cast<u32>(out.size()));  // Size
            // A short buffer receives a prefix and still succeeds; 0 bytes too.
            const u32 copy = std::min<u32>(output == 0 ? 0 : output_size,
                                           static_cast<u32>(out.size()));
            if (copy != 0 && m_memory->write_bytes(output, out.data(), copy).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return false;
            }
            bytes_returned = copy;
            return true;
        }
        if (control_code == IOCTL_DISK_GET_LENGTH_INFO) {
            // FILE_READ_ACCESS; every PhysicalDrive handle Vitreo opens is
            // query-only (create_file.cpp), so this is always denied.
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return false;
        }
        static std::mutex logged_mutex;
        static std::unordered_set<u32> logged;
        std::lock_guard<std::mutex> lock(logged_mutex);
        const u32 in0 = input && input_size >= 4 && m_memory ? m_memory->read_u32(input).value_or(0) : 0;
        const u32 in1 = input && input_size >= 8 && m_memory ? m_memory->read_u32(input + 4).value_or(0) : 0;
        if (logged.insert(control_code ^ (in0 << 8) ^ (in1 << 20)).second) {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED kernel32!DeviceIoControl: "
                         "\\\\.\\PhysicalDrive0 IOCTL 0x%08x (in=%u out=%u in0=%u in1=%u)\n",
                         control_code, input_size, output_size, in0, in1);
        }
        set_last_error(error_codes::ERROR_INVALID_FUNCTION);
        return false;
    }

    if (*device != "cpuz162" || m_memory == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }

    constexpr u32 kCpuzGetDriverVersion = 0x9C402400;
    constexpr u32 kCpuzGetOsVersion = 0x9C402404;
    constexpr u32 kCpuzReadMemory = 0x9C402540;
    constexpr u32 kCpuzDriverVersion = 0x162;

    const auto require_output = [&](u32 required_size) {
        if (output_size < required_size) {
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            return false;
        }
        if (output == 0) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return false;
        }
        return true;
    };
    const auto write_output_u32 = [&](u32 offset, u32 value) {
        if (m_memory->write_u32(output + offset, value).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return false;
        }
        return true;
    };

    switch (control_code) {
        case kCpuzGetDriverVersion:
            if (!require_output(sizeof(u32)) ||
                !write_output_u32(0, kCpuzDriverVersion)) {
                return false;
            }
            bytes_returned = sizeof(u32);
            VITREO_TRACE("[device] cpuz162 IOCTL_GET_DRIVER_VERSION -> "
                         "0x%x (%u bytes)\n",
                         kCpuzDriverVersion, bytes_returned);
            return true;

        case kCpuzGetOsVersion:
            if (!require_output(3 * sizeof(u32)) ||
                !write_output_u32(0, 10) ||
                !write_output_u32(4, 0) ||
                !write_output_u32(8, 19041)) {
                return false;
            }
            bytes_returned = 3 * sizeof(u32);
            VITREO_TRACE("[device] cpuz162 IOCTL_GET_OS_VERSION -> "
                         "10.0.19041 (%u bytes)\n",
                         bytes_returned);
            return true;

        case kCpuzReadMemory: {
            constexpr u32 kInputSize = 3 * sizeof(u32);
            if (input_size < kInputSize || input == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                return false;
            }
            const auto address_high = m_memory->read_u32(input);
            const auto address_low =
                m_memory->read_u32(input + sizeof(u32));
            const auto requested =
                m_memory->read_u32(input + 2 * sizeof(u32));
            if (address_high.is_err() || address_low.is_err() ||
                requested.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return false;
            }

            const u64 physical_address =
                (static_cast<u64>(address_high.value()) << 32) |
                address_low.value();
            const u64 requested_size = requested.value();
            VITREO_TRACE(
                "[device] cpuz162 READ_MEMORY physical=0x%llx bytes=%u "
                "output_capacity=%u\n",
                static_cast<unsigned long long>(physical_address),
                requested.value(), output_size);

            if (requested_size == 0 ||
                physical_address < virtual_firmware::kBiosBase ||
                requested_size > virtual_firmware::kBiosSize ||
                physical_address - virtual_firmware::kBiosBase >
                    virtual_firmware::kBiosSize - requested_size) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                return false;
            }
            if (output_size < sizeof(u32) + requested_size) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                return false;
            }
            if (output == 0) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return false;
            }

            const auto status_write = m_memory->write_u32(output, 0);
            const auto firmware = virtual_firmware::bios_window();
            const usize offset = static_cast<usize>(
                physical_address - virtual_firmware::kBiosBase);
            const auto data_write = m_memory->write_bytes(
                output + sizeof(u32), firmware.data() + offset,
                static_cast<usize>(requested_size));
            if (status_write.is_err() || data_write.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return false;
            }
            bytes_returned =
                static_cast<u32>(sizeof(u32) + requested_size);
            return true;
        }
    }

    std::array<u8, 16> input_prefix{};
    const usize input_prefix_size =
        std::min<usize>(input_size, input_prefix.size());
    bool input_prefix_read = input_prefix_size == 0;
    if (input != 0 && input_prefix_size != 0) {
        input_prefix_read =
            m_memory->read_bytes(
                input, input_prefix.data(), input_prefix_size).is_ok();
    }
    std::array<char, 16 * 3 + 1> input_hex{};
    if (input_prefix_read) {
        for (usize i = 0; i < input_prefix_size; ++i) {
            std::snprintf(
                input_hex.data() + i * 3, input_hex.size() - i * 3,
                "%02x%s", input_prefix[i],
                i + 1 == input_prefix_size ? "" : " ");
        }
    } else {
        std::snprintf(
            input_hex.data(), input_hex.size(), "<unreadable>");
    }
    VITREO_TRACE("[device] cpuz162 unsupported IOCTL=0x%08x input=0x%llx/%u "
                 "bytes=[%s] output=0x%llx/%u\n", control_code,
                 static_cast<unsigned long long>(input), input_size,
                 input_hex.data(), static_cast<unsigned long long>(output),
                 output_size);
    set_last_error(error_codes::ERROR_INVALID_FUNCTION);
    return false;
}

} // namespace vitreo::winapi
