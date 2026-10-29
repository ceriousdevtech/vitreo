#pragma once

/// @file gdi32.hpp
/// @brief GDI32 device context stubs (WIN32-063)

#include <vitreo/common.hpp>
#include "winapi/kernel32/handle.hpp"
#include "winapi/core/thunk_registry.hpp"

#include "winapi/graphics/dwrite/font_backend.hpp"
#include "winapi/graphics/gdi32/bitmap_codec.hpp"

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vitreo { class GuestMemory; }
namespace vitreo::windowing { class CocoaBridge; class HwndManager; }
namespace vitreo::capsule { class Vfs; }

namespace vitreo::winapi {

class Marshaler;  // forward declaration for StretchDIBits

// =============================================================================
// Handle type extensions for GDI (not in HandleType enum — local constants)
// =============================================================================

/// HandleType value for device contexts
constexpr auto kHandleTypeDC = static_cast<HandleType>(100);

/// HandleType value for GDI objects (brushes, pens, fonts, palettes)
constexpr auto kHandleTypeGdiObject = static_cast<HandleType>(101);

/// HandleType value for Windows-format metafiles
constexpr auto kHandleTypeMetaFile = static_cast<HandleType>(102);

/// The lowest value a GDI handle (DC, object, metafile) may take. USER32 reads
/// an HBRUSH of 1..31 as a system colour, COLOR_xxx + 1 (Learn: WNDCLASSEXW
/// hbrBackground, FillRect), which Windows can do because its GDI handles are
/// never that small. Here GDI shares the kernel handle sequence from 4, so
/// without this floor the stock objects took 4..50h and COLOR_BTNFACE + 1 (16)
/// was DKGRAY_BRUSH.
constexpr Handle kFirstGdiHandle = 0x20;

// =============================================================================
// GetDeviceCaps indices
// =============================================================================

constexpr i32 TECHNOLOGY      = 2;
constexpr i32 HORZSIZE_IDX    = 4;   // Physical width in mm
constexpr i32 VERTSIZE_IDX    = 6;   // Physical height in mm
constexpr i32 NUMCOLORS_IDX   = 24;  // Number of colors
constexpr i32 HORZRES         = 8;
constexpr i32 VERTRES         = 10;
constexpr i32 BITSPIXEL       = 12;
constexpr i32 PLANES          = 14;  // Colour planes; 1 on every raster device
constexpr i32 LOGPIXELSX      = 88;
constexpr i32 LOGPIXELSY      = 90;
constexpr i32 COLORRES_IDX    = 108;  // Color resolution (bits per pixel)
constexpr i32 DESKTOPVERTRES  = 117;
constexpr i32 DESKTOPHORZRES  = 118;

// =============================================================================
// GetSystemMetrics indices
// =============================================================================

constexpr i32 SM_CXSCREEN  = 0;
constexpr i32 SM_CYSCREEN  = 1;
constexpr i32 SM_CYCAPTION = 4;
constexpr i32 SM_CXBORDER  = 5;
constexpr i32 SM_CYBORDER  = 6;

// =============================================================================
// Region type return values
// =============================================================================

constexpr i32 RGN_ERROR       = 0;
constexpr i32 NULLREGION      = 1;
constexpr i32 SIMPLEREGION    = 2;
constexpr i32 COMPLEXREGION   = 3;

// =============================================================================
// Stock object IDs
// =============================================================================

constexpr i32 WHITE_BRUSH_ID       = 0;
constexpr i32 LTGRAY_BRUSH_ID     = 1;
constexpr i32 GRAY_BRUSH_ID       = 2;
constexpr i32 DKGRAY_BRUSH_ID     = 3;
constexpr i32 BLACK_BRUSH_ID      = 4;
constexpr i32 NULL_BRUSH_ID       = 5;
constexpr i32 WHITE_PEN_ID        = 6;
constexpr i32 BLACK_PEN_ID        = 7;
constexpr i32 NULL_PEN_ID         = 8;
constexpr i32 OEM_FIXED_FONT_ID   = 10;
constexpr i32 ANSI_FIXED_FONT_ID  = 11;
constexpr i32 ANSI_VAR_FONT_ID    = 12;
constexpr i32 SYSTEM_FONT_ID      = 13;
constexpr i32 DEVICE_DEFAULT_FONT_ID = 14;
constexpr i32 DEFAULT_PALETTE_ID  = 15;
constexpr i32 SYSTEM_FIXED_FONT_ID = 16;
constexpr i32 DEFAULT_GUI_FONT_ID = 17;
constexpr i32 DC_BRUSH_ID         = 18;
constexpr i32 DC_PEN_ID           = 19;
/// The 1x1 monochrome bitmap every new memory DC starts with. Not a
/// documented GetStockObject index, but Windows answers it (s3gdi_bitmap_probe:
/// GetStockObject(21) is the handle CreateBitmap(0, 0, ...) returns).
constexpr i32 DEFAULT_BITMAP_ID   = 21;

// GetObjectType values (wingdi.h OBJ_*).
constexpr u32 OBJ_PEN_TYPE       = 1;
constexpr u32 OBJ_BRUSH_TYPE     = 2;
constexpr u32 OBJ_DC_TYPE        = 3;
constexpr u32 OBJ_PAL_TYPE       = 5;
constexpr u32 OBJ_FONT_TYPE      = 6;
constexpr u32 OBJ_BITMAP_TYPE    = 7;
constexpr u32 OBJ_REGION_TYPE    = 8;
constexpr u32 OBJ_MEMDC_TYPE     = 10;
constexpr u32 OBJ_EXTPEN_TYPE    = 11;

// =============================================================================
// Software Bitmap (pixel-level rendering)
// =============================================================================

/// Simple rectangular clip region
struct ClipRect {
    i32 left{0};
    i32 top{0};
    i32 right{0};
    i32 bottom{0};
    bool valid{false};  ///< true if a clip region is set
    // Non-overlapping rectangles for a complex region; empty means the
    // single bounding rectangle above (including an empty rectangle).
    std::vector<std::array<i32, 4>> rectangles;
    [[nodiscard]] static ClipRect combine(const ClipRect& a, const ClipRect& b, i32 mode);
    [[nodiscard]] std::vector<std::array<i32, 4>> parts() const {
        if (!rectangles.empty()) return rectangles;
        if (right <= left || bottom <= top) return {};
        return {{left, top, right, bottom}};
    }

    /// Check if point is inside the clip region
    [[nodiscard]] bool contains(i32 x, i32 y) const {
        if (!valid) return true;  // No clip = everything visible
        if (!(x >= left && x < right && y >= top && y < bottom)) return false;
        if (rectangles.empty()) return true;
        for (const auto& r : rectangles)
            if (x >= r[0] && x < r[2] && y >= r[1] && y < r[3]) return true;
        return false;
    }

    /// Check if the region is empty (has no area)
    [[nodiscard]] bool is_empty() const {
        return valid && (right <= left || bottom <= top);
    }
};

/// RGBA pixel buffer for software rendering
struct SoftBitmap {
    i32 width = 0;
    i32 height = 0;
    std::vector<u8> pixels; ///< RGBA, row-major, top-down, width * height * 4
    /// The bitmap's format depth as GetObject reports it: 1, 4, 8, 16, 24 or
    /// 32. The pixels stay RGBA whatever it is; a depth below 32 constrains
    /// the values drawing may leave (a 1bpp surface holds only black/white).
    u16 bpp = 32;
    /// A window's redirection surface rather than a GDI bitmap. Windows GDI
    /// writes 0 into the reserved byte of a 32bpp memory bitmap
    /// (s3gdi_blit_probe), but the presenter composites a window surface's
    /// fourth byte as alpha, so drawing on one keeps it opaque.
    bool window_surface = false;
    /// The window surface of a WS_CHILD window. On Windows a child window
    /// has no pixels of its own: its DC draws on the parent's surface, so a
    /// pixel the child never paints still shows the parent and the siblings
    /// beneath it (an owner-draw button whose WM_DRAWITEM draws nothing is
    /// invisible). Its unpainted pixels are therefore transparent (A=0), and
    /// the presenter composites what lies beneath.
    bool child_window = false;
    /// A device-dependent bitmap's packed rows as last written through
    /// CreateBitmap/SetBitmapBits, kept only when its rows carry padding
    /// bits: GetBitmapBits hands those bits back unchanged
    /// (s3gdi_bitmap_probe "mono8x2 gbb").
    std::vector<u8> ddb_raw;
    /// The reserved byte a GDI drawing operation leaves behind.
    [[nodiscard]] u8 draw_alpha() const noexcept { return window_surface ? 0xFF : 0x00; }
    /// Set whenever the pixels change; cleared by GdiManager::flush_window after
    /// presenting. Lets the window flush be skipped when nothing was drawn (a
    /// DirectDraw primary never touches its GDI window framebuffer, yet mouse
    /// messages would otherwise re-blit it every event).
    bool dirty = true;

    void resize(i32 w, i32 h) {
        width = w;
        height = h;
        // Opaque white: the long-standing default for bitmaps.
        size_t total = static_cast<size_t>(w) * h * 4;
        pixels.assign(total, 0xFF); // R=0xFF, G=0xFF, B=0xFF, A=0xFF
        dirty = true;
    }

    /// A window's framebuffer (its redirection surface). Until the window
    /// erases or paints, Windows shows it as opaque black - a NULL class brush
    /// never erases, and white appears only when WM_ERASEBKGND fills a white
    /// brush (tools/oracle_probe/unpainted_window_probe: PrintWindow with
    /// PW_RENDERFULLCONTENT reads 0xff000000 for NULL-brush windows).
    void resize_window(i32 w, i32 h) {
        width = w;
        height = h;
        window_surface = true;
        const size_t count = static_cast<size_t>(w) * h;
        pixels.assign(count * 4, 0x00);
        if (!child_window) {
            for (size_t i = 0; i < count; ++i) pixels[i * 4 + 3] = 0xFF;
        }
        dirty = true;
    }

