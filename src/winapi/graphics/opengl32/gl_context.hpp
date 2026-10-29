#pragma once
/// @file gl_context.hpp
/// @brief The OpenGL 1.1 context: the complete state vector of the
///        OpenGL 1.1 specification's chapter 6 state tables (Tables 6.5-6.22),
///        the error flag, the matrix stacks and the attribute stacks.
///
/// Every GL command is an internal C++ function (gl_commands.hpp) that takes a
/// GlContext& and typed arguments; the opengl32 exports only marshal guest
/// arguments into those functions (gl_exports.cpp). Display lists (Task 12)
/// record and replay calls to the same functions.
///
/// GlState is grouped by attribute bit (spec Table 6.2): each member struct is
/// exactly the state its group's "Attribute" column names, so glPushAttrib /
/// glPopAttrib (and wglCopyContext) copy whole structs. Enables live in one
/// bit set (GL_ENABLE_BIT); gl_cap_group() names the second group each enable
/// belongs to ("fog/enable", "lighting/enable", ...). State marked "-" in the
/// attribute column (matrix stacks, pixel maps, evaluator maps, selection and
/// feedback, texture images) lives in GlContext outside GlState.
///
/// Initial values are set in exactly one place, gl_state_defaults.cpp, one
/// assignment per state-table row.

#include <vitreo/common.hpp>
#include "gl_assembler.hpp"
#include "gl_enums.hpp"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vitreo::graphics {
class BackendTexture2D;
}

