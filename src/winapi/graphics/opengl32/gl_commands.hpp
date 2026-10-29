#pragma once
/// @file gl_commands.hpp
/// @brief Internal GL 1.1 command functions. Each takes the GlContext and typed
///        arguments, applies the specification's validation (recording the
///        error and changing nothing on failure) and then its effect.
///
/// The exports (gl_exports.cpp) marshal guest arguments into these; display
/// lists (Task 12) record and replay the same calls. Scalar and vector forms
/// of one command meet in one function: GlValues carries the values, whether
/// they came from an integer form (spec Table 2.6 conversion for colours) and
/// whether a scalar form was used (a scalar form with a vector pname is
/// GL_INVALID_ENUM).
///
/// Every function below checks spec 2.6.3 itself: those not allowed between
/// glBegin and glEnd record GL_INVALID_OPERATION there and do nothing else.

#include "gl_context.hpp"

#include <atomic>
#include <span>
#include <string_view>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi::gl {

/// Parameter values for the {f,i}{,v} command families.
struct GlValues {
    std::array<double, 4> v{};
    u8 count{0};
    bool from_int{false};   ///< came from an i/iv form
    bool scalar{false};     ///< came from a non-v form (one value)

    [[nodiscard]] double operator[](usize i) const noexcept { return v[i]; }
    [[nodiscard]] static GlValues scalar_value(double value, bool from_int) noexcept {
        GlValues out;
        out.v[0] = value;
        out.count = 1;
        out.from_int = from_int;
        out.scalar = true;
        return out;
    }
    [[nodiscard]] static GlValues vector(std::span<const double> values, bool from_int) noexcept {
        GlValues out;
        out.count = static_cast<u8>(values.size() < 4 ? values.size() : 4);
        for (usize i = 0; i < out.count; ++i) out.v[i] = values[i];
        out.from_int = from_int;
        return out;
    }
};

/// How a queried value converts to the requested type (spec 6.1.2).
enum class GlQueryKind : u8 {
    Boolean,  ///< stored as 0/1
    Integer,  ///< an integer or enumerant
    Float,    ///< a floating-point value: rounded to nearest for GetIntegerv
    Color,    ///< RGBA colour, normal, DepthRange or depth clear value:
              ///< GetIntegerv uses spec Table 4.7's INT conversion
    Mask,     ///< an unsigned bit mask: GetIntegerv returns the bit pattern
};

struct GlQuery {
    GlQueryKind kind{GlQueryKind::Integer};
    u32 count{0};
    std::array<double, 16> v{};

    void set(GlQueryKind k, std::initializer_list<double> values) noexcept {
        kind = k;
        count = 0;
        for (double x : values) v[count++] = x;
    }
};

/// Conversions of spec 6.1.2 for one queried element.
[[nodiscard]] u8 gl_query_to_boolean(double value) noexcept;
[[nodiscard]] i32 gl_query_to_integer(GlQueryKind kind, double value) noexcept;

/// Spec Table 2.6: integer colour components to [-1, 1] / [0, 1].
[[nodiscard]] float gl_byte_to_float(i8 c) noexcept;
[[nodiscard]] float gl_ubyte_to_float(u8 c) noexcept;
[[nodiscard]] float gl_short_to_float(i16 c) noexcept;
[[nodiscard]] float gl_ushort_to_float(u16 c) noexcept;
[[nodiscard]] float gl_int_to_float(i32 c) noexcept;
[[nodiscard]] float gl_uint_to_float(u32 c) noexcept;

/// Prints "[vitreo] opengl32: <text> (logged once)" the first time @p flag is
/// seen clear (one flag per condition).
void gl_log_once(std::atomic<bool>& flag, const char* text);
/// Logs "[vitreo] UNIMPLEMENTED opengl32!<api>: <detail>" once per api.
void gl_log_unimplemented_once(std::string_view api, std::string_view detail);
/// Tests only: forget which apis have logged, so a test can observe a
/// one-time line whatever ran before it in the process.
void gl_forget_unimplemented_logs_for_tests();

