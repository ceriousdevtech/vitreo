#pragma once

/// @file lz32.hpp
/// @brief Legacy LZ32 file copy and SZDD decompression APIs

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <string>
#include <unordered_map>

namespace vitreo { namespace capsule { class Vfs; } }

namespace vitreo::winapi {

namespace lz_errors {
constexpr i32 BADINHANDLE = -1;
constexpr i32 BADOUTHANDLE = -2;
constexpr i32 READ = -3;
constexpr i32 WRITE = -4;
constexpr i32 GLOBALLOC = -5;
constexpr i32 BADVALUE = -7;
constexpr i32 UNKNOWNALG = -8;
} // namespace lz_errors

struct LzOpenResult {
    i32 handle{lz_errors::BADVALUE};
    u16 error{0};
    std::string path;
};

struct ExpandedNameResult {
    i32 code{lz_errors::BADVALUE};
    std::string name;
};

class Lz32Manager {
public:
    explicit Lz32Manager(capsule::Vfs& vfs) : vfs_(vfs) {}
    ~Lz32Manager();

    Lz32Manager(const Lz32Manager&) = delete;
    Lz32Manager& operator=(const Lz32Manager&) = delete;

    [[nodiscard]] LzOpenResult open_file(const std::string& path, u16 style);
    [[nodiscard]] ExpandedNameResult expanded_name(const std::string& source);
    [[nodiscard]] i32 copy(i32 source, i32 destination);
    void close(i32 handle);

private:
    struct OpenFile {
        u32 vfs_handle{0};
        bool readable{false};
        bool writable{false};
        std::string path;
    };

    capsule::Vfs& vfs_;
    std::unordered_map<i32, OpenFile> files_;
    i32 next_handle_{1};
};

DllExports create_lz32_thunks(Lz32Manager& manager);

} // namespace vitreo::winapi