namespace vitreo::winapi::gl {

class GlDrawable;
class GlBatcher;
/// Destroys a context's batcher (gl_backend_draw.cpp), so this header needs no
/// backend types.
struct GlBatcherDeleter {
    void operator()(GlBatcher* batcher) const noexcept;
};
class GlDisplayList;
/// Destroys a list being compiled (gl_display_list.cpp).
struct GlDisplayListDeleter {
    void operator()(GlDisplayList* list) const noexcept;
};

// ---- Implementation-dependent values (spec Tables 6.19-6.20) ---------------
// Values are those Microsoft's GDI Generic GL 1.1 reports, so applications see
// the limits they were tested against (gl11_state_probe64.expected, get-GL_MAX_*).
inline constexpr u32 kMaxLights = 8;                    // get-GL_MAX_LIGHTS
inline constexpr u32 kMaxClipPlanes = 6;                // get-GL_MAX_CLIP_PLANES
inline constexpr u32 kMaxModelviewStackDepth = 32;      // get-GL_MAX_MODELVIEW_STACK_DEPTH
inline constexpr u32 kMaxProjectionStackDepth = 10;     // get-GL_MAX_PROJECTION_STACK_DEPTH
inline constexpr u32 kMaxTextureStackDepth = 10;        // get-GL_MAX_TEXTURE_STACK_DEPTH
inline constexpr u32 kSubpixelBits = 3;                 // get-GL_SUBPIXEL_BITS
inline constexpr u32 kMaxTextureSize = 1024;            // get-GL_MAX_TEXTURE_SIZE
inline constexpr u32 kMaxTextureLevels = 11;            // lod 0..log2(1024) (spec 3.8.1)
inline constexpr u32 kMaxPixelMapTable = 65536;         // get-GL_MAX_PIXEL_MAP_TABLE
inline constexpr u32 kMaxNameStackDepth = 128;          // get-GL_MAX_NAME_STACK_DEPTH
inline constexpr u32 kMaxListNesting = 64;              // get-GL_MAX_LIST_NESTING
inline constexpr u32 kMaxEvalOrder = 30;                // get-GL_MAX_EVAL_ORDER
inline constexpr i32 kMaxViewportDim = 16384;           // get-GL_MAX_VIEWPORT_DIMS
inline constexpr u32 kMaxAttribStackDepth = 16;         // get-GL_MAX_ATTRIB_STACK_DEPTH
inline constexpr u32 kMaxClientAttribStackDepth = 16;   // get-GL_MAX_CLIENT_ATTRIB_STACK_DEPTH
inline constexpr float kPointSizeRange[2] = {0.5f, 10.0f};  // get-GL_POINT_SIZE_RANGE
inline constexpr float kPointSizeGranularity = 0.125f;      // get-GL_POINT_SIZE_GRANULARITY
inline constexpr float kLineWidthRange[2] = {0.5f, 10.0f};  // get-GL_LINE_WIDTH_RANGE
inline constexpr float kLineWidthGranularity = 0.125f;      // get-GL_LINE_WIDTH_GRANULARITY

/// The framebuffer a context renders to, fixed by its pixel format
/// (spec Table 6.21 and the RGBA/DOUBLEBUFFER/STEREO/AUX rows of Table 6.20).
/// WGL (Task 5) fills it from the pixel format (Task 4).
struct GlFramebufferConfig {
    u8 red_bits{8};
    u8 green_bits{8};
    u8 blue_bits{8};
    u8 alpha_bits{0};
    u8 index_bits{0};
    u8 depth_bits{0};
    u8 stencil_bits{0};
    u8 accum_red_bits{0};
    u8 accum_green_bits{0};
    u8 accum_blue_bits{0};
    u8 accum_alpha_bits{0};
    u8 aux_buffers{0};
    bool double_buffer{true};
    bool stereo{false};
};

using GlVec3 = std::array<float, 3>;
using GlVec4 = std::array<float, 4>;
/// A 4x4 matrix in GL's column-major element order (m[col * 4 + row]).
using GlMatrix = std::array<double, 16>;
[[nodiscard]] GlMatrix gl_identity_matrix() noexcept;
[[nodiscard]] GlMatrix gl_multiply(const GlMatrix& a, const GlMatrix& b) noexcept;
/// The inverse of @p m, or nullopt when @p m is singular.
[[nodiscard]] std::optional<GlMatrix> gl_invert(const GlMatrix& m) noexcept;

// ---- Enables ---------------------------------------------------------------

/// Every server-side capability of glEnable/glDisable/glIsEnabled
/// (the "IsEnabled" rows of spec Tables 6.7-6.17).
enum class GlCap : u8 {
    AlphaTest, AutoNormal, Blend,
    ClipPlane0, ClipPlane1, ClipPlane2, ClipPlane3, ClipPlane4, ClipPlane5,
    ColorLogicOp, ColorMaterial, CullFace, DepthTest, Dither, Fog, IndexLogicOp,
    Light0, Light1, Light2, Light3, Light4, Light5, Light6, Light7,
    Lighting, LineSmooth, LineStipple,
    Map1Color4, Map1Index, Map1Normal, Map1TextureCoord1, Map1TextureCoord2,
    Map1TextureCoord3, Map1TextureCoord4, Map1Vertex3, Map1Vertex4,
    Map2Color4, Map2Index, Map2Normal, Map2TextureCoord1, Map2TextureCoord2,
    Map2TextureCoord3, Map2TextureCoord4, Map2Vertex3, Map2Vertex4,
    Normalize, PointSmooth, PolygonOffsetFill, PolygonOffsetLine, PolygonOffsetPoint,
    PolygonSmooth, PolygonStipple, ScissorTest, StencilTest,
    Texture1D, Texture2D, TextureGenS, TextureGenT, TextureGenR, TextureGenQ,
    Count
};
inline constexpr u32 kGlCapCount = static_cast<u32>(GlCap::Count);
static_assert(kGlCapCount <= 64);

[[nodiscard]] std::optional<GlCap> gl_cap_from_enum(u32 cap) noexcept;
[[nodiscard]] u32 gl_cap_enum(GlCap cap) noexcept;
/// The attribute group besides GL_ENABLE_BIT that holds @p cap
/// (e.g. GL_FOG -> GL_FOG_BIT, from the spec tables' "x/enable" entries).
[[nodiscard]] u32 gl_cap_group(GlCap cap) noexcept;
[[nodiscard]] constexpr GlCap gl_cap_offset(GlCap first, u32 i) noexcept {
    return static_cast<GlCap>(static_cast<u32>(first) + i);
}

struct GlEnableState {                          // GL_ENABLE_BIT
    u64 bits{0};
    [[nodiscard]] bool test(GlCap cap) const noexcept {
        return (bits >> static_cast<u32>(cap)) & 1u;
    }
    void set(GlCap cap, bool on) noexcept {
        const u64 bit = u64{1} << static_cast<u32>(cap);
        bits = on ? (bits | bit) : (bits & ~bit);
    }
};

// ---- Attribute groups (spec Table 6.2) --------------------------------------

struct GlCurrentState {                         // GL_CURRENT_BIT, Table 6.5
    GlVec4 color;
    float index;
    GlVec4 tex_coords;
    GlVec3 normal;
    GlVec4 raster_position;
    float raster_distance;
    GlVec4 raster_color;
    float raster_index;
    GlVec4 raster_tex_coords;
    bool raster_position_valid;
    bool edge_flag;
};

struct GlPointState {                           // GL_POINT_BIT, Table 6.10
    float size;
};

struct GlLineState {                            // GL_LINE_BIT, Table 6.10
    float width;
    u16 stipple_pattern;
    i32 stipple_repeat;
};

struct GlPolygonState {                         // GL_POLYGON_BIT, Table 6.10
    u32 cull_face_mode;
    u32 front_face;
    std::array<u32, 2> mode;                    // [0] front, [1] back
    float offset_factor;
    float offset_units;
};

struct GlPolygonStippleState {                  // GL_POLYGON_STIPPLE_BIT
    /// Bit x of rows[y] is the stipple bit for window (x mod 32, y mod 32).
    std::array<u32, 32> rows;
};

struct GlPixelModeState {                       // GL_PIXEL_MODE_BIT, Table 6.16
    bool map_color;
    bool map_stencil;
    i32 index_shift;
    i32 index_offset;
    GlVec4 scale;                               // RED/GREEN/BLUE/ALPHA_SCALE
    GlVec4 bias;                                // RED/GREEN/BLUE/ALPHA_BIAS
    float depth_scale;
    float depth_bias;
    float zoom_x;
    float zoom_y;
    u32 read_buffer;
};

struct GlLight {
    GlVec4 ambient;
    GlVec4 diffuse;
    GlVec4 specular;
    GlVec4 position;                            // eye coordinates
    GlVec3 spot_direction;                      // eye coordinates
    float spot_exponent;
    float spot_cutoff;
    float constant_attenuation;
    float linear_attenuation;
    float quadratic_attenuation;
};

struct GlMaterial {
    GlVec4 ambient;
    GlVec4 diffuse;
    GlVec4 specular;
    GlVec4 emission;
    float shininess;
    GlVec3 color_indexes;                       // am, dm, sm
};

struct GlLightingState {                        // GL_LIGHTING_BIT, Tables 6.8-6.9
    u32 shade_model;
    u32 color_material_face;
    u32 color_material_parameter;
    std::array<GlMaterial, 2> material;         // [0] front, [1] back
    GlVec4 model_ambient;
    bool model_local_viewer;
    bool model_two_side;
    std::array<GlLight, kMaxLights> lights;
};

struct GlFogState {                             // GL_FOG_BIT, Table 6.8
    GlVec4 color;
    float index;
    float density;
    float start;
    float end;
    u32 mode;
};

struct GlDepthBufferState {                     // GL_DEPTH_BUFFER_BIT
    u32 func;
    bool writemask;
    double clear_value;
};

struct GlAccumBufferState {                     // GL_ACCUM_BUFFER_BIT
    GlVec4 clear_value;
};

struct GlStencilBufferState {                   // GL_STENCIL_BUFFER_BIT
    u32 func;
    u32 value_mask;
    i32 ref;
    u32 fail;
    u32 pass_depth_fail;
    u32 pass_depth_pass;
    u32 writemask;
    i32 clear_value;
};

struct GlViewportState {                        // GL_VIEWPORT_BIT, Table 6.7
    i32 x;
    i32 y;
    i32 width;
    i32 height;
    double depth_near;
    double depth_far;
};

struct GlTransformState {                       // GL_TRANSFORM_BIT, Table 6.7
    u32 matrix_mode;
    std::array<std::array<double, 4>, kMaxClipPlanes> clip_planes;  // eye coordinates
};

struct GlColorBufferState {                     // GL_COLOR_BUFFER_BIT, Tables 6.13-6.14
    u32 alpha_func;
    float alpha_ref;
    u32 blend_src;
    u32 blend_dst;
    u32 logic_op_mode;
    u32 draw_buffer;
    u32 index_writemask;
    std::array<bool, 4> color_writemask;
    GlVec4 clear_color;
    float clear_index;
};

struct GlHintState {                            // GL_HINT_BIT, Table 6.18
    u32 perspective_correction;
    u32 point_smooth;
    u32 line_smooth;
    u32 polygon_smooth;
    u32 fog;
};

struct GlEvalState {                            // GL_EVAL_BIT, Table 6.17
    std::array<float, 2> map1_grid_domain;
    i32 map1_grid_segments;
    std::array<float, 4> map2_grid_domain;
    std::array<i32, 2> map2_grid_segments;
};

struct GlListState {                            // GL_LIST_BIT, Table 6.22
    u32 base;
};

struct GlTexGen {
    u32 mode;
    std::array<double, 4> object_plane;
    std::array<double, 4> eye_plane;            // eye coordinates
};

struct GlTextureState {                         // GL_TEXTURE_BIT, Tables 6.11-6.12
    u32 env_mode;
    GlVec4 env_color;
    std::array<GlTexGen, 4> gen;                // S, T, R, Q
    u32 binding_1d;
    u32 binding_2d;
};

struct GlScissorState {                         // GL_SCISSOR_BIT, Table 6.13
    i32 x;
    i32 y;
    i32 width;
    i32 height;
};

struct GlPixelStoreModes {
    bool swap_bytes;
    bool lsb_first;
    i32 row_length;
    i32 skip_rows;
    i32 skip_pixels;
    i32 alignment;
};

struct GlPixelStoreState {                      // GL_CLIENT_PIXEL_STORE_BIT, Table 6.15
    GlPixelStoreModes pack;
    GlPixelStoreModes unpack;
};

struct GlClientArray {
    bool enabled;
    i32 size;
    u32 type;
    i32 stride;
    GuestAddr pointer;
};

struct GlVertexArrayState {                     // GL_CLIENT_VERTEX_ARRAY_BIT, Table 6.6
    GlClientArray vertex;
    GlClientArray normal;
    GlClientArray color;
    GlClientArray index;
    GlClientArray tex_coord;
    GlClientArray edge_flag;
};

/// Every state-table item that belongs to an attribute group.
struct GlState {
    GlCurrentState current;
    GlPointState point;
    GlLineState line;
    GlPolygonState polygon;
    GlPolygonStippleState polygon_stipple;
    GlPixelModeState pixel_mode;
    GlLightingState lighting;
    GlFogState fog;
    GlDepthBufferState depth_buffer;
    GlAccumBufferState accum_buffer;
    GlStencilBufferState stencil_buffer;
    GlViewportState viewport;
    GlTransformState transform;
    GlEnableState enable;
    GlColorBufferState color_buffer;
    GlHintState hint;
    GlEvalState eval;
    GlListState list;
    GlTextureState texture;
    GlScissorState scissor;
    // Client groups.
    GlPixelStoreState pixel_store;
    GlVertexArrayState vertex_array;

