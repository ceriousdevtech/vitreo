// SPDX-License-Identifier: LGPL-2.1-only
/// Windows ICM contracts over process-owned ICC objects and macOS ColorSync.
/// Authoritative API/layout evidence: mscms_contract.inc and mscms_probe*.expected.
#include "mscms.hpp"
#include "mscms_contract.inc"
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include <ColorSync/ColorSync.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vitreo::winapi {
namespace {
constexpr const char* color_directory = "C:\\Windows\\System32\\spool\\drivers\\color";
constexpr u32 resource_limit = 256u * 1024u * 1024u;
struct Profile {
    capsule::Vfs* vfs{};
    std::optional<capsule::FileHandle> file;
    std::vector<u8> bytes;
    ColorSyncProfileRef native{};
    bool blank{};
    ~Profile() {
        if (native) CFRelease(native);
        if (file && vfs) (void)vfs->close(*file);
    }
};
struct Transform {
    ColorSyncTransformRef native{};
    std::vector<ColorSyncProfileRef> retained_profiles;
    u32 input{}, output{};
    ~Transform() {
        if (native) CFRelease(native);
        for (auto profile : retained_profiles) CFRelease(profile);
    }
};
struct State {
    capsule::Vfs* vfs{};
    std::mutex mutex;
    std::map<GuestAddr, std::unique_ptr<Profile>> profiles;
    std::map<GuestAddr, std::unique_ptr<Transform>> transforms;
    std::set<GuestAddr> retired;
    std::set<std::string> warnings;
    GuestAddr next{0x6C000001};
    u32 directory_error{};
    explicit State(capsule::Vfs* filesystem) : vfs(filesystem) {
        if (vfs) {
            for (const char* path : {"C:\\Windows\\System32\\spool",
                                    "C:\\Windows\\System32\\spool\\drivers", color_directory}) {
                if (vfs->stat(path).is_ok()) continue;
                auto result = vfs->create_dir(path);
                if (result.is_err()) { directory_error = vfs_error_to_win32(result.error()); break; }
            }
        }
    }
    ~State() {
        transforms.clear(); profiles.clear();
    }
};
void fail(Marshaler& m, u32 error) { set_last_error(error); m.set_return(0); }
void unsupported(Marshaler& m, State& s, const char* api, const char* reason) {
    if (s.warnings.insert(std::string(api) + ":" + reason).second)
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED mscms!%s: %s\n", api, reason);
    fail(m, icm::ERROR_CALL_NOT_IMPLEMENTED);
}
bool read(Marshaler& m, GuestAddr at, void* data, usize size) {
    if (m.memory().read_bytes(at, static_cast<Byte*>(data), size).is_ok()) return true;
    (void)m.raise_access_violation(false, at); m.set_return(0); return false;
}
bool write(Marshaler& m, GuestAddr at, const void* data, usize size) {
    if (m.memory().write_bytes(at, static_cast<const Byte*>(data), size).is_ok()) return true;
    (void)m.raise_access_violation(true, at); m.set_return(0); return false;
}
void bad_handle(Marshaler& m, State& s, GuestAddr handle) {
    // Oracle: NULL and previously closed objects fail87. An unreadable foreign
    // HPROFILE/HTRANSFORM faults; never dereference it as a host pointer.
    if (handle && !s.retired.contains(handle) &&
        !s.profiles.contains(handle) && !s.transforms.contains(handle)) {
        auto contents = m.read_u32(handle);
        if (contents.is_err()) { (void)m.raise_access_violation(false, handle); m.set_return(0); return; }
    }
    fail(m, icm::ERROR_INVALID_PARAMETER);
}
u32 big32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | p[3];
}
// ICC.1:2022 sections7.2–7.4: fixed header, tag count, then a table
// of three big-endian uint32 fields per entry. Derive sizes/offsets from
// the declared data format rather than scattering literal byte offsets.
struct IccTagRecord { u32 signature, offset, length; };
struct IccTagBase { u32 signature, reserved; };
constexpr usize icc_header_size = icm::PROFILEHEADER_SIZE32;
constexpr usize icc_table = icc_header_size + sizeof(u32);
bool embedded_wcs(const std::vector<u8>& bytes) {
    if (bytes.size() < icc_table) return false;
    const u32 count = big32(bytes.data() + icc_header_size);
    if (count > (bytes.size() - icc_table) / sizeof(IccTagRecord)) return false;
    for (u32 i = 0; i < count; ++i)
        if (big32(bytes.data() + icc_table + i * sizeof(IccTagRecord)) == icm::WCS_EMBEDDED_TAG_SIGNATURE) return true;
    return false;
}
bool xml_profile(const std::vector<u8>& bytes) {
    for (u8 byte : bytes) {
        if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r') continue;
        return byte == '<' || byte == 0xFF || byte == 0xFE || byte == 0xEF;
    }
    return false;
}
bool valid_icc(const std::vector<u8>& bytes) {
    if (bytes.size() < icc_table || big32(bytes.data() + icm::PROFILEHEADER_phSignature_OFFSET32) != icm::ORACLE_SIGNATURE) return false;
    const u32 size = big32(bytes.data()), count = big32(bytes.data() + icc_header_size);
    if (size < icc_table || size > bytes.size() || count > (size - icc_table) / sizeof(IccTagRecord)) return false;
    for (u32 i = 0; i < count; ++i) {
        const u8* tag = bytes.data() + icc_table + i * sizeof(IccTagRecord);
        const u32 offset = big32(tag + offsetof(IccTagRecord, offset));
        const u32 length = big32(tag + offsetof(IccTagRecord, length));
        if (offset > size || length > size - offset || offset % sizeof(u32) || length < sizeof(IccTagBase)) return false;
    }
    return true;
}
void open_profile(Marshaler& m, State& s) {
    const char* api = "OpenColorProfileW";
    const GuestAddr descriptor = m.arg_ptr(0);
    const u32 access = m.arg32(1), share = m.arg32(2), creation = m.arg32(3);
    if (!descriptor || (access != icm::PROFILE_READ && access != icm::PROFILE_READWRITE)) {
        fail(m, icm::ERROR_INVALID_PARAMETER); return;
    }
    const u32 pointer_offset = m.cpu_is_64bit() ? icm::PROFILE_pProfileData_OFFSET64 : icm::PROFILE_pProfileData_OFFSET32;
    const u32 size_offset = m.cpu_is_64bit() ? icm::PROFILE_cbDataSize_OFFSET64 : icm::PROFILE_cbDataSize_OFFSET32;
    auto type = m.read_u32(descriptor);
    auto pointer = m.read_ptr(descriptor + pointer_offset);
    auto size = m.read_u32(descriptor + size_offset);
    if (type.is_err() || pointer.is_err() || size.is_err()) {
        (void)m.raise_access_violation(false, descriptor); m.set_return(0); return;
    }
    if (!pointer.value() || !size.value()) { fail(m, icm::ERROR_INVALID_PARAMETER); return; }
    auto profile = std::make_unique<Profile>(); profile->vfs = s.vfs;
    if (type.value() == icm::PROFILE_MEMBUFFER) {
        if (size.value() > resource_limit) { fail(m, icm::ERROR_NOT_ENOUGH_MEMORY); return; }
        profile->bytes.resize(size.value());
        if (!read(m, pointer.value(), profile->bytes.data(), profile->bytes.size())) return;
        // Share and creation apply only to filenames (Microsoft Learn).
    } else if (type.value() == icm::PROFILE_FILENAME) {
        if (!s.vfs) { unsupported(m, s, api, "profile file access requires capsule VFS"); return; }
        if ((share & ~(icm::FILE_SHARE_READ | icm::FILE_SHARE_WRITE)) != 0 ||
            creation < icm::CREATE_NEW || creation > icm::TRUNCATE_EXISTING) {
            fail(m, icm::ERROR_INVALID_PARAMETER); return;
        }
        auto name = m.read_wide_string(pointer.value(), size.value() / sizeof(u16));
        if (name.is_err()) { (void)m.raise_access_violation(false, pointer.value()); m.set_return(0); return; }
        std::string path = name.value();
        if (path.empty()) { fail(m, icm::ERROR_INVALID_PARAMETER); return; }
        // Bare names refer to the system-wide color profile directory.
        if (path.find_first_of("\\/:") == std::string::npos) path = std::string(color_directory) + "\\" + path;
        const auto mode = access == icm::PROFILE_READ ? capsule::AccessMode::Read : capsule::AccessMode::ReadWrite;
        const bool existed = s.vfs->stat(path).is_ok();
        capsule::CreationDisposition disposition;
        switch (creation) {
        case icm::CREATE_NEW: disposition = capsule::CreationDisposition::CreateNew; break;
        case icm::CREATE_ALWAYS: disposition = capsule::CreationDisposition::CreateAlways; break;
        case icm::OPEN_ALWAYS: disposition = capsule::CreationDisposition::OpenAlways; break;
        case icm::TRUNCATE_EXISTING: disposition = capsule::CreationDisposition::TruncateExisting; break;
        default: disposition = capsule::CreationDisposition::OpenExisting; break;
        }
        auto opened = s.vfs->open(path, mode, static_cast<capsule::ShareMode>(share), disposition);
        if (opened.is_err()) { fail(m, vfs_error_to_win32(opened.error())); return; }
        profile->file = opened.value();
        const bool create_blank = creation == icm::CREATE_NEW || creation == icm::CREATE_ALWAYS ||
            creation == icm::TRUNCATE_EXISTING || (creation == icm::OPEN_ALWAYS && !existed);
        if (create_blank) {
            if (access == icm::PROFILE_READ) { fail(m, icm::ERROR_NOT_ENOUGH_MEMORY); return; }
            profile->bytes.assign(std::begin(icm::BLANK_PROFILE), std::end(icm::BLANK_PROFILE));
            auto written = s.vfs->write(*profile->file, profile->bytes);
            if (written.is_err()) { fail(m, vfs_error_to_win32(written.error())); return; }
            if (written.value() != profile->bytes.size()) { fail(m, icm::ERROR_READ_FAULT); return; }
            profile->blank = true;
        }
        auto info = s.vfs->stat_handle(*profile->file);
        if (info.is_err()) { fail(m, vfs_error_to_win32(info.error())); return; }
        if (info.value().size > resource_limit) { fail(m, icm::ERROR_NOT_ENOUGH_MEMORY); return; }
        profile->bytes.resize(static_cast<usize>(info.value().size));
        if (create_blank) (void)s.vfs->seek(*profile->file, 0, capsule::SeekOrigin::Begin);
        usize done = 0;
        while (done < profile->bytes.size()) {
            auto got = s.vfs->read(*profile->file, std::span<u8>(profile->bytes).subspan(done));
            if (got.is_err()) { fail(m, vfs_error_to_win32(got.error())); return; }
            if (got.value() == 0) { fail(m, icm::ERROR_READ_FAULT); return; }
            done += got.value();
        }
    } else { fail(m, icm::ERROR_INVALID_PARAMETER); return; }
    if (xml_profile(profile->bytes) || embedded_wcs(profile->bytes)) {
        unsupported(m, s, api, "WCS XML and embedded WCS device/appearance/gamut models"); return;
    }
    if (!valid_icc(profile->bytes)) { fail(m, icm::ERROR_INVALID_PROFILE); return; }
    profile->blank = std::equal(profile->bytes.begin(), profile->bytes.end(),
        std::begin(icm::BLANK_PROFILE), std::end(icm::BLANK_PROFILE));
    CFDataRef data = CFDataCreate(nullptr, profile->bytes.data(), static_cast<CFIndex>(profile->bytes.size()));
    if (!data) { fail(m, icm::ERROR_NOT_ENOUGH_MEMORY); return; }
    profile->native = ColorSyncProfileCreate(data, nullptr);
    CFRelease(data);
    if (!profile->native && !std::equal(profile->bytes.begin(), profile->bytes.end(),
            std::begin(icm::BLANK_PROFILE), std::end(icm::BLANK_PROFILE))) {
        fail(m, icm::ERROR_INVALID_PROFILE); return;
    }
    const GuestAddr handle = s.next++;
    s.profiles.emplace(handle, std::move(profile)); m.set_return(handle);
}
void header(Marshaler& m, State& s) {
    auto found = s.profiles.find(m.arg_ptr(0));
    if (found == s.profiles.end()) { bad_handle(m, s, m.arg_ptr(0)); return; }
    std::array<u8, icm::PROFILEHEADER_SIZE32> output;
    std::copy_n(found->second->bytes.begin(), output.size(), output.begin());
    // ICC big endian fields become SDK DWORDs/CIEXYZ; opaque reserved bytes
    // (including ICCv4 profile IDs) retain their exact byte sequence.
    for (u32 offset = 0; offset < icm::PROFILEHEADER_phReserved_OFFSET32; offset += sizeof(u32)) {
        const u32 value = big32(output.data() + offset);
        std::memcpy(output.data() + offset, &value, sizeof value);
    }
    if (write(m, m.arg_ptr(1), output.data(), output.size())) m.set_return(1);
}
bool verified_profile(const Profile& profile) {
    return !profile.blank && profile.native && valid_icc(profile.bytes) &&
        ColorSyncProfileVerify(profile.native, nullptr, nullptr);
}
void is_valid(Marshaler& m, State& s) {
    auto found = s.profiles.find(m.arg_ptr(0));
    if (found == s.profiles.end()) { bad_handle(m, s, m.arg_ptr(0)); return; }
    if (!m.arg_ptr(1)) { fail(m, icm::ERROR_INVALID_PARAMETER); return; }
    const u32 valid = verified_profile(*found->second);
    if (write(m, m.arg_ptr(1), &valid, sizeof valid)) m.set_return(valid);
}
void close_profile(Marshaler& m, State& s) {
    const GuestAddr handle = m.arg_ptr(0);
    if (!s.profiles.contains(handle)) { bad_handle(m, s, handle); return; }
    s.profiles.erase(handle); s.retired.insert(handle); m.set_return(1);
}
void delete_transform(Marshaler& m, State& s) {
    const GuestAddr handle = m.arg_ptr(0);
    if (!s.transforms.contains(handle)) { bad_handle(m, s, handle); return; }
    s.transforms.erase(handle); s.retired.insert(handle); m.set_return(1);
}
CFStringRef native_intent(u32 intent) {
    switch (intent) {
    case icm::INTENT_PERCEPTUAL: return kColorSyncRenderingIntentPerceptual;
    case icm::INTENT_RELATIVE_COLORIMETRIC: return kColorSyncRenderingIntentRelative;
    case icm::INTENT_SATURATION: return kColorSyncRenderingIntentSaturation;
    case icm::INTENT_ABSOLUTE_COLORIMETRIC: return kColorSyncRenderingIntentAbsolute;
    default: return nullptr;
    }
}
bool append_profile(CFMutableArrayRef sequence, ColorSyncProfileRef profile, u32 intent, CFStringRef tag) {
    const void* keys[] = {kColorSyncProfile, kColorSyncRenderingIntent, kColorSyncTransformTag,
                          kColorSyncBlackPointCompensation};
    const void* values[] = {profile, native_intent(intent), tag, kCFBooleanFalse};
    CFDictionaryRef step = CFDictionaryCreate(nullptr, keys, values, std::size(keys),
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!step) return false;
    CFArrayAppendValue(sequence, step); CFRelease(step); return true;
}
void create_transform(Marshaler& m, State& s) {
    const GuestAddr profiles_at = m.arg_ptr(0), intents_at = m.arg_ptr(2);
    const u32 count = m.arg32(1), intent_count = m.arg32(3), flags = m.arg32(4), preferred = m.arg32(5);
    if (!profiles_at || !intents_at || count == 0 || count > 10 ||
        (intent_count != 1 && intent_count != count)) { fail(m, icm::ERROR_INVALID_PARAMETER); return; }
    constexpr u32 mode_mask = icm::PROOF_MODE | icm::NORMAL_MODE;
    constexpr u32 supported = mode_mask | icm::USE_RELATIVE_COLORIMETRIC;
    constexpr u32 unavailable = icm::FAST_TRANSLATE | icm::ENABLE_GAMUT_CHECKING | icm::PRESERVEBLACK |
        icm::WCS_ALWAYS | icm::SEQUENTIAL_TRANSFORM;
    if ((flags & ~(supported | unavailable)) != 0 || preferred > count) { fail(m, icm::ERROR_INVALID_PARAMETER); return; }
    if (flags & unavailable) { unsupported(m, s, "CreateMultiProfileTransform", "WCS, gamut-check or nearest-only transform mode"); return; }
    if ((flags & mode_mask) == icm::PROOF_MODE) {
        unsupported(m, s, "CreateMultiProfileTransform", "Windows proof-mode quality is not equivalent to native draft quality"); return;
    }
    if (preferred != icm::INDEX_DONT_CARE) { unsupported(m, s, "CreateMultiProfileTransform", "profile-selected third-party CMM"); return; }
    auto transform = std::make_unique<Transform>();
    CFMutableArrayRef sequence = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    if (!sequence) { fail(m, icm::ERROR_NOT_ENOUGH_MEMORY); return; }
    const auto abandon = [&] { CFRelease(sequence); };
    const u32 pointer_size = m.cpu_is_64bit() ? sizeof(u64) : sizeof(u32);
    for (u32 i = 0; i < count; ++i) {
        auto handle = m.read_ptr(profiles_at + i * pointer_size);
        if (handle.is_err()) { abandon(); (void)m.raise_access_violation(false, profiles_at + i * pointer_size); m.set_return(0); return; }
        auto profile = s.profiles.find(handle.value());
        if (profile == s.profiles.end()) { abandon(); bad_handle(m, s, handle.value()); return; }
        if (!verified_profile(*profile->second)) { abandon(); fail(m, icm::ERROR_INVALID_PROFILE); return; }
        const auto& bytes = profile->second->bytes;
        const u32 profile_class = big32(bytes.data() + icm::PROFILEHEADER_phClass_OFFSET32);
        if ((count > 1 && profile_class == icm::CLASS_LINK) || (count == 1 && profile_class != icm::CLASS_LINK)) {
            abandon(); fail(m, icm::ERROR_INVALID_PARAMETER); return;
        }
        const u32 intent_index = intent_count == 1 ? 0 : i;
        auto intent_value = m.read_u32(intents_at + intent_index * sizeof(u32));
        if (intent_value.is_err()) { abandon(); (void)m.raise_access_violation(false, intents_at + intent_index * sizeof(u32)); m.set_return(0); return; }
        if (!native_intent(intent_value.value())) { abandon(); fail(m, icm::ERROR_INVALID_PARAMETER); return; }
        const u32 intent = (flags & icm::USE_RELATIVE_COLORIMETRIC) ? icm::INTENT_RELATIVE_COLORIMETRIC :
            ((intent_count == 1 && i != 0) ? icm::INTENT_RELATIVE_COLORIMETRIC : intent_value.value());
        auto native = profile->second->native;
        bool added;
        if (count == 1) added = append_profile(sequence, native, intent, kColorSyncTransformDeviceToDevice);
        else if (i == 0) added = append_profile(sequence, native, intent, kColorSyncTransformDeviceToPCS);
        else if (i == count - 1) added = append_profile(sequence, native, intent, kColorSyncTransformPCSToDevice);
        else if (profile_class == icm::CLASS_ABSTRACT) added = append_profile(sequence, native, intent, kColorSyncTransformPCSToPCS);
        else added = append_profile(sequence, native, intent, kColorSyncTransformPCSToDevice) &&
            append_profile(sequence, native, intent, kColorSyncTransformDeviceToPCS);
        if (!added) { abandon(); fail(m, icm::ERROR_NOT_ENOUGH_MEMORY); return; }
        CFRetain(native); transform->retained_profiles.push_back(native);
        if (i == 0) transform->input = big32(bytes.data() + icm::PROFILEHEADER_phDataColorSpace_OFFSET32);
        if (i == count - 1) transform->output = big32(bytes.data() + (count == 1 ?
            icm::PROFILEHEADER_phConnectionSpace_OFFSET32 : icm::PROFILEHEADER_phDataColorSpace_OFFSET32));
    }
    CFStringRef quality = (flags & mode_mask) == icm::BEST_MODE ? kColorSyncBestQuality : kColorSyncNormalQuality;
    const void* key = kColorSyncConvertQuality; const void* value = quality;
    CFDictionaryRef options = CFDictionaryCreate(nullptr, &key, &value, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!options) { abandon(); fail(m, icm::ERROR_NOT_ENOUGH_MEMORY); return; }
    transform->native = ColorSyncTransformCreate(sequence, options);
    CFRelease(options); abandon();
    if (!transform->native) { fail(m, icm::ERROR_INVALID_TRANSFORM); return; }
    const GuestAddr handle = s.next++;
    s.transforms.emplace(handle, std::move(transform)); set_last_error(0); m.set_return(handle);
}
struct ColorFormat { u32 channels{}, bytes{}; ColorSyncDataDepth depth{kColorSync16BitInteger}; };
ColorFormat color_format(u32 type, u32 space) {
    switch (type) {
    case icm::COLOR_GRAY: return space == icm::SPACE_GRAY ? ColorFormat{1, sizeof(u16), kColorSync16BitInteger} : ColorFormat{};
    case icm::COLOR_RGB: return space == icm::SPACE_RGB ? ColorFormat{3, sizeof(u16), kColorSync16BitInteger} : ColorFormat{};
    case icm::COLOR_CMYK: return space == icm::SPACE_CMYK ? ColorFormat{4, sizeof(u16), kColorSync16BitInteger} : ColorFormat{};
    case icm::COLOR_3_CHANNEL:
        return space == icm::SPACE_RGB || space == icm::SPACE_CMY || space == icm::SPACE_3_CHANNEL ?
            ColorFormat{3, sizeof(u16), kColorSync16BitInteger} : ColorFormat{};
    case icm::COLOR_5_CHANNEL: case icm::COLOR_6_CHANNEL:
    case icm::COLOR_7_CHANNEL: case icm::COLOR_8_CHANNEL: {
        const u32 channels = type - icm::COLOR_5_CHANNEL + 5;
        constexpr u32 spaces[] = {icm::SPACE_5_CHANNEL, icm::SPACE_6_CHANNEL, icm::SPACE_7_CHANNEL, icm::SPACE_8_CHANNEL};
        return space == spaces[channels - 5] ? ColorFormat{channels, sizeof(u8), kColorSync8BitInteger} : ColorFormat{};
    }
    default: return {};
    }
}
void translate(Marshaler& m, State& s) {
    const GuestAddr handle = m.arg_ptr(0), input = m.arg_ptr(1), output = m.arg_ptr(4);
    const u32 count = m.arg32(2), input_type = m.arg32(3), output_type = m.arg32(5);
    auto found = s.transforms.find(handle);
    if (found == s.transforms.end()) { bad_handle(m, s, handle); return; }
    if (!input || !output || !count || input_type < icm::COLOR_GRAY || input_type > icm::COLOR_NAMED ||
        output_type < icm::COLOR_GRAY || output_type > icm::COLOR_NAMED) { fail(m, icm::ERROR_INVALID_PARAMETER); return; }
    if (input_type == icm::COLOR_NAMED || output_type == icm::COLOR_NAMED) {
        unsupported(m, s, "TranslateColors", "named-color index conversion"); return;
    }
    if ((input_type >= icm::COLOR_XYZ && input_type <= icm::COLOR_Lab) ||
        (output_type >= icm::COLOR_XYZ && output_type <= icm::COLOR_Lab)) {
        unsupported(m, s, "TranslateColors", "Windows XYZ/Lab/Yxy encoding is not calibrated to ColorSync"); return;
    }
    if (input_type == icm::COLOR_GRAY || output_type == icm::COLOR_GRAY) {
        unsupported(m, s, "TranslateColors", "Windows gray WORD precision is not calibrated to ColorSync"); return;
    }
    auto& t = *found->second;
    const auto generic_supported = [](u32 type, u32 space) {
        return type != icm::COLOR_3_CHANNEL || space == icm::SPACE_RGB ||
            space == icm::SPACE_CMY || space == icm::SPACE_3_CHANNEL;
    };
    if (!generic_supported(input_type, t.input) || !generic_supported(output_type, t.output)) {
        unsupported(m, s, "TranslateColors", "generic three-channel profile encoding is not calibrated"); return;
    }
    const ColorFormat in_format = color_format(input_type, t.input), out_format = color_format(output_type, t.output);
    if (!in_format.channels || !out_format.channels) { fail(m, icm::ERROR_INVALID_PARAMETER); return; }
    const usize stride = m.cpu_is_64bit() ? icm::COLOR_SIZE64 : icm::COLOR_SIZE32;
    if (count > resource_limit / stride) { fail(m, icm::ERROR_NOT_ENOUGH_MEMORY); return; }
    const usize in_size = in_format.channels * in_format.bytes, out_size = out_format.channels * out_format.bytes;
    std::vector<u8> records(static_cast<usize>(count) * stride);
    std::vector<u8> in(static_cast<usize>(count) * in_size), out(static_cast<usize>(count) * out_size);
    // Windows copies the full input COLOR record before changing its color
    // components, including x64's trailing pointer padding. Stage the entire
    // source array first so overlapping input/output arrays preserve it.
    if (!read(m, input, records.data(), records.size())) return;
    for (u32 i = 0; i < count; ++i)
        std::memcpy(in.data() + i * in_size, records.data() + i * stride, in_size);
    const ColorSyncDataLayout in_layout = static_cast<ColorSyncDataLayout>(kColorSyncAlphaNone) | (in_format.bytes == sizeof(u16) ? kColorSyncByteOrder16Little : kColorSyncByteOrderDefault);
    const ColorSyncDataLayout out_layout = static_cast<ColorSyncDataLayout>(kColorSyncAlphaNone) | (out_format.bytes == sizeof(u16) ? kColorSyncByteOrder16Little : kColorSyncByteOrderDefault);
    if (!ColorSyncTransformConvert(t.native, count, 1, out.data(), out_format.depth, out_layout, out.size(),
            in.data(), in_format.depth, in_layout, in.size(), nullptr)) { fail(m, icm::ERROR_INVALID_TRANSFORM); return; }
    for (u32 i = 0; i < count; ++i)
        std::memcpy(records.data() + i * stride, out.data() + i * out_size, out_size);
    if (write(m, output, records.data(), records.size())) m.set_return(1);
}
void directory(Marshaler& m, State& s) {
    if (m.arg_ptr(0)) { fail(m, icm::ERROR_INVALID_NAME); return; }
    const GuestAddr output = m.arg_ptr(1), size_at = m.arg_ptr(2);
    if (!size_at) { fail(m, icm::ERROR_INVALID_PARAMETER); return; }
    u32 capacity;
    if (!read(m, size_at, &capacity, sizeof capacity)) return;
    const usize chars = std::strlen(color_directory) + 1;
    const u32 needed = static_cast<u32>(chars * sizeof(u16));
    if (!write(m, size_at, &needed, sizeof needed)) return;
    if (!output || capacity < needed) { fail(m, icm::ERROR_INSUFFICIENT_BUFFER); return; }
    if (s.directory_error) { fail(m, s.directory_error); return; }
    if (!s.vfs) { unsupported(m, s, "GetColorDirectoryW", "capsule color directory requires VFS"); return; }
    {
        std::vector<u16> text(chars);
        std::transform(color_directory, color_directory + chars, text.begin(), [](char c) { return static_cast<u16>(c); });
        if (!write(m, output, text.data(), needed)) return;
    }
    m.set_return(1);
}
} // namespace
DllExports create_mscms_thunks(capsule::Vfs* vfs) {
    auto state = std::make_shared<State>(vfs);
    DllExports exports;
    const auto add = [&](const icm::ExportContract& contract, auto function) {
        exports.entries.push_back({contract.name, 0, [state, function](Marshaler& m) {
            std::lock_guard lock(state->mutex); function(m, *state);
        }, contract.stack_bytes});
    };
    add(icm::OpenColorProfileW, open_profile);
    add(icm::CloseColorProfile, close_profile);
    add(icm::IsColorProfileValid, is_valid);
    add(icm::GetColorProfileHeader, header);
    add(icm::CreateMultiProfileTransform, create_transform);
    add(icm::DeleteColorTransform, delete_transform);
    add(icm::TranslateColors, translate);
    add(icm::GetColorDirectoryW, directory);
    return exports;
}
} // namespace vitreo::winapi