namespace cmd {

// ---- Errors and strings (spec 2.5, 6.1.11) ----
u32 GetError(GlContext& ctx);
/// GL_VENDOR..GL_EXTENSIONS: the string (Design 10); null + GL_INVALID_ENUM otherwise.
const char* GetString(GlContext& ctx, u32 name);

// ---- Enables (spec 2.10.3 ff.) ----
void Enable(GlContext& ctx, u32 cap);
void Disable(GlContext& ctx, u32 cap);
bool IsEnabled(GlContext& ctx, u32 cap);

// ---- Queries (spec 6.1) ----
/// Fills @p out for a glGet{Boolean,Integer,Float,Double}v pname; false (and
/// GL_INVALID_ENUM recorded) for an unknown pname.
bool Get(GlContext& ctx, u32 pname, GlQuery& out);
/// glGetPointerv; false (GL_INVALID_ENUM) for an unknown pname.
bool GetPointer(GlContext& ctx, u32 pname, GuestAddr& out);

// ---- Current values (spec 2.7) ----
void Color(GlContext& ctx, float r, float g, float b, float a);
void Index(GlContext& ctx, float index);
void Normal(GlContext& ctx, float x, float y, float z);
void TexCoord(GlContext& ctx, float s, float t, float r, float q);
void EdgeFlag(GlContext& ctx, bool flag);

// ---- Begin/End and vertices (spec 2.6, 2.9; gl_cmd_vertex.cpp) ----
/// glBegin: INVALID_OPERATION inside Begin/End, INVALID_ENUM for a token that
/// is not one of the ten primitive modes.
void Begin(GlContext& ctx, u32 mode);
/// glEnd: INVALID_OPERATION outside Begin/End; otherwise assembles and draws
/// the vertices given since glBegin.
void End(GlContext& ctx);
/// glVertex: inside Begin/End adds a vertex with the current colour, normal,
/// texture coordinates and edge flag; outside it does nothing.
void Vertex(GlContext& ctx, float x, float y, float z, float w);
/// glRect (spec 2.9): Begin(POLYGON) (x1,y1) (x2,y1) (x2,y2) (x1,y2) End.
void Rect(GlContext& ctx, float x1, float y1, float x2, float y2);
/// The client-array values a vertex-array command transfers (spec 2.8),
/// dereferenced from client memory under the client state in effect when the
/// command is issued: an export reads them just before executing, a display
/// list keeps the copy made when the command was compiled (spec 5.4). Each
/// element holds, for the arrays enabled then, the values ArrayElement passes
/// to EdgeFlagv, TexCoord, Color, Index, Normal and Vertex (Table 2.6
/// conversions applied; missing components 0, 0, 0, 1).
struct GlArrayElements {
    static constexpr u8 kEdge = 1, kTex = 2, kColor = 4, kIndex = 8, kNormal = 16, kVertex = 32;
    /// False when an enabled array's range (or the index list) was not
    /// committed and readable: the command records INVALID_OPERATION.
    bool readable{true};
    u8 arrays{0};
    u32 floats_per_element{0};
    u32 count{0};
    std::vector<float> values;   ///< count * floats_per_element, in element order

