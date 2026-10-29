#pragma once

/// @file imagehlp.hpp
/// @brief ImageHlp PE certificate-table APIs

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/handle.hpp"

#include <mutex>

namespace vitreo::winapi {

class FileManager;
class Marshaler;

/// Implements the ImageHlp certificate APIs against capsule-backed file
/// handles. Microsoft documents ImageHlp as single-threaded, so the manager
/// serializes the complete read/modify/write operation for each call.
class ImageHlpManager {
public:
    explicit ImageHlpManager(FileManager& files) : files_(files) {}

    [[nodiscard]] bool enumerate_certificates(
        Marshaler& marshaler, Handle file_handle, u16 type_filter,
        GuestAddr certificate_count, GuestAddr indices, u32 index_count);
    [[nodiscard]] bool add_certificate(
        Marshaler& marshaler, Handle file_handle, GuestAddr certificate,
        GuestAddr index);
    [[nodiscard]] bool remove_certificate(Handle file_handle, u32 index);

private:
    FileManager& files_;
    std::mutex mutex_;
};

DllExports create_imagehlp_thunks(ImageHlpManager& manager);

} // namespace vitreo::winapi
