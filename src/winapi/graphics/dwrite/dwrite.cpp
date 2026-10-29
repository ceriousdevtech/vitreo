// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/dwrite/dwrite.hpp"
#include "winapi/graphics/dwrite/dwrite_text_internal.hpp"

#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dwrite/font_backend.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace vitreo::winapi {
namespace dwrite_internal {

void dwrite_diag_printf(const char* format, ...) {
    std::fprintf(stderr, "[pid=%d] ", static_cast<int>(getpid()));
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}

bool dwrite_diag_enabled() {
    static const bool on = std::getenv("VITREO_DWRITE_DIAG") != nullptr;
    return on;
}

} // namespace dwrite_internal

namespace {

constexpr u32 DWRITE_FACTORY_TYPE_ISOLATED = 1;

struct Guid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
};
static_assert(sizeof(Guid) == 16);

constexpr Guid IID_IUNKNOWN{
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr Guid IID_IDWRITE_FACTORY{
    0xB859EE5Au, 0xD838u, 0x4B5Bu, {0xA2, 0xE8, 0x1A, 0xDC, 0x7D, 0x93, 0xDB, 0x48}};
constexpr Guid IID_IDWRITE_FACTORY1{
    0x30572F99u, 0xDAC6u, 0x41DBu, {0xA1, 0x6E, 0x04, 0x86, 0x30, 0x7E, 0x60, 0x6A}};
constexpr Guid IID_IDWRITE_FACTORY2{
    0x0439FC60u, 0xCA44u, 0x4994u, {0x8D, 0xEE, 0x3A, 0x9A, 0xF7, 0xB7, 0x32, 0xEC}};
constexpr Guid IID_IDWRITE_FACTORY3{
    0x9A1B41C3u, 0xD3BBu, 0x466Au, {0x87, 0xFC, 0xFE, 0x67, 0x55, 0x6A, 0x3B, 0x65}};
constexpr Guid IID_IDWRITE_FONT_COLLECTION{
    0xA84CEE02u, 0x3EEAu, 0x4EEEu, {0xA8, 0x27, 0x87, 0xC1, 0xA0, 0x2A, 0x0F, 0xCC}};
constexpr Guid IID_IDWRITE_FONT_FAMILY{
    0xDA20D8EFu, 0x812Au, 0x4C43u, {0x98, 0x02, 0x62, 0xEC, 0x4A, 0xBD, 0x7A, 0xDF}};
constexpr Guid IID_IDWRITE_FONT{
    0xACD16696u, 0x8C14u, 0x4F5Du, {0x87, 0x7E, 0xFE, 0x3F, 0xC1, 0xD3, 0x27, 0x37}};
constexpr Guid IID_IDWRITE_FONT_FACE{
    0x5F49804Du, 0x7024u, 0x4D43u, {0xBF, 0xA9, 0xD2, 0x59, 0x84, 0xF5, 0x38, 0x49}};
constexpr Guid IID_IDWRITE_FONT_FACE1{
    0xA71EFDB4u, 0x9FDBu, 0x4838u, {0xAD, 0x90, 0xCF, 0xC3, 0xBE, 0x8C, 0x3D, 0xAF}};
constexpr Guid IID_IDWRITE_FONT_FACE2{
    0xD8B768FFu, 0x64BCu, 0x4E66u, {0x98, 0x2B, 0xEC, 0x8E, 0x87, 0xF6, 0x93, 0xF7}};
constexpr Guid IID_IDWRITE_FONT_FACE3{
    0xD37D7598u, 0x09BEu, 0x4222u, {0xA2, 0x36, 0x20, 0x81, 0x34, 0x1C, 0xC1, 0xF2}};
constexpr Guid IID_IDWRITE_FONT_FACE4{
    0x27F2A904u, 0x4EB8u, 0x441Du, {0x96, 0x78, 0x05, 0x63, 0xF5, 0x3E, 0x3E, 0x2F}};
constexpr Guid IID_IDWRITE_FONT_FACE5{
    0x98EFF3A5u, 0xB667u, 0x479Au, {0xB1, 0x45, 0xE2, 0xFA, 0x5B, 0x9F, 0xDC, 0x29}};
constexpr Guid IID_IDWRITE_FONT_FACE6{
    0xC4B1FE1Bu, 0x6E84u, 0x47D5u, {0xB5, 0x4C, 0xA5, 0x97, 0x98, 0x1B, 0x06, 0xAD}};
constexpr Guid IID_IDWRITE_FONT_FACE7{
    0x3945B85Bu, 0xBC95u, 0x40F7u, {0xB7, 0x2C, 0x8B, 0x73, 0xBF, 0xC7, 0xE1, 0x3B}};
constexpr Guid IID_IDWRITE_FONT_FALLBACK{
    0xEFA008F9u, 0xF7A1u, 0x48BFu, {0xB0, 0x5C, 0xF2, 0x24, 0x71, 0x3C, 0xC0, 0xFF}};
constexpr Guid IID_IDWRITE_FONT_FALLBACK_BUILDER{
    0xFD882D06u, 0x8ABAu, 0x4FB8u, {0xB8, 0x49, 0x8B, 0xE8, 0xB7, 0x3E, 0x14, 0xDE}};
constexpr Guid IID_IDWRITE_FONT_SET{
    0x53585141u, 0xD9F8u, 0x4095u, {0x83, 0x21, 0xD7, 0x3C, 0xF6, 0xBD, 0x11, 0x6B}};
constexpr Guid IID_IDWRITE_FONT_DOWNLOAD_QUEUE{
    0xB71E6052u, 0x5AEAu, 0x4FA3u, {0x83, 0x2E, 0xF6, 0x0D, 0x43, 0x1F, 0x7E, 0x91}};
constexpr Guid IID_IDWRITE_NUMBER_SUBSTITUTION{
    0x14885CC9u, 0xBAB0u, 0x4F90u, {0xB6, 0xED, 0x5C, 0x36, 0x6A, 0x2C, 0xD0, 0x3D}};
constexpr Guid IID_IDWRITE_LOCALIZED_STRINGS{
    0x08256209u, 0x099Au, 0x4B34u, {0xB8, 0x6D, 0xC2, 0x2B, 0x11, 0x0E, 0x77, 0x71}};
constexpr Guid IID_IDWRITE_FONT_FILE{
    0x739D886Au, 0xCEF5u, 0x47DCu, {0x87, 0x69, 0x1A, 0x8B, 0x41, 0xBE, 0xBB, 0xB0}};
constexpr Guid IID_IDWRITE_FONT_FILE_LOADER{
    0x727CAD4Eu, 0xD6AFu, 0x4C9Eu, {0x8A, 0x08, 0xD6, 0x95, 0xB1, 0x1C, 0xAA, 0x49}};
constexpr Guid IID_IDWRITE_LOCAL_FONT_FILE_LOADER{
    0xB2D9F3ECu, 0xC9FEu, 0x4A11u, {0xA2, 0xEC, 0xD8, 0x62, 0x08, 0xF7, 0xC0, 0xA2}};
constexpr Guid IID_IDWRITE_FONT_FILE_STREAM{
    0x6D4865FEu, 0x0AB8u, 0x4D91u, {0x8F, 0x62, 0x5D, 0xD6, 0xBE, 0x34, 0xA3, 0xE0}};
constexpr Guid IID_IDWRITE_RENDERING_PARAMS{
    0x2F0DA53Au, 0x2ADDu, 0x47CDu, {0x82, 0xEE, 0xD9, 0xEC, 0x34, 0x68, 0x8E, 0x75}};
constexpr Guid IID_IDWRITE_TYPOGRAPHY{
    0x55F1112Bu, 0x1DC2u, 0x4B3Cu, {0x95, 0x41, 0xF4, 0x68, 0x94, 0xED, 0x85, 0xB6}};
constexpr Guid IID_IDWRITE_TEXT_ANALYZER{
    0xB7E6163Eu, 0x7F46u, 0x43B4u, {0x84, 0xB3, 0xE4, 0xE6, 0x24, 0x9C, 0x36, 0x5D}};
constexpr Guid IID_IDWRITE_GDI_INTEROP{
    0x1EDD9491u, 0x9853u, 0x4299u, {0x89, 0x8F, 0x64, 0x32, 0x98, 0x3B, 0x6F, 0x3A}};
constexpr Guid IID_IDWRITE_GLYPH_RUN_ANALYSIS{
    0x7D97DBF7u, 0xE085u, 0x42D4u, {0x81, 0xE3, 0x6A, 0x88, 0x3B, 0xDE, 0xD1, 0x18}};

bool guid_equal(const Guid& left, const Guid& right) {
    return std::memcmp(&left, &right, sizeof(Guid)) == 0;
}

/// Render an IID for logging. A COM negotiation failure is unreadable without
/// knowing which interface was actually asked for.
std::string format_iid(GuestMemory& memory, GuestAddr address) {
    Guid g{};
    if (address == 0 ||
        memory.read_bytes(address, reinterpret_cast<Byte*>(&g), sizeof(g))
            .is_err()) {
        return "<unreadable>";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  g.data1, g.data2, g.data3, g.data4[0], g.data4[1],
                  g.data4[2], g.data4[3], g.data4[4], g.data4[5], g.data4[6],
                  g.data4[7]);
    return buf;
}

} // namespace