    void clear() noexcept {
        readable = true;
        arrays = 0;
        floats_per_element = 0;
        count = 0;
        values.clear();
    }
};
/// Reads elements first .. first + count - 1 of the enabled arrays
/// (glArrayElement: count 1; glDrawArrays).
void gl_read_array_range(const GlContext& ctx, const GuestMemory& memory, i64 first, i64 count, GlArrayElements& out);
/// Reads the @p count indices of @p type at @p indices and the elements they
/// name, in index order (glDrawElements).
void gl_read_array_indices(const GlContext& ctx, const GuestMemory& memory, i64 count, u32 type, GuestAddr indices,
                           GlArrayElements& out);

/// glArrayElement / glDrawArrays / glDrawElements (spec 2.8) on the elements
/// the export read (@p elements): Begin(mode), ArrayElement per element,
/// End. Elements that were not readable record INVALID_OPERATION after the
/// argument checks and the command does nothing.
void ArrayElement(GlContext& ctx, const GlArrayElements& elements);
void DrawArrays(GlContext& ctx, u32 mode, i32 count, const GlArrayElements& elements);
void DrawElements(GlContext& ctx, u32 mode, i32 count, u32 type, const GlArrayElements& elements);

// ---- Client vertex arrays (spec 2.8) ----
void EnableClientState(GlContext& ctx, u32 array);
void DisableClientState(GlContext& ctx, u32 array);
void VertexPointer(GlContext& ctx, i32 size, u32 type, i32 stride, GuestAddr pointer);
void NormalPointer(GlContext& ctx, u32 type, i32 stride, GuestAddr pointer);
void ColorPointer(GlContext& ctx, i32 size, u32 type, i32 stride, GuestAddr pointer);
void IndexPointer(GlContext& ctx, u32 type, i32 stride, GuestAddr pointer);
void TexCoordPointer(GlContext& ctx, i32 size, u32 type, i32 stride, GuestAddr pointer);
void EdgeFlagPointer(GlContext& ctx, i32 stride, GuestAddr pointer);
void InterleavedArrays(GlContext& ctx, u32 format, i32 stride, GuestAddr pointer);

// ---- Transformation (spec 2.10-2.11) ----
void MatrixMode(GlContext& ctx, u32 mode);
void LoadIdentity(GlContext& ctx);
void LoadMatrix(GlContext& ctx, const GlMatrix& m);
void MultMatrix(GlContext& ctx, const GlMatrix& m);
void Rotate(GlContext& ctx, double angle_degrees, double x, double y, double z);
void Scale(GlContext& ctx, double x, double y, double z);
void Translate(GlContext& ctx, double x, double y, double z);
void Frustum(GlContext& ctx, double l, double r, double b, double t, double n, double f);
void Ortho(GlContext& ctx, double l, double r, double b, double t, double n, double f);
void PushMatrix(GlContext& ctx);
void PopMatrix(GlContext& ctx);
void Viewport(GlContext& ctx, i32 x, i32 y, i32 width, i32 height);
void DepthRange(GlContext& ctx, double near_value, double far_value);
void ClipPlane(GlContext& ctx, u32 plane, const std::array<double, 4>& equation);
bool GetClipPlane(GlContext& ctx, u32 plane, std::array<double, 4>& out);

// ---- Lighting and colouring (spec 2.13, 3.9) ----
void ShadeModel(GlContext& ctx, u32 mode);
void Light(GlContext& ctx, u32 light, u32 pname, const GlValues& values);
void LightModel(GlContext& ctx, u32 pname, const GlValues& values);
void Material(GlContext& ctx, u32 face, u32 pname, const GlValues& values);
void ColorMaterial(GlContext& ctx, u32 face, u32 mode);
bool GetLight(GlContext& ctx, u32 light, u32 pname, GlQuery& out);
bool GetMaterial(GlContext& ctx, u32 face, u32 pname, GlQuery& out);
void Fog(GlContext& ctx, u32 pname, const GlValues& values);

// ---- Rasterization (spec 3.3-3.5) ----
void PointSize(GlContext& ctx, float size);
void LineWidth(GlContext& ctx, float width);
void LineStipple(GlContext& ctx, i32 factor, u16 pattern);
void CullFace(GlContext& ctx, u32 mode);
void FrontFace(GlContext& ctx, u32 mode);
void PolygonMode(GlContext& ctx, u32 face, u32 mode);
void PolygonOffset(GlContext& ctx, float factor, float units);
/// @p rows already unpacked from client memory (gl_unpack_stipple).
void PolygonStipple(GlContext& ctx, const std::array<u32, 32>& rows);
bool GetPolygonStipple(GlContext& ctx, std::array<u32, 32>& rows);

// ---- Per-fragment operations and framebuffer control (spec 4.1-4.2) ----
void Scissor(GlContext& ctx, i32 x, i32 y, i32 width, i32 height);
void AlphaFunc(GlContext& ctx, u32 func, float ref);
void StencilFunc(GlContext& ctx, u32 func, i32 ref, u32 mask);
void StencilOp(GlContext& ctx, u32 fail, u32 zfail, u32 zpass);
void DepthFunc(GlContext& ctx, u32 func);
void BlendFunc(GlContext& ctx, u32 sfactor, u32 dfactor);
void LogicOp(GlContext& ctx, u32 opcode);
void DrawBuffer(GlContext& ctx, u32 mode);
void ReadBuffer(GlContext& ctx, u32 mode);
void ColorMask(GlContext& ctx, bool r, bool g, bool b, bool a);
void DepthMask(GlContext& ctx, bool flag);
void StencilMask(GlContext& ctx, u32 mask);
void IndexMask(GlContext& ctx, u32 mask);
void ClearColor(GlContext& ctx, float r, float g, float b, float a);
void ClearIndex(GlContext& ctx, float c);
void ClearDepth(GlContext& ctx, double depth);
void ClearStencil(GlContext& ctx, i32 s);
void ClearAccum(GlContext& ctx, float r, float g, float b, float a);
/// glClear (spec 4.2.3) into the context's drawable; GL_INVALID_VALUE for a
/// mask bit outside the four buffer bits.
void Clear(GlContext& ctx, u32 mask);
/// glFlush / glFinish (spec 5.5): end the open pass and submit it (glFinish
/// also waits); a single-buffered window presents its front buffer.
void Flush(GlContext& ctx);
void Finish(GlContext& ctx);
void Accum(GlContext& ctx, u32 op, float value);
void Hint(GlContext& ctx, u32 target, u32 mode);

// ---- Pixel state (spec 3.6.1-3.6.3, 4.3) ----
void PixelStore(GlContext& ctx, u32 pname, double value, bool from_int);
void PixelTransfer(GlContext& ctx, u32 pname, double value, bool from_int);
void PixelZoom(GlContext& ctx, float xfactor, float yfactor);
/// @p values already converted to the map's stored floats (gl_exports reads
/// fv/uiv/usv per spec 3.6.3); @p size is the guest's size argument.
void PixelMap(GlContext& ctx, u32 map, i32 size, std::span<const float> values);
/// The map's stored entries; false (GL_INVALID_ENUM) for an unknown map.
const std::vector<float>* GetPixelMap(GlContext& ctx, u32 map);
[[nodiscard]] bool gl_pixel_map_is_index_valued(u32 map) noexcept;

// ---- Texture environment, generation and object parameters (spec 2.10.4, 3.8) ----
void TexEnv(GlContext& ctx, u32 target, u32 pname, const GlValues& values);
bool GetTexEnv(GlContext& ctx, u32 target, u32 pname, GlQuery& out);
void TexGen(GlContext& ctx, u32 coord, u32 pname, const GlValues& values);
bool GetTexGen(GlContext& ctx, u32 coord, u32 pname, GlQuery& out);
void TexParameter(GlContext& ctx, u32 target, u32 pname, const GlValues& values);
bool GetTexParameter(GlContext& ctx, u32 target, u32 pname, GlQuery& out);
bool GetTexLevelParameter(GlContext& ctx, u32 target, i32 level, u32 pname, GlQuery& out);

// ---- Texture images and objects (spec 3.8.1, 3.8.8, 6.1.4; gl_cmd_texture.cpp) ----
/// An image command's client pixels: @p bytes are the
/// gl_pixel_layout(unpack modes, ...).span_bytes bytes at the data pointer.
struct GlClientPixels {
    bool null_pointer{false};   ///< data was NULL (glTexImage: contents unspecified)
    bool readable{true};        ///< false: the client bytes could not be read
    std::span<const u8> bytes;
    /// The UNPACK_* modes @p bytes were read under (spec 5.4: the client
    /// state when the command was issued, kept by a display list); null
    /// means the context's current modes.
    const GlPixelStoreModes* unpack{nullptr};
};
/// glTexImage1D (@p dimensions 1; @p height ignored) / glTexImage2D.
void TexImage(GlContext& ctx, u32 dimensions, u32 target, i32 level, i32 internal_format, i32 width,
              i32 height, i32 border, u32 format, u32 type, const GlClientPixels& pixels);
/// glTexSubImage1D (@p yoffset / @p height ignored) / glTexSubImage2D.
void TexSubImage(GlContext& ctx, u32 dimensions, u32 target, i32 level, i32 xoffset, i32 yoffset, i32 width,
                 i32 height, u32 format, u32 type, const GlClientPixels& pixels);
/// glCopyTexImage1D / glCopyTexImage2D: from the read buffer (spec 3.8.2).
void CopyTexImage(GlContext& ctx, u32 dimensions, u32 target, i32 level, u32 internal_format, i32 x, i32 y,
                  i32 width, i32 height, i32 border);
/// glCopyTexSubImage1D / glCopyTexSubImage2D.
void CopyTexSubImage(GlContext& ctx, u32 dimensions, u32 target, i32 level, i32 xoffset, i32 yoffset, i32 x,
                     i32 y, i32 width, i32 height);
/// glGetTexImage: when the arguments are valid and the level has an image,
/// calls @p client with the byte count the pack modes span; it returns the
/// client's buffer (its current bytes, so untouched bytes keep their value)
/// or an empty span when unreadable. True when @p client's buffer was
/// filled and must be written back.
using GlClientBuffer = std::span<u8> (*)(void* user, usize bytes);
bool GetTexImage(GlContext& ctx, u32 target, i32 level, u32 format, u32 type, GlClientBuffer client, void* user);
// ---- Pixel rectangles (spec 2.12, 3.6.4-3.6.5, 3.7, 4.3; gl_cmd_pixels.cpp) ----
/// glRasterPos*: (x, y, z, w) in object coordinates.
void RasterPos(GlContext& ctx, double x, double y, double z, double w);
/// glBitmap: @p bits are the gl_pixel_layout(unpack modes, GL_COLOR_INDEX,
/// GL_BITMAP, width, height).span_bytes bytes at the data pointer.
void Bitmap(GlContext& ctx, i32 width, i32 height, float xorig, float yorig, float xmove, float ymove,
            const GlClientPixels& bits);
void DrawPixels(GlContext& ctx, i32 width, i32 height, u32 format, u32 type, const GlClientPixels& pixels);
void CopyPixels(GlContext& ctx, i32 x, i32 y, i32 width, i32 height, u32 type);
/// glReadPixels: when the arguments are valid and the rectangle is not
/// empty, calls @p client with the byte count the PACK_* modes span (its
/// current bytes: pixels outside the window, which this leaves untouched, keep
/// them). True when @p client's buffer was filled and must be written back.
bool ReadPixels(GlContext& ctx, i32 x, i32 y, i32 width, i32 height, u32 format, u32 type, GlClientBuffer client,
                void* user);

/// glGenTextures: @p names receives n unused names, now marked used.
void GenTextures(GlContext& ctx, i32 n, std::vector<u32>& names);
void DeleteTextures(GlContext& ctx, i32 n, std::span<const u32> names);
void BindTexture(GlContext& ctx, u32 target, u32 name);
bool IsTexture(GlContext& ctx, u32 name);
/// glAreTexturesResident: the return value, and whether @p residences
/// (one per name) must be written to the client (spec 3.8.8: only when the
/// result is FALSE because some named object is not resident).
struct GlResidency {
    bool all_resident{false};
    bool write_residences{false};
};
GlResidency AreTexturesResident(GlContext& ctx, i32 n, std::span<const u32> names, std::vector<u8>& residences);
void PrioritizeTextures(GlContext& ctx, i32 n, std::span<const u32> names, std::span<const float> priorities);

// ---- Attribute stacks (spec 6) ----
void PushAttrib(GlContext& ctx, u32 mask);
void PopAttrib(GlContext& ctx);
void PushClientAttrib(GlContext& ctx, u32 mask);
void PopClientAttrib(GlContext& ctx);

// ---- Evaluators (spec 5.1; evaluation is Phase 2, Design 11) ----
/// Components per control point for a GL_MAP1_* / GL_MAP2_* target, 0 if invalid.
[[nodiscard]] u32 gl_map_components(u32 target) noexcept;
/// @p points: order (x vorder) control points of gl_map_components(target)
/// values each, gathered from client memory with the guest's stride.
void Map1(GlContext& ctx, u32 target, double u1, double u2, i32 stride, i32 order,
          std::span<const float> points);
void Map2(GlContext& ctx, u32 target, double u1, double u2, i32 ustride, i32 uorder,
          double v1, double v2, i32 vstride, i32 vorder, std::span<const float> points);
void MapGrid1(GlContext& ctx, i32 un, double u1, double u2);
void MapGrid2(GlContext& ctx, i32 un, double u1, double u2, i32 vn, double v1, double v2);
/// glGetMap{i,f,d}v: fills @p out; false on an invalid target/query.
bool GetMap(GlContext& ctx, u32 target, u32 query, std::vector<double>& out, GlQueryKind& kind);
void EvalCoord1(GlContext& ctx, double u);
void EvalCoord2(GlContext& ctx, double u, double v);
void EvalPoint1(GlContext& ctx, i32 i);
void EvalPoint2(GlContext& ctx, i32 i, i32 j);
void EvalMesh1(GlContext& ctx, u32 mode, i32 i1, i32 i2);
void EvalMesh2(GlContext& ctx, u32 mode, i32 i1, i32 i2, i32 j1, i32 j2);

// ---- Selection and feedback (spec 5.2-5.3; SELECT/FEEDBACK are Phase 2) ----
i32 RenderMode(GlContext& ctx, u32 mode);
void SelectBuffer(GlContext& ctx, i32 size, GuestAddr buffer);
void FeedbackBuffer(GlContext& ctx, i32 size, u32 type, GuestAddr buffer);
void InitNames(GlContext& ctx);
void LoadName(GlContext& ctx, u32 name);
void PushName(GlContext& ctx, u32 name);
void PopName(GlContext& ctx);
void PassThrough(GlContext& ctx, float token);

// ---- Extensions GDI Generic exposes (gl_cmd_extensions.cpp, gl_cmd_vertex.cpp) ----
/// glColorTableEXT (GL_EXT_paletted_texture): replaces the colour table of
/// the bound 1D / 2D object (a proxy keeps the state only) and looks its
/// index levels up again.
void ColorTable(GlContext& ctx, u32 target, i32 internal_format, i32 width, u32 format, u32 type,
                const GlClientPixels& pixels);
/// glColorSubTableEXT: entries start .. start + count - 1.
void ColorSubTable(GlContext& ctx, u32 target, i32 start, i32 count, u32 format, u32 type,
                   const GlClientPixels& pixels);
/// glGetColorTableEXT: packs the table into @p client's buffer (as GetTexImage).
bool GetColorTable(GlContext& ctx, u32 target, u32 format, u32 type, GlClientBuffer client, void* user);
/// glGetColorTableParameter{iv,fv}EXT.
bool GetColorTableParameter(GlContext& ctx, u32 target, u32 pname, GlQuery& out);
/// glAddSwapHintRectWIN (GL_WIN_swap_hint): a rectangle the next SwapBuffers
/// copies (the drawable keeps them).
void AddSwapHintRect(GlContext& ctx, i32 x, i32 y, i32 width, i32 height);
/// glDrawRangeElementsWIN: glDrawElements with end < start INVALID_VALUE
/// (ext-dre-*); indices outside [start, end] are still drawn.
void DrawRangeElements(GlContext& ctx, u32 mode, u32 start, u32 end, i32 count, u32 type,
                       const GlArrayElements& elements);

// ---- Display lists (spec 5.4; gl_display_list.cpp) ----
void ListBase(GlContext& ctx, u32 base);
/// glNewList: refused inside Begin/End, then INVALID_ENUM for a bad mode,
/// INVALID_OPERATION while a list is open, INVALID_VALUE for 0 (that order,
/// gl11_state_probe64.expected list-err-newlist-*).
void NewList(GlContext& ctx, u32 n, u32 mode);
/// glEndList: publishes the open list in the share group.
void EndList(GlContext& ctx);
/// glCallList: INVALID_VALUE for 0 (list-err-calllist-zero); an undefined
/// list, or a call deeper than kMaxListNesting, does nothing.
void CallList(GlContext& ctx, u32 n);
/// glCallLists with the offsets the export decoded from @p type's array
/// (empty when @p type is invalid); each runs list LIST_BASE + offset.
void CallLists(GlContext& ctx, i32 n, u32 type, std::span<const u32> offsets);
u32 GenLists(GlContext& ctx, i32 range);
void DeleteLists(GlContext& ctx, u32 list, i32 range);
bool IsList(GlContext& ctx, u32 list);

}  // namespace cmd

