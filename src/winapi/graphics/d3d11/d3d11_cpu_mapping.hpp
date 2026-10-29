// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d11_cpu_mapping.hpp
/// @brief What the CPU sees of a CPU-accessible texture: the layout Map
/// hands out (D3D11MappedImageLayout) and the storage behind it
/// (StagingBytes).
///
/// Split out of d3d11.hpp, which includes it.

#include <vitreo/common.hpp>
#include "winapi/graphics/d3d11/d3d11_standard_swizzle.hpp"

#include <cstring>
#include <utility>
#include <vector>

namespace vitreo::winapi {

/// What the CPU sees of one subresource of a CPU-accessible texture
/// through Map (d3d11_texmap_probe [pitch], [swz], [other]):
///  * Storage -- the shadow's own bytes (STAGING / DYNAMIC, UNDEFINED or
///    ROW_MAJOR layout): rows of the subresource's row_pitch;
///  * Pitched -- a DEFAULT texture's ROW_MAJOR mapping, and any DEFAULT
///    Texture1D's: rows padded to a multiple of 4 bytes;
///  * Swizzled -- 64K_STANDARD_SWIZZLE on a DEFAULT texture, and any
///    layout past ROW_MAJOR on a STAGING or DYNAMIC one
///    (d3d11_standard_swizzle.hpp);
///  * None -- a DEFAULT texture whose layout gives the CPU no pointer
///    (UNDEFINED and every value past 64K_STANDARD_SWIZZLE): Map takes
///    only a NULL pMappedResource.
struct D3D11MappedImageLayout {
    enum class Kind { None, Storage, Pitched, Swizzled } kind{Kind::None};
    u32 row_pitch{0};
    u32 depth_pitch{0};
    usize size{0};
    /// Pitched / Swizzled: the subresource in elements.
    u32 element_bytes{0};
    u32 elements_w{0};
    u32 elements_h{0};
    u32 depth{1};
    d3d11_swizzle::Layout swizzle{};
};

/// A CPU-mappable texture's storage (Texture2D::shadow): its own bytes, or
/// -- a shared STAGING resource -- the shared memory every opening of it, in
/// any process, maps (graphics::BackendSharedMemory). The vector-like
/// surface keeps every staging path unchanged; copy assignment copies bytes
/// into the destination's storage rather than rebinding it.
class StagingBytes {
public:
    StagingBytes() = default;
    StagingBytes(const StagingBytes& other)
        : m_owned(other.data(), other.data() + other.size()) {}
    StagingBytes(StagingBytes&&) noexcept = default;
    StagingBytes& operator=(StagingBytes&&) noexcept = default;
    StagingBytes& operator=(const StagingBytes& other) {
        if (this == &other) return *this;
        if (m_external != nullptr) {
            if (other.size() == m_external_size && other.size() != 0) {
                std::memcpy(m_external, other.data(), m_external_size);
            }
            return *this;
        }
        m_owned.assign(other.data(), other.data() + other.size());
        return *this;
    }
    StagingBytes& operator=(std::vector<u8>&& bytes) {
        m_external = nullptr;
        m_external_size = 0;
        m_owned = std::move(bytes);
        return *this;
    }
    [[nodiscard]] u8* data() noexcept {
        return m_external != nullptr ? m_external : m_owned.data();
    }
    [[nodiscard]] const u8* data() const noexcept {
        return m_external != nullptr ? m_external : m_owned.data();
    }
    [[nodiscard]] usize size() const noexcept {
        return m_external != nullptr ? m_external_size : m_owned.size();
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    /// Owned storage only: shared memory has the creator's fixed size.
    void resize(usize bytes) {
        if (m_external == nullptr) m_owned.resize(bytes);
    }
    /// Use @p bytes bytes of external memory from now on.
    void attach(u8* external, usize bytes) noexcept {
        m_owned.clear();
        m_owned.shrink_to_fit();
        m_external = external;
        m_external_size = bytes;
    }
    [[nodiscard]] bool external() const noexcept {
        return m_external != nullptr;
    }

private:
    std::vector<u8> m_owned;
    u8* m_external{nullptr};
    usize m_external_size{0};
};

} // namespace vitreo::winapi