DWriteManager::DWriteManager(
    GuestMemory& memory, bool is_64bit, capsule::Vfs* vfs,
    GdiManager* gdi)
    : m_memory(memory), m_is_64bit(is_64bit), m_vfs(vfs), m_gdi(gdi) {
    const char* reuse = std::getenv("VITREO_DWRITE_FONT_REUSE");
    m_font_reuse = reuse == nullptr || std::strcmp(reuse, "0") != 0;
}

void DWriteManager::set_method_thunks(MethodThunks thunks) {
    m_method_thunks = std::move(thunks);
}

void DWriteManager::invoke_serialized(
    const ApiHandler& handler, Marshaler& m) {
    std::lock_guard lock(m_api_mutex);
    handler(m);
}

GuestAddr DWriteManager::not_impl_thunk(u32 arg_count) const {
    if (arg_count >= m_method_thunks.not_impl_by_arg_count.size()) return 0;
    return m_method_thunks.not_impl_by_arg_count[arg_count];
}

GuestAddr DWriteManager::return_zero_thunk(u32 arg_count) const {
    if (arg_count >= m_method_thunks.return_zero_by_arg_count.size()) return 0;
    return m_method_thunks.return_zero_by_arg_count[arg_count];
}

bool DWriteManager::write_pointer(GuestAddr address, GuestAddr value) {
    if (m_is_64bit) {
        return m_memory.write_u64(address, value).is_ok();
    }
    return m_memory.write_u32(address, static_cast<u32>(value)).is_ok();
}