    // Non-const access is the write path for every GDI drawing op, so marking
    // dirty here covers them centrally. Reads use the const overload below.
    u8* pixel_at(i32 x, i32 y) {
        dirty = true;
        return &pixels[static_cast<size_t>((y * width + x) * 4)];
    }
    const u8* pixel_at(i32 x, i32 y) const {
        return &pixels[static_cast<size_t>((y * width + x) * 4)];
    }

    bool in_bounds(i32 x, i32 y) const {
        return x >= 0 && x < width && y >= 0 && y < height;
    }
};

// =============================================================================
// GDI Manager
// =============================================================================

/// GDI32 device context manager
class GdiManager {
public:
    // This protects Vitreo's shared tables, not a caller's sharing of an HGDIOBJ.
    // Suspend all tracked levels before guest callbacks or blocking host work.
    [[nodiscard]] int& api_lock_depth() const noexcept;
    class ApiLock {
    public:
        explicit ApiLock(const GdiManager& manager)
            : m_manager(manager), m_lock(manager.m_api_mutex), m_depth(manager.api_lock_depth()) { ++m_depth; }
        ApiLock(const GdiManager& manager, std::try_to_lock_t)
            : m_manager(manager), m_lock(manager.m_api_mutex, std::try_to_lock),
              m_depth(manager.api_lock_depth()) { if (m_lock.owns_lock()) ++m_depth; }
        explicit operator bool() const { return m_lock.owns_lock(); }
        ~ApiLock() {
            if (m_lock.owns_lock() && --m_depth == 0)
                m_manager.request_presentation_locked();
        }
        ApiLock(const ApiLock&) = delete;
        ApiLock& operator=(const ApiLock&) = delete;
    private:
        const GdiManager& m_manager;
        std::unique_lock<std::recursive_mutex> m_lock;
        int& m_depth;
    };
    class ApiUnlock {
    public:
        explicit ApiUnlock(const GdiManager& manager)
            : m_mutex(manager.m_api_mutex), m_current_depth(manager.api_lock_depth()),
              m_depth(m_current_depth) {
            m_current_depth = 0;
            for (int i = 0; i < m_depth; ++i) m_mutex.unlock();
        }
        ~ApiUnlock() {
            for (int i = 0; i < m_depth; ++i) m_mutex.lock();
            m_current_depth = m_depth;
        }
        ApiUnlock(const ApiUnlock&) = delete;
        ApiUnlock& operator=(const ApiUnlock&) = delete;
    private:
        std::recursive_mutex& m_mutex;
        int& m_current_depth;
        int m_depth;
    };

    explicit GdiManager(HandleTable& handles);
    /// Removes this manager's fonts from the process-wide font registry, so
    /// a later manager's handle numbers cannot inherit them.
    ~GdiManager();
    GdiManager(const GdiManager&) = delete;
    GdiManager& operator=(const GdiManager&) = delete;

    // Scheduler must defer its callback to the host presentation thread.
    // Configure before guest execution. No guest commands are deferred here.
    void set_presentation_scheduler(std::function<void(std::function<void()>)> scheduler);
    void stop_presentation();

    // ---- Dependency injection (set after construction) ----
    void set_guest_memory(GuestMemory* mem) { ApiLock api_lock(*this); m_guest_memory = mem; }
    void set_cocoa_bridge(windowing::CocoaBridge* bridge) { ApiLock api_lock(*this); m_cocoa_bridge = bridge; }
    void set_hwnd_manager(windowing::HwndManager* mgr) { ApiLock api_lock(*this); m_hwnd_mgr = mgr; }

    /// CreateDIBSection with a non-NULL hSection places the bits in a view of
    /// that file-mapping object (Learn: CreateDIBSection). Injected so GDI
    /// needs no kernel32 dependency: map returns the view base (0 on
    /// failure) for a FILE_MAP_WRITE view of @p size bytes at @p offset.
    using SectionViewMapper = std::function<GuestAddr(u64 section, u64 offset, u64 size)>;
    using SectionViewUnmapper = std::function<void(GuestAddr base)>;
    void set_system_brush_resolver(std::function<std::optional<u32>(u64)> resolver) { ApiLock api_lock(*this);
        m_system_brush_color = std::move(resolver);
    }
    void set_section_view_mapper(SectionViewMapper map, SectionViewUnmapper unmap) { ApiLock api_lock(*this);
        m_map_section_view = std::move(map);
        m_unmap_section_view = std::move(unmap);
    }

    /// Logical exclusive-mode resolution reported by GetDeviceCaps HORZRES/
    /// VERTRES. Zero restores the host display size. Kept in sync with
    /// User32ExtManager::apply_virtual_display_mode.
    void set_virtual_screen_size(i32 width, i32 height) noexcept { ApiLock api_lock(*this);
        m_virtual_screen_width = width;
        m_virtual_screen_height = height;
    }

    /// GetGuiResources(GR_GDIOBJECTS): live GDI objects this process owns
    /// (DCs, bitmaps, pens, brushes, regions, palettes, metafiles; stock
    /// objects excluded).
    [[nodiscard]] usize live_object_count() const;

    /// GetDC — get DC for window (NULL = screen)
    [[nodiscard]] Handle get_dc(Handle hwnd);

    /// WindowFromDC — return the window associated with a display DC.
    /// Memory, screen, and unknown DCs have no associated window.
    [[nodiscard]] Handle window_from_dc(Handle hdc) const;

    /// GetPixelFormat bookkeeping: the pixel format set on the window behind
    /// @p hdc (on the DC itself when it has no window); 0 when none was set.
    [[nodiscard]] i32 pixel_format(Handle hdc) const;
    /// True once any SetPixelFormat has succeeded in this process (GetPixelFormat
    /// on a DC without a format then fails with ERROR_INVALID_PIXEL_FORMAT).
    [[nodiscard]] bool pixel_format_ever_set() const;

    /// SetPixelFormat bookkeeping. A window's pixel format can be set once:
    /// setting the same format again succeeds, a different one fails with
    /// ERROR_INVALID_PIXEL_FORMAT (measured, gfx_media_exports_probe).
    /// @return 0 on success, otherwise the Win32 error code.
    [[nodiscard]] u32 set_pixel_format(Handle hdc, i32 format, i32 max_format);
    /// The window @p hwnd is being destroyed (WindowManager's destroyed
    /// callback): its pixel format goes with it, so a later window that gets
    /// the same HWND value back starts without one (a pixel format belongs to
    /// the window, Learn SetPixelFormat). A windowless DC's goes at DeleteDC.
    void window_destroyed(Handle hwnd);

    /// SwapBuffers is opengl32's swap (Learn SwapBuffers: it swaps the buffers
    /// of the DC's window when its pixel format has a back buffer). opengl32
    /// installs it when it is registered; the handler returns the BOOL and
    /// sets the last error itself. Unset: SwapBuffers answers the measured
    /// results without a drawable to swap.
    void set_swap_buffers_handler(std::function<bool(Handle hdc)> handler) {
        m_swap_buffers_handler = std::move(handler);
    }
    [[nodiscard]] const std::function<bool(Handle)>& swap_buffers_handler() const noexcept {
        return m_swap_buffers_handler;
    }

    /// ReleaseDC
    i32 release_dc(Handle hwnd, Handle hdc);
    /// Replace a DC's clip region (device units), as BeginPaint does with
    /// the update region.
    void set_dc_clip(Handle hdc, const ClipRect& clip);
    /// Replace a region object's shape; false for an unknown region.
    bool set_region(Handle hrgn, const ClipRect& shape) { ApiLock api_lock(*this);
        const auto it = m_region_objects.find(hrgn);
        if (it == m_region_objects.end()) return false;
        it->second = shape;
        it->second.valid = true;
        return true;
    }

    /// CreateCompatibleDC
    [[nodiscard]] Handle create_compatible_dc(Handle hdc);

    /// DeleteDC
    bool delete_dc(Handle hdc);

    /// GetDeviceCaps
    [[nodiscard]] i32 get_device_caps(Handle hdc, i32 index);

    /// GetStockObject — returns well-known handle
    [[nodiscard]] Handle get_stock_object(i32 id);

    /// SelectObject — returns previous object; 0 on failure with @p error
    /// set to the last error Windows sets (0 = leave it).
    [[nodiscard]] Handle select_object(Handle hdc, Handle obj, u32* error = nullptr);

    /// DeleteObject — returns false for stock objects
    bool delete_object(Handle obj);

    /// Create, inspect, and delete memory-based Windows-format metafiles.
    [[nodiscard]] Handle set_metafile_bits(std::span<const u8> data);
    [[nodiscard]] bool has_metafile(Handle hmf) const;
    bool delete_metafile(Handle hmf);

    /// SetBkMode — returns previous mode
    i32 set_bk_mode(Handle hdc, i32 mode);

    /// SetTextColor — returns previous color
    u32 set_text_color(Handle hdc, u32 color);

    /// SetBkColor — returns previous color
    u32 set_bk_color(Handle hdc, u32 color);

    /// GetSystemMetrics
    [[nodiscard]] i32 get_system_metrics(i32 index);

    /// CreateBrushIndirect — allocate a GDI brush handle
    [[nodiscard]] Handle create_brush_indirect();

    /// CreateSolidBrush — allocate a brush with a solid COLORREF
    [[nodiscard]] Handle create_solid_brush(u32 color);

    /// CreatePatternBrush — snapshot a bitmap into a logical pattern brush.
    [[nodiscard]] Handle create_pattern_brush(Handle bitmap);

    /// Look up brush color by handle.  Returns true and sets *color if found.
    bool lookup_brush_color(Handle hbrush, u32* color) const;

