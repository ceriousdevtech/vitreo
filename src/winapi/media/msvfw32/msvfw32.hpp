#pragma once

/// @file msvfw32.hpp
/// @brief msvfw32.dll — Video for Windows ICM (ICOpen / ICInfo / ICSendMessage)
///
/// Drivers32 (HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32)
/// maps vidc.<FOURCC> to a guest codec DLL. ICOpen LoadLibrary's that module
/// and looks up DriverProc, then sends DRV_LOAD/ENABLE/OPEN into the guest
/// DriverProc. ICM messages are forwarded; without a guest invoke they
/// return ICERR_UNSUPPORTED.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::capsule {
class Registry;
}

namespace vitreo::winapi {

class Marshaler;
class ModuleManager;

struct VfwDriver {
    u32 fcc_type{0};
    u32 fcc_handler{0};
    std::string dll_name;
};

class Msvfw32Manager {
public:
    Msvfw32Manager() = default;

    void set_module_manager(ModuleManager* modules) noexcept {
        modules_ = modules;
    }
    void set_registry(capsule::Registry* registry) noexcept {
        registry_ = registry;
    }

    /// Test / installer seeding of a Drivers32 vidc.<FOURCC> mapping.
    void add_driver(u32 fcc_type, u32 fcc_handler, std::string dll_name);

    /// Invokes a guest function pointer (DriverProc). Signature mirrors
    /// ddraw's run_guest_callback wrapper.
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;
    void set_guest_call(GuestCall call) { guest_call_ = std::move(call); }

    void impl_ICInfo(Marshaler& m);
    void impl_ICOpen(Marshaler& m);
    void impl_ICClose(Marshaler& m);
    void impl_ICSendMessage(Marshaler& m);
    void impl_ICLocate(Marshaler& m);
    void impl_DrawDibOpen(Marshaler& m);
    void impl_DrawDibClose(Marshaler& m);

private:
    struct OpenHic {
        VfwDriver driver;
        GuestAddr driver_proc{0};
        u64 dw_driver_id{0};
        std::string dll_name;
    };

    std::vector<VfwDriver> collect_drivers(u32 fcc_type);
    u32 open_hic(Marshaler& m, u32 fcc_type, u32 fcc_handler, u32 mode);
    bool write_icinfo(Marshaler& m, GuestAddr dest, const VfwDriver& driver);
    u64 call_driver_proc(const GuestCall& call, GuestAddr driver_proc,
                         u64 dw_driver_id, u32 hdrvr, u32 msg, u64 lparam1,
                         u64 lparam2, bool is_64bit) const;
    void abandon_open(u32 hic, const std::string& dll_name);

    ModuleManager* modules_{nullptr};
    capsule::Registry* registry_{nullptr};
    GuestCall guest_call_;
    std::mutex mutex_;
    std::vector<VfwDriver> seeded_;
    std::unordered_map<u32, OpenHic> open_;
    std::unordered_map<std::string, u32> dll_opens_;
    u32 next_hic_{0x56460001}; // 'VF' + counter
};

DllExports create_msvfw32_thunks(Msvfw32Manager& mgr);

} // namespace vitreo::winapi
