#pragma once
/// @file gl_exports.hpp
/// @brief The argument-marshalling layer between the opengl32 GL 1.1 exports
///        (gl11_exports.inc rows) and the internal command functions
///        (gl_commands.hpp).
///
/// Each row's kinds string ("i", "f", "d", "p") fixes where every argument
/// lives: on x86 stdcall a double takes two consecutive 4-byte stack slots
/// (Marshaler::arg_f64 reads `slot` and `slot + 1`), so the slot of argument n
/// is the sum of the widths before it; on x64 argument n is position n, with
/// floats and doubles in the first four positions read from XMM0-3
/// (Marshaler::arg_f32 / arg_f64). A GlHandler reads its arguments through
/// GlCall by argument number and never computes slots itself.
///
/// Commands owned by a later task have no handler yet (gl_export_handler
/// returns null); opengl32 registers them with a placeholder that records
/// GL_INVALID_OPERATION and logs
/// "[vitreo] UNIMPLEMENTED opengl32!<name>: implemented in a later task".
/// A task that implements one adds its handler to gl_exports.cpp.
///
/// Display lists: a handler decodes its arguments and issues the command
/// through gl_issue<&cmd::X>(call, args...), never by calling cmd::X
/// itself; gl_issue records or executes it as the spec 5.4 table says
/// (gl_display_list.hpp describes the seam). The only handlers that call
/// cmd::X directly are those whose command returns a result or writes client
/// memory (queries, glGen*, glIs*, glRenderMode, glReadPixels,
/// glAreTexturesResident, glGetError): such a command cannot be deferred, and
/// spec 5.4 lists every one of them as executed immediately.

#include <vitreo/common.hpp>
#include "gl_context.hpp"
#include "gl_display_list.hpp"

#include "gl_commands.hpp"
#include "core/memory/memory_region.hpp"

#include <array>
#include <atomic>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace vitreo::winapi {
class Marshaler;
}

namespace vitreo::winapi::gl {

/// Per-row argument layout: x86 stack slot of each argument.
struct GlExportSlots {
    std::array<u8, 16> x86{};
    u8 count{0};
};
[[nodiscard]] GlExportSlots gl_export_slots(std::string_view kinds) noexcept;

/// One marshalled GL call: the guest's arguments by number, the current
/// context, and where glGetString publishes its strings.
class GlCall {
public:
    GlCall(Marshaler& marshaler, GlContext& context, const GlExportSlots& slots,
           GuestAddr string_base, GlListRule list_rule = {}) noexcept
        : m(marshaler), ctx(context), m_slots(slots), m_string_base(string_base), m_list_rule(list_rule) {}

    Marshaler& m;
    GlContext& ctx;