    /// Fill with a registered solid or bitmap pattern brush.
    bool fill_rect_with_brush(Handle hdc, i32 left, i32 top, i32 right,
                              i32 bottom, Handle brush);

    /// FillRect — fill a rectangle in a DC with a brush color
    bool fill_rect_gdi(Handle hdc, i32 left, i32 top, i32 right, i32 bottom, u32 color);
    /// DrawFocusRect's border: the colour of each border pixel whose device
    /// x + y is odd is inverted, the reserved byte kept.
    bool xor_border_dots(Handle hdc, i32 left, i32 top, i32 right, i32 bottom);

    /// CreateCompatibleBitmap with real pixel buffer
    [[nodiscard]] Handle create_compatible_bitmap(Handle hdc, i32 width, i32 height);

    /// Create an HBITMAP already holding @p rgba (top-down, width*height*4).
    /// Used by GDI+ to hand a decoded image to callers as a GDI object.
    [[nodiscard]] Handle create_bitmap_from_rgba(i32 width, i32 height,
                                                 std::span<const u8> rgba);

    /// Snapshot an HBITMAP as top-down RGBA8. DIB sections are synced from
    /// guest bits first so CreateIconIndirect sees the pixels the guest wrote.
    struct BitmapRgbaCopy {
        i32 width = 0;
        i32 height = 0;
        u16 bpp = 0;
        bool from_dib = false;
        std::vector<u8> rgba;
    };
    [[nodiscard]] bool copy_bitmap_rgba(Handle bitmap, BitmapRgbaCopy& out);

    /// GetObjectW/GetObjectA — describe a GDI object in the caller's struct
    /// layout (BITMAP/DIBSECTION/LOGBRUSH/EXTLOGPEN widen on x64; LOGFONTA vs
    /// LOGFONTW by @p wide). Returns the byte count GetObject returns; on
    /// failure 0 with @p error set to the last error Windows sets (0 = leave
    /// it). Measured: tools/oracle_probe/s3gdi_bitmap_probe.
    i32 get_object(Handle hgdiobj, i32 cb_buffer, GuestAddr lp_object,
                   bool is_64bit = false, bool wide = true, u32* error = nullptr);

    /// GetObjectType — OBJ_* for a live GDI handle, 0 otherwise.
    [[nodiscard]] u32 object_type(Handle h) const;

    /// CreateBitmap: a device-dependent bitmap whose depth is
    /// nPlanes*nBitCount rounded up to 1/4/8/16/24/32; a zero width or
    /// height answers the stock 1x1 monochrome bitmap. @p bits holds
    /// WORD-aligned rows, top row first. 0 on failure with @p error set.
    [[nodiscard]] Handle create_bitmap(i32 width, i32 height, u32 planes,
                                       u32 bits_per_pixel, GuestAddr bits,
                                       u32* error = nullptr);

    /// A 1bpp bitmap from one byte (0/1) per pixel, top-down: the hbmMask
    /// GetIconInfo hands back.
    [[nodiscard]] Handle create_mono_bitmap(i32 width, i32 height, std::span<const u8> bits01);

    /// CopyImage(IMAGE_BITMAP): a new bitmap, @p cx x @p cy (0 = the
    /// source's), a DDB unless LR_CREATEDIBSECTION, monochrome with
    /// LR_MONOCHROME; LR_COPYRETURNORG / LR_COPYDELETEORG as Learn states.
    /// 0 with @p error = ERROR_INVALID_HANDLE for a non-bitmap.
    [[nodiscard]] Handle copy_bitmap(Handle src, i32 cx, i32 cy, u32 flags, u32* error);

    /// The planes DrawIconEx draws (user32 owns icon objects).
    struct IconDraw {
        u32 width = 0;
        u32 height = 0;
        const u8* color = nullptr;  ///< width*height RGBA, or null (monochrome)
        const u8* mask = nullptr;   ///< 0/1 per pixel; AND rows then XOR rows if monochrome
        bool alpha = false;         ///< colour plane carries per-pixel alpha
    };
    /// DrawIconEx core: @p cx x @p cy at logical (x, y), DI_MASK / DI_IMAGE /
    /// DI_NORMAL, optionally through a flicker-free brush-filled offscreen.
    /// False with @p error set for a bad DC.
    bool draw_icon(Handle hdc, i32 x, i32 y, i32 cx, i32 cy, const IconDraw& icon, u32 flags,
                   Handle flicker_brush, u32* error);

    /// A DC's drawing surface lent to another rasterizer (GDI+ on an HDC).
    struct DeviceSurface {
        u8* rgba{nullptr};              ///< top-down RGBA, width * 4 bytes per row
        i32 width{0};
        i32 height{0};
        const ClipRect* clip{nullptr};  ///< device-unit clip; !valid = whole surface
        /// Logical -> device: x' = [0]x + [2]y + [4], y' = [1]x + [3]y + [5]
        /// (world transform, then the page transform).
        double xform[6]{1, 0, 0, 1, 0, 0};
        u8 alpha{0};                    ///< alpha byte GDI drawing leaves (draw_alpha)
        bool mono{false};               ///< 1 bpp: only black/white may be stored
    };
    /// Run @p draw over @p hdc's surface under the API lock, then publish the
    /// change (dirty mark, DIB-section sync, DDB quantize) as GDI drawing
    /// does. False for an unknown DC or one without a surface.
    bool with_device_surface(Handle hdc, const std::function<void(DeviceSurface&)>& draw);

    struct ReadOnlyDeviceSurface {
        const u8* rgba{nullptr};
        i32 width{0};
        i32 height{0};
    };
    /// Read a synchronized DC surface under the API lock without drawing,
    /// invalidating, quantizing, or copying its pixels back into guest DIBs.
    bool read_device_surface(Handle hdc, const std::function<void(const ReadOnlyDeviceSurface&)>& read);


    /// GetBitmapBits / SetBitmapBits: the bitmap's rows as WORD-aligned
    /// packed pixels in its own depth, top row first.
    i32 get_bitmap_bits(Handle hbm, i32 cb, GuestAddr out, u32* error = nullptr);
    i32 set_bitmap_bits(Handle hbm, u32 cb, GuestAddr in, u32* error = nullptr);

    /// GetDIBits: query (lpvBits NULL) or convert a bitmap's scan lines into a
    /// DIB of the caller's format. Returns the GetDIBits return value; 0 on
    /// failure with @p error set (0 = leave the last error).
    i32 get_dib_bits(Handle hdc, Handle hbm, u32 start_scan, u32 scan_lines,
                     GuestAddr bits, GuestAddr bmi, u32 usage,
                     u32* error = nullptr);

    /// Pen metadata for CreatePen
    struct PenInfo {
        i32 style{0};       ///< PS_SOLID=0, PS_DASH=1, PS_NULL=5, etc.
        i32 width{1};       ///< Pen width in pixels
        u32 color{0};       ///< COLORREF (0x00BBGGRR)
    };

    /// Font metadata for WM_SETFONT handling
    struct FontInfo {
        i32 height = 0;     ///< lfHeight (negative = char height in px)
        u16 weight = 400;   ///< FW_NORMAL=400, FW_BOLD=700
        bool italic = false;
        /// lfFaceName. A font engine that reads raw font tables needs the face
        /// the handle was created from, not just its size and weight.
        std::u16string face_name;
        // The rest of the LOGFONT, kept verbatim so GetObject hands back
        // exactly what the caller created (Learn: GetObject, LOGFONTW).
        i32 width = 0;
        i32 escapement = 0;
        i32 orientation = 0;
        i32 lf_weight = 400;  ///< lfWeight as given (FW_DONTCARE is 0)
        u8 underline = 0;
        u8 strike_out = 0;
        u8 char_set = 0;
        u8 out_precision = 0;
        u8 clip_precision = 0;
        u8 quality = 0;
        u8 pitch_and_family = 0;
        /// ENUMLOGFONTEXW::elfFullName for stock fonts (their face); empty
        /// for a created logical font (s3gdi_info_probe).
        std::u16string full_name;
    };

    /// LOGBRUSH as created (Learn: LOGBRUSH). lbHatch is ULONG_PTR: a hatch
    /// index for BS_HATCHED, the HBITMAP for BS_PATTERN.
    struct BrushInfo {
        u32 style = 0;   ///< BS_SOLID=0, BS_NULL=1, BS_HATCHED=2, BS_PATTERN=3
        u32 color = 0;
        u64 hatch = 0;
    };

    /// EXTLOGPEN as created by ExtCreatePen.
    struct ExtPenInfo {
        u32 style = 0;
        u32 width = 0;
        u32 brush_style = 0;
        u32 color = 0;
        u64 hatch = 0;
        std::vector<u32> style_entries;
    };

    /// Palette entry (matches PALETTEENTRY structure)
    struct PaletteEntry {
        u8 red{0};
        u8 green{0};
        u8 blue{0};
        u8 flags{0};  // PC_RESERVED=0x01, PC_EXPLICIT=0x02, PC_NOCOLLAPSE=0x04
    };

    /// Palette storage
    struct PaletteInfo {
        u16 version{0x300};                 ///< palVersion (always 0x0300)
        std::vector<PaletteEntry> entries;  ///< Palette entries
    };

    /// CreateFontIndirectW — allocate a GDI font handle
    [[nodiscard]] Handle create_font_indirect(i32 height, u16 weight, bool italic,
                                              std::u16string face_name = {});
    /// CreateFontIndirect / CreateFont with every LOGFONT field.
    [[nodiscard]] Handle create_font(FontInfo info);

