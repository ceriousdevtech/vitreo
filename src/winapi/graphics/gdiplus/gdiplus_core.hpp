#pragma once
/// @file gdiplus_core.hpp
/// @brief Shared GDI+ foundation: status codes, value types, the object
/// table, and the marshaling rules every Gdip* export follows.
///
/// Every export of gdiplus.dll (630, see gdiplus_export_table.inc) is
/// registered. An area file (gdiplus_<area>.cpp) supplies the handlers for
/// its family through a registrar; an export no registrar supplies fails
/// with NotImplemented and a one-time UNIMPLEMENTED line, never success.
///
/// Contract sources: Windows SDK GdiPlusFlat.h / GdiPlusEnums.h /
/// GdiPlusTypes.h / GdiPlusPixelFormats.h / GdiPlusImaging.h and the
/// Microsoft Learn GDI+ flat API pages.

#include <vitreo/common.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {
class Marshaler;
class GdiplusManager;
}

namespace vitreo::winapi::gdip {

/// GpStatus (GdiPlusTypes.h `enum Status`).
enum class Status : u32 {
    Ok = 0,
    GenericError = 1,
    InvalidParameter = 2,
    OutOfMemory = 3,
    ObjectBusy = 4,
    InsufficientBuffer = 5,
    NotImplemented = 6,
    Win32Error = 7,
    WrongState = 8,
    Aborted = 9,
    FileNotFound = 10,
    ValueOverflow = 11,
    AccessDenied = 12,
    UnknownImageFormat = 13,
    FontFamilyNotFound = 14,
    FontStyleNotFound = 15,
    NotTrueTypeFont = 16,
    UnsupportedGdiplusVersion = 17,
    GdiplusNotInitialized = 18,
    PropertyNotFound = 19,
    PropertyNotSupported = 20,
    ProfileNotFound = 21,
};

struct PointF { float X{0}, Y{0}; };
struct RectF { float X{0}, Y{0}, Width{0}, Height{0}; };

/// A GDI+ 3x2 affine matrix, row-vector convention as GpMatrix:
/// x' = m11 x + m21 y + dx, y' = m12 x + m22 y + dy.
struct Affine {
    float m11{1}, m12{0}, m21{0}, m22{1}, dx{0}, dy{0};

    [[nodiscard]] PointF apply(PointF p) const {
        return {m11 * p.X + m21 * p.Y + dx, m12 * p.X + m22 * p.Y + dy};
    }
    /// this * other: apply this first, then other.
    [[nodiscard]] Affine then(const Affine& o) const {
        return {m11 * o.m11 + m12 * o.m21, m11 * o.m12 + m12 * o.m22,
                m21 * o.m11 + m22 * o.m21, m21 * o.m12 + m22 * o.m22,
                dx * o.m11 + dy * o.m21 + o.dx, dx * o.m12 + dy * o.m22 + o.dy};
    }
    [[nodiscard]] bool is_identity() const {
        return m11 == 1 && m12 == 0 && m21 == 0 && m22 == 1 && dx == 0 && dy == 0;
    }
};

/// Live GDI+ objects. A GpXxx* the guest holds is one of these handles: an
/// opaque, non-null, pointer-sized value it only passes back to Gdip*
/// exports. Each area stores its own object type; get<T>() checks the type,
/// so a pen handed to a brush export is InvalidParameter, as on Windows.
class ObjectTable {
    /// A per-type tag without RTTI (the build is -fno-rtti): the address of
    /// a function-template static is unique per T.
    using TypeTag = const void*;
    template <typename T>
    static TypeTag tag() { static const char t{}; return &t; }

public:
    template <typename T>
    u64 add(std::shared_ptr<T> object) {
        const u64 h = m_next;
        m_next += 0x10;
        m_objects[h] = Entry{tag<T>(), std::move(object)};
        return h;
    }
    template <typename T>
    u64 add(T object) { return add(std::make_shared<T>(std::move(object))); }