    [[nodiscard]] bool enabled(GlCap cap) const noexcept { return enable.test(cap); }
};

// ---- Texture objects (spec Table 6.11) -------------------------------------

struct GlTexLevel {
    i32 width;
    i32 height;
    i32 border;
    u32 internal_format;
    u8 red_size;
    u8 green_size;
    u8 blue_size;
    u8 alpha_size;
    u8 luminance_size;
    u8 intensity_size;
    /// The image array (spec 3.8.1), not part of the state tables: texel
    /// (i, j) of a level is texels[(j * width + i) * 4 * bytes_per_component],
    /// row 0 the bottom row (the first group the image specifies), each texel
    /// in the canonical RGBA layout of its base internal format
    /// (gl_texture.hpp, GlTexFormat) quantized to 8 (bytes_per_component 1)
    /// or 16 (2) bits. Empty for the null texture and for proxies.
    std::vector<u8> texels;
    u8 bytes_per_component;
    /// GL_EXT_paletted_texture: a COLOR_INDEXn_EXT level keeps its indices
    /// here (row 0 the bottom row) and `texels` holds them looked up in the
    /// object's colour table (gl_cmd_extensions.cpp), redone whenever the
    /// table changes. index_mask is the stored resolution (0xFF for INDEX1-8,
    /// 0xFFFF for INDEX12/16: ext-pal-index1-ushort-3-300,
    /// -index16-ushort-3-300); 0 for every other format. index_base is the
    /// table's base format (GL_RGB or GL_RGBA) the texels were looked up with.
    std::vector<u16> indices;
    u16 index_mask;
    u32 index_base;
};

/// GL_EXT_paletted_texture: a texture object's colour table. GDI Generic
/// starts every table at one entry of format GL_RGBA with 8 bits per
/// component (ext-pal-initial-*), keeps all four components of every entry
/// whatever the internal format (ext-pal-getcolortable-RGBA-ubyte after an
/// RGB table) and reports 8-bit sizes for any format. The initial entry's
/// value is not defined (GDI Generic draws uninitialised memory); Vitreo uses
/// opaque white.
struct GlColorTable {
    u32 internal_format{GL_RGBA};
    u32 base{GL_RGBA};              ///< GL_RGB or GL_RGBA
    i32 width{1};
    std::vector<std::array<u8, 4>> entries{std::array<u8, 4>{255, 255, 255, 255}};
};

/// The parameters glPushAttrib(GL_TEXTURE_BIT) saves for a bound object (spec 6).
struct GlTextureParams {
    GlVec4 border_color;
    u32 min_filter;
    u32 mag_filter;
    u32 wrap_s;
    u32 wrap_t;
    float priority;
};

/// One backend texture holding a texture object's image arrays, with what
/// the draw path needs to use it from several threads.
struct GlTextureStorage {
    std::shared_ptr<graphics::BackendTexture2D> texture;
    /// Serialises recording draws that bind `texture`: the Metal recorder
    /// resolves (and memoises) the texture's shader view per draw, which
    /// needs the callers' serialisation (metal_backend_texture_2d.hpp,
    /// ViewMemo), and two contexts sharing the texture record on their own
    /// threads under their own drawable locks. A leaf lock.
    std::mutex record_mutex;
    /// The drawables whose pending draw or open pass may bind `texture`
    /// (appended when a draw captures it; guarded by the share-group lock).
    std::vector<std::weak_ptr<GlDrawable>> users;
    usize bytes{0};
};

struct GlTextureObject {
    u32 name{0};
    u32 dimensions{0};                          // 0 never bound, 1 or 2
    GlTextureParams params{};
    /// TEXTURE_RESIDENT (spec 3.8.8; GDI Generic, gl11_state_probe64.expected
    /// aretexturesresident-*, texparam-resident-*): true once level 0 holds
    /// client or framebuffer data, false for no image or a NULL-pointer
    /// image. GDI Generic keeps no working set beyond that.
    bool resident{false};
    std::array<GlTexLevel, kMaxTextureLevels> levels{};
    /// GL_EXT_paletted_texture (a proxy object's table has no entries' use).
    GlColorTable color_table;

