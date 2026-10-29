#pragma once
/// @file msi_install.hpp
/// @brief Planning half of the Windows Installer file-copy actions.
///
/// Deciding *what* to install is separable from doing it, and it is the half
/// that fails quietly: every mistake here puts real bytes in the wrong place
/// or skips a file, and neither shows up as an error. So it is a pure function
/// of the package tables, testable without a filesystem.
///
/// Four tables combine to place one file, and each carries a trap:
///
///   * **File.FileName** is `short|long`. The long name is the installed one,
///     and — for a package whose uncompressed payload sits in a long-named
///     source tree, which is every modern one — the source one too.
///   * **File.Attributes** decides compressed versus not, and *overrides* what
///     the media row implies. A file marked Noncompressed is copied from the
///     source tree even when its media row names a cabinet.
///   * **Media** maps a file to a cabinet by sequence *range*: a row's
///     `LastSequence` is the last sequence it holds, and the row that owns a
///     file is the first whose LastSequence is at least the file's Sequence.
///     Matching on equality finds nothing for almost every file.
///   * **Directory** resolves twice — once to the target path and once to the
///     source path, from the two halves of `DefaultDir`. Using the target tree
///     as the source tree looks right for the many rows where both halves are
///     the same name and silently fails for the rest.

#include <vitreo/common.hpp>
#include "winapi/system/msi/msi_engine.hpp"

#include <string>
#include <vector>

namespace vitreo::winapi::msi {

/// One file the package places, with both ends of the copy already resolved.
struct PlannedFile {
    /// The `File` primary key. Also the entry name inside a cabinet: cabinets
    /// built by an MSI packager store files under their key, not their name.
    std::string key;
    /// Absolute guest path the file is installed to.
    std::string target_path;
    /// Absolute guest path it is copied from. Empty for a compressed file,
    /// which comes out of a cabinet instead.
    std::string source_path;
    /// `Media.Cabinet` for a compressed file: a bare name for a cabinet beside
    /// the package, or a leading `#` for one embedded in it. Empty when the
    /// file is copied from the source tree.
    std::string cabinet;
    u32 sequence = 0;
    u64 size = 0;
};

/// Resolve every row of the `File` table into a placed file.
///
/// Rows whose component is missing, whose component's condition evaluates
/// false, or whose directory does not resolve are omitted — those are the
/// documented reasons a file is legitimately not installed. Rows that cannot
/// be resolved for any *other* reason are reported through @p unresolved
/// rather than dropped, because a silently short plan is indistinguishable
/// from a package that simply had fewer files.
[[nodiscard]] std::vector<PlannedFile> plan_files(Session& session,
                                                  const std::string& source_dir,
                                                  std::vector<std::string>& unresolved);

/// Split a `DefaultDir` cell into its source name — the half after the colon,
/// or the whole cell when there is none. Exposed alongside
/// default_dir_target() because the two halves are asymmetric: a missing
/// colon means one name serves as both, but a present one means the target
/// name is *not* the source name.
[[nodiscard]] std::string default_dir_source(const std::string& default_dir);

/// The short half of a `short|long` name pair, or the whole cell when there
/// is no pair (the source name when Word Count bit 0 says short names).
[[nodiscard]] std::string short_file_name(const std::string& file_name);

/// The long half of a `short|long` name pair, or the whole cell when there is
/// no pair.
[[nodiscard]] std::string long_file_name(const std::string& file_name);

} // namespace vitreo::winapi::msi
