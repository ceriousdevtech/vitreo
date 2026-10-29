#pragma once

#include <vitreo/common.hpp>

#include <string>
#include <vector>

namespace vitreo::windowing {

struct Win32MenuItem {
    bool is_popup{false};
    bool is_separator{false};
    bool is_checked{false};
    bool is_grayed{false};
    u16 id{0};
    std::wstring text;
    std::vector<Win32MenuItem> children;
};

struct Win32MenuBar {
    std::vector<Win32MenuItem> items;
};

} // namespace vitreo::windowing