VResult<u64> DWriteManager::read_pointer(GuestAddr address) const {
    if (m_is_64bit) return m_memory.read_u64(address);
    auto value = m_memory.read_u32(address);
    if (value.is_err()) return VResult<u64>::err(value.error());
    return VResult<u64>::ok(value.value());
}

VResult<std::u16string> DWriteManager::read_utf16(
    GuestAddr address, usize length) const {
    if (length != 0 && address == 0) {
        return VResult<std::u16string>::err(Error::InvalidAddress);
    }
    std::u16string result(length, u'\0');
    for (usize index = 0; index < length; ++index) {
        auto unit = m_memory.read_u16(address + index * sizeof(u16));
        if (unit.is_err()) {
            return VResult<std::u16string>::err(unit.error());
        }
        result[index] = static_cast<char16_t>(unit.value());
    }
    return VResult<std::u16string>::ok(std::move(result));
}

VResult<std::u16string> DWriteManager::read_utf16_z(
    GuestAddr address, usize max_length) const {
    if (address == 0) {
        return VResult<std::u16string>::err(Error::InvalidAddress);
    }
    std::u16string result;
    result.reserve(std::min<usize>(max_length, 64));
    for (usize index = 0; index < max_length; ++index) {
        auto unit = m_memory.read_u16(address + index * sizeof(u16));
        if (unit.is_err()) {
            return VResult<std::u16string>::err(unit.error());
        }
        if (unit.value() == 0) {
            return VResult<std::u16string>::ok(std::move(result));
        }
        result.push_back(static_cast<char16_t>(unit.value()));
    }
    return VResult<std::u16string>::ok(std::move(result));
}

