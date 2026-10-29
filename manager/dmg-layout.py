#!/usr/bin/env python3
"""Write the Finder layout for the Vitreo disk image.

Icon positions are centers, in points, with the origin at the top-left of
the window. The background alias points at a single hidden picture at the
root of this volume. A picture inside a folder gets a broken path separator
and Finder draws a blank window. The bookmark record is omitted: macOS 26
ignores a background that includes it.
"""

import sys

from ds_store import DSStore
from mac_alias import Alias


def main():
    if len(sys.argv) != 11:
        sys.stderr.write(
            "usage: dmg-layout.py <mount> <background> <window-width> "
            "<window-height> <icon-size> <app-x> <app-y> "
            "<applications-x> <applications-y> <output>\n"
        )
        return 1

    mount = sys.argv[1]
    background = sys.argv[2]
    window_width = int(sys.argv[3])
    window_height = int(sys.argv[4])
    icon_size = float(sys.argv[5])
    app_x = int(sys.argv[6])
    app_y = int(sys.argv[7])
    applications_x = int(sys.argv[8])
    applications_y = int(sys.argv[9])
    output = sys.argv[10]
    del mount

    alias = Alias.for_file(background)
    if b"\x00" in alias.target.carbon_path:
        sys.stderr.write(
            "error: background alias path is not a single file at the volume root\n"
        )
        return 1

    window = "{{240, 120}, {%d, %d}}" % (window_width, window_height)
    bwsp = {
        "ShowStatusBar": False,
        "WindowBounds": window,
        "ContainerShowSidebar": False,
        "PreviewPaneVisibility": False,
        "SidebarWidth": 0,
        "ShowTabView": False,
        "ShowToolbar": False,
        "ShowPathbar": False,
        "ShowSidebar": False,
    }
    icvp = {
        "viewOptionsVersion": 1,
        "backgroundType": 2,
        "backgroundColorRed": 1.0,
        "backgroundColorGreen": 1.0,
        "backgroundColorBlue": 1.0,
        "backgroundImageAlias": alias.to_bytes(),
        "gridOffsetX": 0.0,
        "gridOffsetY": 0.0,
        "gridSpacing": 100.0,
        "arrangeBy": "none",
        "showIconPreview": True,
        "showItemInfo": False,
        "labelOnBottom": True,
        "textSize": 13.0,
        "iconSize": icon_size,
        "scrollPositionX": 0.0,
        "scrollPositionY": 0.0,
    }

    with DSStore.open(output, "w+") as store:
        store["."]["vSrn"] = ("long", 1)
        store["."]["bwsp"] = bwsp
        store["."]["icvl"] = (b"type", b"icnv")
        store["."]["icvp"] = icvp
        store["Vitreo.app"]["Iloc"] = (app_x, app_y)
        store["Applications"]["Iloc"] = (applications_x, applications_y)
    return 0


if __name__ == "__main__":
    sys.exit(main())
