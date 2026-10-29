#pragma once
/// @file gl_texture.hpp
/// @brief OpenGL 1.1 texture images and their use when drawing (spec 3.8):
///        the internal-format table (Tables 3.14-3.15), the canonical RGBA
///        layout texels are stored in, the texture environment as texture-
///        stage operations (Tables 3.18-3.19), mipmap completeness (3.8.2) and
///        the bound texture a draw samples.
///
/// Storage. Every image is stored as 4 components per texel in the
/// canonical RGBA layout of its base internal format, the "swizzle" from the
/// pixel group's (R, G, B, A) after pixel transfer:
///
///   ALPHA            (0, 0, 0, A)      LUMINANCE   (R, R, R, 1)
///   LUMINANCE_ALPHA  (R, R, R, A)      INTENSITY   (R, R, R, R)
///   RGB              (R, G, B, 1)      RGBA        (R, G, B, A)
///
/// so L_t = I_t = the red component and A_t the alpha component of every
/// fetched texel, and the generator samples one backend format for all six.
/// Components are quantized (rounded to nearest, 3.8.1) to 8 bits
/// (RGBA8Unorm) unless the sized format asks for more than 8 bits for some
/// component, which gets 16 bits (RGBA16Unorm). TEXTURE_*_SIZE report those
/// allocations (3.8.1: "as closely as possible"; one resolution per
/// component is allowed).

#include <vitreo/common.hpp>
#include "gl_context.hpp"
#include "gl_pixels.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "graphics/shaders/ff_emulation.hpp"
#include "graphics/shaders/ff_uniforms.hpp"

#include <array>
#include <memory>
#include <optional>

namespace vitreo::winapi::gl {

/// One internal format (Tables 3.14-3.15, the legacy 1-4).
struct GlTexFormat {
    u32 internal_format{0};
    u32 base{0};                        ///< GL_ALPHA .. GL_RGBA
    /// Requested component bits (Table 3.15; 0 for a base or legacy format).
    u8 r{0}, g{0}, b{0}, a{0}, l{0}, i{0};
    u8 bytes_per_component{1};          ///< storage: 1 (8 bits) or 2 (16 bits)
    graphics::PixelFormat backend{graphics::PixelFormat::RGBA8Unorm};
    /// Canonical component k from the group's (R, G, B, A): 0..3, 4 = 0, 5 = 1.
    std::array<u8, 4> swizzle{0, 1, 2, 3};
};

/// The internal format @p internal_format names, or nullopt (INVALID_VALUE
/// for glTexImage; @p legacy_counts: whether 1..4 are accepted, which
/// glCopyTexImage does not).
[[nodiscard]] std::optional<GlTexFormat> gl_tex_format(i32 internal_format, bool legacy_counts) noexcept;

/// The canonical texel for a pixel group after pixel transfer: clamped to
/// [0, 1] (3.8.1) and swizzled.
[[nodiscard]] GlRgba gl_tex_canonical(const GlTexFormat& format, const GlRgba& rgba) noexcept;

/// Table 6.1: the (R, G, B, A) glGetTexImage returns for a canonical texel
/// of base format @p base (components the table leaves out are 0, alpha 1).
[[nodiscard]] GlRgba gl_tex_return_values(u32 base, const GlRgba& canonical) noexcept;

/// Spec 3.8.2: whether @p t's level arrays 0..p form a complete mipmap of
/// level 0 (same internal format and border, each level half the previous).
[[nodiscard]] bool gl_texture_complete(const GlTextureObject& t) noexcept;
/// Whether @p min_filter needs a mipmap.
[[nodiscard]] bool gl_min_filter_uses_mipmap(u32 min_filter) noexcept;

/// The texture environment (3.8.5, Tables 3.18-3.19) for @p env_mode on a
/// texture of base format @p base, as stage 0 of the generator: arg1 the
/// texture, arg2 the fragment colour, arg0 / TFACTOR the environment colour.
[[nodiscard]] graphics::TextureStageState gl_tex_env_stage(u32 env_mode, u32 base) noexcept;

/// What a draw textures with (spec 3.8.6): the bound texture of the enabled
/// dimensionality with the highest priority (2D over 1D), unless it is the
/// null texture or an incomplete mipmap, which disable texturing.
struct GlDrawTexture {
    bool enabled{false};
    std::shared_ptr<GlTextureStorage> storage;
    graphics::TextureStageState stage;
    graphics::FFUniformInputs::GlTextureUnit unit;
};

/// Resolves the texture a draw of @p ctx samples, building or refreshing its
/// backend storage (under the share group's lock). False texturing when
/// there is no backend device or storage cannot be created (logged once).
[[nodiscard]] GlDrawTexture gl_texture_for_draw(GlContext& ctx);

// ---- GL_EXT_paletted_texture (gl_cmd_texture.cpp, gl_cmd_extensions.cpp) ----

/// The stored-index mask of a COLOR_INDEXn_EXT internal format (0xFF for
/// INDEX1-8, 0xFFFF for INDEX12/16), 0 for any other format.
[[nodiscard]] u16 gl_color_index_mask(i32 internal_format) noexcept;
/// Looks index level @p level of @p t up in its colour table again (the
/// texels the level draws and glGetTexImage returns), marking it dirty.
void gl_lookup_indices(GlTextureObject& t, i32 level);
/// gl_lookup_indices for every level (after the table changed).
void gl_lookup_all_indices(GlTextureObject& t);
/// Marks the whole of @p level changed since the backend storage was written.
void mark_dirty_level(GlTextureObject& t, i32 level);

/// Bytes of renamed storage a share group may hold before the next rename
/// ends the passes that captured the storage it replaces and updates it in
/// place instead (gl_cmd_texture.cpp, refresh_storage).
inline constexpr usize kGlTextureRenameBudget = 32u << 20;

/// Process-wide texture-storage counters (diagnostics and tests).
struct GlTextureStats {
    u64 storages_created{0};    ///< backend textures created
    u64 renames{0};             ///< captured storage replaced by a copy-forward
    u64 budget_flushes{0};      ///< renames refused by the budget: passes ended, updated in place
    u64 bytes_uploaded{0};      ///< CPU -> GPU texel bytes
};
[[nodiscard]] GlTextureStats gl_texture_stats() noexcept;

}  // namespace vitreo::winapi::gl
