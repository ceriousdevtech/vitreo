// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_raytracing.hpp
/// @brief BackendRaytracing on Metal: MTLAccelerationStructure objects, the
/// decode / gather / convert compute passes and the builds, each recorded on
/// the submit queue's timeline (metal_raytracing.mm).
///
/// Top levels use MTLAccelerationStructureInstanceDescriptorTypeIndirect
/// records, which name each bottom level by its MTLResourceID, so a top
/// level can be built from instance descriptions the GPU wrote. Metal needs
/// every bottom level a traced top level reaches to be resident; the
/// backend keeps one resident log of all live bottom levels, which every
/// draw or dispatch binding an acceleration structure declares
/// (metal_rt_bottom_level_token).

#include "graphics/contracts/backend_raytracing.hpp"
#include "graphics/metal/metal_submit_queue.hpp"

namespace vitreo::graphics {

class MetalAccelerationStructure final : public BackendAccelerationStructure {
public:
    MetalAccelerationStructure(void* retained, u64 size, bool top_level);
    ~MetalAccelerationStructure() override;
    [[nodiscard]] bool valid() const noexcept override { return m_handle != nullptr; }
    [[nodiscard]] bool top_level() const noexcept override { return m_top_level; }
    [[nodiscard]] u64 size() const noexcept override { return m_size; }
    /// id<MTLAccelerationStructure>.
    [[nodiscard]] void* native_handle() const noexcept { return m_handle; }
    /// gpuResourceID: what an argument-buffer slot or an instance record holds.
    [[nodiscard]] u64 resource_id() const noexcept { return m_resource_id; }

private:
    void* m_handle{nullptr};
    u64 m_size{0};
    u64 m_resource_id{0};
    bool m_top_level{false};
};

/// The process-wide Metal ray-tracing backend, or null when the device has
/// no ray tracing (MTLDevice.supportsRaytracing).
BackendRaytracing* metal_raytracing(void* device);

/// Every live bottom level, for a draw or dispatch that binds an
/// acceleration structure.
[[nodiscard]] MetalResidentToken metal_rt_bottom_level_token();

}  // namespace vitreo::graphics
