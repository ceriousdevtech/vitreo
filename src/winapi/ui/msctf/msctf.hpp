#pragma once

#include "winapi/core/thunk_registry.hpp"

#include <optional>
#include <string>
#include <vector>

namespace vitreo::windowing {
class HwndManager;
}

namespace vitreo::winapi {

/// Association stored by SetInputScope / SetInputScopes / SetInputScopes2 /
/// SetInputScopeXML (Learn: inputscope.h). Absent means the window has no
/// scope, which is also the result of SetInputScope(hwnd, IS_DEFAULT).
struct StoredInputScope {
    std::vector<i32> scopes;
    std::vector<std::string> phrases;
    std::string regexp;
    std::string srgs;
    std::string xml;
    /// Guest IEnumString* from SetInputScopes2. Retained until replaced.
    /// Learn does not say the API AddRef's the pointer, and nothing here
    /// invokes it.
    u64 enum_string = 0;
};

[[nodiscard]] std::optional<StoredInputScope> stored_input_scope(u32 hwnd);

DllExports create_msctf_thunks(windowing::HwndManager* hwnd_mgr);

} // namespace vitreo::winapi