    /// CreateDIBPatternBrushPt / BS_DIBPATTERNPT: a pattern from a packed
    /// DIB (BITMAPINFO followed by its bits).
    [[nodiscard]] Handle create_dib_pattern_brush(GuestAddr packed_dib, u32 usage);
    /// CreateHatchBrush / CreateBrushIndirect(BS_HATCHED).
    [[nodiscard]] Handle create_hatch_brush(u32 hatch, u32 color);
    /// The LOGBRUSH a brush was created with, or nullptr.
    [[nodiscard]] const BrushInfo* lookup_brush(Handle hbrush) const;
    /// ExtCreatePen: validated EXTLOGPEN; 0 on failure with @p error set.
    [[nodiscard]] Handle create_ext_pen(ExtPenInfo info, u32* error);
    /// SetDCBrushColor / SetDCPenColor: previous colour or CLR_INVALID.
    u32 set_dc_brush_color(Handle hdc, u32 color);
    u32 set_dc_pen_color(Handle hdc, u32 color);
    /// The COLORREF a brush fills with on @p hdc (DC_BRUSH resolves to the
    /// DC's brush colour). False for NULL/hollow and non-solid brushes.
    bool brush_fill_color(Handle hdc, Handle hbrush, u32& color) const;

    /// CreateFontW — allocate a GDI font handle with parameters
    [[nodiscard]] Handle create_font_w(i32 height, u16 weight, bool italic,
                                       std::u16string face_name = {});

    /// Look up font metadata by handle (for WM_SETFONT)
    static const FontInfo* lookup_font(Handle h);

    /// Look up font info from DC's selected font
    const FontInfo* get_dc_font_info(Handle hdc) const { ApiLock api_lock(*this);
        auto it = m_dc_states.find(hdc);
        if (it == m_dc_states.end()) return nullptr;
        return lookup_font(it->second.selected_font);
    }

    /// A DC's selected font as a real font face, for the exports that expose
    /// raw font data (GetFontData) and glyph-space results (GetGlyphIndices,
    /// GetGlyphOutline). Disengaged when no font on the host can back it.
    [[nodiscard]] std::optional<::vitreo::winapi::dwrite::MatchedFont>
    resolve_dc_font(Handle hdc) const;

    /// Em size in pixels for the DC's selected font, matching the lfHeight
    /// contract: a negative height is the em size, a positive one is the cell
    /// height that em size must fit inside.
    [[nodiscard]] float dc_font_em_pixels(Handle hdc) const;

    /// Access CocoaBridge (for text measurement)
    windowing::CocoaBridge* cocoa_bridge() const { ApiLock api_lock(*this); return m_cocoa_bridge; }

    /// Map lfHeight to a macOS point size
    static float font_height_to_pt(i32 lfHeight);

    /// CreateICW — create information context
    [[nodiscard]] Handle create_ic_w();

    // ---- Palette operations ----

    /// CreatePalette — allocate palette from LOGPALETTE structure
    [[nodiscard]] Handle create_palette(GuestAddr lpLogPalette);

    /// CreateHalftonePalette — 256-entry 6×6×6 cube plus greys for HALFTONE StretchBlt
    [[nodiscard]] Handle create_halftone_palette(Handle hdc);

    /// GetDIBColorTable — copy RGBQUAD entries from the DIB selected into hdc
    [[nodiscard]] u32 get_dib_color_table(Handle hdc, u32 iStart, u32 cEntries,
                                          GuestAddr prgbq);

    /// SelectPalette — associate palette with DC, return previous palette
    [[nodiscard]] Handle select_palette(Handle hdc, Handle hPal, bool forceBackground);

    /// RealizePalette — map logical palette to system palette
    [[nodiscard]] u32 realize_palette(Handle hdc);

    /// GetPaletteEntries — copy palette entries to buffer
    [[nodiscard]] u32 get_palette_entries(Handle hPal, u32 iStart, u32 cEntries, GuestAddr pPE);

    /// SetPaletteEntries — set palette entries from buffer
    [[nodiscard]] u32 set_palette_entries(Handle hPal, u32 iStart, u32 cEntries, GuestAddr pPE);

    /// ResizePalette — resize palette entries array
    [[nodiscard]] bool resize_palette(Handle hPal, u32 cEntries);

    /// GetSystemPaletteEntries — return system palette entries
    [[nodiscard]] u32 get_system_palette_entries(Handle hdc, u32 iStart, u32 cEntries, GuestAddr pPE);

    /// SetSystemPaletteUse — set SYSPAL_NOSTATIC/SYSPAL_STATIC mode
    [[nodiscard]] u32 set_system_palette_use(Handle hdc, u32 uUsage);

    /// GetSystemPaletteUse — return the current system palette mode
    [[nodiscard]] u32 get_system_palette_use(Handle hdc) const;

    // ---- Pen & Line operations ----

    /// CreatePen — allocate a GDI pen with style, width, color
    [[nodiscard]] Handle create_pen(i32 style, i32 width, u32 color);

    /// MoveToEx — set current position in DC, optionally return previous
    bool move_to_ex(Handle hdc, i32 x, i32 y, i32* prev_x, i32* prev_y);

    /// LineTo — draw line from current position to (x,y) using selected pen
    bool line_to(Handle hdc, i32 x, i32 y);

    /// SetPixel — set a single pixel in the DC's bitmap
    u32 set_pixel(Handle hdc, i32 x, i32 y, u32 color);

    /// SetROP2 — set drawing mode, return previous
    i32 set_rop2(Handle hdc, i32 mode);

    // ---- Pixel operations ----

    /// SetDIBitsToDevice — decode DIB from guest memory and write to DC's bitmap
    i32 set_dib_bits_to_device(Handle hdc, i32 xDest, i32 yDest, u32 w, u32 h,
                               i32 xSrc, i32 ySrc, u32 startScan, u32 cLines,
                               GuestAddr bits, GuestAddr bmi, u32 colorUse);

    /// SetDIBits — decode DIB into an HBITMAP (not a device DC).
    i32 set_dib_bits(Handle hdc, Handle hbm, u32 startScan, u32 cLines,
                     GuestAddr bits, GuestAddr bmi, u32 colorUse);

    /// BitBlt — copy rectangle between DCs
    bool bit_blt(Handle hdcDest, i32 x, i32 y, i32 cx, i32 cy,
                 Handle hdcSrc, i32 x1, i32 y1, u32 rop);

    /// AlphaBlend (msimg32) — stretch-blit @p hdcSrc onto @p hdcDest applying a
    /// per-pixel and/or constant alpha.
    /// @param src_const_alpha BLENDFUNCTION.SourceConstantAlpha (0..255).
    /// @param src_has_alpha   BLENDFUNCTION.AlphaFormat == AC_SRC_ALPHA, i.e.
    ///                        the source carries premultiplied per-pixel alpha.
    /// Lives here rather than in msimg32 because it needs DC/bitmap internals,
    /// exactly like bit_blt.
    bool alpha_blend(Handle hdcDest, i32 xDest, i32 yDest, i32 wDest, i32 hDest,
                     Handle hdcSrc, i32 xSrc, i32 ySrc, i32 wSrc, i32 hSrc,
                     u8 src_const_alpha, bool src_has_alpha);

    /// UpdateLayeredWindow composition: the window surface behind @p hdcWin
    /// is REPLACED (not blended) by the @p width x @p height source rectangle
    /// at (@p xSrc, @p ySrc), as premultiplied colour + window opacity.
    /// @p opaque: ULW_OPAQUE or no ULW_ALPHA (alpha 255, colour as is).
    /// Otherwise BLENDFUNCTION: colour and alpha scale by @p const_alpha;
    /// with @p per_pixel (AC_SRC_ALPHA) the source alpha is the opacity,
    /// without it the opacity is @p const_alpha. Window pixels the source
    /// rectangle does not cover are transparent.
    bool compose_layered_window(Handle hdcWin, Handle hdcSrc, i32 xSrc, i32 ySrc,
                                i32 width, i32 height, u8 const_alpha,
                                bool per_pixel, bool opaque);

    /// AlphaBlend with its full parameter checks. Returns 0 on success, the
    /// last error to set (ERROR_INVALID_PARAMETER for parameter rejections,
    /// s3gdi_blit_probe "AB ..."), or 0xFFFFFFFF for a failure that leaves
    /// the last error alone (an unknown DC).
    [[nodiscard]] u32 alpha_blend_checked(Handle hdcDest, i32 xDest, i32 yDest, i32 wDest,
                                          i32 hDest, Handle hdcSrc, i32 xSrc, i32 ySrc,
                                          i32 wSrc, i32 hSrc, u8 blend_op, u8 const_alpha,
                                          u8 alpha_format, bool strict = true);

    /// GradientFill (msimg32). Returns 0 on success, the last error to set,
    /// or 0xFFFFFFFF for a failure that leaves the last error alone.
    [[nodiscard]] u32 gradient_fill(Handle hdc, GuestAddr vertices, u32 vertex_count,
                                    GuestAddr mesh, u32 mesh_count, u32 mode);

    /// TransparentBlt (msimg32): copy all but @p key-coloured pixels.
    bool transparent_blt(Handle hdcDest, i32 xd, i32 yd, i32 wd, i32 hd, Handle hdcSrc,
                         i32 xs, i32 ys, i32 ws, i32 hs, u32 key);

    /// PatBlt — fill rectangle with selected brush using raster op
    bool pat_blt(Handle hdc, i32 x, i32 y, i32 cx, i32 cy, u32 rop);

    /// SetStretchBltMode / GetStretchBltMode. set returns the previous mode,
    /// 0 for an unknown DC or a mode outside 1..4.
    i32 set_stretch_blt_mode(Handle hdc, i32 mode);
    [[nodiscard]] i32 get_stretch_blt_mode(Handle hdc) const;

    /// SetBrushOrgEx — set brush origin for DC
    bool set_brush_org_ex(Handle hdc, i32 x, i32 y);

    /// SaveDC — save device context state, returns save level (1+) or 0 on failure
    [[nodiscard]] i32 save_dc(Handle hdc);

