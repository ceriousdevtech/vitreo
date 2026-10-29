// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_class_registry.hpp
/// @brief Capsule-scoped publication of live out-of-process COM class objects.

#pragma once

#include <vitreo/common.hpp>

#include <array>
#include <string>
#include <string_view>

namespace vitreo::ipc {

using ComClassId = std::array<u8, 16>;

struct ComClassSnapshot {
    u32 owner_pid{0};
    u32 cookie{0};
    u64 class_object{0};
    u32 context{0};
    u32 flags{0};
    u32 pointer_size{0};
};

/// Publishes one CoRegisterClassObject registration for this process. Each
/// instance owns exactly one class-table entry and removes it on destruction.
class ComClassRegistration {
public:
    ComClassRegistration(const std::string& capsule_root,
                         const ComClassId& clsid, u32 cookie,
                         u64 class_object, u32 context, u32 flags,
                         u32 pointer_size, bool suspended);
    ~ComClassRegistration();

    ComClassRegistration(const ComClassRegistration&) = delete;
    ComClassRegistration& operator=(const ComClassRegistration&) = delete;
    ComClassRegistration(ComClassRegistration&& other) noexcept;
    ComClassRegistration& operator=(ComClassRegistration&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return !m_path.empty(); }
    [[nodiscard]] bool set_suspended(bool suspended);

private:
    [[nodiscard]] bool write_record(bool suspended);

    std::string m_path;
    std::string m_temp_path;
    ComClassSnapshot m_snapshot;
};

/// Return one live, resumed registration matching the requested CLSCTX bits.
[[nodiscard]] bool find_com_class(const std::string& capsule_root,
                                  const ComClassId& clsid,
                                  u32 requested_context,
                                  ComClassSnapshot& result);

struct RotObjectSnapshot {
    u32 owner_pid{0};
    u32 cookie{0};
    u64 object{0};
    u32 pointer_size{0};
};

/// Publishes one Running Object Table entry in the capsule's interactive
/// session. The entry is removed when its local ROT registration is revoked.
class RotObjectRegistration {
public:
    RotObjectRegistration(const std::string& capsule_root,
                          std::string_view moniker, u32 cookie, u64 object,
                          u32 pointer_size);
    ~RotObjectRegistration();

    RotObjectRegistration(const RotObjectRegistration&) = delete;
    RotObjectRegistration& operator=(const RotObjectRegistration&) = delete;

    [[nodiscard]] bool valid() const noexcept { return !m_path.empty(); }

private:
    std::string m_path;
    std::string m_temp_path;
};

/// Find a live cross-process ROT object with the exact display name.
[[nodiscard]] bool find_rot_object(const std::string& capsule_root,
                                   std::string_view moniker,
                                   RotObjectSnapshot& result);

} // namespace vitreo::ipc