    // ---- Backend storage (gl_cmd_texture.cpp), not GL state ----
    /// The image arrays on the GPU: levels 0..p of a complete mipmap, or
    /// level 0 alone. Built at draw time from `levels`.
    std::shared_ptr<GlTextureStorage> storage;
    /// Bit l: level l changed since `storage` was written, within
    /// dirty_rect[l] (x0, y0, x1, y1), half-open, GL rows.
    u32 dirty_levels{0};
    std::array<std::array<i32, 4>, kMaxTextureLevels> dirty_rect{};
    /// A draw has captured `storage` since its contents were last written,
    /// so a later image change goes to new storage (the recorded draw keeps
    /// the old texels, spec 2.5's command order), or, past the share group's
    /// rename budget, ends the passes that captured it first.
    bool storage_in_use{false};
};

/// Objects shared between contexts by wglShareLists (Task 5): texture
/// objects, and the display lists Task 12 adds.
///
/// Threading. Contexts sharing a group may be current on different threads,
/// so the group's mutex guards everything in it: the name map, the name
/// counter, and the whole of every named texture object (parameters, image
/// arrays, backend storage). The map is reachable only through a held Lock
/// (`textures(lock)` checks that the lock owns this group's mutex), so no
/// code can touch it unlocked. A texture command holds the lock for its
/// whole duration; the draw path holds it while it resolves the bound
/// texture and captures the storage (gl_texture_for_draw). Lock order is
/// WGL handle-table mutex -> this -> GlDrawable mutex -> GlTextureStorage::
/// record_mutex: wglShareLists asks has_objects() under the WGL mutex; the
/// rename budget (gl_cmd_texture.cpp) ends drawables' passes under this one;
/// a drawable records a draw (taking the storage's record mutex) under its
/// own. Nothing takes this lock while holding a GlDrawable mutex
/// (glCopyTexImage reads the framebuffer before locking; keys are built
/// before gl_submit_batch locks the drawable), and nothing takes the WGL
/// mutex under it.
///
/// Display lists (gl_display_list.hpp) live under the same mutex. The list
/// map holds runs of names: an entry [first, last] with a list is one defined
/// list (first == last); an entry with no list is a run of names glGenLists
/// reserved as empty lists (spec 5.4), so glGenLists(2^30) costs one entry.
/// Lists are immutable once published, and are held by shared_ptr: glCallList
/// copies the pointer under the lock and executes without it (executing takes
/// this lock again for texture commands), so a list deleted or replaced by
/// another context stays alive until its replay ends.
struct GlListEntry {
    u32 last{0};
    std::shared_ptr<const GlDisplayList> list;
};

class GlShareGroup {
public:
    using Lock = std::unique_lock<std::mutex>;
    using TextureMap = std::unordered_map<u32, std::shared_ptr<GlTextureObject>>;
    using ListMap = std::map<u32, GlListEntry>;   ///< keyed by the run's first name

