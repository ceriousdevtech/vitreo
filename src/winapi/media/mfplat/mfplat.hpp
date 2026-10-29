#pragma once

#include "winapi/core/thunk_registry.hpp"
#include "winapi/media/mfplat/mf_attributes.hpp"

namespace vitreo::winapi {

/// Media Foundation platform exports (mfplat.dll). The platform starts up and
/// its attribute objects are real (MfPlatManager); transform enumeration
/// finds nothing because Vitreo registers no codecs; everything else the SDK
/// import library publishes fails loudly with its documented failure value.
DllExports create_mfplat_thunks(MfPlatManager& mgr);

/// mf.dll, mfreadwrite.dll, evr.dll, dxva2.dll: every import-library export
/// resolves; the pipeline objects they create do not exist here, so each
/// fails with the documented HRESULT and a one-time UNIMPLEMENTED line.
DllExports create_mf_thunks();
DllExports create_mfreadwrite_thunks();
DllExports create_evr_thunks();
DllExports create_dxva2_thunks();

/// msmpeg2vdec.dll (the Microsoft H.264/MPEG-2 decoder MFT) is a COM server
/// with no import library: DllGetClassObject answers that the class is not
/// available, so a guest that activates the decoder by CLSID sees the same
/// state MFTEnumEx reports (no decoders).
DllExports create_msmpeg2vdec_thunks();

} // namespace vitreo::winapi
