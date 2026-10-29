/// @file oleacc.cpp
/// @brief Microsoft Active Accessibility entry points.

#include "oleacc.hpp"

#include "windowing/model/hwnd_manager.hpp"
#include "winapi/core/marshaler.hpp"

#include <array>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace vitreo::winapi {

namespace {

constexpr u32 E_NOTIMPL = 0x80004001u;
constexpr u32 E_NOINTERFACE = 0x80004002u;
constexpr u32 E_INVALIDARG = 0x80070057u;
constexpr u32 E_FAIL = 0x80004005u;

struct GuestGuid {
    u32 data1;
    u16 data2;
    u16 data3;
    std::array<u8, 8> data4;

    bool operator==(const GuestGuid&) const = default;
};

static_assert(sizeof(GuestGuid) == 16);

constexpr GuestGuid IID_IUNKNOWN{
    0x00000000u, 0x0000u, 0x0000u,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GuestGuid IID_IDISPATCH{
    0x00020400u, 0x0000u, 0x0000u,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GuestGuid IID_IENUMVARIANT{
    0x00020404u, 0x0000u, 0x0000u,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GuestGuid IID_IACCESSIBLE{
    0x618736E0u, 0x3C3Du, 0x11CFu,
    {0x81, 0x0C, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

[[nodiscard]] bool read_guid(
    Marshaler& marshaler, GuestAddr address, GuestGuid& guid) {
    if (address == 0) return false;
    auto result = marshaler.read_struct<GuestGuid>(address);
    if (result.is_err()) return false;
    guid = result.value();
    return true;
}

[[nodiscard]] bool is_accessible_object_iid(const GuestGuid& iid) {
    return iid == IID_IUNKNOWN ||
           iid == IID_IDISPATCH ||
           iid == IID_IENUMVARIANT ||
           iid == IID_IACCESSIBLE;
}

void trace_not_implemented(std::string_view api) {
    static std::mutex reported_mutex;
    static std::set<std::string, std::less<>> reported;
    {
        std::lock_guard lock(reported_mutex);
        if (reported.emplace(api).second) {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED oleacc!%.*s: no MSAA proxy objects\n",
                         static_cast<int>(api.size()), api.data());
        }
    }
    VITREO_TRACE(
        "[oleacc] %.*s -> E_NOTIMPL "
        "(accessibility provider objects are not implemented)\n",
        static_cast<int>(api.size()), api.data());
}

void accessible_children(Marshaler& marshaler) {
    const GuestAddr container = marshaler.arg_ptr(0);
    const i32 child_start = static_cast<i32>(marshaler.arg(1));
    const i32 child_count = static_cast<i32>(marshaler.arg(2));
    const GuestAddr children = marshaler.arg_ptr(3);
    const GuestAddr obtained = marshaler.arg_ptr(4);

    if (obtained == 0 || marshaler.write_u32(obtained, 0).is_err() ||
        container == 0 || child_start < 0 || child_count < 0 ||
        (child_count != 0 && children == 0)) {
        marshaler.set_return(E_INVALIDARG);
        return;
    }

    trace_not_implemented("AccessibleChildren");
    marshaler.set_return(E_NOTIMPL);
}

void accessible_object_from_window(
    Marshaler& marshaler, windowing::HwndManager& hwnd_manager) {
    const auto hwnd =
        static_cast<windowing::HWND>(marshaler.arg(0));
    (void)marshaler.arg(1);
    const GuestAddr iid_address = marshaler.arg_ptr(2);
    const GuestAddr object_out = marshaler.arg_ptr(3);

    if (object_out == 0 ||
        marshaler.write_ptr(object_out, 0).is_err()) {
        marshaler.set_return(E_INVALIDARG);
        return;
    }
    if (!hwnd_manager.is_valid(hwnd)) {
        // Measured (s1win_dwm_probe "-- oleacc"): E_FAIL, *ppv NULL.
        marshaler.set_return(E_FAIL);
        return;
    }

    GuestGuid iid{};
    if (!read_guid(marshaler, iid_address, iid)) {
        marshaler.set_return(E_INVALIDARG);
        return;
    }
    if (!is_accessible_object_iid(iid)) {
        marshaler.set_return(E_NOINTERFACE);
        return;
    }

    trace_not_implemented("AccessibleObjectFromWindow");
    marshaler.set_return(E_NOTIMPL);
}

void create_std_accessible_object(
    Marshaler& marshaler, windowing::HwndManager& hwnd_manager) {
    const auto hwnd =
        static_cast<windowing::HWND>(marshaler.arg(0));
    (void)marshaler.arg(1);
    const GuestAddr iid_address = marshaler.arg_ptr(2);
    const GuestAddr object_out = marshaler.arg_ptr(3);

    if (object_out == 0 ||
        marshaler.write_ptr(object_out, 0).is_err()) {
        marshaler.set_return(E_INVALIDARG);
        return;
    }
    if (!hwnd_manager.is_valid(hwnd)) {
        // Measured (s1win_dwm_probe "-- oleacc"): E_FAIL, *ppv NULL.
        marshaler.set_return(E_FAIL);
        return;
    }

    GuestGuid iid{};
    if (!read_guid(marshaler, iid_address, iid)) {
        marshaler.set_return(E_INVALIDARG);
        return;
    }
    if (!is_accessible_object_iid(iid)) {
        marshaler.set_return(E_NOINTERFACE);
        return;
    }

    trace_not_implemented("CreateStdAccessibleObject");
    marshaler.set_return(E_NOTIMPL);
}

void lresult_from_object(Marshaler& marshaler) {
    const GuestAddr iid_address = marshaler.arg_ptr(0);
    (void)marshaler.arg(1);
    const GuestAddr object = marshaler.arg_ptr(2);

    GuestGuid iid{};
    if (object == 0 || !read_guid(marshaler, iid_address, iid)) {
        marshaler.set_return(E_INVALIDARG);
        return;
    }
    if (iid != IID_IACCESSIBLE) {
        marshaler.set_return(E_NOINTERFACE);
        return;
    }

    trace_not_implemented("LresultFromObject");
    marshaler.set_return(E_NOTIMPL);
}

void window_from_accessible_object(Marshaler& marshaler) {
    const GuestAddr accessible = marshaler.arg_ptr(0);
    const GuestAddr hwnd_out = marshaler.arg_ptr(1);

    if (hwnd_out == 0 ||
        marshaler.write_ptr(hwnd_out, 0).is_err() ||
        accessible == 0) {
        marshaler.set_return(E_INVALIDARG);
        return;
    }

    trace_not_implemented("WindowFromAccessibleObject");
    marshaler.set_return(E_NOTIMPL);
}

} // namespace

DllExports create_oleacc_thunks(windowing::HwndManager& hwnd_manager) {
    DllExports exports;
    exports.entries.reserve(5);

    exports.entries.push_back({
        "AccessibleChildren", 0,
        [](Marshaler& marshaler) {
            accessible_children(marshaler);
        },
        20});
    exports.entries.push_back({
        "AccessibleObjectFromWindow", 0,
        [&hwnd_manager](Marshaler& marshaler) {
            accessible_object_from_window(marshaler, hwnd_manager);
        },
        16});
    exports.entries.push_back({
        "CreateStdAccessibleObject", 0,
        [&hwnd_manager](Marshaler& marshaler) {
            create_std_accessible_object(marshaler, hwnd_manager);
        },
        16});
    exports.entries.push_back({
        "LresultFromObject", 0,
        [](Marshaler& marshaler) {
            lresult_from_object(marshaler);
        },
        12});
    exports.entries.push_back({
        "WindowFromAccessibleObject", 0,
        [](Marshaler& marshaler) {
            window_from_accessible_object(marshaler);
        },
        8});

    return exports;
}

} // namespace vitreo::winapi