    [[nodiscard]] Lock lock() const { return Lock(m_mutex); }

    /// The texture names in use: glGenTextures reserves a name with a null
    /// object; glBindTexture creates the object (spec 3.8.8).
    [[nodiscard]] TextureMap& textures(const Lock& held) noexcept {
        check(held);
        return m_textures;
    }
    /// The name glGenTextures hands out next, before skipping used names.
    [[nodiscard]] u32& next_texture_name(const Lock& held) noexcept {
        check(held);
        return m_next_texture_name;
    }
    /// Bytes of backend storage made by renames (a captured storage replaced
    /// by a new one) since the group last ended the passes holding the old
    /// ones; bounded by the rename budget (gl_cmd_texture.cpp).
    [[nodiscard]] usize& renamed_bytes(const Lock& held) noexcept {
        check(held);
        return m_renamed_bytes;
    }

    /// The display-list names in use (see the class comment).
    [[nodiscard]] ListMap& lists(const Lock& held) noexcept {
        check(held);
        return m_lists;
    }

    /// wglShareLists fails (ERROR_INVALID_PARAMETER) when the destination
    /// already owns objects (wgl_contract_probe64.expected:
    /// share-after-gentextures-dst, -both): a generated name counts, a
    /// texture's or a list's.
    [[nodiscard]] bool has_objects() const {
        const Lock held = lock();
        return !m_textures.empty() || !m_lists.empty();
    }

private:
    void check(const Lock& held) const noexcept;

