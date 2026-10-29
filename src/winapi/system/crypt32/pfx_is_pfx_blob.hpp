// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file pfx_is_pfx_blob.hpp
/// @brief crypt32!PFXIsPFXBlob outer-layer PKCS#12 PFX decoder.

#include <vitreo/common.hpp>

#include <span>

namespace vitreo::winapi {
class Marshaler;
}

namespace vitreo::winapi::crypt32 {

struct PfxBlobResult {
    enum class Kind { NotPfx, Pfx, Fault };
    Kind kind{Kind::NotPfx};
    u64 fault_offset{0};  ///< Kind::Fault: offset of the unreadable byte
};

/// Decide whether a blob of `declared_size` bytes decodes as a PFX PDU.
/// `readable` is the guest-readable prefix of the blob; a decode that needs a
/// byte beyond it (but inside `declared_size`) reports Kind::Fault.
[[nodiscard]] PfxBlobResult classify_pfx_blob(std::span<const u8> readable,
                                              u64 declared_size);

/// BOOL PFXIsPFXBlob(CRYPT_DATA_BLOB* pPFX)
void impl_PFXIsPFXBlob(Marshaler& m);

} // namespace vitreo::winapi::crypt32
