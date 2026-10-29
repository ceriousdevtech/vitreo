// Shared streaming SHA-1 diagnostic for file write paths.
//
// Enabled via VITREO_WRITE_SHA1=1. When enabled, hashes every byte written
// to a registered file handle and emits:
//   - a partial "[file-sha1] ... (partial)" every 1 MiB
//   - a FINAL "[file-sha1] ... " line on close
//
// Streaming SHA-1 of bytes written through a registered file handle, used
// to compare a download against its content hash.
// Callers register handles at create_file/NtCreateFile, feed bytes on every
// successful write, and close at handle-close / NtClose time.

#pragma once

#include "winapi/kernel32/handle.hpp"

#include <cstddef>
#include <string_view>

namespace vitreo::winapi::debug {

// Returns true the first time it's called with VITREO_WRITE_SHA1 set in env.
bool write_sha1_enabled();

// No-ops if not enabled or on non-Apple builds.
void write_sha1_open(Handle h, std::string_view path);
void write_sha1_update(Handle h, const void* data, std::size_t n);
void write_sha1_close(Handle h);

// Read-side counterpart, gated by VITREO_READ_SHA1=1. Same shape as the
// write trio. Used to verify that bytes returned to the guest from ReadFile
// match the bytes that were originally written to disk for the same path.
bool read_sha1_enabled();
void read_sha1_open(Handle h, std::string_view path);
void read_sha1_update(Handle h, const void* data, std::size_t n);
void read_sha1_close(Handle h);

// Per-handle path watch, gated by VITREO_WATCH_PATH=<substring>. When set,
// any opened path containing the substring is registered, and is_watched()
// returns true for that handle until close. Used to selectively enable
// expensive traces (hex dumps, deep API logging) for one file.
bool watch_enabled();
void watch_open(Handle h, std::string_view path);
bool is_watched(Handle h);
const char* watched_path(Handle h); // nullptr if not watched
void watch_close(Handle h);

} // namespace vitreo::winapi::debug