    mutable std::mutex m_mutex;
    TextureMap m_textures;
    ListMap m_lists;
    u32 m_next_texture_name{1};
    usize m_renamed_bytes{0};
};

// ---- Non-grouped state -------------------------------------------------------

struct GlEvalMap {
    i32 uorder{1};
    i32 vorder{1};
    float u1{0}, u2{1}, v1{0}, v2{1};
    std::vector<float> points;                  // uorder * vorder * components
};

struct GlAttribEntry {
    u32 mask{0};
    GlState saved;
    /// GL_TEXTURE_BIT: the bound 1D/2D objects' parameters (spec 6).
    std::array<GlTextureParams, 2> bound_texture_params{};
};

struct GlClientAttribEntry {
    u32 mask{0};
    GlPixelStoreState pixel_store;
    GlVertexArrayState vertex_array;
};

/// The begin_mode value outside glBegin/glEnd.
inline constexpr u32 kGlOutsideBeginEnd = 0xFFFFFFFFu;

class GlContext {
public:
    explicit GlContext(const GlFramebufferConfig& framebuffer,
                       std::shared_ptr<GlShareGroup> share_group = nullptr);

    GlState state;

    // ---- Errors (spec 2.5) ----
    /// Records @p gl_error unless an error is already pending: the first error
    /// is kept until glGetError reads it (gl11_state_probe64.expected:
    /// err-two-errors-sequence).
    void record_error(u32 gl_error) noexcept;
    /// Returns and clears the pending error (GL_NO_ERROR when none).
    u32 take_error() noexcept;
    [[nodiscard]] u32 pending_error() const noexcept { return m_error; }

