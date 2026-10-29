#pragma once

#include "checkpoint_format.hpp"

#include <string>
#include <vector>

namespace vitreo {
class GuestMemory;
namespace cpu {
struct CpuState;
}
}

namespace vitreo::checkpoint {

struct CaptureRequest {
    GuestArchitecture architecture{GuestArchitecture::X64};
    u32 guest_thread_id{1};
    DeterminismMetadata determinism;
    std::vector<ModuleMetadata> modules;
};

[[nodiscard]] bool capture(GuestMemory& memory,
                           const cpu::CpuState& cpu,
                           const CaptureRequest& request,
                           Checkpoint& checkpoint,
                           std::string& error);

/// Restores mapped pages and the sole captured CPU state. This first slice
/// rejects multi-thread checkpoints and checkpoints claiming complete handle
/// state; process-owned host resources remain under RuntimeContext ownership.
[[nodiscard]] bool restore(const Checkpoint& checkpoint,
                           GuestArchitecture expected_architecture,
                           GuestMemory& memory,
                           cpu::CpuState& cpu,
                           std::string& error);

} // namespace vitreo::checkpoint