    /// RestoreDC — restore device context state
    /// nSavedDC > 0: restore to absolute level; < 0: relative (-1 = most recent)
    bool restore_dc(Handle hdc, i32 nSavedDC);

    /// GetBkMode — returns current background mode (TRANSPARENT=1, OPAQUE=2)
    [[nodiscard]] i32 get_bk_mode(Handle hdc);

    /// GetTextColor — returns current text color
    [[nodiscard]] u32 get_text_color(Handle hdc);

    /// GetBkColor — the DC's background COLORREF, or CLR_INVALID.
    [[nodiscard]] u32 get_bk_color(Handle hdc) const { ApiLock api_lock(*this);
        auto it = m_dc_states.find(hdc);
        return it == m_dc_states.end() ? 0xFFFFFFFFu : it->second.bk_color;
    }

    /// The DC's selected font handle (0 when the DC is unknown).
    [[nodiscard]] Handle selected_font(Handle hdc) const { ApiLock api_lock(*this);
        auto it = m_dc_states.find(hdc);
        return it == m_dc_states.end() ? Handle{0} : it->second.selected_font;
    }

    /// Viewport origin, for callers that must render at an offset through the
    /// DC's own mapping (DrawState's DST_COMPLEX callback). False when the DC
    /// is unknown.
    bool get_viewport_org(Handle hdc, i32& x, i32& y) const { ApiLock api_lock(*this);
        auto it = m_dc_states.find(hdc);
        if (it == m_dc_states.end()) return false;
        x = it->second.viewport_org_x;
        y = it->second.viewport_org_y;
        return true;
    }
    bool set_viewport_org(Handle hdc, i32 x, i32 y) { ApiLock api_lock(*this);
        auto it = m_dc_states.find(hdc);
        if (it == m_dc_states.end()) return false;
        it->second.viewport_org_x = x;
        it->second.viewport_org_y = y;
        return true;
    }

    /// The DC's clip region (device units; !valid = none). False for an
    /// unknown DC.
    bool get_dc_clip(Handle hdc, ClipRect& out) const { ApiLock api_lock(*this);
        auto it = m_dc_states.find(hdc);
        if (it == m_dc_states.end()) return false;
        out = it->second.clip_rect;
        return true;
    }

    /// Window origin (SetWindowOrgEx without the guest pointer).
    bool set_window_org(Handle hdc, i32 x, i32 y) { ApiLock api_lock(*this);
        auto it = m_dc_states.find(hdc);
        if (it == m_dc_states.end()) return false;
        it->second.window_org_x = x;
        it->second.window_org_y = y;
        return true;
    }

    /// GetTextMetricsW — retrieve text metrics for selected font
    bool get_text_metrics_w(Handle hdc, GuestAddr lptm);

    /// GetOutlineTextMetricsW/A — TrueType outline metrics plus name strings.
    /// Learn: null `potm` returns the required buffer size; otherwise the
    /// function fills `cjCopy` bytes and returns a nonzero size.
    [[nodiscard]] u32 get_outline_text_metrics(
        Handle hdc, u32 cj_copy, GuestAddr potm, bool wide, bool is_64bit);

    // ---- Text engine (text_engine.cpp) ----

    /// A logical font realised for drawing: the face GDI's mapper picked and
    /// its device metrics at the font's pixel size.
    struct TextFace {
        ::vitreo::winapi::dwrite::MatchedFont font;
        u32 ppem = 16;
        double scale = 1.0;          ///< pixels per design unit
        i32 height = 0, ascent = 0, descent = 0;
        i32 internal_leading = 0, external_leading = 0;
        i32 ave_width = 0, max_width = 0, weight = 400;
        u16 first_char = 32, last_char = 0xFFFF, default_char = 31, break_char = 32;
        u8 italic = 0, underlined = 0, struck_out = 0, pitch_and_family = 0, char_set = 0;
        i32 underline_position = 0, underline_thickness = 1;
        i32 strike_position = 0, strike_size = 1;
        std::vector<u8> device_widths;  ///< hdmx advances by glyph, when present
        std::u16string reported_face;   ///< GetTextFace
        bool antialias = true;
        bool simulated_bold = false;   ///< bold requested, face has none
    };
    struct TextRun {
        std::shared_ptr<const ::vitreo::winapi::dwrite::MatchedFont> font;
        std::vector<u16> glyphs;
        std::vector<i32> advances;
    };
    struct TextLayout {
        std::vector<TextRun> runs;
        std::vector<i32> unit_advances;  ///< per UTF-16 unit (0 for a trail surrogate)
        i32 width = 0;
        bool used_fallback = false;
    };
    /// The DC's current position (MoveToEx), false for an unknown DC.
    bool current_position(Handle hdc, i32& x, i32& y) const { ApiLock api_lock(*this);
        const auto it = m_dc_states.find(hdc);
        if (it == m_dc_states.end()) return false;
        x = it->second.current_x;
        y = it->second.current_y;
        return true;
    }
    /// True for a DC this manager created.
    [[nodiscard]] bool has_dc(Handle hdc) const { ApiLock api_lock(*this); return m_dc_states.contains(hdc); }
    /// The DC's selected font as realised by the mapper; nullptr for an
    /// unknown DC or when no host face can back it.
    std::shared_ptr<const TextFace> text_face(Handle hdc) const;
    /// Advances of @p text in the face (hdmx device widths, fallback faces
    /// for characters it lacks) plus @p char_extra per character.
    bool layout_text(const TextFace& face, std::u16string_view text, i32 char_extra,
                     TextLayout& out) const;
    /// ExtTextOut on a known DC. @p rect is logical; @p dx, when given,
    /// replaces the per-character advances.
    bool ext_text_out(Handle hdc, i32 x, i32 y, u32 options, const ClipRect* rect,
                      std::u16string_view text, const std::vector<i32>* dx, bool glyph_indices);
    /// TEXTMETRICW (60 bytes) or TEXTMETRICA (53 bytes) for the DC's font.
    bool write_text_metrics(Handle hdc, GuestAddr out, bool wide);
    /// The same TEXTMETRICW/A bytes in a host buffer.
    bool text_metrics_bytes(Handle hdc, bool wide, u8 (&buf)[60]) const;
    /// GetTextExtentPoint32: width and height; @p cumulative gets the running
    /// width after each UTF-16 unit.
    bool text_extent(Handle hdc, std::u16string_view text, i32& width, i32& height,
                     std::vector<i32>* cumulative = nullptr, bool* used_fallback = nullptr);
    /// Glyph-index advances (and A/B/C spacing).
    std::optional<std::vector<i32>> glyph_advances(Handle hdc, const std::vector<u16>& glyphs,
                                                   std::vector<std::array<i32, 3>>* abc);
    /// Character advances (and A/B/C spacing) through the face's cmap.
    std::optional<std::vector<i32>> char_advances(Handle hdc, const std::vector<u32>& codepoints,
                                                  std::vector<std::array<i32, 3>>* abc);
    /// SetTextCharacterExtra / GetTextCharacterExtra (0x80000000 for a bad DC).
    i32 set_text_character_extra(Handle hdc, i32 extra);
    [[nodiscard]] i32 get_text_character_extra(Handle hdc) const;
    /// SetTextAlign / GetTextAlign (GDI_ERROR for a bad DC).
    u32 set_text_align_mode(Handle hdc, u32 align);
    [[nodiscard]] u32 get_text_align_mode(Handle hdc) const;
    /// AddFontMemResourceEx: 0 when the data holds no font.
    Handle add_font_mem_resource(std::span<const u8> data, u32* count);
    bool remove_font_mem_resource(Handle h);
    /// AddFontResourceEx: fonts added from a guest path, 0 with @p error.
    i32 add_font_resource(const std::u16string& path, u32* error);
    bool remove_font_resource(const std::u16string& path, u32* error);
    /// Wire the capsule filesystem AddFontResource reads through.
    void set_vfs(::vitreo::capsule::Vfs* vfs) { ApiLock api_lock(*this); m_vfs = vfs; }
    /// EnumFontFamiliesEx: calls @p proc (FONTENUMPROC) for the faces
    /// matching @p face (empty: one per family) and @p charset (1: every
    /// charset); the last callback's value, 1 when none ran, 0 for a bad DC.
    i32 enum_font_families_ex(Handle hdc, const std::u16string& face, u8 charset, GuestAddr proc, u64 lparam,
                              bool wide, bool is_64bit);
    /// Enhanced metafiles as record streams (metafile_ops.cpp).
    [[nodiscard]] Handle set_enh_metafile_bits(std::span<const u8> data);
    [[nodiscard]] const std::vector<u8>* enh_metafile(Handle h) const;
    bool delete_enh_metafile(Handle h);
    i32 enum_enh_metafile(Handle hdc, Handle emf, GuestAddr proc, u64 data, bool is_64bit);
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;
    /// Calls into guest code (EnumFontFamiliesEx callbacks).
    void set_guest_call(GuestCall call) { ApiLock api_lock(*this); m_guest_call = std::move(call); }
    /// The same caller, for GDI+ (IStream methods, EnumerateMetafile procs).
    [[nodiscard]] GuestCall guest_call() const { ApiLock api_lock(*this); return m_guest_call; }
    /// The FontSubstitutes alias of @p face, or @p face.
    [[nodiscard]] static std::u16string substitute_face(const std::u16string& face);
    // ---- Paths, polygons, transforms (geometry_ops.cpp) ----
    /// A path point in device coordinates with its PT_* type byte
    /// (PT_MOVETO 6, PT_LINETO 2, PT_BEZIERTO 4, | PT_CLOSEFIGURE 1).
    struct PathPoint {
        double x{0};
        double y{0};
        u8 type{0};
    };
    /// The DC's path bracket (BeginPath .. EndPath).
    struct PathState {
        i32 state{0};  ///< 0 none, 1 open (being recorded), 2 closed (EndPath)
        std::vector<PathPoint> points;
        bool figure_open{false};  ///< a MOVETO started a figure not yet closed
    };

