// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_backend_buffer.hpp
/// @brief Backend buffers and typed buffer views
///
/// Private to vitreo_graphics_metal, split out of metal_device.mm: included
/// only by that target's .mm files, after <Metal/Metal.h> (the layering
/// check keeps Apple framework imports out of headers).

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "graphics/metal/metal_backend_enums.hpp"
#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_shared_resource.hpp"
#include "graphics/metal/metal_tiled.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace vitreo::graphics {

class MetalBackendBuffer final : public BackendBuffer {
public:
    MetalBackendBuffer(void* device, BackendBufferDesc desc,
                       const void* initial_data)
        : m_desc(desc) {
        if (device == nullptr || desc.byte_width == 0) return;

        BufferUsage usage = BufferUsage::Storage;
        if (desc.constant_buffer) {
            usage = BufferUsage::Uniform;
        } else if (desc.index_buffer) {
            usage = BufferUsage::Index;
        } else if (desc.vertex_buffer) {
            usage = BufferUsage::Vertex;
        }
        // UAV buffers remain Storage; Metal doesn't require special usage bits
        // for unordered access - texture/buffer arguments default to read_write.
        if (desc.shareable) {
            // A shared buffer's storage IS its store's memory, which every
            // opening in every process maps: no copies, only publication.
            SharedStoreRecord record = shared_record_for(desc);
            record.frontend = desc.shared_metadata;
            auto local = std::make_shared<MetalSharedLocal>(device,
                                                            std::move(record));
            if (!local->create_store()) return;
            adopt_store(device, std::move(local), usage, initial_data);
            return;
        }
        if (desc.tiled) {
            // A placement sparse buffer: address space whose 64 KiB pages
            // tile pools back (metal_tiled.hpp). Private storage, so updates
            // travel through a staging copy on the timeline (a pinned
            // MetalBuffer orders every update).
            if (initial_data != nullptr) return;
            void* sparse = MetalTiled::instance().new_sparse_buffer(
                desc.byte_width);
            if (sparse == nullptr) return;
            m_buffer = std::make_unique<MetalBuffer>(device, sparse,
                                                     desc.byte_width, usage);
            CFRelease(sparse);
            if (m_buffer->handle() == nullptr) m_buffer.reset();
            return;
        }
        m_buffer = std::make_unique<MetalBuffer>(
            device, desc.byte_width, usage, initial_data);
        if (m_buffer->handle() == nullptr) {
            m_buffer.reset();
        } else {
            m_buffer->set_gpu_written(desc.unordered_access ||
                                      desc.stream_output);
        }
    }

    /// A buffer whose storage is @p external (an id<MTLBuffer>, retained):
    /// a D3D12 heap's whole-heap buffer (MetalBackendHeap). Never renamed.
    MetalBackendBuffer(void* device, void* external, BackendBufferDesc desc)
        : m_desc(std::move(desc)) {
        m_buffer = std::make_unique<MetalBuffer>(
            device, external, m_desc.byte_width, BufferUsage::Storage);
        if (m_buffer->handle() == nullptr) m_buffer.reset();
    }

    ~MetalBackendBuffer() override {
        if (m_desc.tiled && m_buffer != nullptr) {
            MetalTiled::instance().forget(m_buffer->handle());
        }
    }

    bool valid() const noexcept override {
        // has_storage, not handle(): a draw asks this of every bound buffer,
        // and handle() first runs the out-of-line retired-snapshot check that
        // native_handle() repeats anyway.
        return m_buffer != nullptr && m_buffer->has_storage();
    }

    const BackendBufferDesc& desc() const noexcept override {
        return m_desc;
    }

    bool update(usize offset, const void* data, usize length,
                BufferWrite mode) override {
        if (!valid() || data == nullptr || offset > m_desc.byte_width ||
            length > m_desc.byte_width - offset) {
            return false;
        }
        m_buffer->update(data, offset, length, mode);
        return true;
    }

    // Shared storage: once the GPU is idle, the published allocation's
    // contents ARE the GPU's view of the buffer.
    bool read(usize offset, void* data, usize length) override {
        if (!valid() || data == nullptr || offset > m_desc.byte_width ||
            length > m_desc.byte_width - offset) {
            return false;
        }
        const u8* contents = static_cast<const u8*>(m_buffer->map());
        if (contents == nullptr) return false;
        std::memcpy(data, contents + offset, length);
        return true;
    }