std::optional<u64> DWriteManager::call_guest_method(
    GuestAddr object, u32 slot, const u64* trailing_args,
    usize trailing_count) const {
    if (!m_guest_call || object == 0) return std::nullopt;
    const auto vtable = read_pointer(object);
    if (vtable.is_err() || vtable.value() == 0) return std::nullopt;
    const usize pointer_size = m_is_64bit ? 8 : 4;
    const auto function = read_pointer(
        vtable.value() + static_cast<GuestAddr>(slot) * pointer_size);
    if (function.is_err() || function.value() == 0) return std::nullopt;
    std::vector<u64> args;
    args.reserve(trailing_count + 1);
    args.push_back(object);
    if (trailing_count != 0) {
        args.insert(
            args.end(), trailing_args, trailing_args + trailing_count);
    }
    return m_guest_call(
        function.value(), args.data(), args.size(), m_is_64bit);
}

void DWriteManager::release_guest_object(GuestAddr object) const {
    (void)call_guest_method(object, 2, nullptr, 0);
}

bool DWriteManager::iid_is_supported(GuestAddr iid_address, ObjectType type) const {
    Guid iid{};
    if (iid_address == 0 ||
        m_memory.read_bytes(iid_address, reinterpret_cast<Byte*>(&iid), sizeof(iid)).is_err()) {
        return false;
    }
    if (guid_equal(iid, IID_IUNKNOWN)) return true;
    const auto is = [&](const std::array<u8, 16>& bytes) {
        return std::memcmp(&iid, bytes.data(), bytes.size()) == 0;
    };
    // IDWriteTextFormat1-3 / IDWriteTextLayout1-4 are not implemented, so
    // QueryInterface answers E_NOINTERFACE for them.
    if (type == ObjectType::TextFormat) return is(dwrite_text_contract::iid_IDWriteTextFormat);
    if (type == ObjectType::TextLayout) {
        return is(dwrite_text_contract::iid_IDWriteTextFormat) ||
               is(dwrite_text_contract::iid_IDWriteTextLayout);
    }
    if (type == ObjectType::TrimmingSign) return is(dwrite_text_contract::iid_IDWriteInlineObject);
    if (type == ObjectType::Factory) {
        return guid_equal(iid, IID_IDWRITE_FACTORY) ||
               guid_equal(iid, IID_IDWRITE_FACTORY1) ||
               guid_equal(iid, IID_IDWRITE_FACTORY2) ||
               guid_equal(iid, IID_IDWRITE_FACTORY3);
    }
    if (type == ObjectType::FontCollection) return guid_equal(iid, IID_IDWRITE_FONT_COLLECTION);
    if (type == ObjectType::FontFamily) return guid_equal(iid, IID_IDWRITE_FONT_FAMILY);
    if (type == ObjectType::Font) return guid_equal(iid, IID_IDWRITE_FONT);
    if (type == ObjectType::FontFace) {
        return guid_equal(iid, IID_IDWRITE_FONT_FACE) ||
               guid_equal(iid, IID_IDWRITE_FONT_FACE1) ||
               guid_equal(iid, IID_IDWRITE_FONT_FACE2) ||
               guid_equal(iid, IID_IDWRITE_FONT_FACE3) ||
               guid_equal(iid, IID_IDWRITE_FONT_FACE4) ||
               guid_equal(iid, IID_IDWRITE_FONT_FACE5) ||
               guid_equal(iid, IID_IDWRITE_FONT_FACE6) ||
               guid_equal(iid, IID_IDWRITE_FONT_FACE7);
    }
    if (type == ObjectType::FontFallback) {
        return guid_equal(iid, IID_IDWRITE_FONT_FALLBACK);
    }
    if (type == ObjectType::FontFallbackBuilder) {
        return guid_equal(iid, IID_IDWRITE_FONT_FALLBACK_BUILDER);
    }
    if (type == ObjectType::FontSet) {
        return guid_equal(iid, IID_IDWRITE_FONT_SET);
    }
    if (type == ObjectType::FontDownloadQueue) {
        return guid_equal(iid, IID_IDWRITE_FONT_DOWNLOAD_QUEUE);
    }
    if (type == ObjectType::NumberSubstitution) {
        return guid_equal(iid, IID_IDWRITE_NUMBER_SUBSTITUTION);
    }
    if (type == ObjectType::LocalizedStrings) {
        return guid_equal(iid, IID_IDWRITE_LOCALIZED_STRINGS);
    }
    if (type == ObjectType::FontFile) {
        return guid_equal(iid, IID_IDWRITE_FONT_FILE);
    }
    if (type == ObjectType::GdiInterop) {
        return guid_equal(iid, IID_IDWRITE_GDI_INTEROP);
    }
    if (type == ObjectType::GlyphRunAnalysis) {
        return guid_equal(iid, IID_IDWRITE_GLYPH_RUN_ANALYSIS);
    }
    if (type == ObjectType::FontFileStream) {
        return guid_equal(iid, IID_IDWRITE_FONT_FILE_STREAM);
    }
    if (type == ObjectType::RenderingParams) {
        return guid_equal(iid, IID_IDWRITE_RENDERING_PARAMS);
    }
    if (type == ObjectType::Typography) {
        return guid_equal(iid, IID_IDWRITE_TYPOGRAPHY);
    }
    if (type == ObjectType::TextAnalyzer) {
        return guid_equal(iid, IID_IDWRITE_TEXT_ANALYZER);
    }
    return guid_equal(iid, IID_IDWRITE_FONT_FILE_LOADER) ||
           guid_equal(iid, IID_IDWRITE_LOCAL_FONT_FILE_LOADER);
}