    using IPoint = std::pair<i32, i32>;
    /// Polyline / PolylineTo / Polygon / PolyPolygon / PolyBezier /
    /// PolyBezierTo: drawn, or recorded while a path is open. False (error
    /// untouched) for a bad DC or too few points.
    bool polyline(Handle hdc, const std::vector<IPoint>& pts);
    bool polyline_to(Handle hdc, const std::vector<IPoint>& pts);
    bool poly_polygon(Handle hdc, const std::vector<IPoint>& pts, const std::vector<i32>& counts);
    bool poly_bezier(Handle hdc, const std::vector<IPoint>& pts, bool to);
    /// Path recording for MoveToEx / LineTo / Rectangle / Ellipse /
    /// RoundRect: true when a path is open and the call was recorded.
    bool path_record_move(Handle hdc, i32 x, i32 y);
    bool path_record_line(Handle hdc, i32 x, i32 y);
    bool path_record_rect(Handle hdc, i32 l, i32 t, i32 r, i32 b);
    bool path_record_ellipse(Handle hdc, i32 l, i32 t, i32 r, i32 b);
    bool path_record_round_rect(Handle hdc, i32 l, i32 t, i32 r, i32 b, i32 w, i32 h);
    bool begin_path(Handle hdc);
    bool end_path(Handle hdc);
    bool abort_path(Handle hdc);
    bool close_figure(Handle hdc);
    bool fill_path(Handle hdc);
    bool stroke_path(Handle hdc);
    bool stroke_and_fill_path(Handle hdc);
    bool flatten_path(Handle hdc);
    bool widen_path(Handle hdc);
    bool select_clip_path(Handle hdc, i32 mode);
    [[nodiscard]] Handle path_to_region(Handle hdc);
    /// GetPath: the closed path's points in logical units; -1 when there is
    /// no closed path.
    i32 get_path(Handle hdc, std::vector<IPoint>* pts, std::vector<u8>* types);
    /// Regions from shapes, sampled like GDI's polygon fill.
    [[nodiscard]] Handle create_polygon_region(const std::vector<IPoint>& pts, const std::vector<i32>& counts,
                                               i32 fill_mode);
    [[nodiscard]] Handle create_elliptic_region(i32 l, i32 t, i32 r, i32 b);
    [[nodiscard]] Handle create_round_rect_region(i32 l, i32 t, i32 r, i32 b, i32 w, i32 h);
    /// SetRectRgn with GDI's normalisation.
    bool set_rect_region(Handle hrgn, i32 l, i32 t, i32 r, i32 b);
    /// OffsetClipRgn (logical offset): the new clip's type, SIMPLEREGION
    /// without a clip, RGN_ERROR for a bad DC.
    i32 offset_clip_rgn(Handle hdc, i32 dx, i32 dy);
    /// GetClipRgn: 1 and a copy of the clip region (device units), 0 when
    /// there is none, -1 on error.
    i32 get_clip_region(Handle hdc, Handle hrgn);
    /// DPtoLP / LPtoDP (round half to even, "dptolp -> -18,-29 32,4").
    bool dp_to_lp_points(Handle hdc, std::vector<IPoint>& pts) const;
    bool lp_to_dp_points(Handle hdc, std::vector<IPoint>& pts) const;
    /// SetMapMode: previous mode, 0 for a bad DC or mode.
    i32 set_map_mode(Handle hdc, i32 mode);
    [[nodiscard]] i32 get_map_mode(Handle hdc) const;
    /// Set{Window,Viewport}{Ext,Org}Ex; @p old receives the previous value.
    bool set_window_ext(Handle hdc, i32 x, i32 y, IPoint* old);
    bool set_viewport_ext(Handle hdc, i32 x, i32 y, IPoint* old);
    bool get_window_ext(Handle hdc, IPoint& out) const;
    bool get_viewport_ext(Handle hdc, IPoint& out) const;
    /// World transform (XFORM as six floats).
    bool get_world_transform(Handle hdc, std::array<float, 6>& out) const;
    bool set_world_transform(Handle hdc, const std::array<float, 6>& xf);
    bool modify_world_transform(Handle hdc, const std::array<float, 6>* xf, i32 mode);
    i32 set_graphics_mode(Handle hdc, i32 mode);
    /// SetMiterLimit / GetMiterLimit / SetArcDirection / GetArcDirection.
    bool set_miter_limit(Handle hdc, float limit, float* old);
    bool get_miter_limit(Handle hdc, float& out) const;
    i32 set_arc_direction(Handle hdc, i32 dir);
    [[nodiscard]] i32 get_arc_direction(Handle hdc) const;
    /// GetDeviceCaps for a DC (0 for a bad DC or index).
    [[nodiscard]] i32 device_caps(Handle hdc, i32 index) const;

    /// DRAWTEXTPARAMS (DrawTextEx); tab_length 0 means the default.
    struct DrawTextParams {
        i32 tab_length = 0;
        i32 left_margin = 0;
        i32 right_margin = 0;
        u32 length_drawn = 0;
    };
    /// DrawText/DrawTextEx on a known DC: the height of the text (the
    /// offset of its bottom from the rectangle's top for a vertically
    /// aligned single line), 0 on failure. The rectangle is updated for
    /// DT_CALCRECT.
    i32 draw_text_ex(Handle hdc, std::u16string_view text, i32& left, i32& top, i32& right,
                     i32& bottom, u32 format, DrawTextParams* params);

    /// GetPixel — read a pixel's color from the DC's bitmap
    u32 get_pixel(Handle hdc, i32 x, i32 y);

    /// StretchBlt — stretch-copy rectangle between DCs
    bool stretch_blt(Handle hdcDest, i32 xd, i32 yd, i32 wd, i32 hd,
                     Handle hdcSrc, i32 xs, i32 ys, i32 ws, i32 hs, u32 rop);

    /// GetDCOrgEx — get DC origin in screen coords
    bool get_dc_org_ex(Handle hdc, GuestAddr lppt);

    /// StretchDIBits — copy DIB pixels to DC
    i32 stretch_dib_bits(Marshaler& m, Handle hdcDest,
                         i32 xDest, i32 yDest, i32 wDest, i32 hDest,
                         i32 xSrc, i32 ySrc, i32 wSrc, i32 hSrc,
                         GuestAddr lpBits, GuestAddr lpBMI, u32 colorUsage,
                         u32 dwRop);

    /// Flush window framebuffer to Cocoa display
    /// Reset a layered window's framebuffer to fully transparent.
    /// UpdateLayeredWindow *replaces* a layered window's content rather than
    /// compositing onto it, so neither the previous frame nor the opaque-white
    /// default a framebuffer starts life with may survive underneath.
    void clear_window_framebuffer(u64 hwnd);

    /// Diagnostic: report a DC bitmap's alpha/colour distribution.
    void debug_report_dc_alpha(Handle hdc);

    void flush_window(u64 hwnd);
    bool flush_pending_pixels();

    /// Ensure the window framebuffer is at least the given size
    void ensure_framebuffer_size(u64 hwnd, i32 w, i32 h);


    /// TextOutW — render text at (x,y) in the DC's framebuffer
    bool text_out(Handle hdc, i32 x, i32 y, const std::string& utf8_text);

    /// Measure a string with the DC's currently-selected font via Core Text, so
    /// GetTextExtentPoint*/GetTextExtentExPoint return the width the text will
    /// actually occupy when rendered with the macOS font (callers like PuTTY use
    /// this to lay out controls; a fixed-width guess drifts the layout).
    void get_text_extent(Handle hdc, const std::string& utf8_text,
                         i32& out_width, i32& out_height);

    /// DrawText — render text inside a rectangle with formatting flags
    /// @param rc  Bounding rectangle (updated on DT_CALCRECT)
    /// @param format  DT_* flags (DT_CENTER=1, DT_VCENTER=4, DT_SINGLELINE=0x20, etc.)
    /// @return Height of the rendered text (or calculated rect height)
    i32 draw_text(Handle hdc, const std::string& utf8_text, i32& left, i32& top,
                  i32& right, i32& bottom, u32 format);

    // ---- Clipping region operations ----

    /// CreateRectRgn — create a rectangular region
    [[nodiscard]] Handle create_rect_rgn(i32 left, i32 top, i32 right, i32 bottom);

    /// Look up a region from CreateRectRgn*. Returns false if unknown.
    [[nodiscard]] bool get_region_rect(Handle hrgn, ClipRect& out) const;

    /// SelectClipRgn — select region as clipping region
    /// Returns region complexity (SIMPLEREGION=2, NULLREGION=1, ERROR=0)
    i32 select_clip_rgn(Handle hdc, Handle hrgn);

    /// ExtSelectClipRgn — combine region with clip region using mode
    /// fnMode: RGN_AND=1, RGN_OR=2, RGN_XOR=3, RGN_DIFF=4, RGN_COPY=5
    i32 ext_select_clip_rgn(Handle hdc, Handle hrgn, i32 fnMode);

    /// ExcludeClipRect — exclude rectangle from clipping region
    /// Returns region complexity
    i32 exclude_clip_rect(Handle hdc, i32 left, i32 top, i32 right, i32 bottom);

    /// IntersectClipRect — intersect clip region with rectangle
    /// Returns region complexity
    i32 intersect_clip_rect(Handle hdc, i32 left, i32 top, i32 right, i32 bottom);

    /// GetClipBox — get bounding rectangle of clip region
    /// Returns region complexity
    i32 get_clip_box(Handle hdc, i32& left, i32& top, i32& right, i32& bottom);

