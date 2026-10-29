# ------------------------------------------------------------------------------
# Vitreo Tests
# ------------------------------------------------------------------------------
#
# Tests are split by the production target they exercise, so that linking is
# evidence: a test binary that cannot link without the whole runtime is telling
# you the module under test is not independently usable.
#
# When adding a test, put it in the narrowest binary that can link it. If it
# does not link, prefer fixing the dependency over widening DEPS — see plan
# section 6.1 and cmake/VitreoModule.cmake.
#
# Test names are stable across this split.

include(GoogleTest)

if(EXISTS "${CMAKE_SOURCE_DIR}/tools/generate_typeinfo_layout.py")
    add_test(NAME vitreo_typeinfo_layout_audit
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/generate_typeinfo_layout.py --check)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/oracle_probe/check_typeinfo_bitness.py")
    add_test(NAME vitreo_typeinfo_bitness_oracle
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/oracle_probe/check_typeinfo_bitness.py
                --vitreo $<TARGET_FILE:vitreo>)
    set_tests_properties(vitreo_typeinfo_bitness_oracle PROPERTIES
        SKIP_RETURN_CODE 77 LABELS "oracle;oleaut32" TIMEOUT 180)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/tools/architecture/check_platform_abstraction.py")
    add_test(
        NAME vitreo_platform_abstraction_guard
        COMMAND python3
                ${CMAKE_SOURCE_DIR}/tools/architecture/check_platform_abstraction.py
    )
endif()

# ------------------------------------------------------------------------------
# ISA completeness gates
# ------------------------------------------------------------------------------
#
# Two separate things, because they fail for different reasons.
#
# The hard-gap gate is a CORRECTNESS gate: every instruction our synthetic
# CPUID advertises must be decodable and executable. A gap here means a guest
# that probes CPUID, gets a yes, and then takes #UD.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/isa_audit.py")
    add_test(
        NAME vitreo_isa_no_hard_gaps
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/isa_audit.py --fail-on-hard-gap
    )
endif()

# The D3D12 device is opt-in (see d3d12::d3d12_enabled), so the tests that need a
# device skip in the default run. Run them again with the gate on, or they would
# only ever report "skipped" -- which proves nothing about the device.
add_test(
    NAME vitreo_d3d12_enabled_tests
    COMMAND $<TARGET_FILE:vitreo_winapi_tests> --gtest_filter=*D3D12Test*
)
set_tests_properties(vitreo_d3d12_enabled_tests PROPERTIES
    ENVIRONMENT "VITREO_D3D12=1"
)

# D3D10 devices are on by default; the VITREO_D3D10=0 opt-out is only
# reachable with the variable set, so its test runs here.
add_test(
    NAME vitreo_d3d10_disabled_tests
    COMMAND $<TARGET_FILE:vitreo_winapi_tests>
            --gtest_filter=D3D10ModuleTest.OptOutRefusesDevicesAsUnsupported
)
set_tests_properties(vitreo_d3d10_disabled_tests PROPERTIES
    ENVIRONMENT "VITREO_D3D10=0"
)

# D3D11 vtable stdcall-cleanup gate.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/generate_api_set_hosts.py")
    add_test(
        NAME vitreo_api_set_hosts_audit
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/generate_api_set_hosts.py --check
    )
endif()
add_test(
    NAME vitreo_crt_ucrt_s5_oracle_audit
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/winapi/msvcrt/oracle/gen_crt_ucrt_s5_oracle.py --check
)
add_test(
    NAME vitreo_crt_ucrt_s6_oracle_audit
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/winapi/msvcrt/oracle/gen_crt_ucrt_s6_oracle.py --check
)
add_test(
    NAME vitreo_pfx_is_pfx_blob_oracle_audit
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/winapi/crypt32/oracle/gen_pfx_is_pfx_blob_oracle.py --check
)
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/oracle_probe/gen_gdi_tables.py")
    add_test(
        NAME vitreo_gdi_tables_oracle_audit
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/oracle_probe/gen_gdi_tables.py --check
    )
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/oracle_probe/gen_setup_classes.py")
    add_test(
        NAME vitreo_setup_classes_oracle_audit
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/oracle_probe/gen_setup_classes.py --check
    )
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/generate_rtl_utf8_oracle.py")
    add_test(
        NAME vitreo_rtl_utf8_oracle_audit
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/generate_rtl_utf8_oracle.py --check
    )
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/generate_cert_usage_oracle.py")
    add_test(
        NAME vitreo_cert_usage_oracle_audit
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/generate_cert_usage_oracle.py --check
    )
