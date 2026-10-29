#pragma once
/// @file oleaut32_internal2.hpp
/// @brief Shared surface between oleaut32.cpp and its export table.
///
/// Private to oleaut32. The handlers sit in a nested namespace rather than
/// directly in vitreo::winapi because the BSTR and VARIANT marshaling names are
/// shared vocabulary across COM — ole32 and combase carry their own statics
/// with the same spellings.

#include "winapi/core/marshaler.hpp"
#include "winapi/com/oleaut32/oleaut32.hpp"

namespace vitreo::winapi::oleaut32 {

void impl_SysAllocString(Marshaler& m, OleAut32Manager& mgr);
void impl_SysAllocStringLen(Marshaler& m, OleAut32Manager& mgr);
void impl_SysAllocStringByteLen(Marshaler& m, OleAut32Manager& mgr);
void impl_SysFreeString(Marshaler& m, OleAut32Manager& mgr);
void impl_SysStringLen(Marshaler& m, OleAut32Manager& mgr);
void impl_SysStringByteLen(Marshaler& m, OleAut32Manager& mgr);
void impl_SysReAllocStringLen(Marshaler& m, OleAut32Manager& mgr);
void impl_SysReAllocString(Marshaler& m, OleAut32Manager& mgr);
void impl_VariantInit(Marshaler& m, OleAut32Manager& mgr);
void impl_VariantClear(Marshaler& m, OleAut32Manager& mgr);
void impl_VariantCopy(Marshaler& m, OleAut32Manager& mgr);
void impl_VariantChangeType(Marshaler& m, OleAut32Manager& mgr);
void impl_SafeArrayCreate(Marshaler& m, OleAut32Manager& mgr);
void impl_SafeArrayDestroy(Marshaler& m, OleAut32Manager& mgr);
void impl_OleCreateFontIndirect(Marshaler& m, OleAut32Manager& mgr);
void impl_RegisterTypeLib(Marshaler& m, OleAut32Manager& mgr);
void impl_LoadTypeLib(Marshaler& m, OleAut32Manager& mgr);
void impl_VarUI4FromStr(Marshaler& m);
void impl_VarBstrCat(Marshaler& m, OleAut32Manager& mgr);
void impl_VarBstrCmp(Marshaler& m);
void impl_LoadRegTypeLib(Marshaler& m, OleAut32Manager& mgr);
void impl_SafeArrayCreateVector(Marshaler& m, OleAut32Manager& mgr);
void impl_BSTR_UserFree(Marshaler& m, OleAut32Manager& mgr);
void impl_BSTR_UserSize(Marshaler& m);
void impl_BSTR_UserMarshal(Marshaler& m);
void impl_BSTR_UserUnmarshal(Marshaler& m, OleAut32Manager& mgr);
void impl_VARIANT_UserFree(Marshaler& m, OleAut32Manager& mgr);
void impl_VARIANT_UserSize(Marshaler& m);
void impl_VARIANT_UserMarshal(Marshaler& m);
void impl_VARIANT_UserUnmarshal(Marshaler& m);
void impl_LPSAFEARRAY_UserFree(Marshaler& m, OleAut32Manager& mgr);
void impl_LPSAFEARRAY_UserSize(Marshaler& m);
void impl_LPSAFEARRAY_UserMarshal(Marshaler& m);
void impl_LPSAFEARRAY_UserUnmarshal(Marshaler& m);

} // namespace vitreo::winapi::oleaut32