    /// PtVisible — check if point is in clipping region
    [[nodiscard]] bool pt_visible(Handle hdc, i32 x, i32 y);

    /// CreateDIBSection — create a DIB with direct pixel access
    /// @param bmi_addr  Guest pointer to BITMAPINFO
    /// @param usage     DIB_RGB_COLORS=0 or DIB_PAL_COLORS=1
    /// @param ppvBits   Guest pointer where the pixel data address will be written
    /// @return Bitmap handle, or 0 on failure
    /// CreateDIBSection. `ppvBits` is `VOID**` in the guest, so the pointer
    /// written back is pointer-sized: 8 bytes on x64, 4 on x86.
    [[nodiscard]] Handle create_dib_section(Handle hdc, GuestAddr bmi_addr,
                                            u32 usage, GuestAddr ppvBits,
                                            bool guest_is_64bit = false,
                                            u64 section = 0, u32 offset = 0,
                                            u32* error = nullptr);

    /// A BI_RGB DIB section the runtime creates for itself (uxtheme buffered
    /// paint): 32 bpp, or 1 bpp with the black/white colour table. The bits
    /// live in guest memory; their address is stored in @p bits_out.
    [[nodiscard]] Handle create_runtime_dib_section(i32 width, i32 height, bool bottom_up, u16 bpp,
                                                    GuestAddr* bits_out);

    /// Ellipse — draw an ellipse bounded by the given rectangle
    bool ellipse(Handle hdc, i32 left, i32 top, i32 right, i32 bottom);

    /// Scratch allocator for DIB section pixel data in guest memory
    static constexpr GuestAddr GDI_DIB_SCRATCH_BASE = 0x5E100000;
    // Ends at 0x5ED00000, leaving a 3 MB guard before shell32's allocator.
    // Layered applications commonly retain several full-window DIBs while
    // composing the next frame, so 4 MB was not sufficient for 738x468 BGRA.
    static constexpr u32       GDI_DIB_SCRATCH_SIZE = 0xC00000; // 12 MB

    /// DIBs too large for the fixed arena (a full-screen 32-bpp surface on a
    /// modern display is already bigger than all 12 MB of it) get their own
    /// mapping instead of failing. Keyed by address so dib_scratch_free can
    /// tell them apart from arena blocks and release them.
    std::unordered_map<GuestAddr, u32> m_dib_large_blocks;

    /// Allocate guest memory from the DIB scratch region
    GuestAddr dib_scratch_alloc(u32 size) noexcept;
    void dib_scratch_free(GuestAddr addr, u32 size) noexcept;

private:
    /// Every GDI handle comes from here: at least kFirstGdiHandle.
    [[nodiscard]] Handle allocate_gdi_handle(HandleType type) {
        return m_handles.allocate_at_least(kFirstGdiHandle, type, nullptr);
    }

    struct PresentationState {
        std::mutex lifetime_mutex;
        GdiManager* owner = nullptr;
        std::function<void(std::function<void()>)> schedule;
    };
    std::shared_ptr<PresentationState> m_presentation;
    mutable bool m_presentation_pending = false;
    std::unordered_set<u64> m_presenting_windows;
    void request_presentation_locked() const;
    static void present_scheduled(std::weak_ptr<PresentationState> state);
    mutable std::recursive_mutex m_api_mutex;
    HandleTable& m_handles;
    GuestMemory* m_guest_memory{nullptr};
    windowing::CocoaBridge* m_cocoa_bridge{nullptr};
    i32 m_virtual_screen_width{0};
    i32 m_virtual_screen_height{0};
    windowing::HwndManager* m_hwnd_mgr{nullptr};
    /// True when hwnd is a WS_CHILD window (SoftBitmap::child_window).
    [[nodiscard]] bool is_child_window(u64 hwnd) const;

    /// Lazily convert a CocoaBridge bitmap (0x600xxxxx) to a SoftBitmap
    std::shared_ptr<SoftBitmap> cocoa_bitmap_to_soft(Handle handle);

    /// Win32 XFORM (six IEEE floats) for GM_ADVANCED world transforms.
    struct WorldXform {
        float eM11{1.0f};
        float eM12{0.0f};
        float eM21{0.0f};
        float eM22{1.0f};
        float eDx{0.0f};
        float eDy{0.0f};

        [[nodiscard]] static WorldXform identity() { return {}; }
    };

    struct DcState {
        u64    associated_hwnd{0};  ///< Non-zero for window DCs
        bool   screen_dc{false};    ///< GetDC(NULL), associated with desktop
        bool   memory_dc{false};    ///< CreateCompatibleDC (OBJ_MEMDC)
        u32    dc_brush_color{0x00FFFFFF}; ///< SetDCBrushColor (DC_BRUSH)
        u32    dc_pen_color{0};            ///< SetDCPenColor (DC_PEN)
        i32    stretch_mode{1};            ///< SetStretchBltMode (BLACKONWHITE)
        std::shared_ptr<SoftBitmap> bitmap; ///< Current drawing surface
        Handle selected_brush{0};
        Handle selected_pen{0};
        Handle selected_font{0};
        Handle selected_palette{0};        ///< Selected palette handle
        Handle selected_bitmap_handle{0}; ///< For SelectObject return value
        i32    bk_mode{2};          // OPAQUE (Win32: TRANSPARENT=1, OPAQUE=2)
        u32    text_color{0};       // Black
        u32    bk_color{0xFFFFFF};  // White
        i32    current_x{0};        ///< Current pen position X (MoveToEx/LineTo)
        i32    current_y{0};        ///< Current pen position Y
        i32    rop2{13};            ///< Drawing mode (R2_COPYPEN=13)
        i32    brush_org_x{0};      ///< Brush origin X
        i32    brush_org_y{0};      ///< Brush origin Y
        ClipRect clip_rect;         ///< Current clipping region
        u32    text_align{0};       ///< SetTextAlign mode
        i32    char_extra{0};       ///< SetTextCharacterExtra
        i32    map_mode{1};         ///< Mapping mode (1=MM_TEXT)
        i32    graphics_mode{1};    ///< GM_COMPATIBLE=1, GM_ADVANCED=2
        WorldXform world_xform{};   ///< World transform (GM_ADVANCED)
        i32    poly_fill_mode{1};   ///< Polygon fill mode (1=ALTERNATE)
        i32    viewport_org_x{0};   ///< Viewport origin X
        i32    viewport_org_y{0};   ///< Viewport origin Y
        i32    viewport_ext_x{1};   ///< Viewport extent X
        i32    viewport_ext_y{1};   ///< Viewport extent Y
        i32    window_org_x{0};     ///< Window origin X
        i32    window_org_y{0};     ///< Window origin Y
        i32    window_ext_x{1};     ///< Window extent X
        i32    window_ext_y{1};     ///< Window extent Y
        PathState path;             ///< BeginPath/EndPath
        float  miter_limit{10.0f};  ///< SetMiterLimit
        i32    arc_direction{1};    ///< AD_COUNTERCLOCKWISE
        /// SaveDC/RestoreDC stack: whole copies of the saved state (Learn:
        /// SaveDC saves the selected objects, modes, clip region, path and
        /// transforms).
        std::vector<DcState> state_stack;
    };

    std::unordered_map<Handle, DcState> m_dc_states;

    /// A DC resolved for drawing (raster_ops.cpp).
    struct RasterTarget {
        Handle hdc{0};
        DcState* dc{nullptr};
        SoftBitmap* surface{nullptr};
        bool mono{false};
        u8 alpha{0};
        Handle dib{0};  ///< selected DIB section, re-synced after drawing
    };
    /// Logical -> device through the world (GM_ADVANCED) and page transforms.
    void lp_to_dp(const DcState& dc, double x, double y, double& ox, double& oy) const;
    void lp_to_dp_point(const DcState& dc, i32& x, i32& y) const;
    void lp_to_dp_rect(const DcState& dc, i32& l, i32& t, i32& r, i32& b) const;
    bool raster_target(Handle hdc, RasterTarget& out);
    void finish_raster(const RasterTarget& t);
    [[nodiscard]] bool raster_writable(const RasterTarget& t, i32 x, i32 y) const;
    void raster_store(const RasterTarget& t, i32 x, i32 y, u32 value) const;
    /// The brush's pixel word at device (x, y), realised for the surface.
    /// False for a hollow or unknown brush.
    bool brush_pattern_word(const RasterTarget& t, Handle hbrush, i32 x, i32 y,
                            u32& out) const;
    /// The selected pen's colour and width; false for PS_NULL.
    bool pen_word(const RasterTarget& t, u32& value, i32& width) const;
    void plot_pen(const RasterTarget& t, i32 x, i32 y, u32 pen, i32 width);
    /// A LineTo segment in device pixels, the last point excluded.
    void draw_line_pixels(const RasterTarget& t, i32 x0, i32 y0, i32 x1, i32 y1, u32 pen, i32 width);
    /// SetROP2 mix of a pen/brush word with the destination.
    [[nodiscard]] static u32 rop2_mix(i32 mode, u32 p, u32 d);
    /// Start a path figure at (cx, cy) when none is open.
    static void path_ensure_figure(DcState& dc, double cx, double cy);
    /// The DC's whole surface as a region (the clip when none is set).
    [[nodiscard]] ClipRect dc_surface_region(const DcState& dc) const;
    /// Device <- logical through the DC's world and page transforms, and
    /// the inverse; false for a singular transform.
    bool dp_to_lp_d(const DcState& dc, double x, double y, double& ox, double& oy) const;
    /// The shared stretch/ROP loop (raster_ops.cpp).
    template <typename Sample, typename Pattern>
    void stretch_core(const RasterTarget& t, i32 mode, u8 index, i32 dxo, i32 dyo, i32 dw,
                      i32 dh, i32 sxo, i32 syo, i32 sw, i32 sh, Sample&& sample,
                      Pattern&& pattern);
    /// StretchDIBits / SetDIBitsToDevice core: decode the DIB rows the
    /// caller supplied and blit them. @p scan_available maps a from-bottom
    /// scan to its buffer row, or -1.
    bool blit_dib(Handle hdc, i32 dxo, i32 dyo, i32 dw, i32 dh, i32 xsrc, i32 ysrc, i32 sw,
                  i32 sh, GuestAddr bits, const gdi::DibHeader& hdr,
                  const gdi::PixelFormat& format, u32 first_scan, u32 scan_count, u8 index);
    /// The surface a blit reads from @p dc, resolving a screen DC to the
    /// window under the rectangle.
    std::shared_ptr<SoftBitmap> resolve_screen_source(DcState& dc, i32& sx, i32& sy,
                                                      i32 blit_cx, i32 blit_cy);