endif()
foreach(_contract_generator IN ITEMS
        crt_compare_oracle crt_stat_oracle crt_exports crt_dependencies_sdk msi_record_read_exports
        icmp_contract_constants ip_interface_contract_constants
        crt_errno_oracle crt_winmain_oracle crt_itoa_oracle sync_access_oracle thread_query_oracle thread_times_oracle thread_description_oracle
        msi_product_exports if_row2_contract ip_compartment_contract
        d3d11_format_oracle d3d12_feature_layout post_message_policy
        user32_case_table keyboard_layout iphlp_adapter_contract wasapi_services_contract
        audio_capture_contract propkey_names)
    if(EXISTS "${CMAKE_SOURCE_DIR}/tools/generate_${_contract_generator}.py")
        add_test(
            NAME vitreo_${_contract_generator}_audit
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/generate_${_contract_generator}.py --check
        )
        set_tests_properties(vitreo_${_contract_generator}_audit PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
endforeach()

#
# Every entry in d3d11_stdcall.hpp is how many bytes an x86 stdcall callee pops
# for one vtable slot. A wrong entry corrupts the guest stack on every call to
# that method and cannot fail visibly: the method returns normally and the damage
# surfaces later, elsewhere. d3d9 asserts its whole table; d3d11 did not, and an
# audit found 31 wrong entries in the context table and 2 in the device table --
# four of them on methods the frontend implements.
#
# Checks the tables against the mingw-w64 SDK headers, which come from the same
# IDL as the Windows SDK. Passes with a notice when they are not installed.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/audit_d3d11_vtables.py")
    add_test(
        NAME vitreo_d3d11_vtable_stdcall
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/audit_d3d11_vtables.py
    )
endif()

# The D3D12 guest struct layouts (x86 and x64 member offsets the frontend
# reads) regenerated from the SDK headers with clang and compared with the
# committed src/winapi/graphics/d3d12/d3d12_layouts.hpp.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/gen_d3d12_layouts.py")
    add_test(
        NAME vitreo_d3d12_layouts_match_sdk
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_d3d12_layouts.py
    )
endif()

# Every hand-written GUID constant (GuidBytes IID_*) in src/winapi against the
# SDK headers. A wrong byte makes QueryInterface for that interface fail
# forever, silently; 12 were wrong (IDXGIResource1, ID3D11ComputeShader, ten
# IDirect3D*9) when this was added. Passes with a notice without the headers.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/audit_guid_constants.py")
    add_test(
        NAME vitreo_guid_constants_match_sdk
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/audit_guid_constants.py
    )
endif()

# The Media Foundation export table (every mfplat/mf/mfreadwrite/evr/dxva2
# name the SDK import libraries publish, with x86 stdcall bytes) against the
# mingw-w64 import libraries, which derive from the same .def contract. A name
# missing from the table lands on the missing-export stub, whose 0xFFFFFFFF
# reads as success to BOOL callers. Passes with a notice without mingw.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/gen_mf_export_table.py")
    add_test(
        NAME vitreo_mf_export_table_audit
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_mf_export_table.py --check
    )
endif()

# The D3D11 query constants (D3D11_QUERY, the query/GetData flag bits, the
# primitive topologies the pipeline statistics count with) and the vtable slots
# SetPredication suppresses, against the SDK headers and Learn's list. The
# per-stream stream-output values were typed in the wrong order, so
# SO_OVERFLOW_PREDICATE_STREAM0 was built as a statistics object with every
# test agreeing. Passes with a notice without the headers.
# The critical-section flag byte and RTL_CRITICAL_SECTION /
# RTL_CRITICAL_SECTION_DEBUG layouts (x86 and x64), every nt_status::STATUS_*
# and every error_codes::ERROR_* the SDK defines, compiled against the
# mingw-w64 headers. STATUS_INVALID_FILE_FOR_SECTION was typed 0xC000001E
# (ntstatus.h: 0xC0000020) when this was added. Passes with a notice without
# the mingw cross compilers.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/audit_critical_section_constants.py")
    add_test(
        NAME vitreo_sync_constants_match_sdk
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/audit_critical_section_constants.py
    )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/tools/audit_d3d11_query_constants.py")
    add_test(
        NAME vitreo_d3d11_query_constants_match_sdk
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/audit_d3d11_query_constants.py
    )
endif()

# The translator hand-declares the SPIR-V opcodes it emits, and a MISSING entry
# silently shifts every value below it -- the emitted module then asks for a
# different instruction that is still perfectly valid, so SPIRV-Cross, the
# Metal compiler and the GPU all comply. Omitting OpAtomicISub (235) that way
# made 7 of the 9 SM5 atomics emit the wrong operation (atomic AND became an
# unsigned MAX) with every test green. Checks the tables against the SPIR-V
# header glslang vendors into the build tree, so the checker cannot share a
# mistake with the emitter. Passes with a notice when no build tree is present.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/audit_spirv_constants.py")
    add_test(
        NAME vitreo_spirv_constants_audit
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/audit_spirv_constants.py
    )
endif()

# The same for the DXBC side: every SM5 opcode number the translator declares
# that fxc was measured encoding (the atomic and double-precision probes).
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/audit_dxbc_opcodes.py")
    add_test(
        NAME vitreo_dxbc_opcodes_audit
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/audit_dxbc_opcodes.py
    )
endif()

# And the shader model 5.1 declaration layout (ranges, spaces, flag bits, the
# 40-byte RDEF record) the parser reads, decoded from fxc output and compared
# with fxc's own /Fc listing.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/audit_dxbc_sm51_declarations.py")
    add_test(
        NAME vitreo_dxbc_sm51_declarations_audit
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/audit_dxbc_sm51_declarations.py
    )
endif()

# An exception no handler takes on a WORKER thread's start routine: the
# top-level filter runs on that thread, then the whole process ends with the
# exception code and no DLL_THREAD_DETACH / DLL_PROCESS_DETACH. The probe runs
# under Vitreo and is compared with its Windows recording, x64 and x86.
# Vitreo used to end only the thread, run its detach callbacks, free its stack
# and let the process run on (Spotify's CEF renderer, 2026-10-22). Passes with
# a notice without the mingw cross compilers.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_unhandled_thread_probe.py")
    add_test(
        NAME vitreo_unhandled_thread_exception_ends_process
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_unhandled_thread_probe.py
                $<TARGET_FILE:vitreo>
    )
endif()

# VITREO_SCOPED_ENV must reach the runtime knobs that are read once, while the
# image starts (VITREO_JIT_MAP is the witness), or say loudly that it did not.
# It printed "scoped env: VITREO_JIT_MAP=1" and did nothing on every Chromium
# renderer (2026-10-23). A real guest under the real runtime; skips without the
# mingw-w64 cross compiler. The process-level contract is also covered, without
# a guest, by ScopedEnvironmentProcess.* in vitreo_platform_tests.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_scoped_env.py")
    add_test(
        NAME vitreo_scoped_env_reaches_startup_knobs
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_scoped_env.py
                $<TARGET_FILE:vitreo>
    )
    set_tests_properties(vitreo_scoped_env_reaches_startup_knobs PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()

# The persistent translation store (VITREO_TRANSLATION_STORE=warm) leaves
# guest output unchanged across off / cold / warm / verify / corrupt /
# patched-image runs of real x64 and x86 guests. Skips without mingw-w64.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_translation_store.py")
    add_test(
        NAME vitreo_translation_store_cold_warm_equivalence
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_translation_store.py
                $<TARGET_FILE:vitreo>
    )
    set_tests_properties(vitreo_translation_store_cold_warm_equivalence PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()

# x64 __C_specific_handler (ntdll and the UCRT) runs __except filters and
# __finally blocks from the scope table as Windows does, compared with the
# Windows recording. Skips without clang or mingw-w64.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_c_specific_handler_probe.py")
    add_test(
        NAME vitreo_c_specific_handler_matches_windows
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_c_specific_handler_probe.py
                $<TARGET_FILE:vitreo>
    )
    set_tests_properties(vitreo_c_specific_handler_matches_windows PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()

# x64 RtlUnwind / RtlUnwindEx called from ordinary code (vcruntime longjmp's
# STATUS_LONGJUMP unwind through kernel32 and ntdll, __finally and a collided
# unwind on the way, a NULL-record RtlUnwindEx, an exit unwind), compared with
# the Learn-derived expectation. Skips without clang or mingw-w64.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_rtl_unwind64_probe.py")
    add_test(
        NAME vitreo_rtl_unwind64_matches_windows
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_rtl_unwind64_probe.py
                $<TARGET_FILE:vitreo>
    )
    set_tests_properties(vitreo_rtl_unwind64_matches_windows PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()

# GetBinaryTypeW/A classify a file as an image section would (SCS_* or
# ERROR_BAD_EXE_FORMAT), compared with the Windows recording x64 and x86.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_get_binary_type_probe.py")
    add_test(
        NAME vitreo_get_binary_type_matches_windows
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_get_binary_type_probe.py
                $<TARGET_FILE:vitreo>
    )
endif()

# NtQueryVirtualMemory classes 0 and 2 (MEMORY_BASIC_INFORMATION and the
# mapped-file name) with their length / handle / class statuses, x64 and x86,
# compared with the Windows recording.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_nt_query_virtual_memory_probe.py")
    add_test(
        NAME vitreo_nt_query_virtual_memory_matches_windows
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_nt_query_virtual_memory_probe.py
                $<TARGET_FILE:vitreo>
    )
endif()

# NtCreateSection over NtOpenFile handles (SEC_IMAGE, SEC_IMAGE_NO_EXECUTE,
# SEC_COMMIT, access rights, refusal statuses) and NtMapViewOfSection image
# views, x64 and x86, compared with the Windows recording.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_nt_create_section_probe.py")
    add_test(
        NAME vitreo_nt_create_section_matches_windows
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_nt_create_section_probe.py
                $<TARGET_FILE:vitreo>
    )
endif()

# ntdll's heap (RtlCreateHeap / RtlDestroyHeap / RtlAllocateHeap / ...) and
# its sharing with kernel32's heap, x64 and x86, compared with the Windows
# recording.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_rtl_heap_probe.py")
    add_test(
        NAME vitreo_rtl_heap_matches_windows
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_rtl_heap_probe.py
                $<TARGET_FILE:vitreo>
    )
endif()

# RtlFreeUnicodeString (frees Buffer from the process heap, zeroes the
# bitness-correct UNICODE_STRING, leaves it untouched for a NULL Buffer) and
# RtlGetLastNtStatus (TEB.LastStatusValue, stored by RtlNtStatusToDosError,
# not by its NoTeb variant or SetLastError), x64 and x86, compared with the
# Windows recording. Recorded gaps: RtlNtStatusToDosError's table lacks
# STATUS_PENDING; kernel32 failure paths set the Win32 error without storing
# the NT status (CreateFileW / CloseHandle / WaitForSingleObject); a new
# thread starts with LastStatusValue 0, Windows leaves a loader status there.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_oracle_line_probe.py")
    add_test(
        NAME vitreo_rtl_string_status_matches_windows
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_oracle_line_probe.py
                $<TARGET_FILE:vitreo> rtl_string_status_probe
                --gap "^(to_dos_pending +)dos=[0-9]+=>\\1"
                --gap "^((createfile_missing|closehandle_bogus|wait_bogus) .*) status=0x[0-9a-f]+$=>\\1"
                --gap "^(fresh_thread +).*=>\\1"
    )
endif()

# kernel32 exports Windows forwards to ntdll, called by both names, x64 and
# x86: Encode/DecodeSystemPointer (the KUSER_SHARED_DATA Cookie formula),
# InitOnceInitialize, RestoreLastError, RtlZeroMemory, RtlMoveMemory,
# InterlockedCompareExchange64 (x86) and RtlIsEcCode (x64).
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_oracle_line_probe.py")
    add_test(
        NAME vitreo_k32_ntdll_forwarders_match_windows
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_oracle_line_probe.py
                $<TARGET_FILE:vitreo> k32_ntdll_forwarder_probe
    )
endif()

# kernel32 -> ntdll forwarders with no (or a thin) Learn page, by both names:
# LogUnexpectedCodepath (x64 and x86), and on x64 __misaligned_access and
# __chkstk (no register changes; StackLimit covers the probe; a probe past
# the reservation is STATUS_STACK_OVERFLOW) and _local_unwind (an RtlUnwind
# to the target frame: handlers, flags, resume context).
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_oracle_line_probe.py")
    add_test(
        NAME vitreo_k32_ntdll_misc_forwarders_match_windows
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_oracle_line_probe.py
                $<TARGET_FILE:vitreo> k32_ntdll_misc_forwarder_probe
    )
endif()

# ResolveDelayLoadedAPI / ResolveDelayLoadsFromDll (and their ntdll Ldr
# names), x64 and x86: IAT and module-handle writes, the failure hooks and
# DELAYLOAD_INFO, and ResolveDelayLoadsFromDll on the probe's own
# delay-import directory (built with dlltool -y).
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_oracle_line_probe.py")
    add_test(
        NAME vitreo_delayload_matches_windows
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_oracle_line_probe.py
                $<TARGET_FILE:vitreo> delayload_probe
                --delayimp "normaliz.dll=IdnToAscii@20,IdnToUnicode@20"
                --delayimp "version.dll=GetFileVersionInfoSizeA@8,AaaVitreoNoSuchExport@0"
                --delayimp "vitreo_missing_dl.dll=VitreoMissingDlExport@0"
    )
endif()

# Vectored continue handlers (Add/RemoveVectoredContinueHandler and their
# ntdll names), x64 and x86: run after a vectored handler, a frame-based
# handler or the top-level filter continues, in list order, CONTEXT edits
# applied, stopped by EXCEPTION_CONTINUE_EXECUTION; separate handle lists;
# NULL handlers accepted.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_oracle_line_probe.py")
    add_test(
        NAME vitreo_vectored_continue_matches_windows
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_oracle_line_probe.py
                $<TARGET_FILE:vitreo> vectored_continue_probe
    )
endif()

# ntdll's event services and their names in the Win32 BaseNamedObjects
# namespace, x64 and x86, compared with the Windows recording.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_nt_event_probe.py")
    add_test(
        NAME vitreo_nt_event_matches_windows
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_nt_event_probe.py
                $<TARGET_FILE:vitreo>
    )
endif()

# Every kernel32 export Windows forwards to ntdll is also an ntdll export
# under its forwarder name, x64 and x86 (table from dumpbin /exports).
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_ntdll_forwarders.py")
    add_test(
        NAME vitreo_ntdll_forwarders_exported
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_ntdll_forwarders.py
                $<TARGET_FILE:vitreo>
    )
endif()

# The path GetModuleFileNameW reports for a built-in DLL is a protected file
# holding the module's image, compared with the Windows recording.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_builtin_dll_file_probe.py")
    add_test(
        NAME vitreo_builtin_dll_files_match_windows
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_builtin_dll_file_probe.py
                $<TARGET_FILE:vitreo>
    )
endif()

# The WSAEventSelect network event record (when FD_* events are recorded,
# re-enabled and reported), compared with the Windows recording.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/check_ws2_event_record_probe.py")
    add_test(
        NAME vitreo_ws2_event_record_matches_windows
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_ws2_event_record_probe.py
                $<TARGET_FILE:vitreo>
    )
endif()

# The baseline gate is a RATCHET: JIT coverage may rise freely, never fall.
# It exists because a lowering that stops being *reached* — a dispatch chain
# edit, a stencil-gate removal — passes every behavioural test in the suite,
# since the interpreter delegate returns identical answers. That is exactly how
# x87 sat at 0% on 64-bit blocks unnoticed.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/isa_audit.py")
    add_test(
        NAME vitreo_isa_coverage_ratchet
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/isa_audit.py
                --check-baseline ${CMAKE_SOURCE_DIR}/tools/isa_baseline.json
    )
endif()

# ------------------------------------------------------------------------------
# Instruction-FORM coverage gate (jit-refactor/, blueprint section 3)
# ------------------------------------------------------------------------------
#
# The two gates above are keyed on ICLASS MNEMONICS. An instruction counts as
# covered as soon as ONE of its encodings is, which is a real blind spot and
# not a theoretical one: it is why the JIT could execute VEX ADDPS through the
# legacy two-operand ADDPS lowering -- ignoring vvvv, never zeroing
# YMM[255:128] -- while the audit reported ADDPS covered, and why the x87
# fallback could lack FDIV m32real/m64real while reporting FDIV covered.
#
# This gate is keyed on architectural FORMS generated from Intel XED's ISA
# database (10,974 of them, against 1,270 Opcode enum entries), and fails when
# a CPUID-advertised feature gains a form Vitreo cannot decode. The 59 forms
# already in that state when the gate was introduced are enumerated with a
# reason each in generated/unsupported_allowlist.csv; deleting a line from
# that file is how a fix is claimed, and the gate can only get stricter.
#
# The catalog is committed, so this test needs no network and no XED checkout.
# Regenerate it deliberately:
#   jit-refactor/tools/generate_isa_catalog.py <xed>/datafiles \
#       jit-refactor/generated/OPCODE_FORMS.csv
# The generated opcode-identifier table is indexed positionally, so an enum
# entry added without regenerating it shifts every later name by one -- the
# table becomes confidently wrong rather than obviously broken. The
# Decoder.OpcodeNamesAreNotShifted unit test catches a shift at its anchors;
# this catches the table being stale at all.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/gen_opcode_names.py")
    add_test(
        NAME vitreo_opcode_names_current
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_opcode_names.py --check
    )
endif()

# ADR-0006. A sequence duplicated across a 32-bit and a 64-bit file is how a
# fix reaches one guest and not the other: emit_set_lazy_flags64 held its own
# copy of the lazy-flag publication, so a two-store optimisation landed on
# 32-bit and silently missed x64 while every gate stayed green. This ratchets
# that count down and never lets it rise.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/jit_dup_scan.py")
    add_test(
        NAME vitreo_jit_duplication_ratchet
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/jit_dup_scan.py check
                ${CMAKE_SOURCE_DIR}/jit-refactor/generated/jit_duplication_baseline.json
    )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/jit-refactor/tools/isa_coverage.py")
    add_test(
        NAME vitreo_isa_form_coverage
        COMMAND python3 ${CMAKE_SOURCE_DIR}/jit-refactor/tools/isa_coverage.py
                --catalog ${CMAKE_SOURCE_DIR}/jit-refactor/generated/OPCODE_FORMS.csv
                --annotations ${CMAKE_SOURCE_DIR}/jit-refactor/annotations.csv
                --out ${CMAKE_BINARY_DIR}/OPCODE_COVERAGE.csv
                --allowlist
                  ${CMAKE_SOURCE_DIR}/jit-refactor/generated/unsupported_allowlist.csv
                --ratchet
                  ${CMAKE_SOURCE_DIR}/jit-refactor/generated/coverage_baseline.json
                --check --quiet
    )
endif()

# ------------------------------------------------------------------------------
# Level 1: pure units — no runtime, no frameworks
# ------------------------------------------------------------------------------

vitreo_add_test(NAME vitreo_base_tests
    SOURCES
        test_result.cpp
        test_environment_flag.cpp
        service_registry_test.cpp
        base/test_diagnostics.cpp
        base/test_present_cpu.cpp
    DEPS vitreo_core vitreo_base
)

set(VITREO_MEMORY_TEST_SOURCES
    memory/test_memory.cpp
    memory/test_range_bounds.cpp
    memory/test_linear_reservation.cpp
    memory/test_linear_jit_view.cpp
    memory/test_identity_mapping.cpp
    memory/test_write_watch.cpp
    memory/test_jit_code_page_scan.cpp
    memory/test_x64_high_arena.cpp
)

# test_recoverable_fault.cpp exercises recovery from a fault taken inside
# JIT-generated code. Its stub is hand-assembled ARM64 machine words in a
# MAP_JIT page, so it is meaningless on x86_64 -- and worse than meaningless
# if merely made to compile: the binary builds and then dies with SIGILL on
# the first ARM64 instruction. Exclude it by architecture rather than
# #ifdef-ing the body, so the reason stays visible at the build level.
if(VITREO_ARCH_ARM64)
    list(APPEND VITREO_MEMORY_TEST_SOURCES memory/test_recoverable_fault.cpp)
endif()

# Native x86_64 backend: segment/TEB handling. These tests execute segment
# loads and read through GS, so they only exist on the Intel backend.
if(VITREO_ARCH_X86_64)
    vitreo_add_test(NAME vitreo_cpu_x64_tests
        SOURCES
            cpu/test_x64_segments.cpp
            cpu/test_segment_override_rewrite.cpp
            cpu/test_x64_guest_thread.cpp
            cpu/test_native64.cpp
            cpu/test_compat32.cpp
            cpu/test_x64_exception_bridge.cpp
        DEPS
            vitreo_cpu_x64
            vitreo_memory
            vitreo_cpu_decode
            vitreo_cpu_interpreter
            vitreo_cpu_jit
    )
endif()

vitreo_add_test(NAME vitreo_compression_tests
    SOURCES
        core/compression/test_cabinet_mszip.cpp
    DEPS vitreo_core vitreo_compression
)

# Exercise the real media decoder with a controlled slow host audio device.
# Linking only this backend prevents the device substitute affecting WinAPI
# or CoreAudio tests and keeps the timing regression independent of hardware.
vitreo_add_test(NAME vitreo_media_startup_tests
    SOURCES
        audio/test_media_startup.cpp
        ${CMAKE_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_media_player.cpp
    DEPS vitreo_core
)

vitreo_add_test(NAME vitreo_memory_tests
    SOURCES ${VITREO_MEMORY_TEST_SOURCES}
    DEPS vitreo_memory
)

vitreo_add_test(NAME vitreo_checkpoint_tests
    SOURCES
        runtime/test_checkpoint.cpp
    DEPS
        vitreo_checkpoint
        vitreo_memory
        vitreo_cpu_state
)

vitreo_add_test(NAME vitreo_capsule_tests
    SOURCES
        capsule/test_vfs.cpp
        capsule/test_iso_filesystem.cpp
        capsule/test_image_mount_store.cpp
        capsule/test_paths.cpp
        capsule/vfs/test_directory.cpp
        capsule/vfs/test_file_stream.cpp
        capsule/env/test_env_store.cpp
        capsule/registry/test_registry.cpp
    DEPS vitreo_capsule_vfs vitreo_capsule_env vitreo_capsule_registry
)

# ------------------------------------------------------------------------------
# CPU: decode, interpreter, JIT, SEH
# ------------------------------------------------------------------------------

vitreo_add_test(NAME vitreo_cpu_tests
    SOURCES
        winapi/test_guest_crt_accel.cpp
        winapi/test_guest_accel_x64.cpp
        flag_state_test.cpp
        exception_state_test.cpp
        cpu/test_state.cpp
        cpu/test_decoder.cpp
        cpu/test_vex_alias_routing.cpp
        cpu/test_interpreter.cpp
        cpu/test_arithmetic.cpp
        cpu/test_zero_extension.cpp
        cpu/test_mov.cpp
        cpu/test_stack.cpp
        cpu/test_logical.cpp
        cpu/test_control.cpp
        cpu/test_cmp.cpp
        cpu/test_lea.cpp
        cpu/test_jumps.cpp
        cpu/test_shifts.cpp
        cpu/test_mul_div.cpp
        cpu/test_inc_dec_adc.cpp
        cpu/test_rotates.cpp
        cpu/test_atomics_misc.cpp
        cpu/test_bit_ops.cpp
        cpu/test_cmov_setcc.cpp
        cpu/test_string_ops.cpp
        cpu/test_indirect_ctrl.cpp
        cpu/test_sse_arith.cpp
        cpu/test_sse_data.cpp
        cpu/test_sse_conv.cpp
        cpu/test_sse_cmp.cpp
        cpu/test_sse_int.cpp
        cpu/test_sse_shuffle.cpp
        cpu/test_fpu.cpp
        cpu/test_mmx.cpp
        cpu/test_crypto_ops.cpp
        cpu/test_sse4.cpp
        cpu/test_vptest_contract.cpp
        cpu/test_legacy_usermode_isa.cpp
        cpu/test_avx_bmi.cpp
        cpu/test_avx_permute_gather.cpp
        cpu/test_avx256_widening.cpp
        cpu/test_adx_lazy_flags.cpp
        cpu/test_maskmov_fault_suppression.cpp
        cpu/test_popcnt.cpp
        cpu/test_cpuid_extended.cpp
        cpu/test_cpuid_host_identity.cpp
        cpu/test_seh.cpp
        cpu/test_seh32.cpp
        cpu/test_context64_record.cpp
        cpu/test_block_cache.cpp
        cpu/test_block_cache_reentry.cpp
        cpu/test_block_decoder.cpp
        cpu/test_execute_only_fetch.cpp
        cpu/test_jit_codegen_dump.cpp
        cpu/test_decode_memo.cpp
        cpu/test_ir_release.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/execution/block_decoder.cpp
        cpu/test_jit_clobber_contract.cpp
        cpu/test_jit_execution.cpp
        cpu/test_jit_x87_native.cpp
        cpu/test_jit_esp_frame.cpp
        cpu/test_jit_short_rep.cpp
        cpu/test_jit_fault_precision.cpp
        cpu/test_jit_internal_jcc.cpp
        cpu/test_jit_compile_memo.cpp
        cpu/test_jit_translation_store.cpp
        cpu/test_jit_comba_throughput.cpp
        cpu/test_jit_fused_jcc_cmov.cpp
        cpu/test_jit_link_tail.cpp
        cpu/test_jit_link_patch.cpp
        cpu/test_jit_link_call_ret.cpp
        cpu/test_jit_link_chkstk.cpp
        cpu/test_jit_sticky_link.cpp
        cpu/test_jit_jcc_first_linked.cpp
        cpu/test_jit_jcc_first_lazy.cpp
        cpu/test_jit_native_alu_forms.cpp
        cpu/test_jit_popcnt_cmpxchg16b.cpp
        cpu/test_jit_sse_flag_terminators.cpp
        cpu/test_jit_hop_budget.cpp
        cpu/test_jit_jcc_retarget.cpp
        cpu/test_jit_cold_residency.cpp
        cpu/test_jit_ir_tier.cpp
        cpu/test_jit_sse_forwarding.cpp
        cpu/test_jit_native_callret.cpp
        cpu/test_jit_x64_call_ret.cpp
        cpu/test_jit_lowering_shape.cpp
        cpu/test_jit_mode64_pinned_add.cpp
        cpu/test_jit_mode64_pinned_sub.cpp
        cpu/test_jit_mode64_pinned_reg_alu.cpp
        cpu/test_jit_mode64_pinned_stack.cpp
        cpu/test_jit_shared_link_probe.cpp
        cpu/test_jit_jcc_first64.cpp
        cpu/test_jit_host_fp_env.cpp
        cpu/test_jit_store_ring.cpp
        cpu/test_jit_arm64_field_encoding.cpp
        cpu/test_jit_branch_reach_decline.cpp
        cpu/test_jit_shared_table.cpp
        cpu/test_jit_memo_lock.cpp
        cpu/test_jit_memory_footprint.cpp
        cpu/test_jit_inline_store.cpp
        cpu/test_jit_x64_inline_mem.cpp
        cpu/test_jit_mem_oolining.cpp
        cpu/test_jit_linear.cpp
        cpu/test_jit_linear64_dispatch.cpp
        cpu/test_jit_linear64_entry_proof.cpp
        cpu/test_jit_linear64_guard_shape.cpp
        cpu/test_jit_linear64_tso_crossing.cpp
        cpu/test_jit_linear64_semantics.cpp
        cpu/test_jit_linear64_high_address.cpp
        cpu/test_jit_linear64_extension.cpp
        cpu/test_jit_linear64_policy.cpp
        cpu/test_jit_linear64_load_faults.cpp
        cpu/test_jit_linear64_reservation_edges.cpp
        cpu/test_jit_linear64_store_guards.cpp
        cpu/test_jit_linear64_mixed_page.cpp
        cpu/test_jit_linear64_shared_helpers.cpp
        cpu/test_jit_linear64_random_sse.cpp
        cpu/test_jit_linear_access.cpp
        cpu/test_jit_linear_arena_reclaim.cpp
        cpu/test_jit_linear_arena_safepoint.cpp
        cpu/test_jit_linear_arena_epoch.cpp
        cpu/test_jit_linear_smc.cpp
        cpu/test_jit_linear64_check_byte.cpp
        cpu/test_jit_linear64_high_region.cpp
        cpu/test_jit_linear64_lock_rmw.cpp
        cpu/test_jit_linear_tso.cpp
        cpu/test_jit_linear64_replay.cpp
        cpu/test_jit_linear_view.cpp
        cpu/test_jit_linear_store_checkbyte.cpp
        cpu/test_jit_mmx64.cpp
        cpu/test_jit_crc32.cpp
        cpu/test_jit_sse2_native.cpp
        cpu/test_jit_sha256_native.cpp
        cpu/test_jit_aes_native.cpp
        cpu/test_jit_sse_histogram.cpp
        cpu/test_jit_coverage.cpp
        cpu/test_jit_provenance.cpp
        cpu/test_perf_window_helper_mem.cpp
        cpu/test_flag_liveness.cpp
        cpu/test_jit_ir.cpp
        cpu/test_ir_frontend.cpp
        cpu/test_ir_seam_a.cpp
        cpu/test_ir_emit.cpp
        cpu/test_ir_emit_linear.cpp
        cpu/test_ir_emit_store_protection.cpp
        cpu/test_ir_emit_sse.cpp
        cpu/test_ir_emit_narrow.cpp
        cpu/test_ir_emit_segment.cpp
        cpu/test_ir_emit_shift.cpp
        cpu/test_ir_emit_tier0_narrow.cpp
        cpu/test_ir_emit_branch.cpp
        cpu/test_ir_emit_call_stack.cpp
        cpu/test_ir_emit_folded_memory_operands.cpp
        cpu/test_ir_emit_inc_dec.cpp
        cpu/test_ir_emit_leave_ret.cpp
        cpu/test_ir_emit_regalloc_fusion.cpp
        cpu/test_ir_emit_scalar_reuse.cpp
        cpu/test_ir_emit_snapshot_backing.cpp
        cpu/test_ir_x64_memory.cpp
        cpu/test_ir_x64_flag_recovery.cpp
        cpu/test_ir_x64_carry_chain.cpp
        cpu/test_ir_x64_alu_memory.cpp
        cpu/test_ir_x64_widen.cpp
        cpu/test_ir_nop.cpp
        cpu/test_ir_pe_block_differential.cpp
        cpu/test_ir_x64_narrow_mov.cpp
        cpu/test_ir_x64_narrow_alu_1_integer.cpp
        cpu/test_ir_x64_narrow_alu_2_sse_register.cpp
        cpu/test_ir_x64_narrow_alu_3_sse_memory.cpp
        cpu/test_ir_x64_narrow_alu_4_stack.cpp
        cpu/test_ir_x64_narrow_alu_5_lea_xchg_muldiv.cpp
        cpu/test_ir_x64_narrow_alu_6_shifts_conditions.cpp
        cpu/test_ir_x64_fused_branch.cpp
        cpu/test_ir_x64_side_exit.cpp
        cpu/test_ir_x64_addressing.cpp
        cpu/test_ir_spill_frame.cpp
        cpu/test_ir_passes.cpp
        cpu/test_jit_split_layout.cpp
        cpu/test_materialize_fast_path.cpp
        cpu/test_df_preserves_lazy_flags.cpp
        cpu/test_jit_parity.cpp
        cpu/test_jit_parity_arith.cpp
        cpu/test_jit_parity_carry.cpp
        cpu/test_jit_parity_comis_lahf.cpp
        cpu/test_jit_parity_control_flow.cpp
        cpu/test_jit_parity_crypto.cpp
        cpu/test_jit_parity_lea_mul_div.cpp
        cpu/test_jit_parity_lock.cpp
        cpu/test_jit_parity_mov.cpp
        cpu/test_jit_parity_native_alu.cpp
        cpu/test_jit_parity_rotates.cpp
        cpu/test_jit_parity_sequences.cpp
        cpu/test_jit_parity_shifts.cpp
        cpu/test_jit_parity_sse.cpp
        cpu/test_jit_parity_sse_convert.cpp
        cpu/test_jit_parity_sse_fp_arith.cpp
        cpu/test_jit_parity_sse_movdqu.cpp
        cpu/test_jit_parity_stack.cpp
        cpu/test_jit_parity_string_ops.cpp
        cpu/test_jit_parity_vex.cpp
        cpu/test_jit_parity_x87.cpp
        cpu/test_jit_parity_x87_memory_forms.cpp
        cpu/test_jit_parity_x87_results.cpp
        cpu/test_jit_sse_fp_parity.cpp
        cpu/test_jit_parity_64.cpp
        cpu/test_jit_parity_64_01_guarded_ud2.cpp
        cpu/test_jit_parity_64_02_alu_basic.cpp
        cpu/test_jit_parity_64_03_narrow_mem_movsx.cpp
        cpu/test_jit_parity_64_04_stack_control.cpp
        cpu/test_jit_parity_64_05_muldiv.cpp
        cpu/test_jit_parity_64_06_sse.cpp
        cpu/test_jit_parity_64_07_cmov_shift_rotate.cpp
        cpu/test_jit_parity_64_08_shift32.cpp
        cpu/test_jit_parity_64_09_sse_moves.cpp
        cpu/test_jit_parity_64_10_cmov32_rotate.cpp
        cpu/test_jit_parity_64_11_jcc_fusion.cpp
        cpu/test_jit_parity_64_12_shift_jcc_fusion.cpp
        cpu/test_jit_parity_64_13_inline_memory.cpp
        cpu/test_jit_parity_64_14_shld_bt.cpp
        cpu/test_jit_parity_64_15_string_simd_mem.cpp
        cpu/test_jit_parity_64_16_xchg_lock.cpp
        cpu/test_jit_parity_64_17_vector_misc.cpp
        cpu/test_jit_parity_64_18_adx_wide_mul.cpp
        cpu/test_jit_parity_64_19_narrow_long_mode.cpp
        cpu/test_jit_parity_64_20_helper_marshal_x87.cpp
        cpu/test_jit_parity_64_21_long_mode_width.cpp
        cpu/test_jit_parity_64_22_cvt_broadcast.cpp
        cpu/test_jit_parity_64_23_store_jcc_fusion.cpp
        cpu/test_jit_parity_64_24_vex256_lane_split.cpp
        cpu/test_jit_parity_64_25_vex256_fma_integer.cpp
        cpu/test_jit_parity_64_26_mem_operand_simd.cpp
        cpu/test_jit_parity_64_27_rep_string.cpp
        cpu/test_jit_parity_64_28_sse_alignment.cpp
        cpu/test_jit_parity_64_29_delegation_sweep.cpp
        cpu/test_jit_parity_64_30_round_lane_tranche.cpp
        cpu/test_jit_parity_64_31_flag_codegen.cpp
        cpu/test_jit_parity_64_32_flag_sweep.cpp
        cpu/test_jit_parity_64_33_sse_fallback_forms.cpp
        cpu/test_jit_parity_64_34_partial_writer_liveness.cpp
        cpu/test_jit_lock_prefix.cpp
        cpu/test_jit_idiom_fusion.cpp
        cpu/test_jit_crt_shift_inline.cpp
        cpu/test_jit_terminator_size.cpp
        cpu/test_jit_emission_golden.cpp
        cpu/test_sse_nan_invalid.cpp
        cpu/test_host_fp_environment_scope.cpp
        cpu/test_hardware_differential.cpp
        cpu/test_string_repeat_prefix.cpp
        cpu/test_advertised_isa_executes.cpp
    DEPS
        vitreo_cpu_state
        vitreo_cpu_decode
        vitreo_cpu_interpreter
        vitreo_cpu_jit
        vitreo_cpu_seh
        vitreo_memory
        vitreo_winapi_core
)

# The emission golden baseline is reviewed input that lives beside its test,
# not a build artifact, so the test needs to be told where its own source is.
target_compile_definitions(vitreo_cpu_tests PRIVATE
    VITREO_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cpu")

# The opt-in ESP-frame form ([esp+disp] through X15) is parsed once per
# process (esp_frame_enabled's static) and belongs to Tier 0, which linear
# 32-bit blocks only reach with Tier 1 off, so its displacement-range test
# needs its own process with both knobs set; in the default suite it skips.
add_test(NAME vitreo_cpu_tests_esp_frame_on
    COMMAND vitreo_cpu_tests
        --gtest_filter=JitLinearTest.OrderedEspFrameAccessesReachPastTwelveBitDisplacements)
set_tests_properties(vitreo_cpu_tests_esp_frame_on PROPERTIES
    ENVIRONMENT "VITREO_JIT_ESP_FRAME=1;VITREO_JIT_IR=0")

# The short-REP threshold knob is the immediate of one CMP. 4096 once parsed
# as valid and encoded CMP #0; past the 12-bit field it is now clamped to 4095
# (one log line), never masked or silently dropped. Parsed once per process,
# hence its own run.
add_test(NAME vitreo_cpu_tests_short_rep_knob_overflow
    COMMAND vitreo_cpu_tests
        --gtest_filter=JitExecTest.ShortRepKnobPastTheCmpFieldClampsTo4095:JitExecTest.ShortRepMovsb*:JitExecTest.ShortRepStosdMatchesInterpreter:JitExecTest.LongRepMovsbUsesHelperAndMatchesInterpreter:JitParity64Test.NativeRepMovsMatchesInterpreterAtEveryWidthAndDirection:JitParity64Test.NativeRepStosMatchesInterpreterAtEveryWidthAndDirection)
set_tests_properties(vitreo_cpu_tests_short_rep_knob_overflow PROPERTIES
    ENVIRONMENT "VITREO_JIT_SHORT_REP=4096")

# These require a fresh process: register pinning is chosen at startup.
# Both tests crashed before the pinless helper/recovery fixes.
add_test(NAME vitreo_jit_pinless_regressions
    COMMAND vitreo_cpu_tests
        --gtest_filter=JitLinear64Test.PublishesHelpersCreatedAfterFirstExecution:JitExecTest.InternalForwardJccUsesShr16ResultFlags:IrEmitFaultRecovery.PinlessBuildRetainsFaultObservableRecords)
set_tests_properties(vitreo_jit_pinless_regressions PROPERTIES
    ENVIRONMENT "VITREO_JIT_NO_PIN=1;VITREO_JIT_IR=32")

# ------------------------------------------------------------------------------
# CPU: hardware differential oracle
# ------------------------------------------------------------------------------
#
# Every other divergence test compares Vitreo to Vitreo, which cannot catch an
# interpreter that was wrong to begin with. This builds the same instruction
# executor for x86_64; the host x86 runner runs it, so `test_hardware_differential.cpp`
# can compare against a real x86 core rather than against our reading of the
# SDM.
#
# The oracle has no Vitreo dependencies on purpose: it is compiled for a
# different architecture than everything around it, so it cannot link the
# runtime.
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    add_executable(vitreo_x86_oracle
        ${CMAKE_CURRENT_SOURCE_DIR}/cpu/hardware_oracle/x86_reference.cpp)
    target_include_directories(vitreo_x86_oracle PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/cpu/hardware_oracle)
    set_target_properties(vitreo_x86_oracle PROPERTIES
        OSX_ARCHITECTURES "x86_64"
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    # This file assembles x86 machine code by hand; the C-style casts are the
    # readable form for that and are deliberate, as they are for gtest above.
    target_compile_options(vitreo_x86_oracle PRIVATE -Wno-old-style-cast)
    add_dependencies(vitreo_cpu_tests vitreo_x86_oracle)
    # Passed as a definition rather than a ctest environment entry because the
    # suite uses gtest discovery, so there is no single ctest test to attach an
    # environment to. VITREO_X86_ORACLE overrides it at run time; the test
    # skips itself if neither resolves, so a machine without the host x86 runner still runs
    # the rest of the suite.
    target_compile_definitions(vitreo_cpu_tests PRIVATE
        VITREO_X86_ORACLE_PATH="${CMAKE_CURRENT_BINARY_DIR}/vitreo_x86_oracle")
endif()

# ------------------------------------------------------------------------------
# CPU: Tier 1 IR gate A/B
# ------------------------------------------------------------------------------
#
# The same parity binary, re-run with the Tier 1 gate ON. Both arms assert
# against the interpreter, so a Tier 1 block that diverges fails here even
# though the default-off arm stays green.
#
# This has to be a separate ctest entry rather than a test inside the binary:
# `JitCompiler::ir_policy()` caches its parse in a `static const` ON PURPOSE --
# a gate that could change mid-process would let an IR-compiled block link to a
# stencil block emitted under a different pin ABI. So the only way to exercise
# the enabled arm is a fresh process with a different environment.
#
# `=all` rather than `=32` deliberately: it exercises admitted x64 forms and
# the decline path for unsupported forms. IrEmit64 asserts IR admission so
# an interpreter or baseline fallback cannot masquerade as x64 IR coverage.
# The filter is INTERPRETER-ORACLE tests plus the Tier 1 emission suite.
# `JitExecTest.*` is deliberately excluded: much of it asserts Tier 0's emitted
# SHAPE (`code_contains_word(adds_w(23,23,25))`, `nzcv_kind != 0`). The IR
# backend can choose different registers and flag representations for the same
# semantics. This gate checks execution through interpreter-oracle tests;
# baseline emission-shape tests remain in the default suite.
add_test(
    NAME vitreo_cpu_tests_ir_gate_on
    COMMAND $<TARGET_FILE:vitreo_cpu_tests>
            --gtest_filter=JitParityTest.*:JitParity64Test.*:IrEmit*:IrFrontend.*:IrSeamA.*:IrPasses.*:JitIr.*:SseSemantics.*:*IrX64FlagRecovery.*:JitExecTest.MidBlockJccKeepsItsSlotBesideATerminalSlot
)
set_tests_properties(vitreo_cpu_tests_ir_gate_on PROPERTIES
    ENVIRONMENT "VITREO_JIT_IR=all"
)

# ------------------------------------------------------------------------------
# CPU: the linear64 model and the JIT view (dense translator Phase A)
# ------------------------------------------------------------------------------
#
# JitParity64Test normally runs on the scattered model. These reruns put the
# same interpreter-oracle tests on the hardware-protected linear reservation,
# then on the JIT view, where every x64 access goes through the enforcing
# alias. JitLinear64Test is
# not rerun: its emission-shape tests assert that X15 is the check map.
add_test(
    NAME vitreo_cpu_tests_parity_linear64
    COMMAND $<TARGET_FILE:vitreo_cpu_tests>
            --gtest_filter=JitParity64Test.*
)
set_tests_properties(vitreo_cpu_tests_parity_linear64 PROPERTIES
    ENVIRONMENT "VITREO_TEST_PARITY_LINEAR64=1;VITREO_JIT_LINEAR64=1"
)
add_test(
    NAME vitreo_cpu_tests_jit_view_on
    COMMAND $<TARGET_FILE:vitreo_cpu_tests>
            --gtest_filter=JitParity64Test.*:JitLinearViewTest.*
)
set_tests_properties(vitreo_cpu_tests_jit_view_on PROPERTIES
    ENVIRONMENT "VITREO_TEST_PARITY_LINEAR64=1;VITREO_JIT_LINEAR64=1;VITREO_JIT_VIEW=1"
)

# A fresh process disables the optional pinned-read reuse pass while retaining
# the established flag passes. Compare behaviour without asserting reuse.
add_test(
    NAME vitreo_cpu_tests_ir_read_reuse_off
    COMMAND $<TARGET_FILE:vitreo_cpu_tests>
            --gtest_filter=JitParityTest.*:JitParity64Test.*:IrEmit*-IrEmit.CrossInstructionPinnedReadReuse
)
set_tests_properties(vitreo_cpu_tests_ir_read_reuse_off PROPERTIES
    ENVIRONMENT "VITREO_JIT_IR=all;VITREO_JIT_IR_PASSES=7"
)

# Disable only scalar constant commoning, retaining pinned-value reuse and
# flag passes, to exercise the original instruction-local value shapes.
add_test(
    NAME vitreo_cpu_tests_ir_constant_reuse_off
    COMMAND $<TARGET_FILE:vitreo_cpu_tests>
            --gtest_filter=JitParityTest.*:JitParity64Test.*:IrEmit*
)
set_tests_properties(vitreo_cpu_tests_ir_constant_reuse_off PROPERTIES
    ENVIRONMENT "VITREO_JIT_IR=all;VITREO_JIT_IR_PASSES=15"
)

if(VITREO_ARCH_ARM64)
    add_test(
        NAME vitreo_cpu_tests_ir_x64_memory_linear
        COMMAND $<TARGET_FILE:vitreo_cpu_tests>
                --gtest_filter=IrEmit64Memory.*
    )
    set_tests_properties(vitreo_cpu_tests_ir_x64_memory_linear PROPERTIES
        ENVIRONMENT "VITREO_JIT_IR=all;VITREO_JIT_LINEAR64=1;VITREO_TEST_IR_LINEAR64=1"
    )
endif()

# ------------------------------------------------------------------------------
# CPU: JIT 64-bit dispatch-gap guard
# ------------------------------------------------------------------------------
#
# A source scan, not a behavioural test: it reads the JIT dispatch switches as
# text and links no Vitreo library, so it gets its own target rather than
# forcing VITREO_SOURCE_DIR (and a rebuild of the whole CPU suite) onto
# vitreo_cpu_tests.
vitreo_add_test(NAME vitreo_jit_dispatch_gap_tests
    SOURCES
        cpu/test_jit_dispatch_gap.cpp
    COMPILE_DEFINITIONS
        VITREO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# ------------------------------------------------------------------------------
# CPU: JIT code-arena churn
# ------------------------------------------------------------------------------
#
# Shrinks the process-wide arena from a static initializer (the arena is
# created once, at the first JitCompiler), so it cannot share a process with
# vitreo_cpu_tests. Several dispatcher-shaped threads compile past the budget
# and re-execute a hot set; it prints refusals / retirements / deferrals and
# asserts that recycled bytes are never executed.
vitreo_add_test(NAME vitreo_jit_arena_churn_tests
    SOURCES
        cpu/test_jit_arena_churn.cpp
    DEPS
        vitreo_cpu_state
        vitreo_cpu_decode
        vitreo_cpu_interpreter
        vitreo_cpu_jit
        vitreo_cpu_seh
        vitreo_memory
        vitreo_winapi_core
)

# Same static-initializer arena shaping (64 x 1 MB, 16 MB soft budget): live
# chunks stay within the soft budget, retired code is never entered, and
# recycled chunk pages leave phys_footprint.
vitreo_add_test(NAME vitreo_jit_arena_soft_budget_tests
    SOURCES
        cpu/test_jit_arena_soft_budget.cpp
    DEPS
        vitreo_cpu_state
        vitreo_cpu_decode
        vitreo_cpu_interpreter
        vitreo_cpu_jit
        vitreo_cpu_seh
        vitreo_memory
        vitreo_winapi_core
)

# Cold memory slow-path footprint: its own process, because the corpus
# reports reserve the production 2^46 / 4 GiB guest-base windows.
vitreo_add_test(NAME vitreo_jit_cold_stub_footprint_tests
    SOURCES
        cpu/test_jit_cold_stub_footprint.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/execution/block_decoder.cpp
    DEPS
        vitreo_cpu_state
        vitreo_cpu_decode
        vitreo_cpu_interpreter
        vitreo_cpu_jit
        vitreo_cpu_seh
        vitreo_memory
        vitreo_winapi_core
)

# Lazy cold memory stubs: its own process, because it retires arena chunks.
vitreo_add_test(NAME vitreo_jit_cold_lazy_tests
    SOURCES
        cpu/test_jit_cold_lazy.cpp
    DEPS
        vitreo_cpu_state
        vitreo_cpu_decode
        vitreo_cpu_interpreter
        vitreo_cpu_jit
        vitreo_cpu_seh
        vitreo_memory
        vitreo_winapi_core
)

# ------------------------------------------------------------------------------
# CPU: JIT interpret-first tier (hot threshold)
# ------------------------------------------------------------------------------
#
# Drives run_execution_loop / run_guest_callback, so it needs the execution
# runtime, which vitreo_cpu_tests deliberately does not link.
vitreo_add_test(NAME vitreo_jit_hot_threshold_tests
    SOURCES
        cpu/test_jit_hot_threshold.cpp
    DEPS
        vitreo_execution_runtime
        vitreo_winapi
        vitreo_windowing
        vitreo_graphics_core
        vitreo_graphics_shader
        vitreo_pe_loader
        vitreo_cpu_jit
        vitreo_memory
    FRAMEWORKS CoreFoundation CFNetwork Security
)


# ------------------------------------------------------------------------------
# PE loader
# ------------------------------------------------------------------------------

vitreo_add_test(NAME vitreo_pe_tests
    SOURCES
        pe/test_parser.cpp
        pe/test_manifest_info.cpp
        pe/test_version_info.cpp
        pe/test_mapper.cpp
        pe/test_image_placement.cpp
        pe/test_cor_image.cpp
        pe/test_relocations.cpp
        pe/test_imports.cpp
        pe/test_loader.cpp
        pe/test_load_config.cpp
        pe/test_tls_processor.cpp
        pe/test_teb_setup.cpp
        pe/test_delay_import.cpp
    DEPS
        vitreo_pe_parser
        vitreo_pe_mapper
        vitreo_pe_reloc
        vitreo_pe_imports
        vitreo_pe_loader
        vitreo_memory
)

# ------------------------------------------------------------------------------
# Graphics: shader translation and backend contracts
# ------------------------------------------------------------------------------
#
# Level 1 — pure translation. Runs anywhere, needs no GPU: SM3/DXBC in, MSL or
# SPIR-V out, compared as data.

vitreo_add_test(NAME vitreo_graphics_tests
    SOURCES
        gpu_device_test.cpp
        graphics/test_perf_diag.cpp
        graphics/test_present_rate.cpp
        graphics/test_gpu_busy.cpp
        graphics/test_binding_layout.cpp
        graphics/test_sm3_to_msl.cpp
        graphics/test_ff_vertex_layout.cpp
        graphics/test_ff_emulation_d3d_golden.cpp
        graphics/test_ff_emulation_gl.cpp
        graphics/test_d3d7_present.cpp
        ../src/winapi/graphics/ddraw/d3d7_present.cpp
        graphics/test_dxbc_to_spirv.cpp
        graphics/test_dxbc_to_spirv_stage_io.cpp
        graphics/test_dxbc_to_spirv_buffers_srv.cpp
        graphics/test_dxbc_to_spirv_buffers_uav.cpp
        graphics/test_dxbc_to_spirv_alu_float.cpp
        graphics/test_dxbc_to_spirv_alu_unary.cpp
        graphics/test_dxbc_to_spirv_alu_integer.cpp
        graphics/test_dxbc_to_spirv_bits_convert.cpp
        graphics/test_dxbc_to_spirv_control_flow.cpp
        graphics/test_dxbc_to_spirv_texture_sample.cpp
        graphics/test_dxbc_to_spirv_texture_fetch.cpp
        graphics/test_dxbc_to_spirv_geometry.cpp
        graphics/test_dxbc_to_spirv_compute.cpp
        graphics/test_dxbc_to_spirv_pixel_uav.cpp
        graphics/test_dxbc_to_spirv_oracle_blobs.cpp
        graphics/test_dxbc_sm51.cpp
        graphics/test_gsso_models.cpp
        graphics/test_pipeline_stats_models.cpp
        graphics/test_soft_double.cpp
        graphics/test_block_recycler.cpp
    DEPS
        vitreo_graphics_core
        vitreo_graphics_shader
    # test_soft_double.cpp reads WARP's words from
    # tools/oracle_probe/d3d11_doubles_probe.expected.
    COMPILE_DEFINITIONS
        VITREO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# Level 3 — platform adapter. Handing generated MSL to a real Metal device is
# the only way to prove it actually compiles, so this one needs the GPU backend
# and is kept apart from the translation tests rather than forcing Metal into
# their link.
if(APPLE)
    vitreo_add_test(NAME vitreo_graphics_metal_tests
        SOURCES
            graphics/test_generated_msl_compiles.cpp
            graphics/test_ff_emulation_gl_metal.cpp
            graphics/test_ff_emulation_d3d_metal.cpp
            graphics/test_metal_readback.cpp
            graphics/test_metal_shared_resource.cpp
            graphics/test_indirect_args.cpp
            graphics/test_descriptor_tables.cpp
            graphics/test_timeline_fence.cpp
            graphics/test_gpu_timestamp.cpp
            graphics/test_compute_fence.cpp
            graphics/test_metal_memory_floor.cpp
            graphics/test_autorelease_scope.mm
            graphics/test_gpu_watch.cpp
            graphics/test_loop_guard.cpp
            graphics/test_texture_zero_init.cpp
            graphics/test_volume_layered_rt.cpp
            graphics/test_typed_buffer_offset.cpp
            graphics/test_command_list.cpp
            graphics/test_copy_buffer_region.cpp
            graphics/test_metal_buffer_pool.cpp
            graphics/test_metal_buffer_rename.cpp
            graphics/test_stream_out_repack.cpp
            graphics/test_uav_counter.cpp
            graphics/test_pixel_uav.cpp
            graphics/test_render_pass_merge.cpp
            graphics/test_delta_record.cpp
            graphics/test_texture_view_memo.cpp
            graphics/test_texture1d.cpp
            graphics/test_depth_stencil_readback.cpp
            graphics/test_border_colour.cpp
            graphics/test_gather_compare.cpp
            graphics/test_integer_conversion_gpu.cpp
            graphics/test_shader_alu_gpu.cpp
            graphics/test_shader_double_gpu.cpp
            graphics/test_typed_uav_ordering_gpu.cpp
            graphics/test_resource_ops_gpu.cpp
            graphics/test_occlusion_gpu.cpp
            graphics/test_fl111_gpu.cpp
            graphics/test_ps_uav_msaa_gpu.cpp
            graphics/test_classlink_gpu.cpp
            graphics/test_pipeline_stats_gpu.cpp
            graphics/test_view_line_gpu.cpp
            graphics/test_minmax_gpu.cpp
            graphics/test_system_values.cpp
            graphics/test_tessellation.cpp
            graphics/test_gsso_gpu.cpp
            graphics/test_tiled_resources_gpu.cpp
            graphics/test_d3d7_present_metal.cpp
            ../src/winapi/graphics/ddraw/d3d7_present.cpp
            graphics/test_sm51_gpu.cpp
        DEPS
            vitreo_graphics_core
            vitreo_graphics_shader
            vitreo_graphics_metal
        # test_sm51_gpu.cpp reads WARP's output from
        # tools/oracle_probe/d3d12_sm51_exec_probe.expected.
        COMPILE_DEFINITIONS
            VITREO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )

    # D3D12 command lists executed on the GPU: the d3d12 front-end AND Metal.
    vitreo_add_test(NAME vitreo_d3d12_metal_tests
        SOURCES
            winapi/d3d12/test_d3d12_metal.cpp
        DEPS
            vitreo_winapi
            vitreo_windowing
            vitreo_graphics_core
            vitreo_graphics_shader
            vitreo_graphics_metal
            vitreo_pe_loader
            vitreo_memory
            vitreo_cpu_jit
        FRAMEWORKS CoreFoundation CFNetwork Security
    )

    # Direct3D 7 submission needs the ddraw front-end AND a real GPU, so it
    # cannot live in either vitreo_winapi_tests (no Metal) or the graphics
    # translation tests (no winapi).
    vitreo_add_test(NAME vitreo_ddraw_metal_tests
        SOURCES
            winapi/ddraw/test_ddraw_d3d7_metal.cpp
            winapi/graphics/test_gl_pixel_formats_metal.cpp
            winapi/graphics/test_gl_drawable.cpp
            winapi/graphics/test_gl_draw.cpp
            winapi/graphics/test_gl_raster.cpp
            winapi/graphics/test_gl_texture_metal.cpp
            winapi/graphics/test_gl_pixels_metal.cpp
            winapi/graphics/test_gl_display_list_metal.cpp
            winapi/graphics/test_gl_extensions_metal.cpp
        DEPS
            vitreo_winapi
            vitreo_windowing
            vitreo_graphics_core
            vitreo_graphics_shader
            vitreo_graphics_metal
            vitreo_pe_loader
            vitreo_memory
            vitreo_cpu_jit
        FRAMEWORKS CoreFoundation CFNetwork Security
    )

    # The D3D11 front-end on the real Metal backend, through the guest entry
    # points: what neither the mock-backend D3D11 tests nor the backend's own
    # GPU tests see (UE's colour-LUT bake through the geometry-shader
    # emulation into a Texture3D RTV).
    vitreo_add_test(NAME vitreo_d3d11_metal_tests
        SOURCES
            winapi/d3d11/test_d3d11_metal.cpp
            winapi/d3d11/test_d3d11_metal_typed_buffer_readers.cpp
            winapi/d3d11/test_d3d11_metal_typed_buffer_views.cpp
            winapi/d3d11/test_d3d11_metal_vt_readback.cpp
            winapi/d3d11/test_d3d11_metal_vt_pixel_atomics.cpp
            winapi/d3d11/test_d3d11_metal_vt_textures.cpp
            winapi/d3d11/test_d3d11_metal_vt_dump.cpp
            winapi/d3d11/test_d3d11_metal_array_windows.cpp
            winapi/d3d11/test_d3d11_metal_threading.cpp
        DEPS
            vitreo_winapi
            vitreo_windowing
            vitreo_graphics_core
            vitreo_graphics_shader
            vitreo_graphics_metal
            vitreo_pe_loader
            vitreo_memory
            vitreo_cpu_jit
        FRAMEWORKS CoreFoundation CFNetwork Security
    )
    # Expected files (Windows' own output) the scenes replay.
    target_compile_definitions(vitreo_d3d11_metal_tests PRIVATE
        VITREO_ORACLE_PROBE_DIR="${CMAKE_SOURCE_DIR}/tests/oracle_data")
endif()

# Scene comparison. Runs the same D3D program on native Windows and under
# Vitreo, then compares the rendered pixels. The levels above cannot see a
# defect that needs the real D3D9 front-end, the real Metal backend, and an
# actual draw all at once, which is where the vertex-buffer rotation
# regression lived.
#
# The scene runner is a Win32 binary, so it needs the mingw cross-compiler.
# When any prerequisite is missing the tests skip themselves rather than fail.
if(APPLE)
    find_program(VITREO_MINGW_CC i686-w64-mingw32-gcc)
    find_program(VITREO_MINGW64_CC x86_64-w64-mingw32-gcc)
    find_program(VITREO_SSH_EXE ssh)
    find_program(VITREO_SCP_EXE scp)
    set(VITREO_WINDOWS_ORACLE_HOST "" CACHE STRING
        "SSH host for native Windows scene comparison")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/d3d9_oracle/d3d9_scene_runner.c")

    if(VITREO_MINGW_CC)
        set(_oracle_runner ${CMAKE_CURRENT_BINARY_DIR}/d3d9_scene_runner.exe)
        add_custom_command(
            OUTPUT ${_oracle_runner}
            COMMAND ${VITREO_MINGW_CC} -O1 -o ${_oracle_runner}
                    ${CMAKE_CURRENT_SOURCE_DIR}/d3d9_oracle/d3d9_scene_runner.c
                    -ld3d9 -lgdi32 -luser32
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/d3d9_oracle/d3d9_scene_runner.c
            COMMENT "Building D3D9 oracle scene runner (win32)"
            VERBATIM
        )
        add_custom_target(vitreo_d3d9_oracle_runner DEPENDS ${_oracle_runner})

        set(_present_runner ${CMAKE_CURRENT_BINARY_DIR}/d3d9_present_runner.exe)
        add_custom_command(
            OUTPUT ${_present_runner}
            COMMAND ${VITREO_MINGW_CC} -O1 -o ${_present_runner}
                    ${CMAKE_CURRENT_SOURCE_DIR}/d3d9_oracle/d3d9_present_runner.c
                    -ld3d9 -lgdi32 -luser32
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/d3d9_oracle/d3d9_present_runner.c
            COMMENT "Building D3D9 present oracle runner (win32)"
            VERBATIM
        )
        add_custom_target(vitreo_d3d9_present_runner DEPENDS ${_present_runner})

        set(_d3d11_oracle_runner
            ${CMAKE_CURRENT_BINARY_DIR}/d3d11_scene_runner.exe)
        add_custom_command(
            OUTPUT ${_d3d11_oracle_runner}
            COMMAND ${VITREO_MINGW_CC} -O1 -Wall -Wextra
                    -o ${_d3d11_oracle_runner}
                    ${CMAKE_CURRENT_SOURCE_DIR}/d3d11_oracle/d3d11_scene_runner.c
                    -ld3d11
            DEPENDS
                    ${CMAKE_CURRENT_SOURCE_DIR}/d3d11_oracle/d3d11_scene_runner.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/d3d11_oracle/d3d11_shaders.h
                    ${CMAKE_CURRENT_SOURCE_DIR}/graphics/system_value_shaders.h
                    ${CMAKE_CURRENT_SOURCE_DIR}/graphics/gs_sm5_shaders.h
                    ${CMAKE_CURRENT_SOURCE_DIR}/graphics/compute_readback_shaders.h
                    ${CMAKE_CURRENT_SOURCE_DIR}/graphics/tessellation_shaders.h
            COMMENT "Building D3D11 scene runner (win32)"
            VERBATIM
        )
        add_custom_target(vitreo_d3d11_oracle_runner
            DEPENDS ${_d3d11_oracle_runner})

        # Timing-contract probe. Built here rather than hand-compiled so the
        # same binary can be run under Vitreo and on Windows, and the two
        # outputs diffed.
        set(_timing_runner ${CMAKE_CURRENT_BINARY_DIR}/timing_runner.exe)
        add_custom_command(
            OUTPUT ${_timing_runner}
            COMMAND ${VITREO_MINGW_CC} -O1 -Wall -Wextra -o ${_timing_runner}
                    ${CMAKE_CURRENT_SOURCE_DIR}/timing_oracle/timing_runner.c
                    -lwinmm -luser32
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/timing_oracle/timing_runner.c
            COMMENT "Building timing contract runner (win32)"
            VERBATIM
        )
        add_custom_target(vitreo_timing_runner DEPENDS ${_timing_runner})
    else()
        set(_oracle_runner "")
        set(_present_runner "")
        set(_d3d11_oracle_runner "")
        message(STATUS
            "i686-w64-mingw32-gcc not found: D3D9 scene comparison will skip")
    endif()

    if(NOT VITREO_SSH_EXE)
        set(VITREO_SSH_EXE "ssh")
    endif()
    if(NOT VITREO_SCP_EXE)
        set(VITREO_SCP_EXE "scp")
    endif()

    vitreo_add_test(NAME vitreo_d3d9_oracle_tests
        SOURCES
            d3d9_oracle/test_d3d9_oracle.cpp
            d3d9_oracle/d3d9_oracle_compare.cpp
            d3d9_oracle/d3d9_present_oracle.cpp
            d3d9_oracle/test_d3d9_present_oracle.cpp
        DEPS
            vitreo_base
        COMPILE_OPTIONS
            -I${CMAKE_CURRENT_SOURCE_DIR}/d3d9_oracle
    )
    target_compile_definitions(vitreo_d3d9_oracle_tests PRIVATE
        VITREO_ORACLE_RUNNER_EXE="${_oracle_runner}"
        VITREO_PRESENT_RUNNER_EXE="${_present_runner}"
        VITREO_ORACLE_VITREO_EXE="$<TARGET_FILE:vitreo>"
        VITREO_ORACLE_SSH_EXE="${VITREO_SSH_EXE}"
        VITREO_ORACLE_SCP_EXE="${VITREO_SCP_EXE}"
        VITREO_ORACLE_WINDOWS_HOST="${VITREO_WINDOWS_ORACLE_HOST}"
    )
    add_dependencies(vitreo_d3d9_oracle_tests vitreo)
    if(VITREO_MINGW_CC)
        add_dependencies(vitreo_d3d9_oracle_tests vitreo_d3d9_oracle_runner)
        add_dependencies(vitreo_d3d9_oracle_tests vitreo_d3d9_present_runner)
    endif()

    vitreo_add_test(NAME vitreo_d3d11_oracle_tests
        SOURCES
            d3d11_oracle/test_d3d11_oracle.cpp
            d3d11_oracle/d3d11_oracle_compare.cpp
            graphics_oracle/image_compare.cpp
        DEPS
            vitreo_base
        COMPILE_OPTIONS
            -I${CMAKE_CURRENT_SOURCE_DIR}/d3d11_oracle
            -I${CMAKE_CURRENT_SOURCE_DIR}/graphics_oracle
    )
    target_compile_definitions(vitreo_d3d11_oracle_tests PRIVATE
        VITREO_D3D11_ORACLE_RUNNER_EXE="${_d3d11_oracle_runner}"
        VITREO_ORACLE_VITREO_EXE="$<TARGET_FILE:vitreo>"
        VITREO_ORACLE_SSH_EXE="${VITREO_SSH_EXE}"
        VITREO_ORACLE_SCP_EXE="${VITREO_SCP_EXE}"
        VITREO_ORACLE_WINDOWS_HOST="${VITREO_WINDOWS_ORACLE_HOST}"
    )
    add_dependencies(vitreo_d3d11_oracle_tests vitreo)
    if(VITREO_MINGW_CC)
        add_dependencies(vitreo_d3d11_oracle_tests
            vitreo_d3d11_oracle_runner)
    endif()
    endif()
endif()

# ------------------------------------------------------------------------------
# WinAPI core: ABI, marshaling, thunk registration
# ------------------------------------------------------------------------------
#
# Links the call mechanics without a single DLL implementation, which is what
# proves they are separable. test_thunk_dispatch stays in vitreo_winapi_tests
# because it dispatches through a real kernel32 export.

vitreo_add_test(NAME vitreo_winapi_core_tests
    SOURCES
        winapi/test_thunk.cpp
        winapi/test_scratch_fixtures.cpp
        winapi/test_marshaler_call_context.cpp
        # PathCch* is pure string algebra in a header, so it links against the
        # marshalling core alone — no runtime, which is what keeps this binary
        # narrow enough to be evidence.
        winapi/kernel32/test_pathcch.cpp
    DEPS
        vitreo_winapi_core
)

# ------------------------------------------------------------------------------
# IPC and NT synchronization
# ------------------------------------------------------------------------------
#
# These link the IPC and sync primitives alone — no WinAPI. That is the point:
# it is what proves those modules do not depend on kernel32.

vitreo_add_test(NAME vitreo_ipc_tests
    SOURCES
        ipc/test_stage0_backed_memory.cpp
        ipc/test_stage1_process_handle.cpp
        ipc/test_stage4_named_pipe.cpp
        ipc/test_hwnd_registry.cpp
        ipc/test_com_class_registry.cpp
        ipc/test_process_registry.cpp
        ipc/test_process_session.cpp
        ntsync/test_ntsync.cpp
    DEPS
        vitreo_ipc
        vitreo_sync_core
        vitreo_memory
)

# test_stage2_shared_mapping, test_stage3_named_sync, and test_remote_memory
# drive these primitives through kernel32's mapping/sync/remote-memory APIs,
# so they are WinAPI adapter tests and live in vitreo_winapi_tests. Keeping
# them out of the target above is what lets its narrow linkage keep proving
# that the IPC primitives need no WinAPI.

# ------------------------------------------------------------------------------
# Windowing model
# ------------------------------------------------------------------------------

vitreo_add_test(NAME vitreo_windowing_tests
    SOURCES
        windowing/test_hwnd.cpp
        windowing/test_message_queue.cpp
        windowing/test_message_box_layout.cpp
        windowing/test_cocoa_bridge.cpp
        windowing/test_cocoa_bridge_host_policy.cpp
        windowing/test_cocoa_app_session.mm
        windowing/test_dock_session_election.cpp
        windowing/test_cocoa_bridge_window_lifecycle.cpp
        windowing/test_cocoa_bridge_owned_dialogs.cpp
        windowing/test_cocoa_bridge_window_surface.cpp
        windowing/test_cocoa_bridge_key_window.cpp
        windowing/test_cocoa_bridge_input.cpp
        windowing/test_cocoa_bridge_hosted_peer_view.cpp
        windowing/test_cocoa_bridge_hosted_peer_grow.cpp
        windowing/test_cocoa_bridge_controls.cpp
        windowing/test_native_editing_focus.mm
        windowing/test_tree_controller_ownership.mm
        windowing/test_button_command_notify.mm
        windowing/test_syslink_markup.cpp
        windowing/test_control_font_face.mm
        windowing/test_oem_bitmaps.mm
        windowing/test_capsule_panel.mm
        windowing/test_native_chrome_input.mm
        windowing/test_cursor_warp_deltas.mm
        windowing/test_window_presentation.cpp
        windowing/test_app_icon_presentation.mm
        windowing/test_animated_cursor.cpp
    DEPS
        vitreo_windowing
        vitreo_graphics_core
        vitreo_memory
    NO_DISCOVER
)
# POST_BUILD discovery runs this AppKit binary during ordinary cmake --build.
# That puts a Dock app up and looks like the suite is launching itself.
# Discover at ctest time instead, as a child process.
gtest_discover_tests(vitreo_windowing_tests
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES ENVIRONMENT "VITREO_MACOS_PROCESS_ROLE=child"
)

# ------------------------------------------------------------------------------
# WinAPI
# ------------------------------------------------------------------------------
#
# One binary per DLL is the destination, but every WinAPI
# implementation is compiled into the single vitreo_winapi target today, so
# splitting these further would not narrow linkage — it would only multiply
# copies of the same whole-runtime link. They are split once vitreo_winapi is.

vitreo_add_test(NAME vitreo_winapi_tests
    SOURCES
        # Session-child role before any test creates a window: no Dock tile.
        winapi/test_default_child_process_role.cpp
        # Diagnostic-output hygiene: this binary already links the execution
        # runtime that owns the hostcall ring, so the notice-rate contract is
        # checkable here without a second heavy link.
        runtime/test_hostcall_ring_notice.cpp
        winapi/test_thunk_dispatch.cpp
        winapi/kernel32/test_pathcch_thunks.cpp
        winapi/test_small_dlls_cpuz.cpp
        winapi/iphlpapi/test_iphlpapi_adapters.cpp
        ipc/test_stage2_shared_mapping.cpp
        ipc/test_stage3_named_sync.cpp
        ipc/test_remote_memory.cpp
        winapi/msxml/test_xml_dom.cpp
        winapi/msxml/test_msxml_last_error.cpp
        winapi/msxml/test_msxml_document2.cpp
        winapi/gdiplus/test_image_decode.cpp
        winapi/kernel32/test_error.cpp
        winapi/kernel32/test_format_message.cpp
        winapi/test_critical_section_accel.cpp
        # Test-build-only seam: the hooked instantiation of the accelerated
        # LeaveCriticalSection (never compiled into vitreo_winapi).
        winapi/critical_section_test_seam.cpp
        winapi/kernel32/test_delete_file.cpp
        winapi/kernel32/test_delete_on_close.cpp
        winapi/kernel32/test_handle.cpp
        winapi/kernel32/test_vmem.cpp
        winapi/kernel32/test_vmem_reserve.cpp
        winapi/kernel32/test_vmem_placeholder.cpp
        winapi/kernel32/test_vmem_host_memory_floor.cpp
        winapi/kernel32/test_vmem_free.cpp
        winapi/kernel32/test_vmem_protect.cpp
        winapi/kernel32/test_vmem_discard.cpp
        winapi/kernel32/test_directory_changes.cpp
        winapi/kernel32/test_vmem_write_watch.cpp
        winapi/kernel32/test_heap.cpp
        winapi/kernel32/test_heap_ext.cpp
        winapi/kernel32/test_copyfile_exports.cpp
        winapi/kernel32/test_copyfile_engine.cpp
        winapi/kernel32/test_enum_resource_names_exports.cpp
        winapi/kernel32/test_resource_x64.cpp
        winapi/kernel32/test_thread_affinity.cpp
        winapi/kernel32/test_processor_topology.cpp
        winapi/kernel32/test_hardware_description.cpp
        winapi/kernel32/test_machine_identity_registry.cpp
        winapi/kernel32/test_virtual_firmware.cpp
        winapi/kernel32/test_thread_stack_limits.cpp
        winapi/kernel32/test_enum_process_modules.cpp
        winapi/kernel32/test_thread_error_mode.cpp
        winapi/kernel32/test_process_affinity_mask.cpp
        winapi/kernel32/test_file.cpp
        winapi/kernel32/test_file_console_bridge.cpp
        winapi/kernel32/test_file_info_by_handle_ex.cpp
        winapi/kernel32/test_file_inherited_pipe_handles.cpp
        winapi/kernel32/test_file_create_semantics.cpp
        winapi/kernel32/test_file_inherited_pipe_transfer.cpp
        winapi/kernel32/test_file_named_pipe.cpp
        winapi/kernel32/test_file_overlapped_pipe_write.cpp
        winapi/kernel32/test_file_overlapped_pipe_write_service.cpp
        winapi/kernel32/test_file_pipe_transact.cpp
        winapi/kernel32/test_named_pipe_exports.cpp
        winapi/kernel32/test_pipe_peer_close.cpp
        winapi/kernel32/test_file_information.cpp
        winapi/kernel32/test_disk_free_space.cpp
        winapi/kernel32/test_final_path_by_handle.cpp
        winapi/kernel32/test_comm_port.cpp
        winapi/setupapi/test_setup_classes.cpp
        winapi/kernel32/test_fileio.cpp
        winapi/kernel32/test_console.cpp
        winapi/kernel32/test_string_conv.cpp
        winapi/kernel32/test_codepage_convert.cpp
        winapi/kernel32/test_slist.cpp
        winapi/kernel32/test_module.cpp
        winapi/kernel32/test_module_synthetic_image.cpp
        winapi/kernel32/test_module_exports.cpp
        winapi/kernel32/test_module_load_library.cpp
        winapi/kernel32/test_module_lifetime.cpp
        winapi/kernel32/test_module_dll_notification.cpp
        winapi/kernel32/test_module_thread_callbacks.cpp
        winapi/kernel32/test_module_static_tls.cpp
        winapi/kernel32/test_module_import_cycles.cpp
        winapi/kernel32/test_runtime_function_lookup.cpp
        winapi/kernel32/test_process.cpp
        winapi/kernel32/test_virtual_query.cpp
        winapi/kernel32/test_environment.cpp
        winapi/kernel32/test_tls.cpp
        winapi/kernel32/test_filepos.cpp
        winapi/kernel32/test_sync.cpp
        winapi/kernel32/test_sync_event.cpp
        winapi/kernel32/test_sync_wait.cpp
        winapi/kernel32/test_sync_wait_on_address_sleep.cpp
        winapi/kernel32/test_sync_semaphore.cpp
        winapi/kernel32/test_sync_waitable_timer.cpp
        winapi/kernel32/test_sync_thunks.cpp
        winapi/kernel32/test_sync_access_oracle.cpp
        winapi/kernel32/test_sync_parked_waiters.cpp
        winapi/kernel32/test_srw_word.cpp
        winapi/kernel32/test_thread.cpp
        winapi/kernel32/test_thread_suspend.cpp
        winapi/kernel32/test_threadpool.cpp
        winapi/kernel32/test_critical_section.cpp
        winapi/kernel32/test_critical_section_contract.cpp
        winapi/kernel32/test_wait_on_address.cpp
        winapi/kernel32/test_sysinfo.cpp
        winapi/kernel32/test_verify_version_info.cpp
        winapi/kernel32/test_wow64_fsredir.cpp
        winapi/kernel32/test_system_programs.cpp
        winapi/kernel32/test_fiber_fls.cpp
        winapi/kernel32/test_file_mapping.cpp
        winapi/kernel32/test_file_search.cpp
        winapi/kernel32/test_global_local.cpp
        winapi/kernel32/test_time.cpp
        winapi/kernel32/test_kuser_shared_data.cpp
        winapi/kernel32/test_locale_string.cpp
        winapi/kernel32/test_normalization.cpp
        winapi/kernel32/test_nsis_nsdialogs.cpp
        winapi/kernel32/test_kernel32_cpuz.cpp
        winapi/kernel32/test_kernel32_cpuz_waits.cpp
        winapi/kernel32/test_kernel32_cpuz_jobs.cpp
        winapi/kernel32/test_time_zone_year.cpp
        winapi/kernel32/test_kernel32_cpuz_iocp.cpp
        winapi/kernel32/test_kernel32_cpuz_overlapped_pipes.cpp
        winapi/kernel32/test_kernel32_cpuz_exceptions_stack.cpp
        winapi/kernel32/test_kernel32_cpuz_init_once.cpp
        winapi/kernel32/test_kernel32_cpuz_module_resources.cpp
        winapi/kernel32/test_kernel32_cpuz_process_environment.cpp
        winapi/kernel32/test_kernel32_cpuz_process.cpp
        winapi/kernel32/test_kernel32_cpuz_toolhelp.cpp
        winapi/kernel32/test_kernel32_cpuz_paths.cpp
        winapi/kernel32/test_kernel32_cpuz_profile_atoms.cpp
        winapi/kernel32/test_vectored_exception.cpp
        winapi/ntdll/test_ntdll.cpp
        winapi/ntdll/test_ntdll_ext.cpp
        winapi/ntdll/test_ntdll_ext_virtual_memory.cpp
        winapi/ntdll/test_ntdll_ext_exports.cpp
        winapi/ntdll/test_ntdll_ext_unwind_frames.cpp
        winapi/ntdll/test_ntdll_ext_unwind_saves.cpp
        winapi/ntdll/test_ntdll_ext_directory_query.cpp
        winapi/ntdll/test_ntdll_ext_file.cpp
        winapi/ntdll/test_ntdll_ext_sections.cpp
        winapi/ntdll/test_ntdll_ext_thread_context.cpp
        winapi/ntdll/test_nt_alert_wait.cpp
        winapi/ntdll/test_nt_wait_single.cpp
        winapi/user32/test_window_class.cpp
        winapi/user32/test_window.cpp
        winapi/user32/test_message_send.cpp
        winapi/user32/test_button_bcm.cpp
        winapi/user32/test_show_window.cpp
        winapi/user32/test_message_loop.cpp
        winapi/user32/test_message_wait.cpp
        winapi/user32/test_input_contracts.cpp
        winapi/user32/test_mouse_in_pointer.cpp
        winapi/user32/test_def_window_proc.cpp
        winapi/user32/test_resources.cpp
        winapi/user32/test_painting.cpp
        winapi/user32/test_user_paint.cpp
        winapi/user32/test_user32_ext.cpp
        winapi/user32/test_user32_ext_pointer_input.cpp
        winapi/user32/test_user32_ext_icons_cursors_caret.cpp
        winapi/user32/test_user32_ext_timers_message_time.cpp
        winapi/user32/test_user32_ext_window_queries.cpp
        winapi/user32/test_user32_ext_activation_focus.cpp
        winapi/user32/test_user32_ext_window_pos.cpp
        winapi/user32/test_user32_ext_native_geometry.cpp
        winapi/user32/test_user32_native_resize_host.mm
        winapi/user32/test_user32_ext_monitors_display.cpp
        winapi/user32/test_user32_ext_desktop_station.cpp
        winapi/user32/test_user32_ext_system_integration.cpp
        winapi/user32/test_user32_ext_painting_controls.cpp
        winapi/user32/test_s1_windowing.cpp
        winapi/user32/test_owner_draw.cpp
        winapi/user32/test_user32_cpuz.cpp
        winapi/advapi32/test_registry.cpp
        winapi/advapi32/test_registry_enum.cpp
        winapi/advapi32/test_registry_wow64.cpp
        winapi/advapi32/test_security.cpp
        winapi/advapi32/test_security_descriptors.cpp
        winapi/advapi32/test_security_descriptor_parts.cpp
        winapi/advapi32/test_security_tokens.cpp
        winapi/advapi32/test_security_token_restricted.cpp
        winapi/advapi32/test_security_crypt.cpp
        winapi/advapi32/test_security_object_security.cpp
        winapi/advapi32/test_security_contract.cpp
        winapi/advapi32/test_security_sddl.cpp
        winapi/advapi32/test_security_s5_tokens.cpp
        winapi/advapi32/test_security_s5_acl.cpp
        winapi/advapi32/test_scm.cpp
        winapi/advapi32/test_advapi32_cpuz.cpp
        winapi/msvcrt/test_cxx_eh.cpp
        winapi/msvcrt/test_except_handler3.cpp
        winapi/msvcrt/test_crt_init.cpp
        winapi/msvcrt/test_crt_heap_reuse.cpp
        winapi/msvcrt/test_crt_locale.cpp
        winapi/msvcrt/test_crt_environment.cpp
        winapi/msvcrt/test_crt_memory.cpp
        winapi/msvcrt/test_crt_string.cpp
        winapi/msvcrt/test_crt_ucrt_s5.cpp
        winapi/msvcrt/test_crt_ucrt_s6.cpp
        winapi/msvcrt/test_crt_exception_class.cpp
        winapi/msvcrt/test_crt_audit_contracts.cpp
        winapi/msvcrt/test_crt_math.cpp
        winapi/msvcrt/test_crt_stdio.cpp
        winapi/msvcrt/test_crt_stdio_ucrt_abi.cpp
        winapi/msvcrt/test_crt_stdio_buffered_read.cpp
        winapi/msvcrt/test_crt_stdio_stream_open.cpp
        winapi/msvcrt/test_crt_stdio_osfhandle.cpp
        winapi/msvcrt/test_crt_stdio_blocking.cpp
        winapi/msvcrt/test_crt_stdio_close.cpp
        winapi/msvcrt/test_crt_stdio_std_descriptors.cpp
        winapi/msvcrt/test_crt_stdio_descriptor_limits.cpp
        winapi/msvcrt/test_crt_stdio_binary_reads.cpp
        winapi/msvcrt/test_crt_printf.cpp
        winapi/msvcrt/test_crt_scanf.cpp
        winapi/msvcrt/test_crt_s6_contracts.cpp
        winapi/msvcrt/test_crt_filesystem.cpp
        winapi/msvcrt/test_crt_time.cpp
        winapi/msvcrt/test_rtti.cpp
        winapi/gdi32/test_gdi32.cpp
        winapi/gdi32/test_gdi32_cpuz.cpp
        winapi/gdi32/test_gdi32_contracts.cpp
        winapi/gdi32/test_gdi32_text.cpp
        winapi/gdi32/test_gdi32_geometry.cpp
        winapi/gdiplus/test_gdiplus.cpp
        winapi/gdiplus/test_gdiplus_geometry.cpp
        winapi/gdiplus/test_gdiplus_region.cpp
        winapi/gdiplus/test_gdiplus_graphics.cpp
        winapi/gdiplus/test_gdiplus_pen_brush.cpp
        winapi/gdiplus/test_gdiplus_draw.cpp
        winapi/gdiplus/test_gdiplus_text.cpp
        winapi/gdiplus/test_gdiplus_image.cpp
        winapi/gdiplus/test_gdiplus_codec.cpp
        winapi/gdiplus/test_gdiplus_codec_effects.cpp
        winapi/gdiplus/test_gdiplus_codec_metafile.cpp
        winapi/ole32/test_com_core.cpp
        winapi/ole32/test_com_wait_run.cpp
        winapi/ole32/test_com_core_guid_stream.cpp
        winapi/ole32/test_com_core_registration.cpp
        winapi/ole32/test_com_core_activation.cpp
        winapi/ole32/test_com_core_remote_proxy.cpp
        winapi/ole32/test_com_core_marshal_memory.cpp
        winapi/ole32/test_com_shell_link.cpp
        winapi/ole32/test_com_window_property_store.cpp
        winapi/ole32/test_com_network_options.cpp
        winapi/ole32/test_com_file_dialog.cpp
        winapi/ole32/test_com_audio_endpoints.cpp
        winapi/ole32/test_com_filter_graph_interfaces.cpp
        winapi/ole32/test_com_filter_graph_media.cpp
        winapi/ole32/test_com_filter_graph_events.cpp
        winapi/ole32/test_com_filter_graph_connect.cpp
        winapi/ole32/test_com_wasapi_contract.cpp
        winapi/ole32/test_com_wasapi_services.cpp
        winapi/ole32/test_com_wasapi_sessions.cpp
        winapi/ole32/test_com_wasapi_volume.cpp
        winapi/ole32/test_com_wasapi_capture_devices.cpp
        winapi/ole32/test_com_wasapi_capture_stream.cpp
        winapi/ole32/test_com_audio_engine.cpp
        winapi/ole32/test_com_core_s7_contracts.cpp
        winapi/ole32/test_wic.cpp
        winapi/ole32/test_windowscodecs_dll.cpp
        audio/test_media_player.cpp
        winapi/ole32/test_storage_format.cpp
        winapi/msi/test_msi_engine.cpp
        winapi/msi/test_msi_exports.cpp
        winapi/msi/test_msi_engine_area.cpp
        winapi/msi/test_msi_features.cpp
        winapi/msi/test_msi_files.cpp
        winapi/msi/test_msi_search.cpp
        winapi/msi/test_msi_registration.cpp
        winapi/msi/test_msi_assembly.cpp
        winapi/msi/test_msi_productops.cpp
        winapi/msi/test_msi_customaction.cpp
        winapi/msi/test_msi_remote.cpp
        winapi/msi/test_msi_database.cpp
        winapi/msi/test_msi_ui.cpp
        winapi/oleacc/test_oleacc.cpp
        winapi/oleaut32/test_oleaut32.cpp
        winapi/oleaut32/test_typelib.cpp
        winapi/oleaut32/test_oleaut32_safearray_contract.cpp
        winapi/combase/test_combase.cpp
        winapi/rpcrt4/test_rpcrt4.cpp
        winapi/comctl32/test_comctl32.cpp
        winapi/riched20/test_riched20.cpp
        winapi/comdlg32/test_comdlg32.cpp
        winapi/uxtheme/test_uxtheme.cpp
        winapi/uxtheme/test_uxtheme_oracle.cpp
        winapi/msctf/test_msctf.cpp
        winapi/shell32/test_shell_paths.cpp
        winapi/shell32/test_app_user_model_id.cpp
        winapi/shell32/test_shell_known_folders.cpp
        winapi/shell32/test_notify_icons.cpp
        winapi/dbghelp/test_dbghelp.cpp
        winapi/shlwapi/test_shlwapi.cpp
        winapi/shlwapi/test_shlwapi_path_extras.cpp
        winapi/shlwapi/test_shlwapi_url.cpp
        winapi/shlwapi/test_shlwapi_str_extras.cpp
        winapi/propsys/test_propsys.cpp
        winapi/version/test_version.cpp
        winapi/lz32/test_lz32.cpp
        winapi/advpack/test_advpack.cpp
        winapi/wer/test_wer.cpp
        winapi/mpr/test_mpr.cpp
        winapi/urlmon/test_urlmon.cpp
        winapi/credui/test_credui.cpp
        winapi/cryptui/test_cryptui.cpp
        winapi/esent/test_esent.cpp
        winapi/imagehlp/test_imagehlp.cpp
        winapi/psapi/test_psapi.cpp
        winapi/setupapi/test_setupapi.cpp
        winapi/setupapi/test_setupapi_inf.cpp
        winapi/wtsapi32/test_wtsapi32.cpp
        winapi/powrprof/test_powrprof.cpp
        winapi/hhctrl/test_hhctrl.cpp
        winapi/oledlg/test_oledlg.cpp
        winapi/ws2_32/test_ws2_32.cpp
        winapi/ws2_32/test_ws2_32_network.cpp
        winapi/ws2_32/test_ws2_32_loopback.cpp
        winapi/ws2_32/test_ws2_32_loopback_sockopt.cpp
        winapi/ws2_32/test_ws2_32_loopback_overlapped.cpp
        winapi/ws2_32/test_ws2_32_loopback_events_connect.cpp
        winapi/ws2_32/test_ws2_32_loopback_events_read.cpp
        winapi/ws2_32/test_ws2_32_loopback_datagram.cpp
        winapi/ws2_32/test_ws2_32_loopback_afd.cpp
        winapi/ws2_32/test_ws2_32_contracts.cpp
        winapi/ws2_32/test_ws2_32_contracts_connection.cpp
        winapi/ws2_32/test_ws2_32_contracts_transfer.cpp
        winapi/ws2_32/test_ws2_32_contracts_options.cpp
        winapi/ws2_32/test_ws2_32_contracts_select.cpp
        winapi/ws2_32/test_ws2_32_contracts_overlapped.cpp
        winapi/ws2_32/test_ws2_32_contracts_resolve.cpp
        winapi/ws2_32/test_ws2_32_contracts_address.cpp
        winapi/ws2_32/test_ws2_32_msg.cpp
        winapi/wsock32/test_wsock32.cpp
        winapi/mswsock/test_mswsock.cpp
        winapi/netutils/test_netutils.cpp
        winapi/wkscli/test_wkscli.cpp
        winapi/httpapi/test_httpapi.cpp
        winapi/wininet/test_wininet.cpp
        winapi/winhttp/test_winhttp.cpp
        winapi/dnsapi/test_dns_service.cpp
        winapi/crypt32/test_crypt32_thunk_abi.cpp
        winapi/crypt32/test_crypt32_dpapi.cpp
        winapi/crypt32/test_crypt32.cpp
        winapi/crypt32/test_crypt32_system_store.cpp
        winapi/crypt32/test_crypt32_trust_anchors.cpp
        winapi/crypt32/test_crypt32_chain.cpp
        winapi/crypt32/test_crypt32_encoding.cpp
        winapi/crypt32/test_crypt32_pfx.cpp
        winapi/crypt32/test_crypt32_collection_find.cpp
        winapi/crypt32/test_crypt32_context_info.cpp
        winapi/crypt32/test_crypt32_chain_layout.cpp
        winapi/crypt32/test_crypt32_query_object.cpp
        winapi/crypt32/test_crypt32_signer_trio.cpp
        winapi/crypt32/test_crypt32_usage_contract.cpp
        winapi/crypt32/test_verify_certificate_signature.cpp
        winapi/crypt32/test_auth_root.cpp
        winapi/crypt32/test_crypt32_contracts.cpp
        winapi/bcrypt/test_bcrypt.cpp
        winapi/mscoree/test_mscoree_shim.cpp
        winapi/ncrypt/test_ncrypt.cpp
        winapi/secur32/test_secur32.cpp
        winapi/sspicli/test_sspicli.cpp
        winapi/userenv/test_profile_environment.cpp
        winapi/authz/test_authz.cpp
        winapi/ssl/test_ssl_integration.cpp
        winapi/winmm/test_winmm.cpp
        winapi/winmm/test_timer_scheduler.cpp
        winapi/dsound/test_dsound.cpp
        winapi/d2d1/test_d2d1.cpp
        winapi/dwrite/test_dwrite.cpp
        winapi/dwrite/test_dwrite_gdi_interop.cpp
        winapi/dwrite/test_dwrite_rendering.cpp
        winapi/dwrite/test_dwrite_text_analysis.cpp
        winapi/dwrite/test_dwrite_text_layout.cpp
        winapi/dwrite/test_dwrite_font_collection.cpp
        winapi/dwrite/test_dwrite_font_face.cpp
        winapi/dwrite/test_dwrite_font_backend.cpp
        winapi/dwrite/test_dwrite_local_font_file.cpp
        winapi/dwrite/test_dwrite_custom_collection.cpp
        winapi/dwrite/test_dwrite_custom_collection_bench.cpp
        winapi/d3d9/test_d3d9.cpp
        winapi/d3d9/test_d3d9_factory.cpp
        winapi/d3d9/test_d3d9_formats.cpp
        winapi/d3d9/test_d3d9_thunks.cpp
        winapi/d3d9/test_d3d9_device.cpp
        winapi/d3d9/test_d3d9_state.cpp
        winapi/d3d9/test_d3d9_process_vertices.cpp
        winapi/d3d9/test_d3d9_process_vertices_branch.cpp
        winapi/d3d9/test_d3d9_process_vertices_loop.cpp
        winapi/d3d9/test_d3d9_process_vertices_transform.cpp
        winapi/d3d9/test_d3d9_process_vertices_arith.cpp
        winapi/d3d9/test_d3d9_draw_streams.cpp
        winapi/d3d9/test_d3d9_state_block.cpp
        winapi/d3d9/test_d3d9_render_target.cpp
        winapi/d3d9/test_d3d9_fullscreen_cursor.cpp
        winapi/d3d9/test_d3d9_render_target_binding.cpp
        winapi/d3d9/test_d3d9_draw_primitives.cpp
        winapi/d3d9/test_d3d9_fixed_function.cpp
        winapi/d3d9/test_d3d9_fixed_function_state.cpp
        winapi/d3d9/test_d3d9_shaders.cpp
        winapi/d3d9/test_d3d9_swap_chain.cpp
        winapi/d3d9/test_d3d9_private_data.cpp
        winapi/d3d9/test_d3d9_buffers.cpp
        winapi/d3d9/test_d3d9_texture_lock.cpp
        winapi/d3d9/test_d3d9_cube_volume_texture.cpp
        winapi/d3d9/test_d3d9_binding_query.cpp
        winapi/d3d9/test_d3d9_surface_transfer.cpp
        winapi/d3d9/test_d3d9_stretch_rect.cpp
        winapi/d3d9/test_d3d9_palette.cpp
        winapi/d3d11/test_d3d11_binding_rules.cpp
        winapi/d3d11/test_d3d11_buffer_map_and_copy.cpp
        winapi/d3d11/test_d3d11_capsmall.cpp
        winapi/d3d11/test_d3d11_class_linkage.cpp
        winapi/d3d11/test_d3d11_compute_and_blend.cpp
        winapi/d3d11/test_d3d11_cube_views_and_present.cpp
        winapi/d3d11/test_d3d11_d3d10_device.cpp
        winapi/d3d11/test_d3d11_d3d10_state.cpp
        winapi/d3d11/test_d3d11_dcomp_reentry.cpp
        winapi/d3d11/test_d3d11_deferred_context.cpp
        winapi/d3d11/test_d3d11_depth_only_and_command_lists.cpp
        winapi/d3d11/test_d3d11_device_children.cpp
        winapi/d3d11/test_d3d11_device_creation.cpp
        winapi/d3d11/test_d3d11_draw_binding.cpp
        winapi/d3d11/test_d3d11_draw_dump.cpp
        winapi/d3d11/test_d3d11_draw_memo_view_lines.cpp
        winapi/d3d11/test_d3d11_exclusive_fullscreen.cpp
        winapi/d3d11/test_d3d11_extended_sharing.cpp
        winapi/d3d11/test_d3d11_fixed_function.cpp
        winapi/d3d11/test_d3d11_geometry_tessellation.cpp
        winapi/d3d11/test_d3d11_input_layout_creation_rules.cpp
        winapi/d3d11/test_d3d11_live_diag.cpp
        winapi/d3d11/test_d3d11_ms_srv_windows.cpp
        winapi/d3d11/test_d3d11_native_width.cpp
        winapi/d3d11/test_d3d11_optional_caps.cpp
        winapi/d3d11/test_d3d11_predicates_and_buffers.cpp
        winapi/d3d11/test_d3d11_private_data_and_pitches.cpp
        winapi/d3d11/test_d3d11_query.cpp
        winapi/d3d11/test_d3d11_register_capacity.cpp
        winapi/d3d11/test_d3d11_render_target_views.cpp
        winapi/d3d11/test_d3d11_resource_ops.cpp
        winapi/d3d11/test_d3d11_sharing.cpp
        winapi/d3d11/test_d3d11_state_object_contract.cpp
        winapi/d3d11/test_d3d11_stream_out.cpp
        winapi/d3d11/test_d3d11_surface.cpp
        winapi/d3d11/test_d3d11_swapchain_creation.cpp
        winapi/d3d11/test_d3d11_swapchain_facets.cpp
        winapi/d3d11/test_d3d11_tables.cpp
        winapi/d3d11/test_d3d11_texture_creation.cpp
        winapi/d3d11/test_d3d11_texture_map_and_copy.cpp
        winapi/d3d11/test_d3d11_texture_subresources.cpp
        winapi/d3d11/test_d3d11_tiled.cpp
        winapi/d3d11/test_d3d11_tiled_copy.cpp
        winapi/d3d11/test_d3d11_uav_view_desc.cpp
        winapi/d3d11/test_d3d11_view_contract.cpp
        winapi/d3d11/test_d3d11_volume_and_buffer_views.cpp
        winapi/d3d11/test_d3d11_in_chain.cpp
        winapi/d3d10/test_d3d10.cpp
        winapi/d3d12/test_d3d12.cpp
        winapi/d3d12/test_d3d12_root_signature.cpp
        winapi/d3d12/test_d3d12_layout.cpp
        winapi/d3d12/test_d3d12_caps.cpp
        winapi/dxgi/test_dxgi.cpp
        winapi/dxgi/test_dxgi_adapter.cpp
        winapi/dxgi/test_dxgi_adapter_identity.cpp
        winapi/dxgi/test_dxgi_lifetime.cpp
        winapi/dxgi/test_dxgi_output.cpp
        winapi/dxgi/test_dxgi_private_data.cpp
        winapi/dxgi/test_dxgi_video_memory.cpp
        winapi/graphics/test_dwmapi.cpp
        winapi/ui/test_uiautomationcore.cpp
        winapi/graphics/test_opengl32.cpp
        winapi/graphics/test_gl_context_state.cpp
        winapi/graphics/test_gl_pixel_formats.cpp
        winapi/graphics/test_gl_wgl.cpp
        winapi/graphics/test_gl_assembler.cpp
        winapi/graphics/test_gl_texture.cpp
        winapi/graphics/test_gl_pixels.cpp
        winapi/graphics/test_gl_display_list.cpp
        winapi/graphics/test_opengl32_export_audit.cpp
        winapi/graphics/test_d3dkmt_dxcore.cpp
        winapi/graphics/test_glu32.cpp
        winapi/graphics/test_glu32_export_audit.cpp
        winapi/graphics/test_gl_extensions.cpp
        winapi/media/test_mfplat.cpp
        winapi/media/test_mfplay.cpp
        winapi/media/test_msvfw32.cpp
        winapi/media/test_inproc_servers.cpp
        winapi/ddraw/test_ddraw.cpp
        winapi/vulkan/test_vulkan.cpp
        winapi/dinput8/test_dinput8.cpp
        winapi/xinput/test_xinput.cpp
    DEPS
        vitreo_winapi
        vitreo_windowing
        vitreo_graphics_core
        vitreo_graphics_shader
        vitreo_pe_loader
        vitreo_memory
        # vitreo_winapi's thread-suspend path reports the JIT flag-op histogram
        # at teardown, so objects that pull it in need the JIT library too.
        vitreo_cpu_jit
        # ...and CriticalSectionManager registers a hang-dump section, so the
        # same objects need the runtime that defines register_hang_dump_section.
        vitreo_execution_runtime
    FRAMEWORKS CoreFoundation CFNetwork Security
)
# Oracle-probe expected files (Windows' own output) that WinAPI tests replay.
target_compile_definitions(vitreo_winapi_tests PRIVATE
    VITREO_ORACLE_PROBE_DIR="${CMAKE_SOURCE_DIR}/tests/oracle_data")

# ------------------------------------------------------------------------------
# Level 4: runtime integration — intentionally links the assembled runtime
# ------------------------------------------------------------------------------

vitreo_add_test(NAME vitreo_platform_tests
    SOURCES
        platform/test_elevation_authorization.cpp
        platform/test_host_hardware.cpp
        platform/test_host_volume_space.cpp
        platform/test_scoped_environment.cpp
        platform/test_tcc_responsibility.cpp
    DEPS
        vitreo_platform
)
# VITREO_SCOPED_ENV: a process that latches a knob at static initialisation,
# the way the runtime's ~290 environment knobs do, and gives the variable its
# chances at the process. test_scoped_environment.cpp spawns it.
add_executable(vitreo_scoped_environment_probe
    platform/scoped_environment_probe.cpp)
target_link_libraries(vitreo_scoped_environment_probe PRIVATE vitreo_platform)
target_include_directories(vitreo_scoped_environment_probe PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
add_dependencies(vitreo_platform_tests vitreo_scoped_environment_probe)
target_compile_definitions(vitreo_platform_tests PRIVATE
    VITREO_SCOPED_ENV_PROBE_PATH="$<TARGET_FILE:vitreo_scoped_environment_probe>")
# The runtime's embedded Info.plist, linked the same way as into vitreo, so
# the test reads it through the path TCC uses.
target_link_options(vitreo_platform_tests PRIVATE
    "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_SOURCE_DIR}/src/app/macos/vitreo-runtime-Info.plist")
set_property(TARGET vitreo_platform_tests APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/app/macos/vitreo-runtime-Info.plist)

vitreo_add_test(NAME vitreo_integration_tests
    SOURCES
        cli/test_cli.cpp
        cli/test_cli_callback_policy.cpp
        cli/test_cli_callback_exceptions.cpp
        cli/test_cli_thunk_stack.cpp
        cli/test_cli_sysenter_int2e.cpp
        cli/test_cli_fast_thunk_cache.cpp
        cli/test_direct_thunk.cpp
        cli/test_inchain_thunk.cpp
        cli/test_inchain_kernel32.cpp
        cli/test_opengl32_registration.cpp
        cli/test_capsule.cpp
        cli/test_process_command.cpp
        cli/test_config.cpp
        cli/test_run.cpp
        cli/test_external_app_mapping.cpp
        cli/test_guest_command_line.cpp
    DEPS
        vitreo_cli
        vitreo_pe_loader
        vitreo_windowing
        vitreo_capsule_env
    FRAMEWORKS CoreFoundation CFNetwork Security
)
# The direct COM crossing's last-error test replays Windows' measured output.
target_compile_definitions(vitreo_integration_tests PRIVATE
    VITREO_ORACLE_PROBE_DIR="${CMAKE_SOURCE_DIR}/tests/oracle_data")

# L3 (in-chain API thunk calls) on the JIT tiers that carry guest flags live
# in host NZCV across a block boundary: Tier 0's fused CMP+Jcc legs, x86
# JMP rel32 tails and block-cap fallthroughs reach the in-chain stub with
# NZCV live, and the stub's restore of it is load-bearing there. The IR
# backend and traces settle most of those flags first, so with them on a
# broken restore can stay green; each tier gets its own process (the gates
# are read once).
set(VITREO_INCHAIN_SUITE "InChainThunkTest.*:ServeInChainTest.*:InChainKernel32*")
add_test(NAME vitreo_integration_inchain_ir_off
    COMMAND $<TARGET_FILE:vitreo_integration_tests>
            --gtest_filter=${VITREO_INCHAIN_SUITE})
set_tests_properties(vitreo_integration_inchain_ir_off PROPERTIES
    ENVIRONMENT "VITREO_JIT_IR=0")
add_test(NAME vitreo_integration_inchain_traces_off
    COMMAND $<TARGET_FILE:vitreo_integration_tests>
            --gtest_filter=${VITREO_INCHAIN_SUITE})
set_tests_properties(vitreo_integration_inchain_traces_off PROPERTIES
    ENVIRONMENT "VITREO_JIT_TRACES=0;VITREO_JIT_TRACES64=0")
add_test(NAME vitreo_integration_inchain_tier0
    COMMAND $<TARGET_FILE:vitreo_integration_tests>
            --gtest_filter=${VITREO_INCHAIN_SUITE})
set_tests_properties(vitreo_integration_inchain_tier0 PROPERTIES
    ENVIRONMENT "VITREO_JIT_IR=0;VITREO_JIT_TRACES=0;VITREO_JIT_TRACES64=0")

# Every guest->host crossing drains its host call's autorelease pool (MRC
# Objective-C++: the test sends -autorelease itself).
if(APPLE)
    vitreo_add_test(NAME vitreo_thunk_autorelease_tests
        SOURCES
            cli/test_thunk_autorelease.mm
        DEPS
            vitreo_cli
            vitreo_pe_loader
            vitreo_windowing
            vitreo_capsule_env
        FRAMEWORKS Foundation CoreFoundation CFNetwork Security
    )
endif()

# ------------------------------------------------------------------------------
# x86 Oracle Runner (macOS Apple Silicon only, requires the host x86 runner)
# ------------------------------------------------------------------------------
# Builds a small x86_64 helper binary that executes raw x86 instruction bytes on
# the real CPU via the host x86 runner and returns the resulting register/XMM state. Used
# by the oracle-based contract tests to validate interpreter + JIT correctness.

if(APPLE AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
    set(ORACLE_RUNNER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/oracle/x86_oracle_runner.c)
    set(ORACLE_RUNNER_BIN ${CMAKE_CURRENT_BINARY_DIR}/x86_oracle_runner)

    add_custom_command(
        OUTPUT ${ORACLE_RUNNER_BIN}
        COMMAND xcrun -sdk macosx clang -arch x86_64 -O2 -Wall -Wextra
                -Wl,-pagezero_size,0x1000
                -o ${ORACLE_RUNNER_BIN} ${ORACLE_RUNNER_SRC}
        COMMAND codesign -s - --force ${ORACLE_RUNNER_BIN}
        DEPENDS ${ORACLE_RUNNER_SRC}
                ${CMAKE_CURRENT_SOURCE_DIR}/oracle/oracle_bench_protocol.h
        COMMENT "Building the x86 oracle runner at ${ORACLE_RUNNER_BIN}"
        VERBATIM
    )
    add_custom_target(vitreo_x86_oracle_runner ALL DEPENDS ${ORACLE_RUNNER_BIN})

    vitreo_add_test(NAME vitreo_oracle_tests
        SOURCES
            oracle/oracle_client.cpp
            oracle/oracle_contract.cpp
            oracle/test_oracle_runner.cpp
            oracle/test_oracle_contract.cpp
            oracle/test_oracle_contract_64.cpp
            oracle/test_oracle_integer.cpp
            oracle/test_oracle_integer_64.cpp
            oracle/test_oracle_adx_64.cpp
            oracle/test_oracle_bmi_64.cpp
            oracle/test_oracle_fma_all_64.cpp
            oracle/test_oracle_avx_perm_sweep_64.cpp
            oracle/test_oracle_cmpxchg_64.cpp
            oracle/test_oracle_avx_64.cpp
            oracle/test_oracle_avx2_64.cpp
            oracle/test_oracle_avx_permute_gather_64.cpp
            oracle/test_oracle_sse_more_x64.cpp
            oracle/test_oracle_crypto_64.cpp
            oracle/test_oracle_sse_more.cpp
            oracle/test_oracle_sse_more_64.cpp
            oracle/test_oracle_x87.cpp
            oracle/test_oracle_x87_64.cpp
            oracle/test_oracle_fp_rounding_64.cpp
            oracle/test_oracle_mmx.cpp
            oracle/test_oracle_mmx_more.cpp
            oracle/test_oracle_bcd.cpp
            oracle/test_oracle_misc_64.cpp
            oracle/test_oracle_fma_64.cpp
            oracle/test_oracle_bench.cpp
            oracle/cpu_perf_oracle.cpp
            oracle/cpu_perf_catalog.cpp
            oracle/cpu_perf_windows.cpp
            oracle/test_cpu_perf_oracle.cpp
        DEPS
            vitreo_cpu_state
            vitreo_cpu_decode
            vitreo_cpu_interpreter
            vitreo_cpu_jit
            vitreo_cpu_seh
            vitreo_memory
        COMPILE_DEFINITIONS
            VITREO_ORACLE_RUNNER_PATH="${ORACLE_RUNNER_BIN}"
    )
    add_dependencies(vitreo_oracle_tests vitreo_x86_oracle_runner)

    if(NOT VITREO_SSH_EXE)
        find_program(VITREO_SSH_EXE ssh)
        if(NOT VITREO_SSH_EXE)
            set(VITREO_SSH_EXE "ssh")
        endif()
    endif()
    if(NOT VITREO_SCP_EXE)
        find_program(VITREO_SCP_EXE scp)
        if(NOT VITREO_SCP_EXE)
            set(VITREO_SCP_EXE "scp")
        endif()
    endif()
    set(_cpu_perf_win_runner "")
    if(VITREO_MINGW_CC)
        set(_cpu_perf_win_runner
            ${CMAKE_CURRENT_BINARY_DIR}/cpu_perf_windows_runner_x86.exe)
        add_custom_command(
            OUTPUT ${_cpu_perf_win_runner}
            COMMAND ${VITREO_MINGW_CC} -O2 -Wall -Wextra
                    -I${CMAKE_CURRENT_SOURCE_DIR}/oracle
                    -o ${_cpu_perf_win_runner}
                    ${CMAKE_CURRENT_SOURCE_DIR}/oracle/cpu_perf_windows_runner.c
                    -lkernel32
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/oracle/cpu_perf_windows_runner.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/oracle/cpu_perf_windows_protocol.h
            COMMENT "Building CPU perf Windows runner (x86)"
            VERBATIM
        )
        add_custom_target(vitreo_cpu_perf_windows_runner
            DEPENDS ${_cpu_perf_win_runner})
        add_dependencies(vitreo_oracle_tests vitreo_cpu_perf_windows_runner)
    else()
        message(STATUS
            "i686-w64-mingw32-gcc not found: CPU perf Windows column will skip")
    endif()

    set(_cpu_perf_win64_runner "")
    if(VITREO_MINGW64_CC)
        set(_cpu_perf_win64_runner
            ${CMAKE_CURRENT_BINARY_DIR}/cpu_perf_windows_runner_x64.exe)
        add_custom_command(
            OUTPUT ${_cpu_perf_win64_runner}
            COMMAND ${VITREO_MINGW64_CC} -O2 -Wall -Wextra
                    -I${CMAKE_CURRENT_SOURCE_DIR}/oracle
                    -o ${_cpu_perf_win64_runner}
                    ${CMAKE_CURRENT_SOURCE_DIR}/oracle/cpu_perf_windows_runner_x64.c
                    -lkernel32
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/oracle/cpu_perf_windows_runner_x64.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/oracle/cpu_perf_windows_protocol.h
            COMMENT "Building CPU perf Windows runner (x64)"
            VERBATIM
        )
        add_custom_target(vitreo_cpu_perf_windows_runner_x64
            DEPENDS ${_cpu_perf_win64_runner})
        add_dependencies(vitreo_oracle_tests
            vitreo_cpu_perf_windows_runner_x64)
    else()
        message(STATUS
            "x86_64-w64-mingw32-gcc not found: x64 CPU perf Windows column will skip")
    endif()

    target_compile_definitions(vitreo_oracle_tests PRIVATE
        VITREO_CPU_PERF_WIN_RUNNER="${_cpu_perf_win_runner}"
        VITREO_CPU_PERF_WIN64_RUNNER="${_cpu_perf_win64_runner}"
        VITREO_ORACLE_SSH_EXE="${VITREO_SSH_EXE}"
        VITREO_ORACLE_SCP_EXE="${VITREO_SCP_EXE}"
        VITREO_ORACLE_WINDOWS_HOST="${VITREO_WINDOWS_ORACLE_HOST}"
    )
endif()

if(VITREO_ENABLE_WIN_ORACLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/winapi_oracle/win_oracle_client.cpp")
    vitreo_add_test(NAME vitreo_win_oracle_tests
        SOURCES
            winapi_oracle/win_oracle_client.cpp
            winapi_oracle/test_win_oracle_smoke.cpp
        DEPS vitreo_core
        COMPILE_OPTIONS -I${CMAKE_CURRENT_SOURCE_DIR}/winapi_oracle
    )

    # The remaining oracle harnesses are standalone tools, not GTest binaries:
    # they drive a Windows VM and print diffs. They deliberately link the whole
    # runtime, because comparing Vitreo against Windows requires all of it.
    set(_win_oracle_runtime_deps
        vitreo_cli
        vitreo_pe_loader
        vitreo_windowing
        vitreo_capsule_env
    )

    # Standalone smoke harness: proves the SSH transport + protocol without
    # dragging in the rest of the test suite.
    add_executable(win_oracle_smoke
        winapi_oracle/win_oracle_client.cpp
        winapi_oracle/win_oracle_smoke_main.cpp
    )
    target_include_directories(win_oracle_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/winapi_oracle
    )

    # Standalone pctrace driver: runs a bounded instruction trace on the
    # Windows VM and dumps it in the same text format as Vitreo's PCTRACE so
    # the two streams can be diffed directly. See win_oracle_pctrace_main.cpp.
    add_executable(win_oracle_pctrace
        winapi_oracle/win_oracle_client.cpp
        winapi_oracle/win_oracle_pctrace_main.cpp
    )
    target_include_directories(win_oracle_pctrace PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/winapi_oracle
    )

    # Standalone contract diff harness: runs each annotated export through
    # both the oracle and Vitreo's registered handler, diffs the observable
    # state field-by-field.
    #
    # Generated tests come from tools/winapi_oracle/codegen.py reading
    # tests/winapi_oracle/annotations.py. Regenerated at configure time and
    # whenever annotations.py / codegen.py change.
    set(WIN_ORACLE_GENERATED_CPP
        ${CMAKE_CURRENT_SOURCE_DIR}/winapi_oracle/generated_tests.cpp)
    add_custom_command(
        OUTPUT  ${WIN_ORACLE_GENERATED_CPP}
        COMMAND ${CMAKE_COMMAND} -E env
                python3 ${CMAKE_SOURCE_DIR}/tools/winapi_oracle/codegen.py
        DEPENDS ${CMAKE_SOURCE_DIR}/tools/winapi_oracle/codegen.py
                ${CMAKE_CURRENT_SOURCE_DIR}/winapi_oracle/annotations.py
                ${CMAKE_CURRENT_SOURCE_DIR}/winapi_oracle/annotations_auto.py
        COMMENT "Regenerating win_oracle_diff annotated tests"
        VERBATIM)
    add_executable(win_oracle_diff
        winapi_oracle/win_oracle_client.cpp
        winapi_oracle/win_oracle_diff_main.cpp
        ${WIN_ORACLE_GENERATED_CPP}
    )
    target_include_directories(win_oracle_diff PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/winapi_oracle
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(win_oracle_diff PRIVATE ${_win_oracle_runtime_deps})

    # Companion tool: dumps every Vitreo-registered thunk so the
    # auto-annotator can decide which APIs to test.
    add_executable(dump_thunks
        winapi_oracle/win_oracle_client.cpp
        winapi_oracle/dump_thunks_main.cpp
    )
    target_include_directories(dump_thunks PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/winapi_oracle
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(dump_thunks PRIVATE ${_win_oracle_runtime_deps})

    if(APPLE)
        foreach(_tgt win_oracle_smoke win_oracle_pctrace win_oracle_diff dump_thunks)
            target_link_libraries(${_tgt} PRIVATE
                "-framework CoreFoundation"
                "-framework CFNetwork"
                "-framework Security"
            )
        endforeach()
    endif()
endif()

# ------------------------------------------------------------------------------
# ISA matrix — per-form JIT coverage scoreboard
# ------------------------------------------------------------------------------
# `tools/isa_audit.py` scores a mnemonic as
# "lowered" when any one of its operand forms has a `case Opcode::` arm, and
# counts a `BLR` into jit_sse_op as a native lowering. This target measures the
# real thing: it inverts the decoder to enumerate every reachable form, then
# compiles each one and reads back whether the JIT emitted ARM64, called a C++
# semantic helper, or delegated to the interpreter.
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_isa_matrix_lib_sources
        isa_matrix/isa_form_enum.cpp
        isa_matrix/isa_matrix_probe.cpp)

    add_executable(vitreo-isa-matrix
        ${_isa_matrix_lib_sources}
        isa_matrix/isa_matrix_report.cpp)
    target_include_directories(vitreo-isa-matrix PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/isa_matrix)
    target_link_libraries(vitreo-isa-matrix PRIVATE
        vitreo_cpu_state
        vitreo_cpu_decode
        vitreo_cpu_interpreter
        vitreo_cpu_jit
        vitreo_cpu_seh
        vitreo_memory)

    vitreo_add_test(NAME vitreo_isa_matrix_tests
        SOURCES
            ${_isa_matrix_lib_sources}
            isa_matrix/isa_matrix_exec.cpp
            oracle/oracle_client.cpp
            oracle/oracle_contract.cpp
            isa_matrix/test_isa_matrix.cpp
            isa_matrix/test_isa_matrix_exec.cpp
        DEPS
            vitreo_cpu_state
            vitreo_cpu_decode
            vitreo_cpu_interpreter
            vitreo_cpu_jit
            vitreo_cpu_seh
            vitreo_memory
        COMPILE_DEFINITIONS
            VITREO_ISA_MATRIX_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/isa_matrix/isa_matrix_baseline.json"
            VITREO_ISA_MATRIX_EXEC_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/isa_matrix/isa_matrix_exec_baseline.json"
            VITREO_ISA_MATRIX_WORDS_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/isa_matrix/isa_matrix_words_baseline.json"
            VITREO_ORACLE_RUNNER_PATH="${ORACLE_RUNNER_BIN}"
    )
    target_include_directories(vitreo_isa_matrix_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/isa_matrix
        ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(vitreo_isa_matrix_tests vitreo_x86_oracle_runner)
endif()