    /// The object behind @p h if it is a T, else null.
    template <typename T>
    [[nodiscard]] T* get(u64 h) const {
        const auto it = m_objects.find(h);
        if (it == m_objects.end() || it->second.type != tag<T>()) return nullptr;
        return static_cast<T*>(it->second.object.get());
    }
    template <typename T>
    [[nodiscard]] std::shared_ptr<T> share(u64 h) const {
        const auto it = m_objects.find(h);
        if (it == m_objects.end() || it->second.type != tag<T>()) return {};
        return std::static_pointer_cast<T>(it->second.object);
    }
    /// Destroy @p h if it is a T. False when it is not one.
    template <typename T>
    bool remove(u64 h) {
        if (get<T>(h) == nullptr) return false;
        m_objects.erase(h);
        return true;
    }
    /// True when @p h is a live object of any type (a wrong-class handle
    /// is ObjectBusy on Windows, an unknown one InvalidParameter).
    [[nodiscard]] bool contains(u64 h) const { return m_objects.find(h) != m_objects.end(); }
    [[nodiscard]] usize size() const { return m_objects.size(); }

private:
    struct Entry {
        TypeTag type{nullptr};
        std::shared_ptr<void> object;
    };
    std::unordered_map<u64, Entry> m_objects;
    u64 m_next{0x0BB00010};
};

// ---- Marshaling ------------------------------------------------------------
//
// Bitness rules every handler follows:
//  * a GpXxx* / pointer argument: m.arg_ptr(i); a handle argument: handle_arg;
//  * INT / enum / BOOL / ARGB: m.arg32(i) (x64 only defines the low dword);
//  * REAL: real_arg (XMM0-3 for the first four positions on x64);
//  * a GpXxx** output: write_handle (pointer width: 8 bytes on x64);
//  * every output written on success only unless the Learn page says
//    otherwise; NULL outputs are InvalidParameter.

[[nodiscard]] u64 handle_arg(Marshaler& m, usize index);
[[nodiscard]] float real_arg(Marshaler& m, usize index);
bool write_handle(Marshaler& m, GuestAddr out, u64 handle);
bool write_status(Marshaler& m, Status s);  ///< sets the return value; always true

[[nodiscard]] bool read_f32(Marshaler& m, GuestAddr at, float& v);
bool write_f32(Marshaler& m, GuestAddr at, float v);
[[nodiscard]] bool read_points(Marshaler& m, GuestAddr at, i32 count, std::vector<PointF>& out);
/// Read @p count GDI+ `Point` (INT x, INT y) as PointF.
[[nodiscard]] bool read_points_i(Marshaler& m, GuestAddr at, i32 count, std::vector<PointF>& out);
bool write_points(Marshaler& m, GuestAddr at, const std::vector<PointF>& pts);
bool write_points_i(Marshaler& m, GuestAddr at, const std::vector<PointF>& pts);
[[nodiscard]] bool read_rect(Marshaler& m, GuestAddr at, RectF& r);
[[nodiscard]] bool read_rect_i(Marshaler& m, GuestAddr at, RectF& r);
bool write_rect(Marshaler& m, GuestAddr at, const RectF& r);
bool write_rect_i(Marshaler& m, GuestAddr at, const RectF& r);
[[nodiscard]] bool read_rects(Marshaler& m, GuestAddr at, i32 count, std::vector<RectF>& out);
[[nodiscard]] bool read_rects_i(Marshaler& m, GuestAddr at, i32 count, std::vector<RectF>& out);
[[nodiscard]] bool read_affine(Marshaler& m, GuestAddr at, Affine& a);  ///< 6 REALs
/// A WCHAR string: @p length chars, or NUL-terminated when length is -1.
[[nodiscard]] std::u16string read_wstr(Marshaler& m, GuestAddr at, i32 length = -1);
bool write_wstr(Marshaler& m, GuestAddr at, std::u16string_view s, bool terminate = true);

/// One-time `[vitreo] UNIMPLEMENTED gdiplus!<api>: <what>` line.
void log_unimplemented(std::string_view api, std::string_view what);

// ---- Registration ----------------------------------------------------------

using Handler = std::function<void(Marshaler&)>;
/// Export name -> handler. Registrars add their family; create_gdiplus_thunks
/// pairs it with the generated export table (ordinals, x86 cleanup bytes).
using HandlerMap = std::unordered_map<std::string, Handler>;

} // namespace vitreo::winapi::gdip
