#pragma once
/// @file gl_display_list.hpp
/// @brief OpenGL 1.1 display lists (spec 5.4): the recorded form of a GL
///        command, the list that holds them, and the commands that are never
///        compiled.
///
/// Recording seam (one pattern, used by every compilable export). The export
/// handlers (gl_exports.cpp, gl_cmd_vertex.cpp, gl_cmd_pixels.cpp) decode the
/// guest's arguments, reading client memory, and then issue the internal
/// command through gl_issue<&cmd::X>(call, decoded args...) instead of calling
/// cmd::X directly. gl_issue (gl_exports.hpp) asks the GlCall whether this
/// call is being compiled (a list is open and the export is not in the
/// not-compiled table below); if so it appends a GlRecordedCall<&cmd::X>
/// holding copies of the decoded arguments, and for GL_COMPILE stops there;
/// otherwise (and also for GL_COMPILE_AND_EXECUTE) it calls cmd::X. Replay
/// (glCallList) calls the same cmd::X with the stored values, so a replayed
/// command gets exactly the validation and effect of the export; the cmd
/// functions themselves never record, so a list replayed while another list
/// is being compiled is not re-recorded (spec 5.4: the CallList, not its
/// lists' commands, goes into the list being defined).
///
/// What is stored is always decoded and owned (spec 5.4: "pointers which are
/// passed as arguments to commands are dereferenced when the command is
/// issued"; "the client state in effect at that time applies"): GlValues,
/// matrices and stipple rows by value, spans as vectors, image bytes together
/// with the unpack modes they were read under (GlClientPixels::unpack), and
/// vertex-array draws as GlArrayElements, the array values the client state
/// selected when the draw was compiled. Nothing in a list refers to guest
/// memory, so a list is self-contained and immutable once glEndList
/// publishes it.
///
/// Lists live in the GlShareGroup (gl_context.hpp) under its mutex, as
/// GlListEntry runs keyed by name. A list is published only at glEndList;
/// glCallList copies the list's shared_ptr under the lock and replays it
/// without the lock, so glDeleteLists or a redefinition on another thread
/// never frees a list that is executing.

#include "gl_commands.hpp"

#include <memory>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace vitreo::winapi::gl {

/// One compiled command.
class GlListCommand {
public:
    virtual ~GlListCommand() = default;
    virtual void execute(GlContext& ctx) const = 0;
};

/// The commands of one display list, in the order they were issued.
class GlDisplayList {
public:
    void append(std::unique_ptr<GlListCommand> command) { m_commands.push_back(std::move(command)); }
    [[nodiscard]] usize size() const noexcept { return m_commands.size(); }
    /// Executes every command in order (spec 5.4: "just as if they were
    /// issued without using a display list").
    void execute(GlContext& ctx) const {
        for (const auto& command : m_commands) command->execute(ctx);
    }

private:
    std::vector<std::unique_ptr<GlListCommand>> m_commands;
};

// ---- Stored arguments ----------------------------------------------------------------

/// How a command parameter of type P is kept in a list: Stored is the owned
/// copy, view() gives the parameter back for the call.
template <typename P>
struct GlListArg {
    using Stored = P;
    template <typename A>
    [[nodiscard]] static Stored store(const GlContext&, const A& value) { return static_cast<Stored>(value); }
    [[nodiscard]] static const Stored& view(const Stored& stored) noexcept { return stored; }
};

template <typename T>
struct GlListArg<std::span<const T>> {
    using Stored = std::vector<T>;
    template <typename A>
    [[nodiscard]] static Stored store(const GlContext&, const A& value) {
        const std::span<const T> s(value);
        return Stored(s.begin(), s.end());
    }
    [[nodiscard]] static std::span<const T> view(const Stored& stored) noexcept { return stored; }
};

/// Image bytes with the unpack modes they were read under.
struct GlStoredPixels {
    bool null_pointer{false};
    bool readable{true};
    std::vector<u8> bytes;
    GlPixelStoreModes unpack{};
};

template <>
struct GlListArg<cmd::GlClientPixels> {
    using Stored = GlStoredPixels;
    [[nodiscard]] static Stored store(const GlContext& ctx, const cmd::GlClientPixels& px) {
        return Stored{px.null_pointer, px.readable, std::vector<u8>(px.bytes.begin(), px.bytes.end()),
                      gl_unpack_modes(ctx, px)};
    }
    [[nodiscard]] static cmd::GlClientPixels view(const Stored& s) noexcept {
        cmd::GlClientPixels px;
        px.null_pointer = s.null_pointer;
        px.readable = s.readable;
        px.bytes = s.bytes;
        px.unpack = &s.unpack;
        return px;
    }
};

template <typename P>
using GlListArgOf = GlListArg<std::remove_cvref_t<P>>;

/// A call of command F with its decoded arguments (F is a cmd:: function
/// taking GlContext& first).
template <auto F, typename Sig = decltype(F)>
class GlRecordedCall;

template <auto F, typename R, typename... P>
class GlRecordedCall<F, R (*)(GlContext&, P...)> final : public GlListCommand {
public:
    template <typename... A>
    explicit GlRecordedCall(const GlContext& ctx, const A&... args)
        : m_args(GlListArgOf<P>::store(ctx, args)...) {}

    void execute(GlContext& ctx) const override {
        std::apply([&ctx](const auto&... stored) { (void)F(ctx, GlListArgOf<P>::view(stored)...); }, m_args);
    }

private:
    std::tuple<typename GlListArgOf<P>::Stored...> m_args;
};

// ---- Commands not compiled (spec 5.4) ---------------------------------------------------

/// An export executed immediately while a list is compiled. With
/// proxy_target set, only calls whose first argument is that target are
/// (spec 5.4: TexImage1D/2D with PROXY_TEXTURE_1D/2D).
struct GlNotCompiled {
    std::string_view name;
    u32 proxy_target{0};
};

/// The table, transcribed from spec 5.4 (see gl_display_list.cpp).
[[nodiscard]] std::span<const GlNotCompiled> gl_not_compiled_commands() noexcept;

/// What the not-compiled table says about one export, fixed at registration.
struct GlListRule {
    bool never_compiled{false};
    u32 proxy_target{0};   ///< not compiled when argument 0 equals this
};
[[nodiscard]] GlListRule gl_list_rule(std::string_view export_name) noexcept;

// ---- Share-group list storage (under GlShareGroup's lock) -----------------------------

/// The list named @p n (null for a generated but never defined name), or
/// nullopt when @p n is not a list.
[[nodiscard]] std::optional<std::shared_ptr<const GlDisplayList>> gl_find_list(GlShareGroup& group,
                                                                               const GlShareGroup::Lock& held,
                                                                               u32 n);

}  // namespace vitreo::winapi::gl
