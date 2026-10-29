#pragma once

#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

/// Appends StrPBrk, PathFindFileName, PathStripPath, PathGetDriveNumber,
/// PathCompactPathEx, PathRelativePathTo, StrToInt, StrToIntEx,
/// StrToInt64Ex, StrRChr, StrRChrI, StrStr, StrStrI, PathRemoveBackslash,
/// PathRemoveExtension and PathSkipRoot (A and W), StrStrNW and StrStrNIW to
/// the SHLWAPI exports. Behaviour Learn leaves open is measured by
/// tools/oracle_probe/shlwapi_path_extras_probe.c and
/// tools/oracle_probe/shlwapi_str_extras_probe.c.
void add_shlwapi_path_extras(DllExports& exports);

} // namespace vitreo::winapi
