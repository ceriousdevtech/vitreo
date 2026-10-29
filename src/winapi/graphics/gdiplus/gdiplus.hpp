#pragma once
/// @file gdiplus.hpp
/// @brief GDI+ (gdiplus.dll): the manager the area files share.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/memory/heap.hpp"
#include "winapi/graphics/gdiplus/gdiplus_core.hpp"
#include <mutex>
#include <string>
#include <vector>

namespace vitreo::capsule { class Vfs; }

namespace vitreo::winapi {

class GdiManager;
class ResourceManager;

class Marshaler;

/// gdiplus.dll: every export (gdiplus_export_table.inc) is implemented by
/// one of the area files gdiplus_<area>.cpp, which register their handlers
/// through gdip::register_<area>; contracts in .docs/contracts/gdiplus/.
class GdiplusManager {
public:
    explicit GdiplusManager(HeapManager& heap) : m_heap(heap) {}

    /// Needed to hand out callable addresses through GdiplusStartupOutput.
    void set_thunk_registry(ThunkRegistry* thunks) noexcept {
        m_thunks = thunks;
    }

    /// Image loading reads through the guest filesystem.
    void set_vfs(capsule::Vfs* vfs) noexcept { m_vfs = vfs; }
    /// GdipCreateHBITMAPFromBitmap produces a real GDI bitmap object.
    void set_gdi_manager(GdiManager* gdi) noexcept { m_gdi = gdi; }
    /// user32 icon objects: GdipCreateHICONFromBitmap / GdipCreateBitmapFromHICON.
    void set_resource_manager(ResourceManager* resources) noexcept { m_resources = resources; }

    // ---- Shared state for the area files (gdiplus_<area>.cpp) ----
    /// Every export runs under this lock: GDI+ objects are process-wide and
    /// GDI+ serializes access to them.
    [[nodiscard]] std::recursive_mutex& mutex() noexcept { return m_mutex; }
    [[nodiscard]] gdip::ObjectTable& objects() noexcept { return m_objects; }
    [[nodiscard]] GdiManager* gdi() const noexcept { return m_gdi; }
    [[nodiscard]] ResourceManager* resources() const noexcept { return m_resources; }
    [[nodiscard]] capsule::Vfs* vfs() const noexcept { return m_vfs; }
    [[nodiscard]] ThunkRegistry* thunks() const noexcept { return m_thunks; }
    [[nodiscard]] HeapManager& heap() noexcept { return m_heap; }

private:
    std::recursive_mutex m_mutex;
    gdip::ObjectTable m_objects;
    ThunkRegistry* m_thunks = nullptr;
    capsule::Vfs*  m_vfs = nullptr;
    GdiManager*    m_gdi = nullptr;
    ResourceManager* m_resources = nullptr;
    HeapManager& m_heap;

};

namespace gdip {
// One registrar per area file; each adds the handlers of its family.
void register_geometry(GdiplusManager& mgr, HandlerMap& map);   ///< matrix, path, path iterator
void register_region(GdiplusManager& mgr, HandlerMap& map);     ///< region, graphics clip
void register_graphics(GdiplusManager& mgr, HandlerMap& map);   ///< graphics lifetime/state/transform
void register_pen_brush(GdiplusManager& mgr, HandlerMap& map);  ///< brushes (solid, hatch, texture, gradients)
void register_draw(GdiplusManager& mgr, HandlerMap& map);       ///< pens, line caps, Draw*/Fill* primitives
void register_text(GdiplusManager& mgr, HandlerMap& map);       ///< fonts, string formats, text
void register_image(GdiplusManager& mgr, HandlerMap& map);      ///< images, bitmaps, pixels, attributes, DrawImage
void register_codec(GdiplusManager& mgr, HandlerMap& map);      ///< codecs, encode/save, properties, frames, metafiles, effects
} // namespace gdip

/// All gdiplus.dll export names (the generated table).
[[nodiscard]] std::vector<std::string> gdiplus_export_names();
/// Every registrar's handlers, by export name (tests call through this).
[[nodiscard]] gdip::HandlerMap gdiplus_handlers(GdiplusManager& mgr);
/// Build the gdiplus.dll export table and wire it to @p mgr: every export
/// with its ordinal for @p guest_is_64bit and its x86 cleanup bytes.
DllExports create_gdiplus_thunks(GdiplusManager& mgr, bool guest_is_64bit);

} // namespace vitreo::winapi