    /// An opening of the shared buffer @p local (its store attached).
    MetalBackendBuffer(void* device, std::shared_ptr<MetalSharedLocal> local,
                       BackendBufferDesc desc)
        : m_desc(std::move(desc)) {
        BufferUsage usage = BufferUsage::Storage;
        if (m_desc.constant_buffer) {
            usage = BufferUsage::Uniform;
        } else if (m_desc.index_buffer) {
            usage = BufferUsage::Index;
        } else if (m_desc.vertex_buffer) {
            usage = BufferUsage::Vertex;
        }
        adopt_store(device, std::move(local), usage, nullptr);
    }

    [[nodiscard]] static SharedStoreRecord shared_record_for(
        const BackendBufferDesc& desc) {
        SharedStoreRecord record{};
        record.kind = SharedStoreKind::Buffer;
        record.width = static_cast<u32>(desc.byte_width);
        record.flags =
            (desc.vertex_buffer ? SharedStoreRecord::kVertexBuffer : 0u) |
            (desc.index_buffer ? SharedStoreRecord::kIndexBuffer : 0u) |
            (desc.constant_buffer ? SharedStoreRecord::kConstantBuffer : 0u) |
            (desc.shader_resource ? SharedStoreRecord::kShaderResource : 0u) |
            (desc.unordered_access ? SharedStoreRecord::kUnorderedAccess
                                   : 0u) |
            (desc.stream_output ? SharedStoreRecord::kStreamOutput : 0u) |
            (desc.cpu_writable ? SharedStoreRecord::kCpuWritable : 0u);
        return record;
    }

    [[nodiscard]] static BackendBufferDesc desc_from_record(
        const SharedStoreRecord& record) {
        BackendBufferDesc desc{};
        desc.byte_width = record.width;
        desc.vertex_buffer =
            (record.flags & SharedStoreRecord::kVertexBuffer) != 0;
        desc.index_buffer =
            (record.flags & SharedStoreRecord::kIndexBuffer) != 0;
        desc.constant_buffer =
            (record.flags & SharedStoreRecord::kConstantBuffer) != 0;
        desc.shader_resource =
            (record.flags & SharedStoreRecord::kShaderResource) != 0;
        desc.unordered_access =
            (record.flags & SharedStoreRecord::kUnorderedAccess) != 0;
        desc.stream_output =
            (record.flags & SharedStoreRecord::kStreamOutput) != 0;
        desc.cpu_writable =
            (record.flags & SharedStoreRecord::kCpuWritable) != 0;
        desc.shareable = true;
        desc.shared_metadata = record.frontend;
        return desc;
    }

    [[nodiscard]] const std::shared_ptr<MetalSharedLocal>& shared_local()
        const noexcept {
        return m_shared;
    }

    u64 shared_handle() const noexcept override {
        if (!m_shared) return 0u;
        const auto store = m_shared->store();
        return store ? store->id() : 0u;
    }
    const std::vector<u8>& shared_metadata() const noexcept override {
        return m_desc.shared_metadata;
    }

    [[nodiscard]] void* native_handle() const noexcept {
        return m_buffer != nullptr ? m_buffer->handle() : nullptr;
    }
    [[nodiscard]] usize native_offset() const noexcept {
        return m_buffer != nullptr ? m_buffer->gpu_offset() : 0;
    }
    void note_gpu_bind() const {
        if (m_buffer) m_buffer->note_gpu_bind();
    }
    /// delta-record: MetalBuffer::note_gpu_bind_once.
    void note_gpu_bind_once(u64 gen) const {
        if (m_buffer) m_buffer->note_gpu_bind_once(gen);
    }
    /// Before recording a GPU write (copy destination): see
    /// MetalBuffer::note_gpu_write. Must precede native_handle()/offset.
    void note_gpu_write() {
        if (m_buffer) m_buffer->note_gpu_write();
    }

private:
    void adopt_store(void* device, std::shared_ptr<MetalSharedLocal> local,
                     BufferUsage usage, const void* initial_data) {
        const auto store = local->store();
        if (!store) return;
        m_buffer = std::make_unique<MetalBuffer>(
            device, store->payload_buffer(), m_desc.byte_width, usage,
            initial_data);
        if (m_buffer->handle() == nullptr) {
            m_buffer.reset();
            return;
        }
        MetalSharedRegistry::instance().add(local, store->payload_buffer());
        m_shared = std::move(local);
        m_desc.shareable = true;
        m_desc.shared_metadata = m_shared->record().frontend;
        if (initial_data != nullptr) {
            // The creator's initial contents are a write other processes
            // see after the next publication.
            MetalSharedRegistry::instance().note_written_buffer(
                store->payload_buffer());
        }
    }