/// The UNPACK_* modes an image command's @p pixels were read under.
[[nodiscard]] inline const GlPixelStoreModes& gl_unpack_modes(const GlContext& ctx,
                                                              const cmd::GlClientPixels& pixels) noexcept {
    return pixels.unpack != nullptr ? *pixels.unpack : ctx.state.pixel_store.unpack;
}

/// Bytes per element of a glCallLists @p type, 0 for an invalid type.
[[nodiscard]] u32 gl_call_lists_type_bytes(u32 type) noexcept;
/// Decodes @p n offsets of @p type from @p bytes (spec 5.4; GL_FLOAT offsets
/// are floored, as GDI Generic does: list-calllists-float-minus0.25-base213).
void gl_decode_call_lists(u32 type, std::span<const u8> bytes, i32 n, std::vector<u32>& offsets);

/// Spec 3.6.4 unpacking of a 32x32 bitmap (glPolygonStipple) from @p bytes
/// laid out per @p modes; rows[y] bit x = pixel (x, y). @p bytes holds the
/// span of gl_pixel_layout(modes, GL_COLOR_INDEX, GL_BITMAP, 32, 32) (a bit
/// past the end reads as 0).
[[nodiscard]] std::array<u32, 32> gl_unpack_stipple(const GlPixelStoreModes& modes,
                                                    std::span<const u8> bytes) noexcept;
/// Spec 4.3.2 packing of @p rows into @p bytes (that span long),
/// touching only the stipple's bits.
void gl_pack_stipple(const GlPixelStoreModes& modes, const std::array<u32, 32>& rows,
                     std::span<u8> bytes) noexcept;

}  // namespace vitreo::winapi::gl
