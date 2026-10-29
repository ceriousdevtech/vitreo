// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_display_list.cpp
/// @brief glNewList, glEndList, glCallList, glCallLists, glGenLists,
///        glDeleteLists, glIsList (OpenGL 1.1 spec 5.4), the list names in the
///        share group, and the table of commands that are never compiled.
///
/// Where the specification leaves a behaviour open, or GDI Generic differs,
/// the measured answer is followed and cited by its probe tag
/// (tools/oracle_probe/gl11_state_probe64.expected, list-*).

#include "gl_display_list.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace vitreo::winapi::gl {

void GlDisplayListDeleter::operator()(GlDisplayList* list) const noexcept { delete list; }

// ---- Commands not compiled -----------------------------------------------------------

namespace {

// Spec 5.4: "Certain commands, when called while compiling a display list,
// are not compiled into the display list but are executed immediately. These
// are: IsList, GenLists, DeleteLists, FeedbackBuffer, SelectBuffer,
// RenderMode, VertexPointer, NormalPointer, ColorPointer, IndexPointer,
// TexCoordPointer, EdgeFlagPointer, InterleavedArrays, EnableClientState,
// DisableClientState, PushClientAttrib, PopClientAttrib, ReadPixels,
// PixelStore, GenTextures, DeleteTextures, AreTexturesResident, IsTexture,
// Flush, Finish, as well as IsEnabled and all of the Get commands (see
// Chapter 6). TexImage1D is executed immediately only when the target
// argument is PROXY_TEXTURE_1D and TexImage2D is executed immediately only
// when the target argument is PROXY_TEXTURE_2D."
// The Get commands are every chapter 6 query export, glGetError (2.5, it
// returns the flag: list-geterror-not-compiled) and glGetString included.
// NewList and EndList are the list commands themselves: they act on the
// list being compiled and are never placed in it.
constexpr GlNotCompiled kNotCompiled[] = {
    {"glIsList"}, {"glGenLists"}, {"glDeleteLists"},
    {"glFeedbackBuffer"}, {"glSelectBuffer"}, {"glRenderMode"},
    {"glVertexPointer"}, {"glNormalPointer"}, {"glColorPointer"}, {"glIndexPointer"},
    {"glTexCoordPointer"}, {"glEdgeFlagPointer"}, {"glInterleavedArrays"},
    {"glEnableClientState"}, {"glDisableClientState"},
    {"glPushClientAttrib"}, {"glPopClientAttrib"},
    {"glReadPixels"},
    {"glPixelStoref"}, {"glPixelStorei"},
    {"glGenTextures"}, {"glDeleteTextures"}, {"glAreTexturesResident"}, {"glIsTexture"},
    {"glFlush"}, {"glFinish"},
    {"glIsEnabled"},
    {"glGetError"}, {"glGetString"}, {"glGetBooleanv"}, {"glGetIntegerv"}, {"glGetFloatv"},
    {"glGetDoublev"}, {"glGetPointerv"}, {"glGetClipPlane"}, {"glGetLightfv"}, {"glGetLightiv"},
    {"glGetMaterialfv"}, {"glGetMaterialiv"}, {"glGetTexEnvfv"}, {"glGetTexEnviv"},
    {"glGetTexGendv"}, {"glGetTexGenfv"}, {"glGetTexGeniv"}, {"glGetTexParameterfv"},
    {"glGetTexParameteriv"}, {"glGetTexLevelParameterfv"}, {"glGetTexLevelParameteriv"},
    {"glGetTexImage"}, {"glGetPixelMapfv"}, {"glGetPixelMapuiv"}, {"glGetPixelMapusv"},
    {"glGetMapdv"}, {"glGetMapfv"}, {"glGetMapiv"}, {"glGetPolygonStipple"},
    {"glNewList"}, {"glEndList"},
    {"glTexImage1D", GL_PROXY_TEXTURE_1D}, {"glTexImage2D", GL_PROXY_TEXTURE_2D},
};

}  // namespace

std::span<const GlNotCompiled> gl_not_compiled_commands() noexcept { return kNotCompiled; }

GlListRule gl_list_rule(std::string_view export_name) noexcept {
    for (const GlNotCompiled& entry : kNotCompiled) {
        if (entry.name != export_name) continue;
        return entry.proxy_target != 0 ? GlListRule{false, entry.proxy_target} : GlListRule{true, 0};
    }
    return {};
}

// ---- List names (runs in the share group's map) --------------------------------------

