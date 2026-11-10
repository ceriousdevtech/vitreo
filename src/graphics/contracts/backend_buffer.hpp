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
    /// A window buffer: no storage of its own. Its bytes are a range of a
    /// CPU-visible heap's buffer (BackendHeap::buffer()) that the frontend
    /// lends it with BackendBuffer::set_window, and can move to another
    /// range at any time (a DISCARD rename done by the frontend). valid() is
    /// false until the first set_window. A backend that cannot lend windows
    /// returns a buffer whose set_window fails.
    bool window{false};
};

class BackendBuffer;

/// Bytes [offset, offset + byte_width) of a CPU-visible heap's buffer
/// (BackendHeap::buffer()), lent to a window buffer. `offset` is a multiple
/// of 256 (constant-buffer binding and linear-texture alignment).
struct BackendBufferWindow {
    BackendBuffer* storage{nullptr};
    usize offset{0};
};

/// Supplies fresh windows to a window buffer from inside the backend.
class BackendBufferWindowSource {
public:
    virtual ~BackendBufferWindowSource() = default;
    /// Called on the recording thread before the backend records a GPU write
    /// into @p buffer (a copy destination) while its window may still be
    /// read by recorded work. True: the source moved @p buffer to a fresh
    /// window no recorded work uses (set_window succeeded) and reports it in
    /// @p out; the backend then keeps every byte outside the write. False:
    /// nothing changed -- no fresh window to be had, or set_window refused
    /// it -- and the backend waits for the current window's work instead.
    /// The move is atomic: the source's own view of the buffer's bytes and
    /// the backend's window never disagree.
    [[nodiscard]] virtual bool replace_window(BackendBuffer& buffer,
                                              BackendBufferWindow& out) = 0;
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
    /// The device address of byte 0 of the buffer's current storage (0 when
    /// the backend has none). A shader that reads the bytes itself (a typed
    /// view read as raw memory, graphics::kTypedBufferRaw) uses it; only
    /// storage that is never renamed (heaps, reserved buffers) qualifies.
    [[nodiscard]] virtual u64 device_address() const noexcept { return 0; }
    /// The global id of a shareable buffer's storage (0: not shared).
    [[nodiscard]] virtual u64 shared_handle() const noexcept { return 0; }
    /// The creator's BackendBufferDesc::shared_metadata.
    [[nodiscard]] virtual const std::vector<u8>& shared_metadata()
        const noexcept {
        static const std::vector<u8> none;
        return none;
    }
    /// Window buffers (BackendBufferDesc::window): publish @p window as this
    /// buffer's storage for every GPU use recorded from now on; work
    /// recorded before keeps the storage it was recorded with. False: not a
    /// window buffer, or the range does not fit the window's storage.
    [[nodiscard]] virtual bool set_window(const BackendBufferWindow& window) {
        (void)window;
        return false;
    }
    /// Window buffers: who supplies a fresh window before a GPU write into
    /// a window still in use (null: the backend waits instead).
    virtual void set_window_source(BackendBufferWindowSource* source) noexcept {
        (void)source;
    }
    /// Recorded GPU work may still read or write the storage published now.
    /// Conservative default: true.
    [[nodiscard]] virtual bool storage_in_use() const noexcept { return true; }
    /// Block until no recorded GPU work uses the storage published now
    /// (submitting the recording that holds it first). False: the backend
    /// cannot wait (no completion tracking, or its queue shut down).
    [[nodiscard]] virtual bool wait_storage_idle() { return false; }
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
