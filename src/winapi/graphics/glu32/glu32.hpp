#pragma once

/// @file glu32.hpp
/// @brief glu32.dll: the OpenGL Utility Library 1.2 on Vitreo's opengl32.
///
/// Every export of Windows' glu32.dll is registered from glu32_exports.inc
/// (generated from winoracle's System32 / SysWOW64 glu32.dll: names,
/// ordinals, x86 stdcall bytes, argument kinds). GLU issues GL the way a
/// client library does: on the calling thread's current context
/// (gl::current_gl_context()), through opengl32's display-list seam
/// (gl::gl_issue_to with each command's spec 5.4 rule), so a GLU call inside
/// glNewList/glEndList is compiled into the list and one inside glBegin/glEnd
/// gets the GL errors the commands give there. With no current context the
/// GL commands do nothing, as the opengl32 exports do.
///
/// Implemented: the matrix helpers (gluPerspective, gluLookAt, gluOrtho2D,
/// gluPickMatrix), gluProject / gluUnProject, gluScaleImage,
/// gluBuild1DMipmaps / gluBuild2DMipmaps, gluErrorString,
/// gluErrorUnicodeStringEXT, gluGetString, and the quadrics
/// (glu32_quadric.hpp) with their GLU_ERROR callback. The tessellator
/// (gluNewTess ... gluEndPolygon) and NURBS (gluNewNurbsRenderer ...) are
/// callback-driven geometry processors Vitreo does not implement: they fail
/// with GLU's documented failure value (NULL from the constructors, nothing
/// from the rest) and log "[vitreo] UNIMPLEMENTED glu32!<api>: <case>" once
/// per family.
///
/// Measurements: tools/oracle_probe/gl11_state_probe{32,64}.expected, glu-*
/// lines (Microsoft glu32 1.2.2.0 on GDI Generic).

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/graphics/glu32/glu32_quadric.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace vitreo::winapi {

class Marshaler;
namespace gl { class GlContext; }

// Preferred base only: DLL registration reserves the region and publishes
// where it landed through set_scratch_base (a guest can claim this fixed
// range first; 0 when no region could be reserved, and the string exports
// then return NULL). Holds the error and version strings (ANSI and UTF-16,
// at fixed offsets so each pointer is stable).
static constexpr GuestAddr GLU32_SCRATCH_BASE = 0x5ED11000;
static constexpr u32       GLU32_SCRATCH_SIZE = 0x2000;

class Glu32Manager {
public:
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;

    Glu32Manager() = default;

    void set_scratch_base(GuestAddr base) noexcept { m_scratch_base = base; }
    [[nodiscard]] GuestAddr scratch_base() const noexcept { return m_scratch_base; }
    /// Calls into guest code: a quadric's GLU_ERROR callback,
    /// void CALLBACK fn(GLenum).
    void set_guest_call(GuestCall call) { m_guest_call = std::move(call); }

    /// Where GLUquadric objects live: the guest's process heap, as in
    /// Windows' glu32, so their number is bounded only by the heap.
    /// alloc returns 0 on failure (gluNewQuadric then returns NULL, its
    /// documented out-of-memory result).
    struct Heap {
        std::function<GuestAddr(u64 size)> alloc;
        std::function<void(GuestAddr address)> free;
    };
    void set_heap(Heap heap) { m_heap = std::move(heap); }
    /// The bytes gluNewQuadric allocates per object (its state is kept
    /// host-side, keyed by the address).
    static constexpr u64 kQuadricBytes = 32;

    /// The handler for a glu32 export (every name in glu32_exports.inc has one).
    [[nodiscard]] ApiHandler handler(std::string_view name);

    /// Tests only: a quadric's state (nullopt for a handle that is not live).
    [[nodiscard]] std::optional<glu::QuadricState> quadric_state(GuestAddr handle) const;

private:
    class Args;

    // Strings, matrices, projection.
    void gluErrorString(Args& a);
    void gluErrorUnicodeStringEXT(Args& a);
    void gluGetString(Args& a);
    void gluOrtho2D(Args& a);
    void gluPerspective(Args& a);
    void gluLookAt(Args& a);
    void gluPickMatrix(Args& a);
    void gluProject(Args& a);
    void gluUnProject(Args& a);
    // Images.
    void gluScaleImage(Args& a);
    void gluBuild1DMipmaps(Args& a);
    void gluBuild2DMipmaps(Args& a);
    i32 build_mipmaps(Args& a, u32 dimensions);
    i32 build_levels(Args& a, u32 dimensions, u32 target, i32 components, i32 width, i32 height, u32 format,
                     u32 type, GuestAddr data, i32 new_w, i32 new_h, gl::GlContext& ctx);
    // Quadrics.
    void gluNewQuadric(Args& a);
    void gluDeleteQuadric(Args& a);
    void gluQuadricDrawStyle(Args& a);
    void gluQuadricNormals(Args& a);
    void gluQuadricOrientation(Args& a);
    void gluQuadricTexture(Args& a);
    void gluQuadricCallback(Args& a);
    void gluSphere(Args& a);
    void gluCylinder(Args& a);
    void gluDisk(Args& a);
    void gluPartialDisk(Args& a);

    /// Sets one field of the quadric @p handle under the lock; false when the
    /// handle is not live.
    template <typename Fn>
    bool with_quadric(GuestAddr handle, Fn&& fn);
    /// Reports @p error to the quadric's GLU_ERROR callback, if it has one.
    void quadric_error(Args& a, GuestAddr callback, u32 error);
    /// Runs a draw against a snapshot of the quadric's state.
    template <typename Draw>
    void draw_quadric(Args& a, Draw&& draw);

    /// The guest address of string @p index of the string table, written there.
    GuestAddr publish_string(Marshaler& m, usize index, bool wide);

    GuestAddr m_scratch_base{GLU32_SCRATCH_BASE};
    GuestCall m_guest_call;
    Heap m_heap;
    mutable std::mutex m_mutex;
    std::unordered_map<GuestAddr, glu::QuadricState> m_quadrics;
};

/// Tests only: forget which unimplemented families have logged, so a test can
/// observe the one-time line whatever ran before it in the process.
void glu32_forget_unimplemented_logs_for_tests();

/// Every glu32.dll export, from glu32_exports.inc.
DllExports create_glu32_thunks(Glu32Manager& mgr);

} // namespace vitreo::winapi