    // ---- Begin/End (spec 2.6.3) ----
    [[nodiscard]] bool inside_begin_end() const noexcept { return begin_mode != kGlOutsideBeginEnd; }
    /// For commands spec 2.6.3 does not allow inside glBegin/glEnd: records
    /// GL_INVALID_OPERATION and returns true when inside.
    bool reject_inside_begin_end() noexcept;
    /// The primitive of the open glBegin, or kGlOutsideBeginEnd. Owned by
    /// glBegin/glEnd (Task 7).
    u32 begin_mode{kGlOutsideBeginEnd};
    /// The vertices of the open glBegin, each with the current values at its
    /// glVertex (spec 2.7), and their edge flags (2.6.2). glEnd assembles them.
    std::vector<GlBatchVertex> immediate_vertices;
    std::vector<u8> immediate_edge_flags;

    // ---- Framebuffer ----
    [[nodiscard]] const GlFramebufferConfig& framebuffer() const noexcept { return m_framebuffer; }
    /// The first wglMakeCurrent sets the viewport and scissor box to the
    /// window size (spec 2.10.1, 4.1.2); later calls do nothing.
    void on_first_make_current(i32 width, i32 height) noexcept;
    [[nodiscard]] bool has_been_current() const noexcept { return m_has_been_current; }
    /// The HGLRC value wglCreateContext returned for this context (set by
    /// gl_wgl.cpp; see wgl_handle_of).
    u64 wgl_handle{0};
    /// The default framebuffer of the window this context is current on
    /// (gl_drawable.hpp): attached by wglMakeCurrent, dropped when the context
    /// is released. Null with no backend device or no window.
    std::shared_ptr<GlDrawable> drawable;
    /// The per-context streaming buffers, pipeline cache and pending draw
    /// (gl_backend_draw.hpp, Design 1); created by the first draw.
    std::unique_ptr<GlBatcher, GlBatcherDeleter> batcher;

    // ---- Matrix stacks (spec 2.10.2) ----
    std::vector<GlMatrix> modelview_stack;
    std::vector<GlMatrix> projection_stack;
    std::vector<GlMatrix> texture_stack;
    /// The stack GL_MATRIX_MODE selects.
    [[nodiscard]] std::vector<GlMatrix>& current_stack() noexcept;
    [[nodiscard]] u32 current_stack_limit() const noexcept;
    [[nodiscard]] const GlMatrix& modelview() const noexcept { return modelview_stack.back(); }
    [[nodiscard]] const GlMatrix& projection() const noexcept { return projection_stack.back(); }
    [[nodiscard]] const GlMatrix& texture_matrix() const noexcept { return texture_stack.back(); }

    // ---- Pixel maps (spec 3.6.3, Table 3.3), index = map - GL_PIXEL_MAP_I_TO_I ----
    std::array<std::vector<float>, 10> pixel_maps;

    // ---- Evaluator maps (spec 5.1), index = target - GL_MAP1_COLOR_4 / GL_MAP2_COLOR_4 ----
    std::array<GlEvalMap, 9> map1;
    std::array<GlEvalMap, 9> map2;

