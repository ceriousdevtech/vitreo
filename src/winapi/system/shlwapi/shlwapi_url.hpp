#pragma once

#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class RegistryThunks;
class GlobalLocalManager;

/// Appends shlwapi's URL family to the SHLWAPI exports: UrlCanonicalize,
/// UrlEscape, UrlUnescape, UrlCombine, UrlGetPart, UrlIs, UrlIsOpaque,
/// UrlIsNoHistory, UrlGetLocation, UrlCompare, UrlCreateFromPath,
/// PathCreateFromUrl, UrlApplyScheme, ParseURL, PathIsURL, UrlHash (A and
/// W), PathCreateFromUrlAlloc, UrlFixupW, HashData and GetAcceptLanguagesA/W.
/// The string rules live in shlwapi_url_core.cpp.
/// @param registry Backs UrlApplyScheme's URL\\Prefixes / DefaultPrefix
///        lookups and GetAcceptLanguages' AcceptLanguage value (absent keys
///        behave as on a Windows install without them).
/// @param local_heap Allocates PathCreateFromUrlAlloc's LocalAlloc string.
void add_shlwapi_url_exports(DllExports& exports, RegistryThunks* registry, GlobalLocalManager* local_heap);

} // namespace vitreo::winapi
