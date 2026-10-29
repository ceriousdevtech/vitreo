// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_live_diag.hpp
/// @brief D3D11LiveDiagTest and the log-capture / temp-marker helpers the live-diagnostic suites share.

#include "d3d11_test_surface.hpp"
#include "d3d11_test_fixture_x86.hpp"

namespace vitreo::winapi::d3d11_test {

// ---------------------------------------------------------------------------
// Capability members, indirect-argument bounds, and the live-run diagnostics
// (/tmp/vitreo-d3d11-caps, /tmp/vitreo-draw-dump).
// ---------------------------------------------------------------------------

namespace live_diag_tests {

/// The lines live_diag::emit printed.
struct CapturedLog {
    std::vector<std::string> lines;
    /// The lines that contain @p text.
    [[nodiscard]] std::vector<std::string> matching(
        std::string_view text) const {
        std::vector<std::string> out;
        for (const auto& line : lines) {
            if (line.find(text) != std::string::npos) out.push_back(line);
        }
        return out;
    }
};

inline void capture_line(void* user, std::string_view line) {
    static_cast<CapturedLog*>(user)->lines.emplace_back(line);
}

/// A scratch directory with marker files in it, removed afterwards.
class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("vitreo-live-diag-" + std::to_string(::getpid()) + "-" +
                 std::to_string(counter_++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] std::string write(const std::string& name,
                                    const std::string& contents) const {
        const auto file = path_ / name;
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << contents;
        return file.string();
    }
    [[nodiscard]] std::string missing(const std::string& name) const {
        return (path_ / name).string();
    }
    void remove(const std::string& name) const {
        std::error_code ignored;
        std::filesystem::remove(path_ / name, ignored);
    }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

/// The slot of a context method, by the SDK's name for it.
constexpr usize context_slot(std::string_view name) {
    for (usize i = 0; i < d3d11::CONTEXT_METHOD_NAMES.size(); ++i) {
        if (name == d3d11::CONTEXT_METHOD_NAMES[i]) return i;
    }
    return ~usize{0};
}

}  // namespace live_diag_tests

using namespace live_diag_tests;

class D3D11LiveDiagTest : public D3D11OptionalCapsTest {
protected:
    void SetUp() override {
        D3D11OptionalCapsTest::SetUp();
        set_log_sink(&capture_line, &log);
    }
    void TearDown() override {
        set_log_sink(nullptr, nullptr);
        // Back to the real markers, so no later test sees these.
        live_diag::caps_marker().set_path("/tmp/vitreo-d3d11-caps");
        live_diag::draw_dump_marker().set_path("/tmp/vitreo-draw-dump");
        D3D11OptionalCapsTest::TearDown();
    }
    static void set_log_sink(live_diag::LogSink sink, void* user) {
        live_diag::set_log_sink(sink, user);
    }
    static std::string pid_prefix(std::string_view tag) {
        return "[" + std::string(tag) + "] pid=" + std::to_string(::getpid()) +
               " ";
    }
    static std::string hex8(u32 value) {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08x", value);
        return text;
    }
    /// The guest bytes at @p at as the log prints them: 4-byte words of
    /// lower-case hex.
    std::string hex_of(GuestAddr at, u32 size) {
        std::string out;
        for (u32 i = 0; i < size; ++i) {
            if (i != 0 && i % 4 == 0) out += ' ';
            char byte[4];
            std::snprintf(byte, sizeof(byte), "%02x",
                          memory.read_u8(at + i).value());
            out += byte;
        }
        return out;
    }

    TempDir dir;
    CapturedLog log;
};

}  // namespace vitreo::winapi::d3d11_test