namespace {

using ListMap = GlShareGroup::ListMap;

/// The run holding @p n, or end().
ListMap::iterator find_run(ListMap& map, u32 n) {
    auto it = map.upper_bound(n);
    if (it == map.begin()) return map.end();
    --it;
    return n <= it->second.last ? it : map.end();
}

/// Removes names [first, last] from the map, splitting runs that overlap.
void erase_names(ListMap& map, u32 first, u32 last) {
    auto it = map.upper_bound(first);
    if (it != map.begin() && std::prev(it)->second.last >= first) --it;
    while (it != map.end() && it->first <= last) {
        const u32 run_first = it->first;
        const GlListEntry run = it->second;
        it = map.erase(it);
        if (run_first < first) map.emplace(run_first, GlListEntry{first - 1, run.list});
        if (run.last > last) {
            map.emplace(last + 1, GlListEntry{run.last, run.list});
            break;
        }
    }
}

}  // namespace

std::optional<std::shared_ptr<const GlDisplayList>> gl_find_list(GlShareGroup& group, const GlShareGroup::Lock& held,
                                                                 u32 n) {
    ListMap& map = group.lists(held);
    const auto it = find_run(map, n);
    if (it == map.end()) return std::nullopt;
    return it->second.list;
}

// ---- glCallLists offsets ---------------------------------------------------------------

u32 gl_call_lists_type_bytes(u32 type) noexcept {
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_2_BYTES: return 2;
    case GL_3_BYTES: return 3;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: case GL_4_BYTES: return 4;
    default: return 0;
    }
}

void gl_decode_call_lists(u32 type, std::span<const u8> bytes, i32 n, std::vector<u32>& offsets) {
    offsets.clear();
    const u32 size = gl_call_lists_type_bytes(type);
    if (size == 0 || n <= 0 || bytes.size() < static_cast<usize>(n) * size) return;
    offsets.resize(static_cast<usize>(n));
    for (usize k = 0; k < offsets.size(); ++k) {
        const u8* p = bytes.data() + k * size;
        u32 v = 0;
        switch (type) {
        case GL_BYTE: v = static_cast<u32>(static_cast<i32>(static_cast<i8>(p[0]))); break;
        case GL_UNSIGNED_BYTE: v = p[0]; break;
        case GL_SHORT: { i16 s; std::memcpy(&s, p, 2); v = static_cast<u32>(static_cast<i32>(s)); break; }
        case GL_UNSIGNED_SHORT: { u16 s; std::memcpy(&s, p, 2); v = s; break; }
        case GL_INT: case GL_UNSIGNED_INT: std::memcpy(&v, p, 4); break;
        case GL_FLOAT: {
            // Spec: "floating-point values are truncated"; GDI Generic floors
            // (list-calllists-float-minus0.25-base213 runs list 212,
            // -minus0.5-base211 runs 210, -float-3.999 runs 203).
            float f;
            std::memcpy(&f, p, 4);
            const double fl = std::floor(static_cast<double>(f));
            const double c = std::isnan(fl) ? 0.0 : std::clamp(fl, -2147483648.0, 4294967295.0);
            v = static_cast<u32>(static_cast<i64>(c));
            break;
        }
        // Spec 5.4: offset = (offset << 8) + byte, most significant first.
        case GL_2_BYTES: v = (u32{p[0]} << 8) | p[1]; break;
        case GL_3_BYTES: v = (u32{p[0]} << 16) | (u32{p[1]} << 8) | p[2]; break;
        case GL_4_BYTES: v = (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3]; break;
        default: break;
        }
        offsets[k] = v;
    }
}

// ---- Commands --------------------------------------------------------------------------

namespace cmd {

namespace {

/// Runs list @p n (an undefined name does nothing, spec 5.4), unless the
/// nesting limit is reached: GDI Generic executes 64 levels counting the
/// outermost call (list-nesting-self-call executed-levels=64,
/// list-nesting-via-outer 63), so a call at depth 64 is ignored.
void execute_list(GlContext& ctx, u32 n) {
    if (ctx.list_call_depth >= kMaxListNesting) return;
    std::shared_ptr<const GlDisplayList> list;
    {
        const GlShareGroup::Lock held = ctx.share_group->lock();
        auto found = gl_find_list(*ctx.share_group, held, n);
        if (!found || !*found) return;
        list = std::move(*found);
    }
    // The lock is released: commands in the list take it themselves, and
    // `list` keeps the commands alive if another context deletes the name.
    ++ctx.list_call_depth;
    struct Unwind {
        GlContext& ctx;
        ~Unwind() { --ctx.list_call_depth; }
    } unwind{ctx};
    list->execute(ctx);
}

}  // namespace

void NewList(GlContext& ctx, u32 n, u32 mode) {
    if (ctx.reject_inside_begin_end()) return;                         // err-glNewList-inside-glBegin
    if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) {
        ctx.record_error(GL_INVALID_ENUM);                             // list-err-newlist-bad-mode-while-compiling
        return;
    }
    if (ctx.compiling_list) {
        ctx.record_error(GL_INVALID_OPERATION);                        // list-err-newlist-zero-while-compiling
        return;
    }
    if (n == 0) {
        ctx.record_error(GL_INVALID_VALUE);                            // err-bad-value-glNewList-zero
        return;
    }
    ctx.compiling_list.reset(new GlDisplayList());
    ctx.list_index = n;
    ctx.list_mode = mode;
}