    [[nodiscard]] u32 u(usize n) const;          ///< GLenum, GLbitfield, GLuint, GLsizei
    [[nodiscard]] i32 i(usize n) const { return static_cast<i32>(u(n)); }
    [[nodiscard]] bool b(usize n) const { return (u(n) & 0xFFu) != 0; }  ///< GLboolean
    [[nodiscard]] float f(usize n) const;        ///< GLfloat, GLclampf
    [[nodiscard]] double d(usize n) const;       ///< GLdouble, GLclampd
    [[nodiscard]] GuestAddr p(usize n) const;    ///< any pointer
    [[nodiscard]] GuestAddr string_base() const noexcept { return m_string_base; }
    void ret(u64 value);
    /// The list this call is compiled into: the open list, unless none is
    /// open or spec 5.4 executes this export immediately (gl_list_rule).
    [[nodiscard]] GlDisplayList* recording_list() const;

private:
    [[nodiscard]] usize slot(usize n) const noexcept;
    const GlExportSlots& m_slots;
    GuestAddr m_string_base;
    GlListRule m_list_rule;
};

/// The list a call of an export with rule @p rule and first argument
/// @p arg0 is compiled into: the open list, unless none is open or spec 5.4
/// executes the export immediately. GlCall::recording_list is this for the
/// guest's call; a client library issuing GL on the current context (glu32)
/// asks it directly.
[[nodiscard]] GlDisplayList* gl_recording_list(GlContext& ctx, const GlListRule& rule, u32 arg0) noexcept;

/// Issues command @p F with the decoded @p args into @p list (null: none):
/// compiled there (GL_COMPILE stops there), and executed when no list is
/// open or the list is GL_COMPILE_AND_EXECUTE (spec 5.4). The list stores
/// copies.
template <auto F, typename... A>
void gl_issue_to(GlContext& ctx, GlDisplayList* list, const A&... args) {
    if (list != nullptr) {
        list->append(std::make_unique<GlRecordedCall<F>>(ctx, args...));
        if (ctx.list_mode == GL_COMPILE) return;
    }
    F(ctx, args...);
}

/// gl_issue_to for the guest's call @p c (its export's rule and arguments).
template <auto F, typename... A>
void gl_issue(GlCall& c, const A&... args) {
    gl_issue_to<F>(c.ctx, c.recording_list(), args...);
}

/// Argument @p n of @p c as the C type of its parameter (integers narrowed to
/// the declared width; gl_exports.cpp file comment).
template <typename T>
[[nodiscard]] T gl_arg_as(GlCall& c, usize n) {
    if constexpr (std::is_same_v<T, float>) return c.f(n);
    else if constexpr (std::is_same_v<T, double>) return c.d(n);
    else return static_cast<T>(c.u(n));   // narrows i8/u8/i16/u16/i32/u32
}

// ---- Client memory (gl_client_memory.hpp's policy) ------------------------------------

struct GlPixelLayout;

/// The client bytes an unpacking pixel command reads: @p layout's span at
/// @p addr, copied into @p storage. An invalid layout (no address range can
/// hold it) or memory that is not committed and readable is unreadable
/// (`readable` false: the command has no effect, as Vitreo answers what is an
/// access violation on Windows); NULL is `null_pointer`. When the host cannot
/// hold the copy, GL_OUT_OF_MEMORY is recorded and the pixels are unreadable.
/// The unpack modes are the context's current ones.
[[nodiscard]] cmd::GlClientPixels gl_client_pixels(GlCall& c, GuestAddr addr, const GlPixelLayout& layout,
                                                   std::vector<u8>& storage);

/// Reads @p bytes client bytes at @p addr into @p out under the same policy
/// (@p access: Read, or ReadWrite for a buffer the command writes back).
/// False (GL_OUT_OF_MEMORY recorded when that was the reason) when they
/// cannot be had; @p addr 0 with @p bytes 0 is an empty success.
[[nodiscard]] bool gl_read_client_bytes(GlCall& c, GuestAddr addr, usize bytes, MemoryProtection access,
                                        std::vector<u8>& out);

/// The client buffer a packing command (glReadPixels, glGetTexImage,
/// glGetColorTableEXT) writes: GlClientBuffer::open checks the span is
/// committed, readable and writable guest memory, then reads its current
/// bytes (packing leaves the bytes between groups as they were);
/// write_back stores the packed bytes.
struct GlClientOutput {
    GlCall* call;
    GuestAddr addr;
    std::vector<u8> bytes;

    /// A cmd::GlClientBuffer: the buffer, or empty when it is not accessible.
    static std::span<u8> open(void* user, usize size);
    void write_back();
};

using GlHandler = void (*)(GlCall&);

using GlHandlerEntry = std::pair<std::string_view, GlHandler>;
/// The current-value, vertex, rectangle and vertex-array exports
/// (gl_cmd_vertex.cpp).
[[nodiscard]] std::span<const GlHandlerEntry> gl_vertex_export_handlers();
/// The raster-position and pixel-rectangle exports (gl_cmd_pixels.cpp).
[[nodiscard]] std::span<const GlHandlerEntry> gl_pixel_export_handlers();

/// An extension entry point wglGetProcAddress returns (gl_cmd_extensions.cpp):
/// an internal opengl32.dll entry, never in its export table, as on Windows.
struct GlExtensionEntry {
    std::string_view name;
    const char* kinds;     ///< argument kinds, as in gl11_exports.inc
    GlHandler handler;
};
/// Every name GDI Generic's wglGetProcAddress returns non-NULL for (ext-gpa).
[[nodiscard]] std::span<const GlExtensionEntry> gl_extension_entries();
/// glDrawRangeElementsWIN's handler (gl_cmd_vertex.cpp).
[[nodiscard]] GlHandler gl_draw_range_elements_handler();

/// The handler for a GL 1.1 export, or null for one a later task implements.
[[nodiscard]] GlHandler gl_export_handler(std::string_view name);
/// Every name gl_export_handler knows (tests check each is a real export).
[[nodiscard]] std::vector<std::string_view> gl_export_handler_names();

/// glGetString's strings live at string_base + slot * kGlStringSlotSize.
inline constexpr u32 kGlStringSlotSize = 64;

}  // namespace vitreo::winapi::gl