    BackendBufferDesc m_desc;
    std::unique_ptr<MetalBuffer> m_buffer;
    std::shared_ptr<MetalSharedLocal> m_shared;
};

[[nodiscard]] void* metal_heap_handle(const BackendHeap* heap);

/// A D3D typed buffer view. Metal's texture-buffer type aliases an MTLBuffer's
/// storage as a 1D texture of a chosen pixel format, so the sampler hardware
/// performs the format decode the shader cannot do for itself.
///
/// Learn (D3D11_BUFFER_SRV / D3D11_BUFFER_UAV): FirstElement is any element,
/// so the view's byte offset is only a multiple of the element size, while a
/// linear texture's base must be a multiple of the device's
/// minimumLinearTextureAlignmentForPixelFormat. The alias therefore starts at
/// the largest aligned offset at or below the view, element_bias() elements
/// in front of FirstElement, and is element_bias() + NumElements texels wide:
/// it ends exactly where the view ends, so an index past the view is past the
/// texture. The translated shader adds the bias to every element index and
/// takes it off the width it reports (DxbcTranslationOptions::
/// typed_buffer_bias). The alias is the buffer's own storage -- never a copy
/// -- so every draw and dispatch reads the contents every earlier command in
/// the stream left there, and a UAV's writes land in the resource.
///
/// The bias depends only on FirstElement and the format: MetalBuffer
/// publishes storage at offsets that are multiples of 256 (kDiscardAlign),
/// which every linear-texture alignment divides. A rename to an offset that
/// is not (none exists today) fails the view loudly rather than shifting it.
///
/// `MetalBuffer` may rename its allocation, so the alias is rebuilt whenever
/// the source handle or GPU offset moves. The view does not own the buffer;
/// the guest resource that created it holds the reference that keeps it alive.
class MetalBackendTextureBuffer final : public BackendTextureBuffer {
public:
    MetalBackendTextureBuffer(void* device, const MetalBackendBuffer& buffer,
                              PixelFormat pixel_format, unsigned first_element,
                              unsigned num_elements, bool shader_write,
                              u16 component_mapping = 0)
        : m_component_mapping(component_mapping),
          m_device(device),
          m_buffer(&buffer),
          m_pixel_format(
              static_cast<MTLPixelFormat>(
                  format_table(pixel_format).mtl_pixel_format)),
          m_texel_bytes(format_table(pixel_format).bytes_per_pixel),
          m_first_element(first_element),
          m_num_elements(num_elements),
          m_shader_write(shader_write) {
        if (m_device != nullptr && m_pixel_format != MTLPixelFormatInvalid &&
            m_texel_bytes != 0) {
            m_alignment = static_cast<usize>(
                [(__bridge id<MTLDevice>)m_device
                    minimumLinearTextureAlignmentForPixelFormat:
                        m_pixel_format]);
            const usize misalign =
                m_alignment > 1u
                    ? (static_cast<usize>(m_first_element) * m_texel_bytes) %
                          m_alignment
                    : 0u;
            if (misalign % m_texel_bytes != 0) {
                // An alignment that is not a multiple of the texel: no
                // aligned base is a whole number of elements away.
                report_unservable("the alignment is not a whole number of "
                                  "elements");
                return;
            }
            m_element_bias = static_cast<u32>(misalign / m_texel_bytes);
        }
        (void)rebuild();
    }

    ~MetalBackendTextureBuffer() override { release_texture(); }

    bool valid() const noexcept override { return m_texture != nullptr; }

    u32 element_bias() const noexcept override { return m_element_bias; }

    /// Current alias, rebuilt if the buffer moved underneath it. Called
    /// while a draw or dispatch is being built, immediately before it is
    /// recorded.
    [[nodiscard]] void* native_shader_handle() {
        if (m_buffer == nullptr) return nullptr;
        if (m_texture == nullptr ||
            m_buffer->native_handle() != m_source_handle ||
            m_buffer->native_offset() != m_source_offset) {
            if (!rebuild()) return nullptr;
        }
        return m_texture;
    }