    std::unordered_map<Handle, i32> m_pixel_formats;
    bool m_pixel_format_ever_set{false};
    std::function<bool(Handle)> m_swap_buffers_handler;
    std::unordered_map<i32, Handle>     m_stock_objects;
    std::unordered_set<Handle>          m_stock_handles;

    /// Bitmap objects: handle → pixel buffer
    std::unordered_map<Handle, std::shared_ptr<SoftBitmap>> m_bitmap_objects;

    /// Pen objects: handle → pen info (style, width, color)
    std::unordered_map<Handle, PenInfo> m_pen_objects;

    /// Brush objects: handle → COLORREF (solid color)
    std::unordered_map<Handle, u32> m_brush_colors;
    /// Every brush's LOGBRUSH (solid, hatched, pattern), for GetObject and
    /// hatch rendering.
    std::unordered_map<Handle, BrushInfo> m_brush_info;
    /// ExtCreatePen pens (OBJ_EXTPEN). Each also has a PenInfo in
    /// m_pen_objects so line drawing needs no second path.
    std::unordered_map<Handle, ExtPenInfo> m_extpen_objects;
    /// The DC a non-stock bitmap is selected into. Windows lets a bitmap be
    /// selected into one DC at a time (s3gdi_bitmap_probe).
    std::unordered_map<Handle, Handle> m_bitmap_owner_dc;
    /// Pattern brushes own a snapshot independent of the source bitmap.
    std::unordered_map<Handle, std::shared_ptr<SoftBitmap>> m_brush_patterns;

    /// Region objects: handle → ClipRect
    std::unordered_map<Handle, ClipRect> m_region_objects;

    /// Palette objects: handle → palette info
    std::unordered_map<Handle, PaletteInfo> m_palette_objects;

    /// Memory-based Windows-format metafiles created by SetMetaFileBitsEx.
    std::unordered_map<Handle, std::vector<u8>> m_metafile_objects;

    /// System palette use mode (SYSPAL_STATIC=1, SYSPAL_NOSTATIC=2)
    u32 m_system_palette_use{1};  // Default: SYSPAL_STATIC

    /// DIB section tracking — maps bitmap handle to guest pixel buffer info
    struct DIBSectionInfo {
        GuestAddr guest_bits{0};  ///< Guest address of pixel data
        i32 width{0};
        i32 height{0};
        u16 bpp{0};
        u32 stride{0};           ///< Row stride in bytes (DWORD-aligned)
        bool bottom_up{true};    ///< true = positive biHeight (bottom-up DIB)
        u32 compression{0};      ///< BI_RGB or BI_BITFIELDS as created
        /// Effective R/G/B masks for 16/32bpp: the BI_BITFIELDS masks, or the
        /// BI_RGB defaults (555 for 16bpp, 0 for 32bpp - DIBSECTION reports
        /// dsBitfields 7c00/3e0/1f and 0/0/0 respectively).
        std::array<u32, 3> masks{};
        u32 clr_used{0};
        u32 clr_important{0};
        i32 x_pels_per_meter{0};
        i32 y_pels_per_meter{0};
        u64 section{0};          ///< hSection argument (dshSection)
        u32 offset{0};           ///< dwOffset argument (dsOffset)
        std::vector<u8> color_table; ///< RGBQUAD palette for indexed formats
        Handle logical_palette{0}; ///< Palette used by DIB_PAL_COLORS
        std::vector<u16> palette_indices; ///< Color-table indexes into logical_palette
    };
    std::unordered_map<Handle, DIBSectionInfo> m_dib_sections;
    u32 m_dib_scratch_offset{0};
    /// Freed DIB scratch blocks, kept sorted by address and coalesced.
    /// A skinned app allocates fresh DIBs while composing each frame, so a pure
    /// bump allocator exhausts even a generously sized region and every later
    /// CreateDIBSection fails.
    std::vector<std::pair<GuestAddr, u32>> m_dib_scratch_free;

    /// Sync DIB section pixels from guest memory into the SoftBitmap
    void sync_dib_from_guest(Handle bitmap_handle);
    /// The scan-line format of a DIB section.
    [[nodiscard]] gdi::PixelFormat dib_pixel_format_of(const DIBSectionInfo& dib) const;
    void sync_dib_to_guest(Handle bitmap_handle);

    /// Colour of a user32 GetSysColorBrush handle; those are stock brushes
    /// on Windows (injected by user32, which owns the system colours).
    std::function<std::optional<u32>(u64)> m_system_brush_color;
    [[nodiscard]] std::optional<u32> system_brush_color(Handle brush) const;

    SectionViewMapper m_map_section_view;
    SectionViewUnmapper m_unmap_section_view;
    /// View bases mapped for section-backed DIB sections, keyed by bitmap.
    std::unordered_map<Handle, GuestAddr> m_dib_section_views;
    /// Objects DeleteObject was called on while selected into a DC: Windows
    /// answers TRUE and frees them once no DC holds them (s3gdi_bitmap_probe
    /// "deferred").
    std::unordered_set<Handle> m_pending_delete;

    /// The surface behind an HBITMAP (importing a CocoaBridge resource
    /// bitmap on first use), or null.
    std::shared_ptr<SoftBitmap> bitmap_surface(Handle hbm);
    /// Create a DIB section; the shared core of CreateDIBSection and
    /// CreateCompatibleBitmap on a DIB-selected DC. @p color_table is BGRX.
    Handle make_dib_section(i32 width, i32 height, bool bottom_up, u16 bpp,
                            u32 compression, const std::array<u32, 3>& masks,
                            std::vector<u8> color_table, Handle logical_palette,
                            std::vector<u16> palette_indices, u32 clr_used,
                            u32 clr_important, u64 section, u32 offset,
                            GuestAddr* bits_out, u32* error);
    /// A DC's selected logical palette as 0x00RRGGBB (the 20-colour default
    /// palette when none is selected), for DIB_PAL_COLORS.
    std::vector<u32> dc_logical_palette(Handle hdc) const;
    /// Re-quantise rows [y0, y1) of a device-dependent bitmap below 32bpp to
    /// the values its format can hold.
    void quantize_ddb_rows(SoftBitmap& bmp, i32 y0, i32 y1) const;
    /// True when @p obj is selected into any DC.
    [[nodiscard]] bool object_is_selected(Handle obj) const;
    /// Free an object's records and handle (DeleteObject's final step).
    void free_object(Handle obj);

    /// Window framebuffers: hwnd → shared pixel buffer
    std::unordered_map<u64, std::shared_ptr<SoftBitmap>> m_window_framebuffers;

    /// Track the actual client area (paint) dimensions per window
    std::unordered_map<u64, std::pair<i32,i32>> m_window_client_size;

    u32 m_font_counter{0};  ///< Counter for CreateFontW fake handles
    /// Font handles this manager put in gdi_font_registry().
    std::unordered_set<Handle> m_owned_fonts;

    /// Realised faces by font handle (dropped when the font is deleted or
    /// the private font set changes).
    mutable std::unordered_map<Handle, std::shared_ptr<const TextFace>> m_text_faces;
    /// Fonts added by AddFontMemResourceEx (source empty) or
    /// AddFontResourceEx (source = folded path).
    struct PrivateFontSet {
        Handle handle{0};
        std::u16string source;
        std::vector<::vitreo::winapi::dwrite::MatchedFont> faces;
    };
    std::vector<PrivateFontSet> m_private_fonts;
    ::vitreo::capsule::Vfs* m_vfs{nullptr};
    GuestCall m_guest_call;
    std::unordered_map<Handle, std::vector<u8>> m_enh_metafiles;
    struct FaceMatch {
        ::vitreo::winapi::dwrite::MatchedFont font;
        std::u16string reported;
    };
    [[nodiscard]] std::optional<FaceMatch> match_face(const std::u16string& requested, u32 weight,
                                                      bool italic) const;
    /// A logical font realised on a matched face.
    [[nodiscard]] std::shared_ptr<TextFace> realize_face(const FontInfo& logical, FaceMatch matched) const;
    bool draw_glyph_runs(RasterTarget& t, const TextFace& face, const TextLayout& layout,
                         i32 origin_x, i32 baseline, const std::vector<i32>* dx_override,
                         u32 color);

    void init_stock_objects();

    /// The export registrars need access to DC and region state for inline stubs
    friend DllExports make_gdi32_exports(
        GdiManager& mgr,
        std::function<std::string()> display_adapter_name_provider);
    friend void add_gdi32_viewport_region_exports(DllExports&, GdiManager&);
    friend void add_gdi32_shape_region_exports(DllExports&, GdiManager&);
    friend void add_gdi32_dc_attribute_exports(DllExports&, GdiManager&);
    friend void add_gdi32_path_print_exports(DllExports&, GdiManager&);
};

/// Create gdi32.dll exports
[[nodiscard]] DllExports make_gdi32_exports(
    GdiManager& mgr,
    std::function<std::string()> display_adapter_name_provider = {});

} // namespace vitreo::winapi
