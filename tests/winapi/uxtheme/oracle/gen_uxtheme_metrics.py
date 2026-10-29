#!/usr/bin/env python3
"""Generate src/winapi/ui/uxtheme/theme_aero_metrics.inc from the oracle probe.

Usage: gen_uxtheme_metrics.py tests/oracle_data/uxtheme_aero_probe64.expected > theme_aero_metrics.inc

The input is tools/oracle_probe/uxtheme_aero_probe.c run on Windows 11's Aero
theme (winoracle, interactive desktop). It records behaviour only: part
sizes, content/sizing margins, and the int/bool/enum/margins/position/font/
int-list properties apps lay out against. No colours, pixels, image files or
strings are recorded, so none are emitted: uxtheme draws its own (macOS)
artwork and derives every colour from AppKit.
"""
import re
import sys

BASE = {"BUTTON": "Button", "EDIT": "Edit", "COMBOBOX": "Combobox", "SCROLLBAR": "Scrollbar", "SPIN": "Spin",
        "TAB": "Tab", "PROGRESS": "Progress", "TRACKBAR": "Trackbar", "TOOLBAR": "Toolbar", "REBAR": "Rebar",
        "HEADER": "Header", "LISTVIEW": "Listview", "TREEVIEW": "Treeview", "MENU": "Menu", "TOOLTIP": "Tooltip",
        "STATUS": "Status", "WINDOW": "Window"}
TYPES = {"enum": "Enum", "string": "String", "int": "Int", "bool": "Bool", "color": "Color", "margins": "Margins",
         "filename": "Filename", "size": "Size", "position": "Position", "rect": "Rect", "font": "Font",
         "intlist": "IntList", "bitmap": "Bitmap"}


def nums(s):
    return [int(x) for x in s.split(",")]


def classify_draw(f, tr):
    """TS_DRAW as Windows answers it for prc = {0,0,100,30} and {0,0,8,8}."""
    if not f["drawA"].startswith("00000000"):
        return "Invalid"
    da, db = nums(f["drawA"][9:]), nums(f["drawB"][9:])
    if tr == [0, 0] and da == [0, 0]:
        return "Zero"
    if da == [100, 30]:
        return "Stretch"
    if db == tr:
        return "TrueFixed"
    if tr[0] == tr[1] or db == [min(tr[0], 8), min(tr[1], 8)]:
        # A square image, or one each side of which is clamped on its own.
        if tr[0] != tr[1]:
            return "TrueClamp"
    scale = min(8 / tr[0], 8 / tr[1])
    if db == [int(tr[0] * scale), int(tr[1] * scale)]:
        return "TrueUniform"
    if db == [min(tr[0], 8), min(tr[1], 8)]:
        return "TrueClamp"
    sys.exit("unexplained TS_DRAW: %s %s %s" % (tr, da, db))


