// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file backend_buffer.hpp
/// @brief Backend buffers and typed buffer views.

#include "graphics/contracts/buffer_write.hpp"
#include <vitreo/common.hpp>
#include <vector>

namespace vitreo::graphics {

struct BackendBufferDesc {
    usize byte_width{0};
    bool vertex_buffer{false};
    bool index_buffer{false};
    bool constant_buffer{false};
    bool shader_resource{false};
    bool cpu_writable{false};
    /// Unordered-access view: GPU can write to this buffer from compute or
    /// pixel shaders. D3D11 UAV buffers require explicit sync before read.
    bool unordered_access{false};
    /// Stream-output target: the GPU writes it, like a UAV.
    bool stream_output{false};
    /// Allocate the buffer in storage another device or process opens by
    /// BackendBuffer::shared_handle() (GraphicsBackendDevice::
    /// open_shared_resource): every opening then reads and writes the same
    /// memory. `shared_metadata` is stored with it, as for textures.
    bool shareable{false};
    std::vector<u8> shared_metadata;
    /// A tiled buffer (D3D11_RESOURCE_MISC_TILED): address space only, in
    /// BACKEND_TILE_BYTES tiles that GraphicsBackendDevice::
    /// update_tile_mappings points at tile-pool memory. Nothing is allocated
    /// for it, so `byte_width` is not bounded by the resource-size limit.
    bool tiled{false};
};

class BackendBuffer {
public:
    virtual ~BackendBuffer() = default;

    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual const BackendBufferDesc& desc() const noexcept = 0;
    /// Replace `length` bytes at `offset`, ordered against GPU work already
    /// recorded as `mode` says (BufferWrite). Every byte outside the range
    /// keeps its value. The default is the plain-write contract: never under
    /// work that may still read the buffer.
    [[nodiscard]] virtual bool update(
        usize offset, const void* data, usize length,
        BufferWrite mode = BufferWrite::Ordered) = 0;
    /// Copy `length` bytes at `offset` out of the buffer as the GPU left it.
    /// The caller first makes sure the GPU work that wrote it has completed
    /// (BackendDevice::wait_idle). False when the backend cannot read it back.
    [[nodiscard]] virtual bool read(usize /*offset*/, void* /*data*/,
                                    usize /*length*/) {
        return false;
    }
    /// The global id of a shareable buffer's storage (0: not shared).
    [[nodiscard]] virtual u64 shared_handle() const noexcept { return 0; }
    /// The creator's BackendBufferDesc::shared_metadata.
    [[nodiscard]] virtual const std::vector<u8>& shared_metadata()
        const noexcept {
        static const std::vector<u8> none;
        return none;
    }
};

/// A D3D typed buffer view (`Buffer<T>` / `RWBuffer<T>`) — a texture whose
/// texels live in a buffer's storage. Learn (Buffer): one element is one texel
/// of the *view's* DXGI format, and the shader reads it expanded to four
/// components, so the format decode belongs to the hardware and not to the
/// translated shader. It occupies a t# texture slot, never a buffer slot.
class BackendTextureBuffer {
public:
    virtual ~BackendTextureBuffer() = default;

    [[nodiscard]] virtual bool valid() const noexcept = 0;

    /// Elements between the start of the texture the backend binds for this
    /// view and the view's first element: 0 when the view's own first
    /// element can start a texture. Learn (D3D11_BUFFER_SRV /
    /// D3D11_BUFFER_UAV): FirstElement is any element, while a backend's
    /// linear textures may need an aligned base; such a view is bound from
    /// the aligned offset below it, spanning element_bias() + NumElements
    /// elements of the buffer's own storage. A shader reading or writing it
    /// adds the bias to every element index and takes it off the width it
    /// reports (DxbcTranslationOptions::typed_buffer_bias). Fixed for the
    /// view's lifetime.
    [[nodiscard]] virtual u32 element_bias() const noexcept { return 0; }
};

} // namespace vitreo::graphics
