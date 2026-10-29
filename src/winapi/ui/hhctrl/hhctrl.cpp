#include "hhctrl.hpp"
#include "winapi/core/marshaler.hpp"
#include <cstdio>

namespace vitreo::winapi {

// HtmlHelpA / HtmlHelpW — return NULL (help not available)
static void stub_HtmlHelp(Marshaler& m) {
    VITREO_TRACE("[hhctrl] HtmlHelp: hwnd=0x%x file=0x%x cmd=0x%x -> NULL\n",
                 static_cast<u32>(m.arg(0)),
                 static_cast<u32>(m.arg(1)),
                 static_cast<u32>(m.arg(2)));
    m.set_return(0);
}

DllExports create_hhctrl_thunks() {
    DllExports exports;
    exports.entries.reserve(2);

    // HtmlHelpA — ordinal 14
    exports.entries.push_back(ExportEntry{
        "HtmlHelpA", 14,
        [](Marshaler& m) { stub_HtmlHelp(m); },
        16  // 4 args: hwnd, file, cmd, data
    });

    // HtmlHelpW — ordinal 15
    exports.entries.push_back(ExportEntry{
        "HtmlHelpW", 15,
        [](Marshaler& m) { stub_HtmlHelp(m); },
        16
    });

    return exports;
}

} // namespace vitreo::winapi