void EndList(GlContext& ctx) {
    // Refused inside an executed glBegin; the list stays open
    // (list-endlist-inside-executed-begin index-after-glEnd=104).
    if (ctx.reject_inside_begin_end()) return;
    if (!ctx.compiling_list) {
        ctx.record_error(GL_INVALID_OPERATION);                        // err-glEndList-without-glNewList
        return;
    }
    std::shared_ptr<const GlDisplayList> list(ctx.compiling_list.release());
    const u32 n = ctx.list_index;
    ctx.list_index = 0;   // list_mode keeps its value (list-index-mode after-mode=0x1301)
    const GlShareGroup::Lock held = ctx.share_group->lock();
    ListMap& map = ctx.share_group->lists(held);
    erase_names(map, n, n);
    map.emplace(n, GlListEntry{n, std::move(list)});
}

void CallList(GlContext& ctx, u32 n) {
    // Allowed inside Begin/End (spec 2.6.3).
    if (n == 0) {
        ctx.record_error(GL_INVALID_VALUE);                            // list-err-calllist-zero
        return;
    }
    execute_list(ctx, n);
}

void CallLists(GlContext& ctx, i32 n, u32 type, std::span<const u32> offsets) {
    // n < 0 first, then n = 0 does nothing whatever the type, then the type
    // (list-err-calllists-neg-bad-type, -zero-bad-type, -bad-type).
    if (n < 0) {
        ctx.record_error(GL_INVALID_VALUE);
        return;
    }
    if (n == 0) return;
    if (gl_call_lists_type_bytes(type) == 0) {
        ctx.record_error(GL_INVALID_ENUM);
        return;
    }
    const u32 base = ctx.state.list.base;
    for (u32 offset : offsets) {
        const u32 list = base + offset;   // unsigned wrap (list-calllists-*-base-wrap-*)
        // An offset that reaches list 0 is skipped silently (list-calllists-offset-to-list-zero).
        if (list != 0) execute_list(ctx, list);
    }
}

u32 GenLists(GlContext& ctx, i32 range) {
    if (ctx.reject_inside_begin_end()) return 0;                      // list-err-genlists-neg-inside-begin
    if (range < 0) {
        ctx.record_error(GL_INVALID_VALUE);                            // err-neg-size-glGenLists
        return 0;
    }
    if (range == 0) return 0;                                          // err-glGenLists-zero
    const u64 want = static_cast<u64>(range);
    const GlShareGroup::Lock held = ctx.share_group->lock();
    ListMap& map = ctx.share_group->lists(held);
    // First fit from 1 (list-genlists-sequence gen1=1 gen3=2 ... gen15=21).
    // The list being compiled is not a name yet (list-genlists-during-compile-of-1 gen1=1).
    u64 candidate = 1;
    for (const auto& [first, run] : map) {
        if (first >= candidate && first - candidate >= want) break;
        candidate = std::max<u64>(candidate, u64{run.last} + 1);
    }
    if (candidate + want - 1 > std::numeric_limits<u32>::max()) return 0;
    const auto start = static_cast<u32>(candidate);
    // Spec 5.4: "GenLists also has the effect of creating an empty display
    // list for each of the indices".
    map.emplace(start, GlListEntry{static_cast<u32>(candidate + want - 1), nullptr});
    return start;
}

void DeleteLists(GlContext& ctx, u32 list, i32 range) {
    if (ctx.reject_inside_begin_end()) return;                         // list-err-deletelists-inside-begin
    if (range < 0) {
        ctx.record_error(GL_INVALID_VALUE);                            // err-neg-size-glDeleteLists
        return;
    }
    if (range == 0) return;                                            // list-err-deletelists-zero-range
    // Names past 2^32 - 1 do not exist (list-err-deletelists-wrap); 0 is never a list.
    const u64 last = std::min<u64>(u64{list} + static_cast<u64>(range) - 1, std::numeric_limits<u32>::max());
    const GlShareGroup::Lock held = ctx.share_group->lock();
    erase_names(ctx.share_group->lists(held), list, static_cast<u32>(last));
}

bool IsList(GlContext& ctx, u32 list) {
    if (ctx.reject_inside_begin_end()) return false;                   // list-err-islist-inside-begin
    if (list == 0) return false;                                       // err-glIsList-zero
    const GlShareGroup::Lock held = ctx.share_group->lock();
    return gl_find_list(*ctx.share_group, held, list).has_value();
}

}  // namespace cmd

}  // namespace vitreo::winapi::gl