    // ---- Selection and feedback (spec 5.2-5.3) ----
    u32 render_mode{GL_RENDER};
    std::vector<u32> name_stack;
    GuestAddr selection_buffer{0};
    i32 selection_buffer_size{0};
    GuestAddr feedback_buffer{0};
    i32 feedback_buffer_size{0};
    u32 feedback_buffer_type{0};

    // ---- Display list under construction (spec 5.4; gl_display_list.cpp) ----
    /// GL_LIST_INDEX: the list glNewList opened, 0 when none.
    u32 list_index{0};
    /// GL_LIST_MODE: the mode of the last glNewList (GDI Generic keeps it
    /// after glEndList, list-index-mode after-mode=0x1301; initially 0).
    u32 list_mode{0};
    /// The commands compiled so far; null when no list is open. Not visible
    /// to any context until glEndList publishes it.
    std::unique_ptr<GlDisplayList, GlDisplayListDeleter> compiling_list;
    /// glCallList levels executing now (spec 5.4 nesting, kMaxListNesting).
    u32 list_call_depth{0};

    // ---- Attribute stacks (spec 6) ----
    std::vector<GlAttribEntry> attrib_stack;
    std::vector<GlClientAttribEntry> client_attrib_stack;

    // ---- Texture objects ----
    std::array<GlTextureObject, 2> default_textures;   // [0] TEXTURE_1D, [1] TEXTURE_2D
    std::array<GlTextureObject, 2> proxy_textures;     // [0] PROXY_TEXTURE_1D, [1] PROXY_TEXTURE_2D
    std::shared_ptr<GlShareGroup> share_group;
    /// The object bound to TEXTURE_1D (@p dimension 1) or TEXTURE_2D (2);
    /// the default object for binding 0. @p held is the share group's lock.
    /// A named object stays this context's binding while the context keeps
    /// it bound, even after another context sharing the group deletes the
    /// name (the object then lives until unbound here). A binding whose name
    /// no longer names an object here (glPopAttrib restoring a deleted name)
    /// reverts to 0, as deleting a bound texture does (spec 3.8.8). Never
    /// null.
    [[nodiscard]] GlTextureObject* bound_texture(u32 dimension, const GlShareGroup::Lock& held) noexcept;
    /// The named objects bound now ([0] TEXTURE_1D, [1] TEXTURE_2D); null
    /// for binding 0. Owned by glBindTexture / glDeleteTextures.
    std::array<std::shared_ptr<GlTextureObject>, 2> bound_objects;

private:
    GlFramebufferConfig m_framebuffer;
    u32 m_error{GL_NO_ERROR};
    bool m_has_been_current{false};
};

/// Sets every GlState item to its state-table initial value (gl_state_defaults.cpp).
void gl_set_initial_state(GlState& state, const GlFramebufferConfig& framebuffer);
/// A texture object's initial state (spec Table 6.11).
[[nodiscard]] GlTextureObject gl_initial_texture_object(u32 name);
/// Initial evaluator maps (spec 5.1 / Table 5.1).
void gl_set_initial_eval_maps(std::array<GlEvalMap, 9>& map1, std::array<GlEvalMap, 9>& map2);
/// Initial pixel maps (spec Table 3.3).
void gl_set_initial_pixel_maps(std::array<std::vector<float>, 10>& maps);

/// Copies the attribute groups named by @p mask from @p from into @p to, the
/// way glPopAttrib restores them (enables per gl_cap_group). Used by
/// glPopAttrib and by wglCopyContext (Task 5).
void gl_copy_attrib_groups(GlState& to, const GlState& from, u32 mask) noexcept;

// ---- Current context ---------------------------------------------------------

/// The calling thread's current context (guest threads are host threads), or
/// null when none is current.
[[nodiscard]] GlContext* current_gl_context() noexcept;

/// Installs @p ctx as the calling thread's current context (null releases).
/// The only callers are wglMakeCurrent (Task 5) and tests; nothing else may
/// change the current context.
void set_current_gl_context_for_thread(GlContext* ctx) noexcept;

}  // namespace vitreo::winapi::gl
