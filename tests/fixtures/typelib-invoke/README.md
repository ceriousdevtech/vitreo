# Type libraries for the Automation call-engine probe

`invoke32.tlb` (SYS_WIN32) and `invoke64.tlb` (SYS_WIN64) were compiled on
`Windows` (Tiny11) on 2026-10-30 from `typeinfo_invoke_probe.idl` with the
Windows SDK 10.0.26100.0 `midl.exe`:

```
midl /nologo /env win32 /tlb invoke32.tlb /proxy nul /dlldata nul typeinfo_invoke_probe.idl
midl /nologo /env x64   /tlb invoke64.tlb /proxy nul /dlldata nul typeinfo_invoke_probe.idl
```

`tools/oracle_probe/typeinfo_invoke_probe.c` implements `IInvokeProbe` and
records native ITypeInfo::Invoke, DispInvoke and DispCallFunc behaviour into
`tests/oracle_data/typeinfo_invoke_probe.{x86,x64}.expected`; `IInvokeDefaults`
only carries [defaultvalue] encodings (`def` rows); `IInvokeTypes` (with
`IPlainUnk`, `IDispDerived`, `DPure`, `CProbe`, `EColor` and aliases) measures
user-defined, imported (stdole2 IEnumVARIANT / IFontDisp) and SAFEARRAY
parameter shapes (`ty` rows). Tests:
`vitreo_typeinfo_invoke_probe` (CTest) and
`OleAut32Dispatch.DefaultValuesMatchNativeFuncDesc`.

SHA-256:

```
4dda2dae8fa26fc8d38226c077ffded1ee480b9fd5761d9a5044b885e4859284  invoke32.tlb
566c30009af8459688a0e51b88cf743fb57ae864dc48f5c4aa2dad86db4eb45e  invoke64.tlb
```
