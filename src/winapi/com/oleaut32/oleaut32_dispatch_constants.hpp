// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file oleaut32_dispatch_constants.hpp
/// @brief Automation constants used by the DispCallFunc / ITypeInfo::Invoke
///        engine. Values are the Windows SDK 10.0.26100.0 definitions
///        (wtypes.h VARENUM, winerror.h DISP_E_*, oleauto.h DISPATCH_*,
///        oaidl.h CALLCONV / FUNCKIND / INVOKEKIND / PARAMFLAG / DISPID_*),
///        recorded in tests/oracle_data/oleaut32_dispatch_constants.expected
///        and audited against it by test_oleaut32_dispatch.cpp.

#include <vitreo/common.hpp>

namespace vitreo::winapi::oleaut32_dispatch {

// wtypes.h VARENUM (names oleaut32_internal.hpp does not already define).
inline constexpr u16 VT_CY          = 6;
inline constexpr u16 VT_DATE        = 7;
inline constexpr u16 VT_ERROR       = 10;
inline constexpr u16 VT_DECIMAL     = 14;
inline constexpr u16 VT_I8          = 20;
inline constexpr u16 VT_UI8         = 21;
inline constexpr u16 VT_INT         = 22;
inline constexpr u16 VT_UINT        = 23;
inline constexpr u16 VT_VOID        = 24;
inline constexpr u16 VT_HRESULT     = 25;
inline constexpr u16 VT_PTR         = 26;
inline constexpr u16 VT_USERDEFINED = 29;
inline constexpr u16 VT_RECORD      = 36;
inline constexpr u16 VT_INT_PTR     = 37;
inline constexpr u16 VT_VECTOR      = 0x1000;
inline constexpr u16 VT_ARRAY       = 0x2000;
inline constexpr u16 VT_BYREF       = 0x4000;
inline constexpr u16 VT_TYPEMASK    = 0x0FFF;

// winerror.h
inline constexpr u32 DISP_E_MEMBERNOTFOUND   = 0x80020003u;
inline constexpr u32 DISP_E_PARAMNOTFOUND    = 0x80020004u;
inline constexpr u32 DISP_E_BADVARTYPE       = 0x80020008u;
inline constexpr u32 DISP_E_EXCEPTION        = 0x80020009u;
inline constexpr u32 DISP_E_OVERFLOW         = 0x8002000Au;
inline constexpr u32 DISP_E_BADPARAMCOUNT    = 0x8002000Eu;
inline constexpr u32 DISP_E_PARAMNOTOPTIONAL = 0x8002000Fu;
inline constexpr u32 DISP_E_BADCALLEE        = 0x80020010u;
inline constexpr u32 DISP_E_NOTACOLLECTION   = 0x80020011u;

// oleauto.h
inline constexpr u32 DISPATCH_METHOD         = 0x1;
inline constexpr u32 DISPATCH_PROPERTYGET    = 0x2;
inline constexpr u32 DISPATCH_PROPERTYPUT    = 0x4;
inline constexpr u32 DISPATCH_PROPERTYPUTREF = 0x8;

// oaidl.h
inline constexpr i32 DISPID_VALUE       = 0;
inline constexpr i32 DISPID_PROPERTYPUT = -3;
inline constexpr u32 CC_FASTCALL = 0;
inline constexpr u32 CC_CDECL    = 1;
inline constexpr u32 CC_STDCALL  = 4;
inline constexpr u16 FUNC_VIRTUAL     = 0;
inline constexpr u16 FUNC_PUREVIRTUAL = 1;
inline constexpr u16 FUNC_NONVIRTUAL  = 2;
inline constexpr u16 FUNC_STATIC      = 3;
inline constexpr u16 FUNC_DISPATCH    = 4;
inline constexpr u16 INVOKE_FUNC           = 1;
inline constexpr u16 INVOKE_PROPERTYGET    = 2;
inline constexpr u16 INVOKE_PROPERTYPUT    = 4;
inline constexpr u16 INVOKE_PROPERTYPUTREF = 8;
inline constexpr u16 PARAMFLAG_FIN         = 0x01;
inline constexpr u16 PARAMFLAG_FOUT        = 0x02;
inline constexpr u16 PARAMFLAG_FLCID       = 0x04;
inline constexpr u16 PARAMFLAG_FRETVAL     = 0x08;
inline constexpr u16 PARAMFLAG_FOPT        = 0x10;
inline constexpr u16 PARAMFLAG_FHASDEFAULT = 0x20;

/// oaidl.h EXCEPINFO {WORD wCode; WORD wReserved; BSTR bstrSource;
/// BSTR bstrDescription; BSTR bstrHelpFile; DWORD dwHelpContext;
/// PVOID pvReserved; pfnDeferredFillIn; SCODE scode;} with natural
/// alignment. Sizes measured: typeinfo_invoke_probe `abi excepinfo=32/64`.
struct ExcepInfoLayout {
    u32 bstrSource, bstrDescription, bstrHelpFile, dwHelpContext,
        pvReserved, pfnDeferredFillIn, scode, size;
};
inline constexpr ExcepInfoLayout kExcepInfoX86{4, 8, 12, 16, 20, 24, 28, 32};
inline constexpr ExcepInfoLayout kExcepInfoX64{8, 16, 24, 32, 40, 48, 56, 64};

} // namespace vitreo::winapi::oleaut32_dispatch