    /// The alias reads and (for a UAV) writes the buffer's own storage, so a
    /// bind is a GPU use of that buffer: Map and discard renaming must see it.
    void note_gpu_bind() const {
        if (m_buffer != nullptr) m_buffer->note_gpu_bind();
    }
    /// delta-record: MetalBuffer::note_gpu_bind_once.
    void note_gpu_bind_once(u64 gen) const {
        if (m_buffer != nullptr) m_buffer->note_gpu_bind_once(gen);
    }

    /// The MTLBuffer the alias texture was made from. Valid after
    /// native_shader_handle(). Every encoder binding the alias must also
    /// declare this buffer: Metal tracks hazards per MTLResource, and a
    /// buffer-backed texture is not the buffer, so a compute write through
    /// the alias and a later blit reading the buffer were left unordered
    /// (d3d11_a10_dispatch_readback_probe: a RWBuffer<uint> DispatchIndirect
    /// result copied to STAGING came back before the dispatch had run, 3/20
    /// vs Windows' 20/20).
    [[nodiscard]] void* storage_handle() const noexcept {
        return m_source_handle;
    }

private:
    void release_texture() {
        if (m_texture != nullptr) {
            CFRelease(m_texture);
            m_texture = nullptr;
        }
    }

    void report_unservable(const char* why) const {
        static std::atomic<u32> s_reports{0};
        if (s_reports.fetch_add(1) < 8u) {
            std::fprintf(stderr,
                         "[metal] typed buffer view fmt %lu first=%u n=%u "
                         "write=%d cannot be aliased: %s (align %zu)\n",
                         static_cast<unsigned long>(m_pixel_format),
                         m_first_element, m_num_elements,
                         m_shader_write ? 1 : 0, why, m_alignment);
        }
    }

    bool rebuild() {
        release_texture();
        if (m_device == nullptr || m_buffer == nullptr ||
            m_pixel_format == MTLPixelFormatInvalid || m_texel_bytes == 0 ||
            m_num_elements == 0) {
            return false;
        }
        void* buffer_handle = m_buffer->native_handle();
        if (buffer_handle == nullptr) return false;
        m_source_handle = buffer_handle;
        m_source_offset = m_buffer->native_offset();

        id<MTLBuffer> mtl_buffer = (__bridge id<MTLBuffer>)buffer_handle;
        // The alias: element_bias() elements in front of the view, then the
        // view's own elements.
        const usize texels =
            static_cast<usize>(m_element_bias) + m_num_elements;
        const usize length = texels * m_texel_bytes;
        const usize base =
            m_source_offset +
            (static_cast<usize>(m_first_element) - m_element_bias) *
                m_texel_bytes;
        if (base + length > [mtl_buffer length]) return false;
        if (m_alignment > 1u && base % m_alignment != 0) {
            report_unservable("its storage moved to an offset the alignment "
                              "does not divide");
            return false;
        }
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            textureBufferDescriptorWithPixelFormat:m_pixel_format
                                             width:texels
                                   resourceOptions:[mtl_buffer resourceOptions]
                                             usage:m_shader_write
                                                       ? (MTLTextureUsageShaderRead |
                                                          MTLTextureUsageShaderWrite)
                                                       : MTLTextureUsageShaderRead];
        if (td == nil) return false;
        if (m_component_mapping != 0) {
            td.swizzle = metal_component_mapping(
                MTLTextureSwizzleChannelsDefault, m_component_mapping);
        }
        // A texture buffer is one row, so bytesPerRow is the whole span.
        id<MTLTexture> texture = [mtl_buffer newTextureWithDescriptor:td
                                                               offset:base
                                                          bytesPerRow:length];
        if (texture == nil) return false;
        m_texture = (__bridge_retained void*)texture;
        return true;
    }

    u16 m_component_mapping{0};
    void* m_device{nullptr};
    const MetalBackendBuffer* m_buffer{nullptr};
    MTLPixelFormat m_pixel_format{MTLPixelFormatInvalid};
    unsigned m_texel_bytes{0};
    unsigned m_first_element{0};
    unsigned m_num_elements{0};
    bool m_shader_write{false};
    usize m_alignment{0};
    u32 m_element_bias{0};
    void* m_texture{nullptr};
    void* m_source_handle{nullptr};
    usize m_source_offset{0};
};

} // namespace vitreo::graphics