void DWriteManager::impl_DWriteCreateFactory(Marshaler& m) {
    const u32 factory_type = static_cast<u32>(m.arg(0));
    const GuestAddr iid = m.arg_ptr(1);
    const GuestAddr factory_out = m.arg_ptr(2);
    if (factory_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(factory_out, 0);
    if (factory_type > DWRITE_FACTORY_TYPE_ISOLATED || iid == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (!iid_is_supported(iid, ObjectType::Factory)) {
        m.set_return(E_NOINTERFACE);
        return;
    }

    const GuestAddr factory = create_factory();
    if (factory == 0 || m.write_ptr(factory_out, factory).is_err()) {
        if (dwrite_diag_enabled()) {
            dwrite_diag_printf(
                         "[dwrite-diag] DWriteCreateFactory -> DWRITE_E_FILEFORMAT factory=0x%llx\n",
                         static_cast<unsigned long long>(factory));
        }
        m.set_return(DWRITE_E_FILEFORMAT);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] DWriteCreateFactory -> S_OK factory=0x%llx\n",
                     static_cast<unsigned long long>(factory));
    }
    m.set_return(S_OK);
}

void DWriteManager::dump_vtable(const char* label, GuestAddr object,
                                GuestAddr vtable) const {
    if (!dwrite_diag_enabled()) return;
    const usize pointer_size = m_is_64bit ? 8 : 4;
    dwrite_diag_printf( "[dwrite-diag] %s object=0x%llx vtable=0x%llx\n",
                 label, static_cast<unsigned long long>(object),
                 static_cast<unsigned long long>(vtable));
    for (u32 slot = 0; slot < 20; ++slot) {
        u64 value = 0;
        const GuestAddr at = vtable + slot * pointer_size;
        if (m_is_64bit) {
            auto v = m_memory.read_u64(at);
            value = v.is_ok() ? v.value() : 0;
        } else {
            auto v = m_memory.read_u32(at);
            value = v.is_ok() ? v.value() : 0;
        }
        dwrite_diag_printf( "[dwrite-diag]   slot %2u @0x%llx = 0x%llx%s\n",
                     slot, static_cast<unsigned long long>(at),
                     static_cast<unsigned long long>(value),
                     value == 0 ? "   <-- NULL" : "");
    }
}