def main():
    variants = {}
    order = []
    dpi_sizes = []
    fonts = []
    intlists = []
    for line in open(sys.argv[1], encoding="ascii"):
        w = line.split()
        if not w:
            continue
        if w[0] == "class" and len(w) >= 2 and "open=null" not in line:
            name = w[1]
            order.append(name)
            variants[name] = {"ps": [], "props": []}
        elif w[0] == "ps":
            name, p, s = w[1], int(w[2]), int(w[3])
            f = dict(re.findall(r"(\w+)=(\S+)", line))
            flags = (1 if f["def"] == "1" else 0) | (2 if f["pt"] == "1" else 0)
            if f["rgn"].startswith("8000ffff"):
                flags |= 4
            mc = [0, 0, 0, 0]
            ms = [0, 0, 0, 0]
            if f["margc"].startswith("00000000"):
                flags |= 8
                mc = nums(f["margc"][9:])
            if f["margs"].startswith("00000000"):
                flags |= 16
                ms = nums(f["margs"][9:])
            mn = nums(f["min"][9:])
            tr = nums(f["true"][9:])
            draw = classify_draw(f, tr)
            c = nums(f["content"][9:])
            content = [c[0], c[1], 100 - c[2], 30 - c[3]]
            variants[name]["ps"].append((p, s, flags, draw, mn, tr, content, mc, ms))
        elif w[0] == "dpi" and "open=null" not in line:
            # TS_DRAW of a DPI-scaled true-size image at 120/144/192 DPI: Aero
            # selects a per-DPI image, so the size is measured, not computed.
            name, dpi, p = w[1], int(w[2]), int(w[3])
            f = dict(re.findall(r"(\w+)=(\S+)", line))
            if f["drawA"].startswith("00000000") and f["drawA"][9:] != f["true"][9:]:
                da = nums(f["drawA"][9:])
                if da[0] != 100 and da[1] != 30:  # not clamped to the probe's 100x30 rectangle
                    dpi_sizes.append((name, p, dpi, da))
        elif w[0] == "prop":
            name, p, s, pid = w[1], int(w[2]), int(w[3]), int(w[4])
            f = dict(re.findall(r"(\w+)=(\S+)", line))
            o = int(f["o"])
            t = f["t"]
            v = [0, 0, 0, 0]
            extra = 0
            if t in ("int", "bool", "enum"):
                v[0] = int(f[t])
            elif t in ("margins", "rect"):
                v = nums(f[t])
            elif t == "position":
                v[:2] = nums(f["pos"])
            elif t == "font":
                parts = f["font"].split(",", 5)
                fonts.append((int(parts[0]), int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]),
                              parts[5].replace("_", " ")))
                extra = len(fonts) - 1
                v[0] = int(parts[0])
            elif t == "intlist":
                _, vals = f["intlist"].split(":")
                vals = nums(vals) if vals else []
                extra = len(intlists)
                intlists.append(len(vals))
                intlists.extend(vals)
                v[0] = len(vals)
            elif t in ("color", "bitmap"):
                pass
            else:
                sys.exit("unexpected property type " + t)
            part = p if o <= 1 else -1
            state = s if o == 0 else -1
            variants[name]["props"].append((pid, part, state, TYPES[t], v, extra))

    out = ["// Generated by tests/winapi/uxtheme/oracle/gen_uxtheme_metrics.py from",
           "// tests/oracle_data/uxtheme_aero_probe64.expected (Windows 11 Aero, measured).",
           "// Behaviour only: sizes, margins and layout properties. Do not edit.", ""]
    out.append("inline constexpr PartStateMetrics kPartStates[] = {")
    ps_index = {}
    n = 0
    for name in order:
        ps_index[name] = (n, len(variants[name]["ps"]))
        for (p, s, flags, draw, mn, tr, content, mc, ms) in variants[name]["ps"]:
            out.append("    {%d, %d, 0x%02x, DrawKind::%s, %d, %d, %d, %d, {%d, %d, %d, %d}, {%d, %d, %d, %d}, "
                       "{%d, %d, %d, %d}}," % (p, s, flags, draw, mn[0], mn[1], tr[0], tr[1], *content, *mc, *ms))
            n += 1
    out.append("};")
    out.append("inline constexpr PropertyRecord kProperties[] = {")
    pr_index = {}
    n = 0
    for name in order:
        pr_index[name] = (n, len(variants[name]["props"]))
        for (pid, part, state, t, v, extra) in variants[name]["props"]:
            out.append("    {%d, %d, %d, PropType::%s, {%d, %d, %d, %d}, %d}," % (pid, part, state, t, *v, extra))
            n += 1
    out.append("};")
    out.append("inline constexpr ThemeFontRecord kFonts[] = {")
    for (h, wd, wt, it, cs, face) in fonts:
        out.append('    {%d, %d, %d, %d, %d, u"%s"},' % (h, wd, wt, it, cs, face))
    out.append("};")
    out.append("inline constexpr i32 kIntLists[] = {")
    for i in range(0, len(intlists), 16):
        out.append("    " + ", ".join(str(x) for x in intlists[i:i + 16]) + ",")
    out.append("};")
    out.append("inline constexpr DpiDrawSize kDpiDrawSizes[] = {")
    for (name, p, dpi, da) in dpi_sizes:
        out.append("    {%d, %d, %d, %d, %d}," % (order.index(name), p, dpi, da[0], da[1]))
    out.append("};")
    out.append("inline constexpr VariantRecord kVariants[] = {")
    for name in order:
        sub, _, cls = name.rpartition("::")
        base = BASE[cls.upper()]
        out.append('    {u"%s", u"%s", ThemeClass::%s, %d, %d, %d, %d},' % (
            sub, cls.upper(), base, *ps_index[name], *pr_index[name]))
    out.append("};")
    print("\n".join(out))


if __name__ == "__main__":
    main()
