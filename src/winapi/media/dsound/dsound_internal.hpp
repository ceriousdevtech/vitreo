#pragma once

/// @file dsound_internal.hpp
/// @brief Private declarations shared by the dsound*.cpp translation units.
///
/// PRIVATE: include from src/winapi/media/dsound/dsound*.cpp only. dsound.cpp
/// was one 2,900-line translation unit whose helpers, constants and GUIDs sat
/// in an anonymous namespace; they are here so the per-object files that were
/// split out of it (dsound_buffer.cpp, dsound_capture.cpp, ...) still see them.
/// The constants keep internal linkage, so each including TU has its own copy
/// exactly as before. Constants used by a single TU live in an anonymous
/// namespace at the top of that TU instead. The tiny helpers and the hot
/// DirectSoundManager lookups (write_ptr, find_*, signal, signal_crossed) are
/// `inline` here, so the per-call Lock/Unlock/GetCurrentPosition paths and the
/// notify and capture pumps still inline them across the TU boundary. The
/// trailing using-directive keeps the call sites unqualified.

#include "winapi/media/dsound/dsound.hpp"

#include "audio/contracts/audio_capture.hpp"
#include "audio/contracts/audio_renderer.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace vitreo::winapi {
namespace dsound_internal {

[[nodiscard]] inline bool dsound_diag_enabled() {
    static const bool enabled = std::getenv("VITREO_DSOUND_DIAG") != nullptr;
    return enabled;
}

inline void dsound_diag(const char* format, ...) {
    if (!dsound_diag_enabled()) return;
    std::fprintf(stderr, "[dsound-diag] ");
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}

inline void unimplemented_once(std::once_flag& once, const char* what) {
    std::call_once(once, [what] { std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s\n", what); });
}

/// sizeof(WAVEFORMATEX) without the trailing cbSize word.
constexpr u32 WAVEFORMAT_FIELD_BYTES = 16;
constexpr u32 WAVEFORMATEX_BYTES = 18;
constexpr u16 WAVE_FORMAT_PCM = 1;

struct Guid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
};

#include "winapi/media/dsound/dsound_capture_guids.inc"

constexpr Guid IID_IUNKNOWN{
    0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr Guid IID_ICLASSFACTORY{
    1, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr Guid IID_IDIRECTSOUND{
    0x279AFA83u, 0x4981, 0x11CE, {0xA5, 0x21, 0, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
constexpr Guid IID_IDIRECTSOUND8{
    0xC50A7E93u, 0xF395, 0x4834, {0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66}};
constexpr Guid IID_IDIRECTSOUNDBUFFER{
    0x279AFA85u, 0x4981, 0x11CE, {0xA5, 0x21, 0, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
constexpr Guid IID_IDIRECTSOUND3DLISTENER{
    0x279AFA84u, 0x4981, 0x11CE, {0xA5, 0x21, 0, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
constexpr Guid CLSID_DIRECTSOUND{
    0x47d4d946u, 0x62e8, 0x11cf, {0x93, 0xbc, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr Guid CLSID_DIRECTSOUND8{
    0x3901cc3fu, 0x84b5, 0x4fa4, {0xba, 0x35, 0xaa, 0x81, 0x72, 0xb8, 0xa0, 0x9b}};
// Stable identity for the runtime's real host playback renderer.
constexpr Guid PLAYBACK_DEVICE{0xb823b86b, 0x6c2d, 0x4bb7,
    {0x9a,0x18,0x7e,0x10,0x23,0xe6,0x5b,0x09}};
constexpr Guid GUID_NULL{0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};

inline bool guid_equal(const Guid& left, const Guid& right) {
    return std::memcmp(&left, &right, sizeof(Guid)) == 0;
}

inline bool guid_equal(const std::array<u8, 16>& left, const Guid& right) {
    return std::memcmp(left.data(), &right, sizeof(Guid)) == 0;
}

inline u32 float_bits(float value) {
    u32 bits = 0;
    static_assert(sizeof(float) == sizeof(u32));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline float bits_float(u32 bits) {
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace dsound_internal

using namespace dsound_internal;

inline bool DirectSoundManager::write_ptr(GuestAddr address, u64 value) const {
    if (!m_memory || address == 0) return false;
    return m_x64 ? m_memory->write_u64(address, value).is_ok()
                 : m_memory->write_u32(address, static_cast<u32>(value)).is_ok();
}

inline DirectSoundManager::Object* DirectSoundManager::find_object(GuestAddr object) {
    auto it = m_objects.find(object);
    return it == m_objects.end() ? nullptr : &it->second;
}

inline const DirectSoundManager::Object* DirectSoundManager::find_object(GuestAddr object) const {
    auto it = m_objects.find(object);
    return it == m_objects.end() ? nullptr : &it->second;
}

inline DirectSoundManager::BufferObject* DirectSoundManager::find_buffer(GuestAddr buffer) {
    auto it = m_buffers.find(buffer);
    if (it == m_buffers.end()) {
        auto facet = m_facets.find(buffer);
        if (facet == m_facets.end()) return nullptr;
        it = m_buffers.find(facet->second);
        if (it == m_buffers.end()) return nullptr;
    }
    return &it->second;
}

inline DirectSoundManager::ListenerObject* DirectSoundManager::find_listener(
    GuestAddr listener) {
    auto it = m_listeners.find(listener);
    return it == m_listeners.end() ? nullptr : &it->second;
}

inline DirectSoundManager::CaptureObject* DirectSoundManager::find_capture(GuestAddr object,
                                                                     GuestAddr* primary) {
    auto it = m_captures.find(object);
    if (it == m_captures.end()) {
        auto facet = m_facets.find(object);
        if (facet == m_facets.end()) return nullptr;
        it = m_captures.find(facet->second);
        if (it == m_captures.end()) return nullptr;
    }
    if (primary) *primary = it->first;
    return &it->second;
}

inline DirectSoundManager::CaptureBufferObject* DirectSoundManager::find_cbuffer(
    GuestAddr buffer, GuestAddr* primary) {
    auto it = m_cbuffers.find(buffer);
    if (it == m_cbuffers.end()) {
        auto facet = m_facets.find(buffer);
        if (facet == m_facets.end()) return nullptr;
        it = m_cbuffers.find(facet->second);
        if (it == m_cbuffers.end()) return nullptr;
    }
    if (primary) *primary = it->first;
    return &it->second;
}

inline void DirectSoundManager::signal(u64 handle) const {
    if (handle != 0 && m_signal) (void)m_signal(handle);
}

inline void DirectSoundManager::signal_crossed(const std::vector<NotifyPosition>& list, u32 from,
                                        u32 to) const {
    for (const auto& position : list) {
        if (position.offset == dsound::DSBPN_OFFSETSTOP) continue;
        if (position.offset >= from && position.offset < to) signal(position.event);
    }
}

} // namespace vitreo::winapi