void DWriteManager::impl_QueryInterface(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr iid = m.arg_ptr(1);
    const GuestAddr object_out = m.arg_ptr(2);
    if (object_out == 0) {
        if (dwrite_diag_enabled()) {
            dwrite_diag_printf(
                         "[dwrite-diag] QueryInterface self=0x%llx iid=%s "
                         "-> E_POINTER (null out)\n",
                         static_cast<unsigned long long>(self),
                         format_iid(m_memory, iid).c_str());
        }
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(object_out, 0);
    auto object = m_objects.find(self);
    if (object == m_objects.end() || !iid_is_supported(iid, object->second.type)) {
        if (dwrite_diag_enabled()) {
            dwrite_diag_printf(
                         "[dwrite-diag] QueryInterface self=0x%llx iid=%s "
                         "-> E_NOINTERFACE (%s)\n",
                         static_cast<unsigned long long>(self),
                         format_iid(m_memory, iid).c_str(),
                         object == m_objects.end() ? "unknown object"
                                                   : "iid not supported");
        }
        m.set_return(E_NOINTERFACE);
        return;
    }
    const u32 before = object->second.ref_count;
    ++object->second.ref_count;
    (void)m.write_ptr(object_out, self);
    if (dwrite_diag_enabled()) {
        u64 vtable = 0;
        if (m_is_64bit) {
            auto v = m_memory.read_u64(self);
            vtable = v.is_ok() ? v.value() : 0;
        } else {
            auto v = m_memory.read_u32(self);
            vtable = v.is_ok() ? v.value() : 0;
        }
        dwrite_diag_printf(
                     "[dwrite-diag] QueryInterface self=0x%llx iid=%s -> S_OK "
                     "out=0x%llx vtable=0x%llx ref %u->%u type=%u\n",
                     static_cast<unsigned long long>(self),
                     format_iid(m_memory, iid).c_str(),
                     static_cast<unsigned long long>(self),
                     static_cast<unsigned long long>(vtable), before,
                     object->second.ref_count,
                     static_cast<unsigned>(object->second.type));
        dump_vtable("QueryInterface result",
                    self, static_cast<GuestAddr>(vtable));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_AddRef(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    if (object == m_objects.end()) {
        m.set_return(0);
        return;
    }
    m.set_return(++object->second.ref_count);
}

void DWriteManager::impl_Release(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    auto object = m_objects.find(self);
    if (object == m_objects.end()) {
        m.set_return(0);
        return;
    }
    const u32 ref_count = --object->second.ref_count;
    if (ref_count == 0) {
        const ObjectType type = object->second.type;
        const GuestAddr key_address = object->second.key_address;
        const usize key_mapping_size = object->second.key_mapping_size;
        const GuestAddr font_file_loader =
            object->second.font_file_loader;
        if (type == ObjectType::Factory) {
            const auto collection_loaders =
                object->second.collection_loaders;
            const auto file_loaders = object->second.file_loaders;
            for (const GuestAddr loader : collection_loaders) {
                release_guest_object(loader);
            }
            for (const GuestAddr loader : file_loaders) {
                release_guest_object(loader);
            }
        }
        if (self == m_system_collection) m_system_collection = 0;
        if (type == ObjectType::FontFile) {
            if (key_address != 0) {
                (void)m_memory.unmap_region(
                    key_address, key_mapping_size);
            }
            if (font_file_loader != 0) {
                release_guest_object(font_file_loader);
            } else if (m_local_loader != 0) {
                auto loader = m_objects.find(m_local_loader);
                if (loader != m_objects.end() &&
                    loader->second.ref_count > 1) {
                    --loader->second.ref_count;
                }
            }
        }
        if (type == ObjectType::FontFace) {
            for (auto table = m_font_tables.begin();
                 table != m_font_tables.end();) {
                if (table->second.face != self) {
                    ++table;
                    continue;
                }
                (void)m_memory.unmap_region(
                    table->first, table->second.mapped_size);
                table = m_font_tables.erase(table);
            }
        }
        if (type == ObjectType::FontFileStream) {
            if (object->second.stream_shared_mapping) {
                release_stream_mapping(object->second.stream_data_address);
            } else if (object->second.stream_data_address != 0) {
                (void)m_memory.unmap_region(
                    object->second.stream_data_address,
                    object->second.stream_mapping_size);
            }
        }
        if (self == m_local_loader) m_local_loader = 0;
        // Text objects hold COM references (collection, effects, inline
        // objects, trimming sign). Drop them after the object is gone:
        // releasing a guest object can re-enter this manager.
        const auto text = object->second.text;
        m_objects.erase(self);
        m_free_objects.push_back(self);
        if (text) {
            release_text_state(*text);
            if (text->sign_format) release_text_state(*text->sign_format);
        }
    }
    m.set_return(ref_count);
}

void DWriteManager::impl_NotImpl(Marshaler& m, u32 argument_count) {
    const GuestAddr self = m.arg_ptr(0);
    if (dwrite_diag_enabled()) {
        static std::atomic<u32> not_impl_logs{0};
        const u32 log_index =
            not_impl_logs.fetch_add(1, std::memory_order_relaxed);
        if (log_index < 64) {
            const auto object = m_objects.find(self);
            const char* type_name = "unknown";
            if (object != m_objects.end()) {
                switch (object->second.type) {
                case ObjectType::Factory: type_name = "Factory"; break;
                case ObjectType::FontCollection:
                    type_name = "FontCollection";
                    break;
                case ObjectType::FontFamily: type_name = "FontFamily"; break;
                case ObjectType::Font: type_name = "Font"; break;
                case ObjectType::FontFace: type_name = "FontFace"; break;
                case ObjectType::FontFallback: type_name = "FontFallback"; break;
                case ObjectType::FontFallbackBuilder:
                    type_name = "FontFallbackBuilder";
                    break;
                case ObjectType::FontSet: type_name = "FontSet"; break;
                case ObjectType::FontDownloadQueue:
                    type_name = "FontDownloadQueue";
                    break;
                case ObjectType::NumberSubstitution:
                    type_name = "NumberSubstitution";
                    break;
                case ObjectType::LocalizedStrings:
                    type_name = "LocalizedStrings";
                    break;
                case ObjectType::FontFile: type_name = "FontFile"; break;
                case ObjectType::LocalFontFileLoader:
                    type_name = "LocalFontFileLoader";
                    break;
                case ObjectType::FontFileStream:
                    type_name = "FontFileStream";
                    break;
                case ObjectType::RenderingParams:
                    type_name = "RenderingParams";
                    break;
                case ObjectType::Typography: type_name = "Typography"; break;
                case ObjectType::TextAnalyzer: type_name = "TextAnalyzer"; break;
                case ObjectType::GdiInterop: type_name = "GdiInterop"; break;
                case ObjectType::GlyphRunAnalysis:
                    type_name = "GlyphRunAnalysis";
                    break;
                }
            }
            // Name remaining hot DWrite gaps that share the generic not-impl
            // trampoline so the next contract is obvious from a diag run.
            const char* hint = "";
            if (object != m_objects.end() &&
                object->second.type == ObjectType::Factory &&
                argument_count == 2) {
                hint = " (Factory 1-arg: CreateFontSetBuilder)";
            }
            dwrite_diag_printf(
                         "[dwrite-diag] unsupported method self=0x%llx "
                         "type=%s args=%u -> E_NOTIMPL%s\n",
                         static_cast<unsigned long long>(self), type_name,
                         argument_count, hint);
        }
    }
    m.set_return(E_NOTIMPL);
}

void DWriteManager::impl_ReturnZero(Marshaler& m) {
    (void)m;
    m.set_return(0);
}

} // namespace vitreo::winapi
