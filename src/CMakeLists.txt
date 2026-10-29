# ------------------------------------------------------------------------------
# Vitreo Source Build
# ------------------------------------------------------------------------------

# ==============================================================================
# CLI Executable: vitreo
# ==============================================================================
#
# On x86_64, dyld maps this binary's __GUEST zerofill (0x100000..0x80000000)
# before libmalloc runs, so no host arena can land anywhere in the 32-bit
# guest window. The early-reservation constructor then drops that placeholder
# and claims the low 4GB PROT_NONE. ARM64 is a plain executable: guest addresses
# are translated, so nothing competes for that range.

set(_vitreo_engine_libs
    vitreo_cli
    vitreo_windowing
    vitreo_pe
    vitreo_capsule
)

if(VITREO_ARCH_X86_64)
    enable_language(ASM)
    add_executable(vitreo
        main.cpp
        app/intel/guest_reserve.s
    )
    target_include_directories(vitreo PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(vitreo PRIVATE ${_vitreo_engine_libs})
    if(VITREO_X64_GUEST_LINK_OPTIONS)
        target_link_options(vitreo PRIVATE
            ${VITREO_X64_GUEST_LINK_OPTIONS}
            -Wl,-segaddr,__GUEST,0x100000
            -Wl,-segprot,__GUEST,rw,rw
            # TEXT is at 0x400000000; without this ld64's "huge" pass would
            # fold the zerofill into __huge and the address would not stick.
            -Wl,-no_huge
        )
    endif()
else()
    add_executable(vitreo main.cpp)
    target_include_directories(vitreo PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(vitreo PRIVATE ${_vitreo_engine_libs})
endif()

# A guest re-executes itself as its app bundle responsible for itself
# (platform/macos/tcc_responsibility.hpp, src/main.cpp), and the bundle's
# executable links to this binary, so TCC judges this executable and reads
# the privacy usage strings from its embedded Info.plist.
set(VITREO_RUNTIME_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/app/macos/vitreo-runtime-Info.plist)
target_link_libraries(vitreo PRIVATE vitreo_platform)
target_link_options(vitreo PRIVATE
    "LINKER:-sectcreate,__TEXT,__info_plist,${VITREO_RUNTIME_INFO_PLIST}")
set_property(TARGET vitreo APPEND PROPERTY LINK_DEPENDS ${VITREO_RUNTIME_INFO_PLIST})

# Core library (header-only for now, will grow)
add_library(vitreo_core INTERFACE)
target_include_directories(vitreo_core INTERFACE
    ${CMAKE_SOURCE_DIR}/include
)

# ------------------------------------------------------------------------------
# Base Subsystem - platform-neutral primitives below every other layer
# ------------------------------------------------------------------------------
#
# Only things that genuinely have no lower layer to live in belong here. Today
# that is the diagnostic hooks: the CPU, WinAPI, and IPC all request a hostcall
# dump, but the ring is owned by the CLI, which none of them can link. Keeping
# the hook here is what lets those modules be linked — and unit-tested —
# without dragging in the process runtime.
#
# This must not become a utility junk drawer.
add_library(vitreo_base STATIC
    core/base/diagnostics.cpp
    core/base/perf_window.cpp
    core/base/perf_window_control.cpp
    core/base/perf_window_report.cpp
    core/base/perf_window_report_dispatch.cpp
    core/base/perf_window_report_jit.cpp
    core/base/perf_window_report_memory.cpp
    core/base/perf_window_report_runtime.cpp
    core/base/present_cpu.cpp
    core/base/time_diag.cpp
    core/math/float80.cpp
    core/math/x87_transcendental.cpp
    core/math/host_fenv.cpp
)

target_include_directories(vitreo_base PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(vitreo_base PUBLIC
    vitreo_core
    # The guest TSC rate (vitreo/virtual_tsc.hpp) is the host's core clock.
    vitreo_platform
)

# ------------------------------------------------------------------------------
# Compression Formats
# ------------------------------------------------------------------------------
#
# Pure data-format codecs: bytes in, bytes out. No guest memory, no VFS, no
# Win32, so they sit below every consumer and are unit-testable on their own.
# Today the consumer is the Windows Installer engine, which cannot install a
# package without reading its cabinets.

vitreo_add_module(
    NAME vitreo_compression
    SOURCES
        core/compression/cabinet.cpp
        core/compression/lzx.cpp
        core/compression/mszip.cpp
    PUBLIC_DEPS
        vitreo_core
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# ------------------------------------------------------------------------------
# NT Synchronization Core
# ------------------------------------------------------------------------------
#
# Waitable-object semantics, owned independently of the thunks that expose
# them. kernel32 (sync, thread) and the IPC named-sync path are both adapters
# over this; neither owns it, so it sits below both.

vitreo_add_module(
    NAME vitreo_sync_core
    SOURCES
        core/sync/ntsync.cpp
    PUBLIC_DEPS
        vitreo_core
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# ------------------------------------------------------------------------------
# IPC Subsystem - cross-process transport and process model
# ------------------------------------------------------------------------------
#
# Process handles, the process/HWND registries, session state, shared memory,
# named synchronization, and pipes. These are runtime primitives: kernel32 and
# ntdll are adapters over them, which is why they must not depend on WinAPI.
#
# Every source here compiles without any WinAPI header. That is the property
# worth protecting (rule ipc-no-winapi).

vitreo_add_module(
    NAME vitreo_ipc
    SOURCES
        ipc/process_handle.cpp
        ipc/process_registry.cpp
        ipc/com_class_registry.cpp
        ipc/process_session.cpp
        ipc/hwnd_registry.cpp
        ipc/shared_memory.cpp
        ipc/named_sync.cpp
        ipc/unix_pipe.cpp
    PUBLIC_DEPS
        vitreo_core
        vitreo_sync_core
    PRIVATE_DEPS
        vitreo_base
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# ------------------------------------------------------------------------------
# Memory Subsystem
# ------------------------------------------------------------------------------

add_library(vitreo_memory STATIC
    core/memory/early_low_reservation.cpp
    core/memory/x64_high_arena.cpp
    core/memory/main_stack_relocation.cpp
    core/memory/memory_region.cpp
    core/memory/guest_memory.cpp
    core/memory/guest_image_registry.cpp
    core/memory/host_memory_floor.cpp
    core/memory/linear_guest_base_reservation.cpp
    core/memory/linear_jit_view.cpp
    core/memory/region_management.cpp
    core/memory/write_watch_signal.cpp
    core/memory/address_translation.cpp
    core/memory/write_operations.cpp
)

target_include_directories(vitreo_memory PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_memory PUBLIC
    vitreo_core
)
# ------------------------------------------------------------------------------
# CPU Subsystem - State
# ------------------------------------------------------------------------------

add_library(vitreo_cpu_state INTERFACE)

target_include_directories(vitreo_cpu_state INTERFACE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_cpu_state INTERFACE
    vitreo_core
)

# ------------------------------------------------------------------------------
# CPU Subsystem - Decoder
# ------------------------------------------------------------------------------

add_library(vitreo_cpu_decode STATIC
    core/cpu/decode/decoder.cpp
    core/cpu/decode/opcodes.cpp
    core/cpu/decode/modrm.cpp
    core/cpu/decode/opcodes/mov.cpp
    core/cpu/decode/opcodes/arithmetic.cpp
    core/cpu/decode/opcodes/control.cpp
    core/cpu/decode/opcodes/stack.cpp
    core/cpu/decode/opcodes/misc.cpp
    core/cpu/decode/opcodes/two_byte.cpp
    core/cpu/decode/opcodes/logical.cpp
    core/cpu/decode/opcodes/shifts.cpp
    core/cpu/decode/opcodes/atomics_misc.cpp
    core/cpu/decode/opcodes/cmov_setcc.cpp
    core/cpu/decode/opcodes/adc_sbb.cpp
    core/cpu/decode/opcodes/mul_div.cpp
    core/cpu/decode/opcodes/indirect_ctrl.cpp
    core/cpu/decode/opcodes/string_ops.cpp
    core/cpu/decode/opcodes/bit_ops.cpp
    core/cpu/decode/opcodes/fpu.cpp
    core/cpu/decode/opcodes/sse_arith.cpp
    core/cpu/decode/opcodes/sse_data.cpp
    core/cpu/decode/opcodes/sse_conv.cpp
    core/cpu/decode/opcodes/sse_cmp.cpp
    core/cpu/decode/opcodes/sse_int.cpp    core/cpu/decode/opcodes/sse_shuffle.cpp
    core/cpu/decode/opcodes/three_byte_38.cpp
    core/cpu/decode/opcodes/three_byte_3a.cpp
    core/cpu/decode/opcodes/vex.cpp
    core/cpu/decode/opcodes/evex.cpp)

target_include_directories(vitreo_cpu_decode PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_cpu_decode PUBLIC
    vitreo_core
)

# ------------------------------------------------------------------------------
# CPU Subsystem - Interpreter
# ------------------------------------------------------------------------------

add_library(vitreo_cpu_interpreter STATIC
    core/cpu/cpuid_synth.cpp
    core/cpu/interpreter/interpreter.cpp
    core/cpu/interpreter/interpreter_step.cpp
    core/cpu/interpreter/operand_read_write_public_interface.cpp
    core/cpu/interpreter/ops/helpers.cpp
    core/cpu/interpreter/ops/mov.cpp
    core/cpu/interpreter/ops/arithmetic.cpp
    core/cpu/interpreter/ops/control.cpp
    core/cpu/interpreter/ops/stack.cpp
    core/cpu/interpreter/ops/misc.cpp
    core/cpu/interpreter/ops/logical.cpp
    core/cpu/interpreter/ops/shifts.cpp
    core/cpu/interpreter/ops/rotates.cpp
    core/cpu/interpreter/ops/atomics_misc.cpp
    core/cpu/interpreter/ops/cmov_setcc.cpp
    core/cpu/interpreter/ops/inc_dec_adc.cpp
    core/cpu/interpreter/ops/mul_div.cpp
    core/cpu/interpreter/ops/indirect_ctrl.cpp
    core/cpu/interpreter/ops/string_ops.cpp
    core/cpu/interpreter/ops/bit_ops.cpp
    core/cpu/interpreter/ops/fpu.cpp
    core/cpu/interpreter/ops/sse_arith.cpp
    core/cpu/interpreter/ops/sse_data.cpp
    core/cpu/state/fx_image.cpp
    core/cpu/state/context64_record.cpp
    core/cpu/state/context32_record.cpp
    core/cpu/interpreter/ops/sse_conv.cpp
    core/cpu/interpreter/ops/sse_cmp.cpp
    core/cpu/interpreter/ops/sse_int.cpp    core/cpu/interpreter/ops/sse_shuffle.cpp
    core/cpu/interpreter/ops/crypto_ops.cpp      core/cpu/interpreter/ops/sse4_ops.cpp
    core/cpu/interpreter/ops/mmx.cpp
    core/cpu/interpreter/ops/legacy_usermode.cpp
    core/cpu/interpreter/ops/avx_ops.cpp
    core/cpu/interpreter/ops/bmi_ops.cpp
    core/cpu/interpreter/ops/fma_ops.cpp
    core/cpu/interpreter/ops/avx_sse_bridge.cpp
    core/cpu/interpreter/ops/xsave_ops.cpp
    core/cpu/interpreter/ops/f16c_adx.cpp)

target_include_directories(vitreo_cpu_interpreter PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_cpu_interpreter PUBLIC
    vitreo_core
    vitreo_base
    vitreo_memory
    vitreo_cpu_state
    vitreo_cpu_decode
)

# ------------------------------------------------------------------------------
# CPU Subsystem - SEH (Structured Exception Handling)
# ------------------------------------------------------------------------------

add_library(vitreo_cpu_seh STATIC
    core/cpu/seh/seh.cpp
    core/cpu/seh/seh32.cpp
)

target_include_directories(vitreo_cpu_seh PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_cpu_seh PUBLIC
    vitreo_core
)

# ==============================================================================
# CPU Subsystem - JIT
# ==============================================================================
#
# Split into five targets rather than one, because "the JIT" is five things
# with different dependencies and different lifetimes:
#
#   vitreo_jit_model      bookkeeping and observability over compiled blocks
#   vitreo_jit_ir         the block IR: data model, frontend, passes
#   vitreo_jit_analysis   decisions made before emission (liveness, idioms)
#   vitreo_jit_runtime    the helper library emitted code CALLS at run time
#   vitreo_jit_backend    the ARM64 emitter itself
#
# The first three contain no ARM64 codegen and build on both backends -- that
# was already true, and was expressed by listing them twice in two divergent
# `add_library(vitreo_cpu_jit)` calls that had to be kept in sync by hand.
# Declaring them once is the point: the arch gate now covers only the code
# that is actually arch-specific.
#
# The dependency direction inside the JIT, which the target graph now enforces:
# backend -> runtime, ir, analysis, model. Nothing points back. In particular
# `ir_frontend.cpp` deliberately does not include `jit_compiler.hpp`, and
# neither does `jit_helpers.cpp` -- the helpers are a library the emitted code
# calls, not part of the compiler.
#
# `vitreo_cpu_jit` remains the name everything links, now as an INTERFACE
# facade carrying no objects. Adding or splitting a target inside it is
# invisible to the runtime, the tools and the tests.

# --- Arch-neutral: block bookkeeping and observability -------------------------
#
# What the rest of the runtime asks about compiled code: is there a block for
# this RIP, how often did it run, what did the JIT decline to compile. None of
# it emits an instruction.
vitreo_add_module(
    NAME vitreo_jit_model
    SOURCES
        core/cpu/jit/block_cache.cpp
        core/cpu/jit/jit_stats.cpp
        core/cpu/jit/jit_coverage.cpp
        core/cpu/jit/hot_block_dump.cpp
        core/cpu/jit/jit_symbol_map.cpp
        core/cpu/jit/translation_store_stats.cpp
    PUBLIC_DEPS
        vitreo_core
        vitreo_base
        vitreo_cpu_decode
        vitreo_memory
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# --- Arch-neutral: the block IR ------------------------------------------------
vitreo_add_module(
    NAME vitreo_jit_ir
    SOURCES
        core/cpu/jit/ir/jit_ir.cpp
        core/cpu/jit/ir/ir_frontend.cpp
        core/cpu/jit/ir/ir_frontend_forms.cpp
        core/cpu/jit/ir/ir_frontend32.cpp
        core/cpu/jit/ir/ir_frontend64.cpp
        core/cpu/jit/ir/ir_frontend64_alu.cpp
        core/cpu/jit/ir/ir_frontend64_mov.cpp
        core/cpu/jit/ir/ir_frontend64_mul_div.cpp
        core/cpu/jit/ir/ir_frontend64_shifts.cpp
        core/cpu/jit/ir/ir_frontend64_rotates.cpp
        core/cpu/jit/ir/ir_frontend64_sse.cpp
        core/cpu/jit/ir/ir_frontend64_stack.cpp
        core/cpu/jit/ir/ir_frontend_stats.cpp
        core/cpu/jit/ir/ir_passes.cpp
    PUBLIC_DEPS
        vitreo_core
        vitreo_base
        vitreo_cpu_decode
        vitreo_cpu_state
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# --- Arch-neutral: pre-emission analysis ---------------------------------------
#
# Flag liveness and idiom detection decide *what* to emit by reading decoded
# instructions. They know nothing about ARM64, which is why a native x86_64
# backend gets to reuse them unchanged.
vitreo_add_module(
    NAME vitreo_jit_analysis
    SOURCES
        core/cpu/jit/flag_liveness.cpp
        core/cpu/jit/idiom_detector.cpp
        core/cpu/jit/jit_config.cpp
    PUBLIC_DEPS
        vitreo_core
        vitreo_base
        vitreo_cpu_decode
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

if(VITREO_ARCH_ARM64)

# --- The helper library emitted code calls at run time -------------------------
#
# Separate from the emitter because the dependency runs one way and is heavy:
# these files reach the interpreter, SEH, the x87 transcendentals and the
# write-watch traps, and none of them includes `jit_compiler.hpp`. Emitted code
# branches to them; they never compile anything.
vitreo_add_module(
    NAME vitreo_jit_runtime
    SOURCES
        core/cpu/jit/jit_helpers.cpp
        core/cpu/jit/jit_helpers_memory.cpp
        core/cpu/jit/jit_helpers_memory_fault.cpp
        core/cpu/jit/jit_helpers_x87.cpp
        core/cpu/jit/jit_helpers_x87_op.cpp
        core/cpu/jit/jit_helpers_sse.cpp
        core/cpu/jit/jit_helpers_sse_cvt.cpp
        core/cpu/jit/jit_helpers_lock.cpp
        core/cpu/jit/jit_helpers_lock_narrow.cpp
        core/cpu/jit/jit_helpers_lock_cmpxchg.cpp
        core/cpu/jit/jit_helpers_lock64.cpp
        core/cpu/jit/jit_helpers_lock_carry.cpp
        core/cpu/jit/jit_helpers_lock_bt.cpp
        core/cpu/jit/jit_helpers_fused_memcpy.cpp
        core/cpu/jit/jit_helpers_delegate.cpp
        core/cpu/jit/jit_helpers_string.cpp
        core/cpu/jit/store_ring_trampoline.cpp
    PUBLIC_DEPS
        vitreo_core
        vitreo_base
        vitreo_memory
        vitreo_cpu_state
        vitreo_cpu_decode
        vitreo_cpu_interpreter
        vitreo_cpu_seh
        vitreo_jit_model
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# --- The ARM64 emitter ---------------------------------------------------------
#
# Every file here writes ARM64 instructions into a MAP_JIT arena. This is the
# only part of the JIT that a native x86_64 backend replaces.
#
# `linear_fault.cpp` is here rather than in the runtime because it includes
# `jit_compiler.hpp` while `jit_compiler.hpp` includes `linear_fault.hpp` --
# the fault fixup and the emitter that produced the faulting instruction are
# one thing, and splitting them would be a cycle, not a boundary.
vitreo_add_module(
    NAME vitreo_jit_backend
    SOURCES
        core/cpu/jit/jit_compiler.cpp
        core/cpu/jit/translation_store.cpp
        core/cpu/jit/jit_translation_store_emit.cpp
        core/cpu/jit/jit_low_level_emit.cpp
        core/cpu/jit/jit_xmm_cache.cpp
        core/cpu/jit/jit_cold_exits.cpp
        core/cpu/jit/jit_ir_gate.cpp
        core/cpu/jit/jit_flags_emit.cpp
        core/cpu/jit/jit_lazy_condition.cpp
        core/cpu/jit/jit_access_emit.cpp
        core/cpu/jit/jit_compile_stats.cpp
        core/cpu/jit/jit_block_watch.cpp
        core/cpu/jit/jit_finalize.cpp
        core/cpu/jit/jit_block_publish.cpp
        core/cpu/jit/jit_direct_link.cpp
        core/cpu/jit/jit_cold_residency.cpp
        core/cpu/jit/jit_ir_tier.cpp
        core/cpu/jit/jit_memory_caches.cpp
        core/cpu/jit/jit_execute.cpp
        core/cpu/jit/jit_memory_emit.cpp
        core/cpu/jit/jit_memory_emit_linear32.cpp
        core/cpu/jit/jit_memory_emit_narrow32.cpp
        core/cpu/jit/jit_memory_emit_guest64.cpp
        core/cpu/jit/jit_memory_emit_linear64_guard.cpp
        core/cpu/jit/jit_memory_emit_vec128.cpp
        core/cpu/jit/jit_memory_emit_vec256.cpp
        core/cpu/jit/jit_memory_emit_vec_cold_stubs.cpp
        core/cpu/jit/jit_memory_emit_cold_stubs.cpp
        core/cpu/jit/jit_memory_emit_process_stubs.cpp
        core/cpu/jit/jit_cold_lazy.cpp
        core/cpu/jit/stencils.cpp
        core/cpu/jit/constructor_destructor.cpp
        core/cpu/jit/jit_code_arena.cpp
        core/cpu/jit/jit_split_layout.cpp
        core/cpu/jit/jit_compile_memo.cpp
        core/cpu/jit/64_bit_emit_primitives_used.cpp
        core/cpu/jit/stage_b1_inline_block_link.cpp
        core/cpu/jit/jit_inchain_thunk.cpp
        core/cpu/jit/instruction_compilation.cpp
        core/cpu/jit/linear_fault.cpp
        core/cpu/jit/linear_fault_patch.cpp
        core/cpu/jit/linear_fault_registry.cpp
        core/cpu/jit/linear_fault_rip_map.cpp
        core/cpu/jit/ir/ir_emit.cpp
        core/cpu/jit/ir/ir_emit_plan.cpp
        core/cpu/jit/ir/ir_emit_feasible.cpp
        core/cpu/jit/ir/ir_emit_snapshot.cpp
        core/cpu/jit/ir/ir_runtime_helpers.cpp
        core/cpu/jit/ops/mov.cpp
        core/cpu/jit/ops/arithmetic.cpp
        core/cpu/jit/ops/carry_flag.cpp
        core/cpu/jit/ops/alu32.cpp
        core/cpu/jit/ops/alu16.cpp
        core/cpu/jit/ops/alu8.cpp
        core/cpu/jit/ops/adc_sbb.cpp
        core/cpu/jit/ops/unary.cpp
        core/cpu/jit/ops/shifts.cpp
        core/cpu/jit/ops/shifts_rm8.cpp
        core/cpu/jit/ops/shifts_rm16.cpp
        core/cpu/jit/ops/shifts_rotate_narrow.cpp
        core/cpu/jit/ops/shifts_rotate_rm32.cpp
        core/cpu/jit/ops/shifts_rcl_rcr_rm32.cpp
        core/cpu/jit/ops/shifts_rcl_rcr_narrow.cpp
        core/cpu/jit/ops/shifts_shld_shrd.cpp
        core/cpu/jit/ops/shifts64.cpp
        core/cpu/jit/ops/mul_div.cpp
        core/cpu/jit/ops/control.cpp
        core/cpu/jit/ops/stack.cpp
        core/cpu/jit/ops/memory_ops.cpp
        core/cpu/jit/ops/memory_ops_xchg.cpp
        core/cpu/jit/ops/memory_ops_cond.cpp
        core/cpu/jit/ops/memory_ops_bt.cpp
        core/cpu/jit/ops/memory_ops_bitscan.cpp
        core/cpu/jit/ops/memory_ops_bmi.cpp
        core/cpu/jit/ops/native_lock.cpp
        core/cpu/jit/ops/locked_rmw32.cpp
        core/cpu/jit/ops/locked_rmw64.cpp
        core/cpu/jit/ops/locked_narrow.cpp
        core/cpu/jit/ops/string_ops.cpp
        core/cpu/jit/ops/fpu.cpp
        core/cpu/jit/ops/fpu_control.cpp
        core/cpu/jit/ops/fpu_stack.cpp
        core/cpu/jit/ops/fpu_load.cpp
        core/cpu/jit/ops/fpu_store.cpp
        core/cpu/jit/ops/fpu_arith_reg.cpp
        core/cpu/jit/ops/fpu_arith_mem.cpp
        core/cpu/jit/ops/fpu_arith_int.cpp
        core/cpu/jit/ops/fpu_math.cpp
        core/cpu/jit/ops/fpu_compare.cpp
        core/cpu/jit/ops/avx.cpp
        core/cpu/jit/ops/sse.cpp
        core/cpu/jit/ops/sse_vex256.cpp
        core/cpu/jit/ops/sse_emit.cpp
        core/cpu/jit/ops/sse_minmax.cpp
        core/cpu/jit/ops/sse_data.cpp
        core/cpu/jit/ops/sse_conv_packed.cpp
        core/cpu/jit/ops/sse_int.cpp
        core/cpu/jit/ops/sse_fp_packed.cpp
        core/cpu/jit/ops/sse_neon_stencils.cpp
        core/cpu/jit/ops/sse_fp_reciprocal.cpp
        core/cpu/jit/ops/sse_fp_scalar.cpp
        core/cpu/jit/ops/sse_crypto.cpp
        core/cpu/jit/ops/sse_shuffle.cpp
        core/cpu/jit/ops/sse_shift.cpp
        core/cpu/jit/ops/sse_insert_extract.cpp
        core/cpu/jit/ops/sse_vex_direct.cpp
        core/cpu/jit/ops/sse_round.cpp
        core/cpu/jit/ops/sse_cmp.cpp
        core/cpu/jit/ops/sse_mmx_forms.cpp
        core/cpu/jit/ops/sse_state.cpp
        core/cpu/jit/ops/sse4_helper_ops.cpp
        core/cpu/jit/ops/sse_conv_scalar.cpp
        core/cpu/jit/ops/direct_armv8_sha_256_message.cpp
        core/cpu/jit/ops/direct_armv8_aes.cpp
        core/cpu/jit/ops/ops64.cpp
        core/cpu/jit/ops/align_checks.cpp
        core/cpu/jit/ops/mov64.cpp
        core/cpu/jit/ops/bit_ops64.cpp
        core/cpu/jit/ops/narrow_alu64.cpp
        core/cpu/jit/ops/arithmetic64.cpp
        core/cpu/jit/ops/adc_sbb64.cpp
        core/cpu/jit/ops/compare64.cpp
        core/cpu/jit/ops/unary64.cpp
        core/cpu/jit/ops/stack64.cpp
        core/cpu/jit/ops/control64.cpp
        core/cpu/jit/ops/flags64.cpp
        core/cpu/jit/ops/mul_div64.cpp
        core/cpu/jit/ops/misc64.cpp
        core/cpu/jit/ops/string_ops64.cpp
        core/cpu/jit/ops/sse64.cpp
        core/cpu/jit/ops/sse_move64.cpp
        core/cpu/jit/ops/mmx64.cpp
    PUBLIC_DEPS
        vitreo_core
        vitreo_base
        vitreo_memory
        vitreo_cpu_state
        vitreo_cpu_decode
        vitreo_cpu_interpreter
        vitreo_cpu_seh
        vitreo_jit_model
        vitreo_jit_ir
        vitreo_jit_analysis
        vitreo_jit_runtime
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

set(_vitreo_jit_backend_targets vitreo_jit_backend vitreo_jit_runtime)
set(_vitreo_cpu_arch_targets "")

# `linear_fault*.cpp` run in signal context, where updating a profile counter
# is not safe. It sat in the CLI section, which worked only because source
# properties are directory-scoped -- keep it beside the target that compiles
# the file. See the VITREO_COVERAGE block in the top-level CMakeLists.txt.
if(VITREO_COVERAGE)
    set_source_files_properties(
        core/cpu/jit/linear_fault.cpp
        core/cpu/jit/linear_fault_patch.cpp
        core/cpu/jit/linear_fault_registry.cpp
        core/cpu/jit/linear_fault_rip_map.cpp
        PROPERTIES COMPILE_OPTIONS
        "-fno-profile-instr-generate;-fno-coverage-mapping"
    )
endif()

else() # VITREO_ARCH_X86_64

# ------------------------------------------------------------------------------
# CPU Subsystem - Native x86_64 backend
# ------------------------------------------------------------------------------
#
# Segment management for native guest execution: the guest TEB in GS, and the
# 32-bit code/data pair for compatibility-mode execution.

add_library(vitreo_cpu_x64 STATIC
    core/cpu/x64/x64_segments.cpp
    core/cpu/x64/compat32.cpp
    core/cpu/x64/cpuid_intercept.cpp
    core/cpu/x64/segment_override_rewrite.cpp
    core/cpu/x64/x64_exception_bridge.cpp
    core/cpu/x64/x64_guest_thread.cpp
    core/cpu/x64/native64.cpp
    core/cpu/x64/native64_stubs.S
)

target_include_directories(vitreo_cpu_x64 PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_cpu_x64 PUBLIC
    vitreo_core
    vitreo_base
)

# The emitter's stand-in: the same symbols, reporting that no block can be
# compiled. It occupies the backend slot so the facade below has the same
# shape on both architectures and no link list needs an arch gate.
vitreo_add_module(
    NAME vitreo_jit_backend
    SOURCES
        core/cpu/jit/jit_unavailable.cpp
    PUBLIC_DEPS
        vitreo_core
        vitreo_base
        vitreo_memory
        vitreo_cpu_state
        vitreo_cpu_decode
        vitreo_cpu_interpreter
        vitreo_cpu_seh
        vitreo_jit_model
        vitreo_jit_ir
        vitreo_jit_analysis
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

set(_vitreo_jit_backend_targets vitreo_jit_backend)
set(_vitreo_cpu_arch_targets vitreo_cpu_x64)

endif()

# --- Facade --------------------------------------------------------------------
#
# Carries no objects, by rule. The moment this grows a source file it stops
# being a boundary and becomes another blob.
add_library(vitreo_cpu_jit INTERFACE)
target_link_libraries(vitreo_cpu_jit INTERFACE
    vitreo_jit_model
    vitreo_jit_ir
    vitreo_jit_analysis
    ${_vitreo_jit_backend_targets}
)

# ------------------------------------------------------------------------------
# CPU Subsystem - facade
# ------------------------------------------------------------------------------
#
# One name for "the CPU": state, decoder, the interpreter that is its semantic
# reference, SEH, and whichever execution backend this architecture has.
#
# Consumers outside the CPU link this and stop enumerating six targets, which
# is what makes the split above and any later one a private matter. The x64
# native backend is inside the facade for the same reason the JIT is: which
# backend runs a guest is not a fact the run loop's link line should encode.
add_library(vitreo_cpu INTERFACE)
target_link_libraries(vitreo_cpu INTERFACE
    vitreo_cpu_state
    vitreo_cpu_decode
    vitreo_cpu_interpreter
    vitreo_cpu_seh
    vitreo_cpu_jit
    ${_vitreo_cpu_arch_targets}
)

# ------------------------------------------------------------------------------
# PE Loader Subsystem - Parser
# ------------------------------------------------------------------------------

add_library(vitreo_pe_parser STATIC
    core/pe/parser/manifest_info.cpp
    core/pe/parser/pe_parser.cpp
    core/pe/parser/version_info.cpp
)

target_include_directories(vitreo_pe_parser PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_pe_parser PUBLIC
    vitreo_core
)

# ------------------------------------------------------------------------------
# PE Loader Subsystem - Section Mapper
# ------------------------------------------------------------------------------

add_library(vitreo_pe_mapper STATIC
    core/pe/mapper/section_mapper.cpp
    core/pe/mapper/image_layout_file.cpp
    core/pe/mapper/image_placement.cpp
)

target_include_directories(vitreo_pe_mapper PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_pe_mapper PUBLIC
    vitreo_core
    vitreo_memory
    vitreo_pe_parser
)

# ------------------------------------------------------------------------------
# PE Loader Subsystem - Relocator
# ------------------------------------------------------------------------------

add_library(vitreo_pe_reloc STATIC
    core/pe/reloc/relocator.cpp
)

target_include_directories(vitreo_pe_reloc PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_pe_reloc PUBLIC
    vitreo_core
    vitreo_memory
    vitreo_pe_parser
)

# ------------------------------------------------------------------------------
# PE Loader Subsystem - Imports
# ------------------------------------------------------------------------------

add_library(vitreo_pe_imports STATIC
    core/pe/imports/import_resolver.cpp
    core/pe/imports/iat_patcher.cpp
    core/pe/imports/delay_import.cpp
    core/pe/imports/api_set_resolver.cpp
)

target_include_directories(vitreo_pe_imports PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_pe_imports PUBLIC
    vitreo_core
    vitreo_memory
    vitreo_pe_parser
)

# ------------------------------------------------------------------------------
# PE Loader Subsystem - Loader (PE-005)
# ------------------------------------------------------------------------------

add_library(vitreo_pe_loader STATIC
    core/pe/loader/load_config.cpp
    core/pe/loader/pe_loader.cpp
    core/pe/loader/cor_image.cpp
    core/pe/loader/tls_processor.cpp
    core/pe/loader/teb_setup.cpp
)

target_include_directories(vitreo_pe_loader PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_pe_loader PUBLIC
    vitreo_core
    vitreo_memory
    vitreo_pe_parser
    vitreo_pe_mapper
    vitreo_pe_reloc
    vitreo_pe_imports
    vitreo_winapi
)

# ------------------------------------------------------------------------------
# Capsule Subsystem - VFS (Virtual Filesystem)
# ------------------------------------------------------------------------------

add_library(vitreo_capsule_vfs STATIC
    capsule/vfs/vfs.cpp
    capsule/vfs/host_filesystem.cpp
    capsule/vfs/iso_filesystem.cpp
    capsule/vfs/image_mount_store.cpp
    capsule/vfs/win_path.cpp
    capsule/vfs/path_translator.cpp
    capsule/vfs/path_encoding.cpp
    capsule/vfs/share_access.cpp
    capsule/vfs/win32_full_path.cpp
    capsule/vfs/file_stream.cpp
    capsule/vfs/directory.cpp
)

target_include_directories(vitreo_capsule_vfs PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_capsule_vfs PUBLIC
    vitreo_core
    vitreo_platform
)

# ------------------------------------------------------------------------------
# Capsule Subsystem - Environment Store
# ------------------------------------------------------------------------------

add_library(vitreo_capsule_env STATIC
    capsule/env/env_store.cpp
)

target_include_directories(vitreo_capsule_env PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_capsule_env PUBLIC
    vitreo_core
)

# ------------------------------------------------------------------------------
# Capsule Subsystem - Registry
# ------------------------------------------------------------------------------

add_library(vitreo_capsule_registry STATIC
    capsule/registry/hive.cpp
    capsule/registry/registry.cpp
    capsule/registry/enum_keys.cpp
    capsule/registry/init_defaults.cpp
    capsule/registry/wow64_redirector.cpp
)

target_include_directories(vitreo_capsule_registry PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_capsule_registry PUBLIC
    vitreo_core
)

# ------------------------------------------------------------------------------
# Platform Adapters - Darwin
# ------------------------------------------------------------------------------
#
# Host services behind portable contracts. Elevation authorization is the
# first: the contract in platform/macos/elevation_authorization.hpp names what the
# runtime needs, and the macOS implementation answers it with a consent dialog
# plus a signed guest-elevation capability (no macOS admin password).
#
# It was compiled into vitreo_cli, which made a host adapter reachable only by
# linking the application layer. Its own target is what lets the process
# runtime below the CLI depend on it.

vitreo_add_module(
    NAME vitreo_platform
    SOURCES
        platform/macos/elevation_authorization.cpp
        platform/macos/host_hardware.cpp
        platform/macos/host_volume_space.cpp
        platform/macos/scoped_environment.cpp
        platform/macos/tcc_responsibility.cpp
    PUBLIC_DEPS
        vitreo_core
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

if(APPLE)
    target_link_libraries(vitreo_platform PUBLIC
        "-framework CoreFoundation"
        # host_hardware.cpp reads the device tree, pmgr and the GPU driver.
        "-framework IOKit"
    )
endif()

# ------------------------------------------------------------------------------
# Process Runtime - the composition root for one guest process
# ------------------------------------------------------------------------------
#
# RuntimeContext owns every service a guest process needs: guest memory, the
# handle table, the capsule VFS and registry, the thunk registry, and the
# WinAPI managers built over them. It is a concrete aggregate with visible
# fields, deliberately not a service locator.
#
# It lived in src/cli/commands/, which made the composition root a detail of
# the command-line front end. Nothing in it depended on the CLI — the move was
# only ever blocked by the elevation adapter above being stuck there too.

vitreo_add_module(
    NAME vitreo_process_runtime
    SOURCES
        runtime/process/runtime_context.cpp
    PUBLIC_DEPS
        vitreo_core
        vitreo_memory
        vitreo_winapi
        vitreo_capsule_vfs
        vitreo_capsule_registry
        vitreo_ipc
        vitreo_platform
        vitreo_graphics_metal
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# ------------------------------------------------------------------------------
# Checkpoint Runtime - versioned guest memory and CPU snapshots
# ------------------------------------------------------------------------------

vitreo_add_module(
    NAME vitreo_checkpoint
    SOURCES
        runtime/checkpoint/checkpoint_format.cpp
        runtime/checkpoint/runtime_checkpoint.cpp
    PUBLIC_DEPS
        vitreo_core
        vitreo_memory
        vitreo_cpu_state
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# ------------------------------------------------------------------------------
# CLI Subsystem
# ------------------------------------------------------------------------------

# ------------------------------------------------------------------------------
# Execution Runtime - the guest run loop and everything around it
# ------------------------------------------------------------------------------
#
# Owns what actually runs a guest: the dispatch loop, guest callbacks,
# exception dispatch for both bitnesses, block decoding for the JIT, the
# hot-thunk fast path, and the diagnostics that observe all of it.
#
# This was inside vitreo_cli, which made running a guest a feature of the
# command-line front end. The CLI parses arguments and calls in here; nothing
# in this target may depend on it (rule runtime-no-cli).

vitreo_add_module(
    NAME vitreo_execution_runtime
    SOURCES
        runtime/execution/guest_run_loop.cpp
        runtime/execution/guest_run_loop_callback.cpp
        runtime/execution/guest_run_loop_thunks.cpp
        runtime/execution/guest_run_loop_chain_thunk.cpp
        runtime/execution/guest_run_loop_native_service.cpp
        runtime/execution/guest_run_loop_thread_state.cpp
        runtime/execution/guest_run_loop_diag.cpp
        runtime/execution/exception_dispatch.cpp
        runtime/execution/exception_dispatch32.cpp
        runtime/execution/block_decoder.cpp
        runtime/execution/thunk_fast_path.cpp
        runtime/execution/thunk_direct.cpp
        runtime/execution/thunk_inchain.cpp
        runtime/diagnostics/pc_trace.cpp
    runtime/diagnostics/callstream.cpp
        runtime/diagnostics/hostcall_ring.cpp
        runtime/diagnostics/fault_report.cpp
        runtime/diagnostics/hang_dump.cpp
        runtime/diagnostics/host_resource_probe.cpp
        runtime/diagnostics/memory_brake.cpp
    PUBLIC_DEPS
        vitreo_core
        vitreo_base
        vitreo_memory
        vitreo_cpu
        vitreo_winapi
        vitreo_process_runtime
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

if(VITREO_ARCH_X86_64)
    target_sources(vitreo_execution_runtime PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/execution/native32_loop.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/execution/native64_loop.cpp)
    # vitreo_cpu_x64 arrives through the vitreo_cpu facade.
endif()
# Every host call runs inside its own autorelease pool
# (graphics/common/autorelease_scope.hpp): the objc runtime's pool entry points.
if(APPLE)
    target_link_libraries(vitreo_execution_runtime PUBLIC objc)
endif()

add_library(vitreo_cli STATIC
    app/cli/parser/arg_parser.cpp
    app/cli/commands/command.cpp
    app/cli/commands/capsule.cpp
    app/cli/commands/process.cpp
    app/cli/commands/mount.cpp
    app/cli/commands/config.cpp
    app/cli/commands/run.cpp
    app/cli/commands/installer_packages.cpp
    app/cli/commands/run_command.cpp
    app/cli/commands/run_app_metadata.cpp
    app/cli/commands/run_image_load.cpp
    app/cli/commands/run_memory_model.cpp
    app/cli/commands/run_main_tls.cpp
    app/cli/commands/run_thunk_region.cpp
    app/cli/commands/run_launch_setup.cpp
    app/cli/commands/run_process_setup.cpp
    app/cli/commands/run_remote_memory.cpp
    app/cli/commands/run_callbacks_window.cpp
    app/cli/commands/run_callbacks_thread.cpp
    app/cli/commands/run_callbacks_media.cpp
    app/cli/commands/run_callbacks_runtime.cpp
    app/cli/commands/run_lifecycle.cpp
    app/cli/commands/dll_registry.cpp
    app/cli/commands/dll_registry_core.cpp
    app/cli/commands/dll_registry_crt.cpp
    app/cli/commands/dll_registry_mscoree.cpp
    app/cli/commands/dll_registry_advapi_ui.cpp
    app/cli/commands/dll_registry_shell_ole.cpp
    app/cli/commands/dll_registry_ui_controls.cpp
    app/cli/commands/dll_registry_winsock_crypto.cpp
    app/cli/commands/dll_registry_input_legacy_graphics.cpp
    app/cli/commands/dll_registry_system_services.cpp
    app/cli/commands/dll_registry_dwrite.cpp
    app/cli/commands/dll_registry_system_programs.cpp
    app/cli/commands/dll_registry_trust_profile_tpm.cpp
    app/cli/commands/dll_registry_graphics_dxgi.cpp
    app/cli/commands/dll_registry_perf_diag.cpp
    app/cli/commands/dll_registry_combase.cpp
    app/cli/commands/external_app_mapping.cpp
    app/cli/commands/import_setup.cpp
    app/cli/commands/guest_thread_context.cpp
)

target_include_directories(vitreo_cli PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_cli PUBLIC
    vitreo_core
    vitreo_base
    vitreo_execution_runtime
    vitreo_checkpoint
    vitreo_platform
    vitreo_process_runtime
    vitreo_memory
    vitreo_cpu
    vitreo_pe
    vitreo_winapi
    vitreo_graphics
)

# ------------------------------------------------------------------------------
# Windowing Subsystem - HWND Manager
# ------------------------------------------------------------------------------

add_library(vitreo_windowing STATIC
    windowing/model/hwnd_manager.cpp
    windowing/messages/message_queue.cpp
    windowing/messages/input_latency.cpp
    windowing/cocoa/window_presentation.cpp
    windowing/cocoa/cocoa_bridge.mm
    windowing/cocoa/cocoa_main_thread.mm
    windowing/cocoa/cocoa_activation.mm
    windowing/cocoa/cocoa_app_session.mm
    windowing/cocoa/cocoa_views.mm
    windowing/cocoa/cocoa_window_delegate.mm
    windowing/cocoa/cocoa_cursor.mm
    windowing/cocoa/cocoa_key_mapping.mm
    windowing/cocoa/cocoa_pointer_mapping.mm
    windowing/cocoa/cocoa_control_handlers.mm
    windowing/cocoa/cocoa_syslink.mm
    windowing/cocoa/cocoa_edit.mm
    windowing/cocoa/cocoa_folder_picker.mm
    windowing/cocoa/cocoa_window_style.mm
    windowing/cocoa/cocoa_window_lifecycle.mm
    windowing/cocoa/cocoa_window_geometry.mm
    windowing/cocoa/cocoa_window_state.mm
    windowing/cocoa/cocoa_child_windows.mm
    windowing/cocoa/cocoa_hosted_peers.mm
    windowing/cocoa/cocoa_presentation_views.mm
    windowing/cocoa/cocoa_framebuffer.mm
    windowing/cocoa/cocoa_video_overlay.mm
    windowing/cocoa/cocoa_move_size.mm
    windowing/cocoa/cocoa_message_box.mm
    windowing/cocoa/cocoa_strings.mm
    windowing/cocoa/cocoa_input.mm
    windowing/cocoa/cocoa_mouse_tracking.mm
    windowing/cocoa/cocoa_display.mm
    windowing/cocoa/cocoa_shell.mm
    windowing/cocoa/cocoa_notify_icons.mm
    windowing/cocoa/cocoa_controls.mm
    windowing/cocoa/cocoa_control_classes.mm
    windowing/cocoa/cocoa_theme_palette.mm
    windowing/cocoa/cocoa_control_props.mm
    windowing/cocoa/cocoa_event_pump.mm
    windowing/cocoa/cocoa_menu.mm
    windowing/cocoa/cocoa_perf_overlay.mm
    windowing/cocoa/cocoa_text.mm
    windowing/cocoa/app_icon_presentation.mm
    windowing/cocoa/audio_bridge.mm
    windowing/cocoa/wave_output.mm
    windowing/resources/dialog_template.cpp
    windowing/resources/resource_walker.cpp
)

target_include_directories(vitreo_windowing PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_windowing PUBLIC
    vitreo_core
    vitreo_graphics_core
    # cocoa_perf_overlay.mm describes the host machine (host_hardware.hpp).
    vitreo_platform
    # cocoa_hosted_peers.mm calls ipc::hwnd_registry_* to publish the presenting
    # host for a hosted peer child; without this the final link fails on an
    # undefined hwnd_registry_set_present_host.
    vitreo_ipc
    "-framework Cocoa"
    "-framework QuartzCore"
    "-framework AVFoundation"
    "-framework AudioToolbox"
)

# ------------------------------------------------------------------------------
# Audio Subsystem - Core Contract + CoreAudio Backend
# ------------------------------------------------------------------------------

add_library(vitreo_audio STATIC
    audio/coreaudio/coreaudio_renderer.mm
    audio/coreaudio/coreaudio_capture.mm
    audio/ffmpeg/ffmpeg_media_player.cpp
)

target_include_directories(vitreo_audio PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_audio PUBLIC
    vitreo_core
    # The microphone consent check asks which process TCC judges.
    vitreo_platform
    "-framework AudioToolbox"
    "-framework AVFoundation"
    "-framework CoreAudio"
    "-framework Foundation"
)

# ------------------------------------------------------------------------------
# Hardware Bridges - Host Device Contracts + macOS Backends
# ------------------------------------------------------------------------------

add_library(vitreo_hardware STATIC
    hardware/macos/bluetooth_host_iobluetooth.mm
    hardware/macos/gamepad_host_gamecontroller.mm
    hardware/macos/now_playing_mpnowplaying.mm
)

target_include_directories(vitreo_hardware PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_hardware PUBLIC
    vitreo_core
    # No IOBluetooth: every entry point in it reaches the CoreBluetooth
    # coordinator, and a TCC denial there kills the process (see the backend).
    "-framework IOKit"
    "-framework GameController"
    "-framework MediaPlayer"
    "-framework CoreFoundation"
    "-framework Foundation"
)

# DirectShow-era titles commonly ship WMA/WMV that macOS does not decode.
# ffmpeg is a runtime host dependency (install it; optional VITREO_FFMPEG).
# find_program is only a path hint — a binary copied to another Mac still
# looks up ffmpeg on PATH there.
find_program(VITREO_FFMPEG_EXECUTABLE NAMES ffmpeg)
if(VITREO_FFMPEG_EXECUTABLE)
    target_compile_definitions(vitreo_audio PRIVATE
        VITREO_FFMPEG_EXECUTABLE="${VITREO_FFMPEG_EXECUTABLE}")
endif()

# ------------------------------------------------------------------------------
# WinAPI Core - ABI, guest-call frame, thunk registry
# ------------------------------------------------------------------------------
#
# The mechanics every DLL implementation is written against: how a guest call
# arrives, how its arguments are marshaled across the guest/host boundary, and
# how an export is registered and dispatched. No DLL behavior lives here.
#
# thunk_registry.hpp and marshaler.hpp are the two largest include magnets in
# src/ — 114 files each, across ~60 modules. Separating the mechanics from the
# implementations is what eventually lets those headers be split into focused
# ones without touching every DLL at once.
#
# The dependency set is deliberately tiny, and must stay that way: guest
# memory and CPU state are what marshaling is defined in terms of. Anything
# WinAPI-specific belongs in a DLL module, not here.

vitreo_add_module(
    NAME vitreo_winapi_core
    SOURCES
        winapi/core/thunk_registry.cpp
        winapi/core/com_direct.cpp
        winapi/core/host_in_chain.cpp
        winapi/core/api_profile.cpp
        winapi/core/marshaler.cpp
        winapi/msvcrt/guest_accel.cpp
        winapi/msvcrt/guest_accel_x64.cpp
    PUBLIC_DEPS
        vitreo_core
        vitreo_memory
        vitreo_cpu_state
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# ------------------------------------------------------------------------------
# WinAPI Subsystem - DLL implementations
# ------------------------------------------------------------------------------

# ------------------------------------------------------------------------------
# WinAPI - one target per DLL family
# ------------------------------------------------------------------------------
#
# `vitreo_winapi` was one STATIC target with 442 sources. Nine targets now
# carry them, in a graph that is ACYCLIC:
#
#   base <- msvcrt
#   base <- graphics <- user32 <- ui
#   base <- media
#   base, user32, media <- com <- system <- networking
#
# Five of the eight module include cycles are inside a single target here
# (kernel32<->ntdll, kernel32<->advapi32, common<->kernel32 in base;
# dwrite<->gdi32 in graphics; ole32<->rpcrt4 in com) and so cannot break a
# link. The remaining edges all run one way, which is the property this
# split exists to establish: adding a dependency that runs the wrong way is
# now a link error instead of a habit.
#
# The three external deps every family needs -- graphics backends, the
# capsule, IPC -- are PUBLIC on base and reach the rest transitively.

# --- vitreo_winapi_base: core vocabulary, common helpers, kernel32, advapi32 and ntdll.
# These five are one target because they are one cycle: kernel32 <-> ntdll,
# kernel32 <-> advapi32 and common <-> kernel32 all cross between them. The
# shared state behind those edges is the handle table and the thunk registry,
# and deciding where those belong is design work, not a build-file edit -- so
# the cycle is contained in one target and named, rather than spread across
# four that could not link.
#
# `remote_memory.cpp` is here because it is the server answering cross-process
# memory requests, composing this process's own kernel32 handle, mapping and
# virtual-memory managers over the IPC transport. Its path used to say ipc/,
# which is what produced the ipc -> kernel32 -> ntdll -> ipc include cycle;
# moving the file to match the target it compiles into retires that cycle
# rather than exempting it.
vitreo_add_module(
    NAME vitreo_winapi_base
    SOURCES
        winapi/common/display_device_identity.cpp
        winapi/common/timer_resolution.cpp
        winapi/common/c_specific_handler.cpp
        winapi/common/write_sha1_debug.cpp
        # Remote memory is the *server* answering cross-process memory requests:
        # it composes the IPC transport below it with this process's own kernel32
        # managers (handles, mappings, virtual memory), so it belongs at the guest
        # platform service layer rather than in vitreo_ipc.
        winapi/kernel32/memory/remote_memory.cpp
        winapi/kernel32/error.cpp
        winapi/kernel32/handle.cpp
        winapi/kernel32/synthetic_handle_registry.cpp
        winapi/kernel32/shared_resource_handle.cpp
        winapi/kernel32/memory/memory.cpp
        winapi/kernel32/memory/protection_conversion.cpp
        winapi/kernel32/memory/virtualmemorymanager.cpp
        winapi/kernel32/memory/virtualalloc.cpp
        winapi/kernel32/memory/virtualalloc2_placeholder_operations.cpp
        winapi/kernel32/memory/virtualfree.cpp
        winapi/kernel32/memory/virtualprotect.cpp
        winapi/kernel32/memory/heap.cpp
        winapi/kernel32/memory/heap_ext.cpp
        winapi/kernel32/filesystem/file.cpp
        winapi/kernel32/filesystem/import_inherited_named_pipes.cpp
        winapi/kernel32/filesystem/prepare_inherited_named_pipes.cpp
        winapi/kernel32/filesystem/named_pipe_server_process_id.cpp
        winapi/kernel32/filesystem/connect_named_pipe.cpp
        winapi/kernel32/filesystem/create_file.cpp
        winapi/kernel32/filesystem/device_io_control.cpp
        winapi/kernel32/filesystem/file_thunks_and_pipes.cpp
        winapi/kernel32/filesystem/filemanager_construction.cpp
        winapi/kernel32/filesystem/close_file.cpp
        winapi/kernel32/filesystem/readfile_writefile_setfilepointer_getfilesize_wi.cpp
        winapi/kernel32/filesystem/nt_style_file_i_o.cpp
        winapi/kernel32/filesystem/file_pipe_helpers.cpp
        winapi/kernel32/filesystem/file_path_helpers.cpp
        winapi/kernel32/filesystem/file_profile.cpp
        winapi/kernel32/filesystem/win32_vfs_conversion_helpers.cpp
        winapi/kernel32/filesystem/file_ops.cpp
        winapi/kernel32/filesystem/file_operations_stubs.cpp
        winapi/kernel32/filesystem/file_info_by_handle_class.cpp
        winapi/kernel32/filesystem/system_queries.cpp
        winapi/kernel32/filesystem/copy_file_engine.cpp
        winapi/kernel32/filesystem/movefiletransactedw_deletefiletransactedw_passth.cpp
        winapi/kernel32/kernel32_ext.cpp
        winapi/kernel32/kernel32_ext_helpers.cpp
        winapi/kernel32/handle_ext.cpp
        winapi/kernel32/process/process_ext.cpp
        winapi/kernel32/process/actctx.cpp
        winapi/kernel32/process/seh.cpp
        winapi/kernel32/process/vectored_exception.cpp
        winapi/kernel32/process/session_misc.cpp
        winapi/kernel32/process/priority.cpp
        winapi/kernel32/process/toolhelp.cpp
        winapi/kernel32/process/process_apis.cpp
        winapi/kernel32/synchronization/sync_ext.cpp
        winapi/kernel32/synchronization/register_wait.cpp
        winapi/kernel32/synchronization/semaphore_ext.cpp
        winapi/kernel32/filesystem/file_io_ext.cpp
        winapi/kernel32/filesystem/file_i_o.cpp
        winapi/kernel32/filesystem/volume_info.cpp
        winapi/kernel32/filesystem/change_notification.cpp
        winapi/kernel32/filesystem/path_volume_ext.cpp
        winapi/kernel32/filesystem/file_lock.cpp
        winapi/kernel32/filesystem/comm_port.cpp
        winapi/kernel32/filesystem/overlapped_io.cpp
        winapi/kernel32/filesystem/directory_changes.cpp
        winapi/kernel32/filesystem/ansi_paths.cpp
        winapi/kernel32/filesystem/file_time.cpp
        winapi/kernel32/filesystem/file_ops_stubs.cpp
        winapi/kernel32/filesystem/directory_disk.cpp
        winapi/kernel32/kernel32_ext_registration.cpp
        winapi/kernel32/kernel32_ext_registration_unwind_path.cpp
        winapi/kernel32/kernel32_ext_registration_crt_thread.cpp
        winapi/kernel32/kernel32_ntdll_forwarded.cpp
        winapi/kernel32/kernel32_ext_registration_handle_resource.cpp
        winapi/kernel32/kernel32_ext_registration_topology_legacy.cpp
        winapi/kernel32/kernel32_ext_registration_process_misc.cpp
        winapi/kernel32/kernel32_ext_registration_additional_win32.cpp
        winapi/kernel32/kernel32_ext_registration_wave_forwarders.cpp
        winapi/kernel32/kernel32_ext_registration_cef_wer.cpp
        winapi/kernel32/kernel32_ext_registration_atom_comm.cpp
        winapi/kernel32/environment/environment_ext.cpp
        winapi/system/userenv/profile_environment.cpp
        winapi/kernel32/environment/disk.cpp
        winapi/kernel32/environment/profile.cpp
        winapi/kernel32/environment/profile_ansi.cpp
        winapi/kernel32/environment/profile_int.cpp
        winapi/kernel32/filesystem/pipe.cpp
        winapi/kernel32/process/job.cpp
        winapi/kernel32/process/remote.cpp
        winapi/kernel32/thread/context64.cpp
        winapi/kernel32/thread/thread_context.cpp
        winapi/kernel32/thread/restore_context64.cpp
        winapi/kernel32/thread/unwind64.cpp
        winapi/kernel32/synchronization/slist.cpp
        winapi/kernel32/thread/timer_queue.cpp
        winapi/kernel32/thread/fiber_ext.cpp
        winapi/kernel32/thread/thread_apis.cpp
        winapi/kernel32/thread/fiber_apis.cpp
        winapi/kernel32/console/console.cpp
        winapi/kernel32/console/console_alloc.cpp
        winapi/kernel32/console/console_apis.cpp
        winapi/kernel32/console/console_screen.cpp
        winapi/kernel32/console/pseudo_console.cpp
        winapi/kernel32/process/process.cpp
        winapi/kernel32/locale/string_conv.cpp
        winapi/kernel32/locale/codepage.cpp
        winapi/kernel32/locale/codepage_convert.cpp
        winapi/kernel32/text/format_message.cpp
        winapi/kernel32/locale/lcmap.cpp
        winapi/kernel32/locale/normalization.cpp
        winapi/kernel32/locale/number_format.cpp
        winapi/kernel32/locale/string_ops.cpp
        winapi/kernel32/module/module.cpp
        winapi/kernel32/module/lookup_runtime_function_entry.cpp
        winapi/kernel32/module/get_proc_address.cpp
        winapi/kernel32/module/fire_tls_callbacks.cpp
        winapi/kernel32/module/get_module_handle.cpp
        winapi/kernel32/module/reattach_detached_module.cpp
        winapi/kernel32/module/free_library.cpp
        winapi/kernel32/module/notify_dll.cpp
        winapi/kernel32/module/remap_detached_image.cpp
        winapi/kernel32/module/make_companion_resolver.cpp
        winapi/kernel32/module/call_deferred_dll_mains.cpp
        winapi/kernel32/module/import_graph.cpp
        winapi/kernel32/module/setup_dll_tls.cpp
        winapi/kernel32/module/load_library.cpp
        winapi/kernel32/module/construction.cpp
        winapi/kernel32/module/registration.cpp
        winapi/kernel32/module/system_programs.cpp
        winapi/kernel32/module/module_helpers.cpp
        winapi/kernel32/module/module_image_protect.cpp
        winapi/kernel32/module/name_normalisation.cpp
        winapi/kernel32/module/psapi_k32.cpp
        winapi/kernel32/module/resource.cpp
        winapi/kernel32/module/module_loading.cpp
        winapi/kernel32/environment/environment.cpp
        winapi/kernel32/memory/virtual_query.cpp
        winapi/kernel32/memory/interlocked_ext.cpp
        winapi/kernel32/memory/memory_apis.cpp
        winapi/kernel32/memory/heap_extended.cpp
        winapi/kernel32/thread/tls.cpp
        winapi/kernel32/filesystem/filepos.cpp
        winapi/kernel32/synchronization/sync.cpp
        winapi/kernel32/synchronization/sync_wait_profile.cpp
        winapi/kernel32/synchronization/construction.cpp
        winapi/kernel32/synchronization/event_operations.cpp
        winapi/kernel32/synchronization/sync_name_helpers.cpp
        winapi/kernel32/synchronization/sync_cs_mutex.cpp
        winapi/kernel32/synchronization/sync_semaphore.cpp
        winapi/kernel32/synchronization/sync_wait.cpp
        winapi/kernel32/synchronization/sync_sleep.cpp
        winapi/kernel32/synchronization/sync_wait_on_address.cpp
        winapi/core/host_gui_pump.cpp
        winapi/kernel32/thread/thread.cpp
        winapi/kernel32/thread/host_thread_name.cpp
        winapi/kernel32/thread/threadpool.cpp
        winapi/kernel32/thread/thread_suspend.cpp
        winapi/kernel32/synchronization/critical_section.cpp
        winapi/kernel32/synchronization/srw_word.cpp
        winapi/kernel32/system/hardware_description.cpp
        winapi/kernel32/system/machine_identity_registry.cpp
        winapi/kernel32/system/processor_topology.cpp
        winapi/kernel32/system/sysinfo.cpp
        winapi/kernel32/system/new_sysinfomanager_method_implementations.cpp
        winapi/kernel32/system/virtual_firmware.cpp
        winapi/kernel32/system/timezone.cpp
        winapi/kernel32/system/version_info.cpp
        winapi/kernel32/system/atom.cpp
        winapi/kernel32/system/processor_info.cpp
        winapi/kernel32/system/crt_probes.cpp
        winapi/kernel32/system/debug_output.cpp
        winapi/kernel32/thread/fiber_fls.cpp
        winapi/kernel32/kernel32_in_chain.cpp
        winapi/kernel32/memory/file_mapping.cpp
        winapi/kernel32/memory/createfilemappingw.cpp
        winapi/kernel32/memory/openfilemappingw.cpp
        winapi/kernel32/memory/mapviewoffile.cpp
        winapi/kernel32/memory/unmapviewoffile.cpp
        winapi/kernel32/memory/flushviewoffile.cpp
        winapi/kernel32/memory/file_writeback.cpp
        winapi/kernel32/memory/file_mapping_helpers.cpp
        winapi/kernel32/named_object_directory.cpp
        winapi/kernel32/filesystem/file_search.cpp
        winapi/kernel32/memory/global_local.cpp
        winapi/kernel32/locale/locale_string.cpp
        winapi/kernel32/locale/locale_string_registration.cpp
        winapi/kernel32/path/pathcch.cpp
        winapi/kernel32/system/time.cpp
        winapi/kernel32/system/kuser_shared_data.cpp
        winapi/system/ntdll/ntdll.cpp
        winapi/system/ntdll/ntdll_ext.cpp
        winapi/system/ntdll/ntwritefile_implementation.cpp
        winapi/system/ntdll/ntdllextmanager_construction.cpp
        winapi/system/ntdll/ntquerysysteminformation.cpp
        winapi/system/ntdll/ntclose.cpp
        winapi/system/ntdll/ntcreatefile_helpers.cpp
        winapi/system/ntdll/ntcreatefile_implementation.cpp
        winapi/system/ntdll/ntquerydirectoryfile_implementation.cpp
        winapi/system/ntdll/ntreadfile_implementation.cpp
        winapi/system/ntdll/ntsetinformationfile.cpp
        winapi/system/ntdll/ntqueryinformationfile.cpp
        winapi/system/ntdll/ntcreatefile_thunk.cpp
        winapi/system/ntdll/ntopenfile_thunk.cpp
        winapi/system/ntdll/ntquerydirectoryfile_thunk.cpp
        winapi/system/ntdll/ntdeviceiocontrolfile.cpp
        winapi/system/ntdll/ntreadfile_thunk.cpp
        winapi/system/ntdll/ntwritefile_thunk.cpp
        winapi/system/ntdll/ntdll_registration.cpp
        winapi/system/ntdll/ntdll_registration_context_memory.cpp
        winapi/system/ntdll/ntdll_registration_loader_function_tables.cpp
        winapi/system/ntdll/ntdll_registration_object_query.cpp
        winapi/system/ntdll/ntdll_registration_thread_wait.cpp
        winapi/system/ntdll/ntdll_registration_rtl_memory_compression.cpp
        winapi/system/ntdll/ntdll_registration_file.cpp
        winapi/system/ntdll/ntdll_registration_rtl_unwind_version.cpp
        winapi/system/ntdll/nt_query_virtual_memory.cpp
        winapi/system/ntdll/nt_event.cpp
        winapi/system/ntdll/ntdll_registration_native_services.cpp
        winapi/system/ntdll/ntdll_registration_thread_control.cpp
        winapi/system/ntdll/ntdll_registration_key_object.cpp
        winapi/system/ntdll/ntdll_registration_debug_slist_thread_start.cpp
        winapi/system/ntdll/nt_timeout.cpp
        winapi/advapi32/registry/registry.cpp
        winapi/advapi32/registry/regqueryvalueexw.cpp
        winapi/advapi32/registry/regenumvaluew.cpp
        winapi/advapi32/registry/regqueryinfokeya_return_key_metadata.cpp
        winapi/advapi32/registry/serialise_value_convert_regvalue_raw.cpp
        winapi/advapi32/registry/regqueryvalueexa.cpp
        winapi/advapi32/registry/regsetvalueexw.cpp
        winapi/advapi32/registry/registry_registration.cpp
        winapi/advapi32/security/security.cpp
        winapi/advapi32/security/scm_sid_bump_allocator_helpers.cpp
        winapi/advapi32/security/openprocesstoken.cpp
        winapi/advapi32/security/gettokeninformation.cpp
        winapi/advapi32/security/named_security.cpp
        winapi/advapi32/security/credentials.cpp
        winapi/system/hid/hid.cpp
        winapi/advapi32/security/adjusttokenprivileges.cpp
        winapi/advapi32/security/lookupaccountsidw.cpp
        winapi/advapi32/security/sid_helpers_static_guest_memory.cpp
        winapi/advapi32/security/security_registration.cpp
        winapi/advapi32/security/token_exports.cpp
        winapi/advapi32/security/account_lookup_exports.cpp
        winapi/advapi32/security/security_descriptor_exports.cpp
        winapi/advapi32/security/event_tracing_exports.cpp
        winapi/advapi32/security/impersonation_exports.cpp
        winapi/advapi32/security/crypto_support.cpp
        winapi/advapi32/security/crypto_provider_exports.cpp
        winapi/advapi32/security/crypto_hash_exports.cpp
        winapi/advapi32/security/crypto_key_exports.cpp
        winapi/advapi32/security/crypto_param_exports.cpp
        winapi/advapi32/security/crypto_signature_exports.cpp
        winapi/advapi32/security/crypto_cipher_exports.cpp
        winapi/advapi32/security/file_object_security_exports.cpp
        winapi/advapi32/security/sid_conversion_exports.cpp
        winapi/advapi32/security/security_info_exports.cpp
        winapi/advapi32/security/token_operations_exports.cpp
        winapi/advapi32/security/explicit_access_build_exports.cpp
        winapi/advapi32/security/explicit_access_query_exports.cpp
        winapi/advapi32/security/set_token_information_exports.cpp
        winapi/advapi32/security/decrypt_file_exports.cpp
        winapi/advapi32/security/sddl.cpp
        winapi/advapi32/security/acl_api.cpp
        winapi/advapi32/security/access_check.cpp
        winapi/advapi32/security/object_security.cpp
        winapi/advapi32/services/scm.cpp
        winapi/advapi32/services/scm_status_file.cpp
        winapi/advapi32/services/scm_database.cpp
        winapi/advapi32/services/scm_process_control.cpp
        winapi/advapi32/services/scm_service_side.cpp
        winapi/advapi32/services/scm_open_start_exports.cpp
        winapi/advapi32/services/scm_status_control_exports.cpp
        winapi/advapi32/services/scm_config_exports.cpp
        winapi/advapi32/services/scm_service_side_exports.cpp
        winapi/common/guid_text.cpp
        winapi/common/synthetic_version_resource.cpp
    PUBLIC_DEPS
        vitreo_core
        vitreo_base
        vitreo_winapi_core
        vitreo_ipc
        vitreo_sync_core
        vitreo_memory
        vitreo_capsule_vfs
        vitreo_capsule_registry
        vitreo_pe_parser
        vitreo_pe_mapper
        vitreo_compression
        vitreo_audio
        vitreo_hardware
        vitreo_graphics_core
        vitreo_graphics_shader
        # Declared because it is real: dxgi, gdi32, vulkan, user32_ext and
        # setupapi include graphics/metal headers directly. A layering
        # violation, baselined under winapi-frontend-no-metal and removed
        # in Phase E when the frontends target the GraphicsBackend
        # contract. Declared rather than left transitive so the build
        # graph stays honest until then.
        vitreo_graphics_metal
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# --- vitreo_winapi_msvcrt: the C runtime surface. Depends on kernel32 and nothing else.
vitreo_add_module(
    NAME vitreo_winapi_msvcrt
    SOURCES
        winapi/msvcrt/runtime/crt_data.cpp
        winapi/msvcrt/runtime/crt_init.cpp
        winapi/msvcrt/runtime/crt_fpenv.cpp
        winapi/msvcrt/runtime/crt_init_registration.cpp
        winapi/msvcrt/text/crt_string.cpp
        winapi/msvcrt/text/crt_string_registration.cpp
        winapi/msvcrt/text/crt_wctype.cpp
        winapi/msvcrt/text/crt_string_locale_registration.cpp
        winapi/msvcrt/text/crt_string_search_registration.cpp
        winapi/msvcrt/text/crt_string_integer_registration.cpp
        winapi/msvcrt/text/crt_path.cpp
        winapi/msvcrt/text/crt_string_compare_registration.cpp
        winapi/msvcrt/text/crt_string_memory_registration.cpp
        winapi/msvcrt/text/crt_string_classic_registration.cpp
        winapi/msvcrt/text/crt_string_ansi_registration.cpp
        winapi/msvcrt/text/crt_string_wide_registration.cpp
        winapi/msvcrt/text/crt_string_token_registration.cpp
        winapi/msvcrt/text/crt_string_charset_conversion_registration.cpp
        winapi/msvcrt/text/crt_string_fcvt_registration.cpp
        winapi/msvcrt/text/crt_secure_string.cpp
        winapi/msvcrt/text/crt_strtod.cpp
        winapi/msvcrt/math/crt_math.cpp
        winapi/msvcrt/math/crt_math_exact.cpp
        winapi/msvcrt/io/crt_stdio.cpp
        winapi/msvcrt/io/crt_stdio_registration.cpp
        winapi/msvcrt/io/crt_stdio_state.cpp
        winapi/msvcrt/io/crt_stdio_stream_state.cpp
        winapi/msvcrt/io/crt_stdio_std_descriptors.cpp
        winapi/msvcrt/io/crt_stdio_stream_binding.cpp
        winapi/msvcrt/io/crt_stdio_stream_registration.cpp
        winapi/msvcrt/io/crt_stdio_guest_char_registration.cpp
        winapi/msvcrt/io/crt_stdio_open_registration.cpp
        winapi/msvcrt/io/crt_stdio_stream_io_registration.cpp
        winapi/msvcrt/io/crt_stdio_stream_position_registration.cpp
        winapi/msvcrt/io/crt_stdio_char_registration.cpp
        winapi/msvcrt/io/crt_stdio_fprintf_registration.cpp
        winapi/msvcrt/io/crt_stdio_export_locking.cpp
        winapi/msvcrt/io/crt_lowio_open_registration.cpp
        winapi/msvcrt/io/crt_lowio_transfer_registration.cpp
        winapi/msvcrt/io/crt_lowio_descriptor_registration.cpp
        winapi/msvcrt/io/crt_lowio_stat_registration.cpp
        winapi/msvcrt/io/crt_printf.cpp
        winapi/msvcrt/io/crt_printf_registration.cpp
        winapi/msvcrt/io/crt_scanf.cpp
        winapi/msvcrt/memory/crt_memory.cpp
        winapi/msvcrt/memory/crt_memory_registration.cpp
        winapi/msvcrt/memory/crt_aligned.cpp
        winapi/msvcrt/system/crt_environment.cpp
        winapi/msvcrt/text/crt_locale.cpp
        winapi/msvcrt/system/crt_time.cpp
        winapi/msvcrt/io/crt_filesystem.cpp
        winapi/msvcrt/crt_ordinals.cpp
        winapi/msvcrt/runtime/cxx_eh.cpp
        winapi/msvcrt/runtime/crt_exception_class.cpp
        winapi/msvcrt/runtime/except_handler3.cpp
        winapi/msvcrt/runtime/rtti.cpp
    PUBLIC_DEPS
        vitreo_winapi_base
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# --- vitreo_winapi_graphics: GDI, DirectDraw, D3D9/11, DXGI, DirectWrite, OpenGL, Vulkan.
# dwrite <-> gdi32 is a cycle; both are here, so it is contained.
vitreo_add_module(
    NAME vitreo_winapi_graphics
    SOURCES
        winapi/graphics/gdi32/gdi32.cpp
        winapi/graphics/gdi32/construction.cpp
        winapi/graphics/gdi32/device_context_operations.cpp
        winapi/graphics/gdi32/stock_objects.cpp
        winapi/graphics/gdi32/bmp_softbitmap_conversion_cocoabridge_bitmap.cpp
        winapi/graphics/gdi32/bmp_decode.cpp
        winapi/graphics/gdi32/object_creation.cpp
        winapi/graphics/gdi32/palette_operations.cpp
        winapi/graphics/gdi32/pen_line_operations.cpp
        winapi/graphics/gdi32/getobject_retrieve_information_about_gdi.cpp
        winapi/graphics/gdi32/setdibitstodevice_decode_windows_dib_write.cpp
        winapi/graphics/gdi32/bitblt_copy_rectangle_between_dcs.cpp
        winapi/graphics/gdi32/patblt_fill_rectangle_current_brush.cpp
        winapi/graphics/gdi32/savedc_restoredc_dc_state_stack.cpp
        winapi/graphics/gdi32/clipping_region_operations.cpp
        winapi/graphics/gdi32/flush_window_framebuffer_cocoa.cpp
        winapi/graphics/gdi32/gdi32_dib.cpp
        winapi/graphics/gdi32/dib_section_support.cpp
        winapi/graphics/gdi32/ellipse_draw_ellipse_bounded_by.cpp
        winapi/graphics/gdi32/getoutlinetextmetrics.cpp
        winapi/graphics/gdi32/font_data_glyph_access.cpp
        winapi/graphics/gdi32/text_engine.cpp
        winapi/graphics/gdi32/text_exports.cpp
        winapi/graphics/gdi32/geometry_ops.cpp
        winapi/graphics/gdi32/geometry_exports.cpp
        winapi/graphics/gdi32/font_enum.cpp
        winapi/graphics/gdi32/metafile_ops.cpp
        winapi/graphics/gdi32/stretchblt_stretch_copy_rectangle_between.cpp
        winapi/graphics/gdi32/stretchdibits_blit_dib_dc.cpp
        winapi/graphics/gdi32/gdi32_registration.cpp
        winapi/graphics/gdi32/gdi32_d3dkmt_exports.cpp
        winapi/graphics/gdi32/gdi32_dc_object_exports.cpp
        winapi/graphics/gdi32/gdi32_drawing_exports.cpp
        winapi/graphics/gdi32/pixel_format_api.cpp
        winapi/graphics/gdi32/gdi32_dc_state_palette_exports.cpp
        winapi/graphics/gdi32/gdi32_viewport_region_exports.cpp
        winapi/graphics/gdi32/gdi32_shape_region_exports.cpp
        winapi/graphics/gdi32/gdi32_dc_attribute_exports.cpp
        winapi/graphics/gdi32/gdi32_metafile_text_query_exports.cpp
        winapi/graphics/gdi32/gdi32_text_font_exports.cpp
        winapi/graphics/gdi32/gdi32_path_print_exports.cpp
        winapi/graphics/gdi32/bitmap_codec.cpp
        winapi/graphics/gdi32/pixel_transfer.cpp
        winapi/graphics/gdi32/raster_ops.cpp
        winapi/graphics/dcomp/dcomp.cpp
        winapi/graphics/d3dkmt/d3dkmt_dxcore.cpp
        winapi/graphics/dwmapi/dwmapi.cpp
        winapi/graphics/gdiplus/gdiplus_core.cpp
        winapi/graphics/gdiplus/gdiplus_render.cpp
        winapi/graphics/gdiplus/gdiplus_geometry.cpp
        winapi/graphics/gdiplus/gdiplus_geometry_math.cpp
        winapi/graphics/gdiplus/gdiplus_region.cpp
        winapi/graphics/gdiplus/gdiplus_region_model.cpp
        winapi/graphics/gdiplus/gdiplus_graphics.cpp
        winapi/graphics/gdiplus/gdiplus_pen_brush.cpp
        winapi/graphics/gdiplus/gdiplus_draw.cpp
        winapi/graphics/gdiplus/gdiplus_pen.cpp
        winapi/graphics/gdiplus/gdiplus_text.cpp
        winapi/graphics/gdiplus/gdiplus_text_layout.cpp
        winapi/graphics/gdiplus/gdiplus_image.cpp
        winapi/graphics/gdiplus/gdiplus_image_pixels.cpp
        winapi/graphics/gdiplus/gdiplus_image_attributes.cpp
        winapi/graphics/gdiplus/gdiplus_image_draw.cpp
        winapi/graphics/gdiplus/gdiplus_codec.cpp
        winapi/graphics/gdiplus/gdiplus_codec_support.cpp
        winapi/graphics/gdiplus/gdiplus_codec_formats.cpp
        winapi/graphics/gdiplus/gdiplus_codec_imageio.mm
        winapi/graphics/gdiplus/gdiplus_codec_effects.cpp
        winapi/graphics/gdiplus/gdiplus_codec_metafile.cpp
        winapi/graphics/gdiplus/image_decode.mm
        winapi/graphics/ddraw/ddraw.cpp
        winapi/graphics/ddraw/ddraw_clipper.cpp
        winapi/graphics/ddraw/ddraw_display.cpp
        winapi/graphics/ddraw/ddraw_dd7.cpp
        winapi/graphics/ddraw/ddraw_palette.cpp
        winapi/graphics/ddraw/ddraw_surface.cpp
        winapi/graphics/ddraw/ddraw_surface_pixels.cpp
        winapi/graphics/ddraw/ddraw_thunks.cpp
        winapi/graphics/ddraw/d3d7_interface.cpp
        winapi/graphics/ddraw/d3d7_device.cpp
        winapi/graphics/ddraw/d3d7_device_state.cpp
        winapi/graphics/ddraw/d3d7_device_draw.cpp
        winapi/graphics/ddraw/d3d7_state.cpp
        winapi/graphics/ddraw/d3d7_present.cpp
        winapi/graphics/d3d9/d3d9.cpp
        winapi/graphics/d3d9/construction.cpp
        winapi/graphics/d3d9/d3d9_interfaces.cpp
        winapi/graphics/d3d9/d3d9_defaults.cpp
        winapi/graphics/d3d9/d3d9_device.cpp
        winapi/graphics/d3d9/d3d9_device_dispatch.cpp
        winapi/graphics/d3d9/d3d9_device_present.cpp
        winapi/graphics/d3d9/d3d9_device_display.cpp
        winapi/graphics/d3d9/d3d9_device_create.cpp
        winapi/graphics/d3d9/d3d9_device_scene.cpp
        winapi/graphics/d3d9/d3d9_device_state.cpp
        winapi/graphics/d3d9/d3d9_device_texture_state.cpp
        winapi/graphics/d3d9/d3d9_state_block.cpp
        winapi/graphics/d3d9/d3d9_vertex_input.cpp
        winapi/graphics/d3d9/d3d9_vertex_declaration.cpp
        winapi/graphics/d3d9/d3d9_device_shaders.cpp
        winapi/graphics/d3d9/d3d9_query.cpp
        winapi/graphics/d3d9/d3d9_frame_dump.cpp
        winapi/graphics/d3d9/d3d9_resources.cpp
        winapi/graphics/d3d9/d3d9_texture_create.cpp
        winapi/graphics/d3d9/d3d9_texture_autogen.cpp
        winapi/graphics/d3d9/d3d9_texture.cpp
        winapi/graphics/d3d9/d3d9_cube_texture.cpp
        winapi/graphics/d3d9/d3d9_volume.cpp
        winapi/graphics/d3d9/d3d9_buffer.cpp
        winapi/graphics/d3d9/d3d9_transfers.cpp
        winapi/graphics/d3d9/idirect3dresource9_shared_by_every_resource.cpp
        winapi/graphics/d3d9/idirect3dswapchain9.cpp
        winapi/graphics/d3d9/idirect3dvertexshader9_idirect3dpixelshader9.cpp
        winapi/graphics/d3d9/d3d9_draw.cpp
        winapi/graphics/d3d9/d3d9_draw_up.cpp
        winapi/graphics/d3d9/d3d9_draw_pass.cpp
        winapi/graphics/d3d9/d3d9_draw_execute.cpp
        winapi/graphics/d3d9/d3d9_draw_ff_state.cpp
        winapi/graphics/d3d9/d3d9_pv_analyze.cpp
        winapi/graphics/d3d9/d3d9_pv_execute.cpp
        winapi/graphics/d3d9/d3d9_process_vertices.cpp
        winapi/graphics/d3d9/d3d9_cs.cpp
        winapi/graphics/d3d11/d3d11.cpp
        winapi/graphics/d3d11/d3d11_object_storage.cpp
        winapi/graphics/d3d11/d3d11_device_lifetime.cpp
        winapi/graphics/d3d11/d3d11_child_refcount.cpp
        winapi/graphics/d3d11/d3d11_buffer_create.cpp
        winapi/graphics/d3d11/d3d11_texture_create.cpp
        winapi/graphics/d3d11/d3d11_texture_transfer.cpp
        winapi/graphics/d3d11/d3d11_copy_region.cpp
        winapi/graphics/d3d11/d3d11_copy_resource.cpp
        winapi/graphics/d3d11/d3d11_view_create.cpp
        winapi/graphics/d3d11/d3d11_create_device.cpp
        winapi/graphics/d3d11/d3d11_private_data.cpp
        winapi/graphics/d3d11/d3d11_registration.cpp
        winapi/graphics/d3d11/d3d11_swapchain.cpp
        winapi/graphics/d3d11/d3d11_swapchain_factory.cpp
        winapi/graphics/d3d11/d3d11_swapchain_buffers.cpp
        winapi/graphics/d3d11/d3d11_swapchain_fullscreen.cpp
        winapi/graphics/d3d11/d3d11_swapchain_backend.cpp
        winapi/graphics/d3d11/d3d11_swapchain_foreign.cpp
        winapi/graphics/d3d11/d3d11_swapchain_present.cpp
        winapi/graphics/d3d11/d3d11_swapchain_timing.cpp
        winapi/graphics/d3d11/d3d11_swapchain_methods.cpp
        winapi/graphics/d3d11/d3d11_swapchain_media.cpp
        winapi/graphics/d3d11/d3d11_resource_methods.cpp
        winapi/graphics/d3d11/d3d11_sharing.cpp
        winapi/graphics/d3d11/d3d11_device_methods.cpp
        winapi/graphics/d3d11/d3d11_device_resources.cpp
        winapi/graphics/d3d11/d3d11_device_srv.cpp
        winapi/graphics/d3d11/d3d11_device_uav.cpp
        winapi/graphics/d3d11/d3d11_device_rtv_dsv.cpp
        winapi/graphics/d3d11/d3d11_device_caps.cpp
        winapi/graphics/d3d11/d3d11_device_shaders.cpp
        winapi/graphics/d3d11/d3d11_device_states.cpp
        winapi/graphics/d3d11/d3d11_device_state_pool.cpp
        winapi/graphics/d3d11/d3d11_dxgi_device_methods.cpp
        winapi/graphics/d3d11/d3d11_video_context_methods.cpp
        winapi/graphics/d3d11/d3d11_context.cpp
        winapi/graphics/d3d11/d3d11_mapped_image.cpp
        winapi/graphics/d3d11/d3d11_map_texture.cpp
        winapi/graphics/d3d11/d3d11_map_buffer.cpp
        winapi/graphics/d3d11/d3d11_context_state.cpp
        winapi/graphics/d3d11/d3d11_bind_hazards.cpp
        winapi/graphics/d3d11/d3d11_context_constant_buffers.cpp
        winapi/graphics/d3d11/d3d11_context_shader_stages.cpp
        winapi/graphics/d3d11/d3d11_context_fixed_stages.cpp
        winapi/graphics/d3d11/d3d11_context_draw.cpp
        winapi/graphics/d3d11/d3d11_context_copy.cpp
        winapi/graphics/d3d11/d3d11_context_cpu_access.cpp
        winapi/graphics/d3d11/d3d11_context_clear.cpp
        winapi/graphics/d3d11/d3d11_context_getters.cpp
        winapi/graphics/d3d11/d3d11_in_chain.cpp
        winapi/graphics/d3d11/d3d11_rt_dump.cpp
        winapi/graphics/d3d11/d3d11_pipeline.cpp
        winapi/graphics/d3d11/d3d11_pipeline_diag.cpp
        winapi/graphics/d3d11/d3d11_shader_translate.cpp
        winapi/graphics/d3d11/d3d11_pipeline_objects.cpp
        winapi/graphics/d3d11/d3d11_shader_object_methods.cpp
        winapi/graphics/d3d11/d3d11_state_object_methods.cpp
        winapi/graphics/d3d11/d3d11_descriptor_tables.cpp
        winapi/graphics/d3d11/d3d11_resource_binding.cpp
        winapi/graphics/d3d11/d3d11_sampler_binding.cpp
        winapi/graphics/d3d11/d3d11_dispatch.cpp
        winapi/graphics/d3d11/d3d11_stream_out.cpp
        winapi/graphics/d3d11/d3d11_geometry.cpp
        winapi/graphics/d3d11/d3d11_tessellation.cpp
        winapi/graphics/d3d11/d3d11_device_ext.cpp
        winapi/graphics/d3d11/d3d11_format_caps_context.cpp
        winapi/graphics/d3d11/d3d11_dxgi_surface.cpp
        winapi/graphics/d3d11/d3d11_dxgi_surface_facets.cpp
        winapi/graphics/d3d11/d3d11_device_context_state.cpp
        winapi/graphics/d3d11/d3d11_context_clear_view.cpp
        winapi/graphics/d3d11/d3d11_context_annotations.cpp
        winapi/graphics/d3d11/d3d11_multithread.cpp
        winapi/graphics/d3d11/d3d11_class_linkage.cpp
        winapi/graphics/d3d11/d3d11_class_instance_binding.cpp
        winapi/graphics/d3d11/d3d11_query.cpp
        winapi/graphics/d3d11/d3d11_state_census.cpp
        winapi/graphics/d3d11/d3d11_caps_log.cpp
        winapi/graphics/d3d11/d3d11_draw_dump.cpp
        winapi/graphics/d3d11/d3d11_deferred.cpp
        winapi/graphics/d3d11/d3d11_tiled.cpp
        winapi/graphics/d3d11/d3d11_format_caps.cpp
        winapi/graphics/common/gpu_event_signaler.cpp
        winapi/graphics/common/device_lock_profile.cpp
        winapi/graphics/common/live_diag.cpp
        winapi/graphics/common/shared_keyed_mutex.cpp
        winapi/graphics/common/d3d_sampler.cpp
        winapi/core/guest_com.cpp
        winapi/graphics/d3d10/d3d10.cpp
        winapi/graphics/d3d10/d3d10_device.cpp
        winapi/graphics/d3d10/d3d10_create_device.cpp
        winapi/graphics/d3d10/d3d10_device_create_methods.cpp
        winapi/graphics/d3d10/d3d10_forwarding.cpp
        winapi/graphics/d3d10/d3d10_facets.cpp
        winapi/graphics/d3d10/d3d10_blob.cpp
        winapi/graphics/d3d10/d3d10_state_block.cpp
        winapi/graphics/d3d12/d3d12.cpp
        winapi/graphics/d3d12/d3d12_objects.cpp
        winapi/graphics/d3d12/d3d12_root_signature.cpp
        winapi/graphics/d3d12/d3d12_layout.cpp
        winapi/graphics/d3d12/d3d12_caps.cpp
        winapi/graphics/d3d12/d3d12_device.cpp
        winapi/graphics/d3d12/d3d12_resource.cpp
        winapi/graphics/d3d12/d3d12_descriptor.cpp
        winapi/graphics/d3d12/d3d12_pipeline.cpp
        winapi/graphics/d3d12/d3d12_command_list.cpp
        winapi/graphics/d3d12/d3d12_queue.cpp
        winapi/graphics/d3d12/d3d12_swapchain.cpp
        winapi/graphics/d3d12/d3d12_replay.cpp
        winapi/graphics/d3d12/d3d12_replay_bindings.cpp
        winapi/graphics/d3d12/d3d12_replay_draw.cpp
        winapi/graphics/d3d12/d3d12_replay_copy.cpp
        winapi/graphics/d3d12/d3d12_replay_clear.cpp
        winapi/graphics/d3d12/d3d12_replay_query.cpp
        winapi/graphics/dxgi/dxgi.cpp
        winapi/graphics/dxgi/dxgi_objects.cpp
        winapi/graphics/dxgi/dxgi_interop.cpp
        winapi/graphics/dxgi/dxgi_object_methods.cpp
        winapi/graphics/dxgi/dxgi_factory.cpp
        winapi/graphics/dxgi/dxgi_factory_ext.cpp
        winapi/graphics/dxgi/dxgi_output.cpp
        winapi/graphics/dxgi/dxgi_output_methods.cpp
        winapi/graphics/dxgi/dxgi_adapter.cpp
        winapi/graphics/vulkan/vulkan.cpp
        winapi/graphics/vulkan/logical_device.cpp
        winapi/graphics/d2d1/d2d1.cpp
        winapi/graphics/d2d1/d2d1_render.cpp
        winapi/graphics/d2d1/d2d1_bitmap.cpp
        winapi/graphics/d2d1/d2d1_geometry.cpp
        winapi/graphics/d2d1/d2d1_geometry_draw.cpp
        winapi/graphics/d2d1/d2d1_text.cpp
        winapi/graphics/dwrite/dwrite.cpp
        winapi/graphics/dwrite/dwrite_storage.cpp
        winapi/graphics/dwrite/dwrite_font_files.cpp
        winapi/graphics/dwrite/dwrite_font_file.cpp
        winapi/graphics/dwrite/dwrite_collection.cpp
        winapi/graphics/dwrite/dwrite_font_set.cpp
        winapi/graphics/dwrite/dwrite_font.cpp
        winapi/graphics/dwrite/dwrite_face_metrics.cpp
        winapi/graphics/dwrite/dwrite_face_tables.cpp
        winapi/graphics/dwrite/dwrite_gdi_interop.cpp
        winapi/graphics/dwrite/dwrite_glyph_run_analysis.cpp
        winapi/graphics/dwrite/dwrite_text_analyzer.cpp
        winapi/graphics/dwrite/dwrite_rendering_params.cpp
        winapi/graphics/dwrite/dwrite_localized_strings.cpp
        winapi/graphics/dwrite/dwrite_exports.cpp
        winapi/graphics/dwrite/font_backend.mm
        winapi/graphics/dwrite/text_layout_engine.mm
        winapi/graphics/dwrite/dwrite_text_format.cpp
        winapi/graphics/dwrite/dwrite_text_layout.cpp
        winapi/graphics/msimg32/msimg32.cpp
        winapi/graphics/opengl32/opengl32.cpp
        winapi/graphics/opengl32/gl_context.cpp
        winapi/graphics/opengl32/gl_pixel_formats.cpp
        winapi/graphics/opengl32/gl_wgl.cpp
        winapi/graphics/opengl32/gl_state_defaults.cpp
        winapi/graphics/opengl32/gl_cmd_query.cpp
        winapi/graphics/opengl32/gl_cmd_transform.cpp
        winapi/graphics/opengl32/gl_cmd_lighting.cpp
        winapi/graphics/opengl32/gl_cmd_raster.cpp
        winapi/graphics/opengl32/gl_cmd_framebuffer.cpp
        winapi/graphics/opengl32/gl_cmd_extensions.cpp
        winapi/graphics/opengl32/gl_drawable.cpp
        winapi/graphics/opengl32/gl_cmd_pixel_texture_state.cpp
        winapi/graphics/opengl32/gl_cmd_texture.cpp
        winapi/graphics/opengl32/gl_pixels.cpp
        winapi/graphics/opengl32/gl_client_memory.cpp
        winapi/graphics/opengl32/gl_cmd_pixels.cpp
        winapi/graphics/opengl32/gl_cmd_vertex_state.cpp
        winapi/graphics/opengl32/gl_cmd_vertex.cpp
        winapi/graphics/opengl32/gl_assembler.cpp
        winapi/graphics/opengl32/gl_backend_draw.cpp
        winapi/graphics/opengl32/gl_raster_state.cpp
        winapi/graphics/opengl32/gl_cmd_special.cpp
        winapi/graphics/opengl32/gl_exports.cpp
        winapi/graphics/opengl32/gl_display_list.cpp
        winapi/graphics/glu32/glu32.cpp
        winapi/graphics/glu32/glu32_image.cpp
        winapi/graphics/glu32/glu32_quadric.cpp
    PUBLIC_DEPS
        vitreo_winapi_base
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# --- vitreo_winapi_user32: windows, messages, menus, dialogs. Draws through gdi32.
vitreo_add_module(
    NAME vitreo_winapi_user32
    SOURCES
        winapi/user32/classes/window_class.cpp
        winapi/user32/classes/class_menu.cpp
        winapi/user32/windows/window.cpp
        winapi/user32/windows/destroy_window.cpp
        winapi/user32/windows/write_createstruct.cpp
        winapi/user32/windows/wndproc.cpp
        winapi/user32/windows/window_queries.cpp
        winapi/user32/windows/window_timer.cpp
        winapi/user32/windows/monitor_misc.cpp
        winapi/user32/windows/game_stubs.cpp
        winapi/user32/windows/icon_cursor.cpp
        winapi/user32/windows/dde_timer_msg.cpp
        winapi/user32/windows/rect_util.cpp
        winapi/user32/windows/mdi.cpp
        winapi/user32/messages/message_send.cpp
        winapi/user32/messages/button_control_messages.cpp
        winapi/user32/messages/send_message_thunks.cpp
        winapi/user32/messages/send_message_peer.cpp
        winapi/user32/messages/send_message_timeout.cpp
        winapi/user32/messages/control_treeview.cpp
        winapi/user32/messages/control_syslink.cpp
        winapi/user32/messages/control_edit.cpp
        winapi/user32/messages/control_tab.cpp
        winapi/user32/messages/control_owner_draw_list.cpp
        winapi/user32/messages/control_combolist.cpp
        winapi/user32/messages/control_richedit.cpp
        winapi/user32/messages/control_bars.cpp
        winapi/user32/messages/control_tooltip.cpp
        winapi/user32/messages/cross_process_send.cpp
        winapi/user32/messages/handle_control_message_intercept_common.cpp
        winapi/user32/messages/post_message.cpp
        winapi/user32/messages/send_message.cpp
        winapi/user32/messages/char_nav.cpp
        winapi/user32/messages/char_apis.cpp
        winapi/user32/windows/show_window.cpp
        winapi/user32/windows/enable_window.cpp
        winapi/user32/windows/set_cursor_pos.cpp
        winapi/user32/messages/message_loop.cpp
        winapi/user32/messages/mouse_in_pointer.cpp
        winapi/user32/messages/translate_message.cpp
        winapi/user32/messages/msgwaitformultipleobjects.cpp
        winapi/user32/windows/def_window_proc.cpp
        winapi/user32/resources/resources.cpp
        winapi/user32/resources/icon_planes.cpp
        winapi/user32/dialogs/dialog.cpp
        winapi/user32/dialogs/create_dialog.cpp
        winapi/user32/dialogs/dialog_box_modal_dialog_dialogboxparamw.cpp
        winapi/user32/dialogs/create_controls_template.cpp
        winapi/user32/dialogs/dialog_helpers.cpp
        winapi/user32/dialogs/get_dlg_item.cpp
        winapi/user32/dialogs/dialog_controls.cpp
        winapi/user32/dialogs/dialog_apis_ext.cpp
        winapi/user32/user32_ext.cpp
        winapi/user32/input/mouse_cursor_input.cpp
        winapi/user32/input/caret.cpp
        winapi/user32/input/input_queue_state.cpp
        winapi/user32/input/send_input.cpp
        winapi/user32/input/hotkeys.cpp
        winapi/user32/input/raw_input_registration.cpp
        winapi/user32/input/raw_input_data.cpp
        winapi/user32/input/raw_input_devices.cpp
        winapi/user32/desktop/device_notifications.cpp
        winapi/user32/desktop/dpi.cpp
        winapi/user32/desktop/shell_station_objects.cpp
        winapi/user32/messages/register_window_message.cpp
        winapi/user32/dialogs/dialog_box_indirect_param.cpp
        winapi/user32/classes/class_charset.cpp
        winapi/user32/menus/menu.cpp
        winapi/user32/menus/menu_accel.cpp
        winapi/user32/windows/geometry.cpp
        winapi/user32/windows/keyboard.cpp
        winapi/user32/windows/window_management.cpp
        winapi/user32/windows/window_state.cpp
        winapi/user32/windows/window_pos.cpp
        winapi/user32/hooks/hooks.cpp
        winapi/user32/windows/scroll.cpp
        winapi/user32/clipboard/clipboard.cpp
        winapi/user32/painting/visual.cpp
        winapi/user32/painting/drawing_ext.cpp
        winapi/user32/painting/frame_drawing.cpp
        winapi/user32/messages/wsprintf.cpp
        winapi/user32/classes/class_info.cpp
        winapi/user32/user32_ext_registration.cpp
        winapi/user32/registration/exports_core.cpp
        winapi/user32/registration/exports_window_objects.cpp
        winapi/user32/registration/exports_focus_cursor.cpp
        winapi/user32/registration/exports_input_menu_scroll.cpp
        winapi/user32/registration/exports_icon_clipboard_window.cpp
        winapi/user32/registration/exports_pointer_display_rawinput.cpp
        winapi/user32/registration/exports_caret_keyboard_layout.cpp
        winapi/user32/registration/exports_dpi_displayconfig.cpp
        winapi/user32/registration/exports_audit.cpp
        winapi/user32/desktop/display.cpp
        winapi/user32/desktop/system_metrics.cpp
        winapi/user32/desktop/dde.cpp
        winapi/user32/desktop/station.cpp
        winapi/user32/desktop/system_parameters.cpp
        winapi/user32/painting/painting.cpp
        winapi/user32/classes/system_classes.cpp
    PUBLIC_DEPS
        vitreo_winapi_base
        vitreo_winapi_graphics
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# --- vitreo_winapi_ui: the common-control and theming DLLs, which are user32 clients.
vitreo_add_module(
    NAME vitreo_winapi_ui
    SOURCES
        winapi/ui/oleacc/oleacc.cpp
        winapi/ui/comctl32/comctl32.cpp
        winapi/ui/comctl32/task_dialog.cpp
        winapi/ui/riched20/riched20.cpp
        winapi/ui/uxtheme/uxtheme.cpp
        winapi/ui/uxtheme/uxtheme_properties.cpp
        winapi/ui/uxtheme/uxtheme_draw.cpp
        winapi/ui/uxtheme/uxtheme_buffered_paint.cpp
        winapi/ui/uxtheme/theme_data.cpp
        winapi/ui/uxtheme/theme_render.cpp
        winapi/ui/msctf/msctf.cpp
        winapi/ui/comdlg32/comdlg32.cpp
        winapi/ui/uiautomationcore/uiautomationcore.cpp
        winapi/ui/hhctrl/hhctrl.cpp
        winapi/ui/oledlg/oledlg.cpp
        winapi/ui/imm32/imm32.cpp
    PUBLIC_DEPS
        vitreo_winapi_base
        vitreo_winapi_user32
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# --- vitreo_winapi_media: DirectInput, DirectSound, Media Foundation, winmm, XInput.
vitreo_add_module(
    NAME vitreo_winapi_media
    SOURCES
        winapi/media/mfplat/mfplat.cpp
        winapi/media/mfplat/mf_attributes.cpp
        winapi/media/mfplay/mfplay.cpp
        winapi/media/mfplay/mfplay_player.cpp
        winapi/media/mfplay/mfplay_item.cpp
        winapi/media/dinput8/dinput8.cpp
        winapi/media/dinput/dinput.cpp
        winapi/media/xinput/xinput.cpp
        winapi/media/dsound/dsound.cpp
        winapi/media/dsound/dsound_device.cpp
        winapi/media/dsound/dsound_buffer.cpp
        winapi/media/dsound/dsound_buffer_control.cpp
        winapi/media/dsound/dsound_listener.cpp
        winapi/media/dsound/dsound_notify.cpp
        winapi/media/dsound/dsound_capture.cpp
        winapi/media/dsound/dsound_capture_buffer.cpp
        winapi/media/dsound/dsound_fullduplex.cpp
        winapi/media/dsound/dsound_factory.cpp
        winapi/media/dsound/dsound_thunks.cpp
        winapi/media/winmm/winmm.cpp
        winapi/media/winmm/winmm_wave_callbacks.cpp
        winapi/media/winmm/winmm_wave_format.cpp
        winapi/media/winmm/winmm_playsound.cpp
        winapi/media/winmm/winmm_mmio.cpp
        winapi/media/winmm/winmm_waveout.cpp
        winapi/media/winmm/winmm_wavein.cpp
        winapi/media/winmm/winmm_midi_mixer.cpp
        winapi/media/winmm/winmm_errors.cpp
        winapi/media/winmm/winmm_thunks.cpp
        winapi/media/winmm/timer_scheduler.cpp
        winapi/media/msvfw32/msvfw32.cpp
        winapi/media/inproc_servers/inproc_servers.cpp
    PUBLIC_DEPS
        vitreo_winapi_base
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# --- vitreo_winapi_com: COM: ole32, combase, oleaut32, rpcrt4, msxml, propsys.
# ole32 <-> rpcrt4 is a cycle; both are here, so it is contained.
# ole32 reaches user32 for its message filters and dinput for its class
# factories, which is why this sits above both.
vitreo_add_module(
    NAME vitreo_winapi_com
    SOURCES
        winapi/com/ole32/task_memory.cpp
        winapi/com/ole32/com_core.cpp
        winapi/com/ole32/com_activation.cpp
        winapi/com/ole32/com_create_instance.cpp
        winapi/com/ole32/com_remote_proxy.cpp
        winapi/com/ole32/com_remote_invoke.cpp
        winapi/com/ole32/com_context.cpp
        winapi/com/ole32/com_agile_marshal.cpp
        winapi/com/ole32/com_guest_calls.cpp
        winapi/com/ole32/com_propvariant.cpp
        winapi/com/ole32/com_network_list_manager.cpp
        winapi/com/ole32/com_network_objects.cpp
        winapi/com/ole32/com_shelllink.cpp
        winapi/com/ole32/com_window_property_store.cpp
        winapi/com/ole32/shell_item_dialogs.cpp
        winapi/com/ole32/com_memory.cpp
        winapi/com/ole32/com_server_security.cpp
        winapi/com/ole32/com_core_thunks.cpp
        winapi/com/ole32/com_core_thunks_objects.cpp
        winapi/com/ole32/com_core_thunks_network.cpp
        winapi/com/ole32/com_core_thunks_audio.cpp
        winapi/com/ole32/com_core_thunks_ole.cpp
        winapi/com/ole32/wasapi_services.cpp
        winapi/com/ole32/wasapi_endpoints.cpp
        winapi/com/ole32/wasapi_audio_client.cpp
        winapi/com/ole32/additional_ole32_stubs.cpp
        winapi/com/ole32/com_wait_run.cpp
        winapi/com/ole32/ole_stream_persistence_stubs.cpp
        winapi/com/ole32/ole_class_marshal_stubs.cpp
        winapi/com/ole32/ole_guid_helpers.cpp
        winapi/com/ole32/filter_graph.cpp
        winapi/com/ole32/filter_graph_create.cpp
        winapi/com/ole32/filter_graph_thunks.cpp
        winapi/com/ole32/filter_graph_playback.cpp
        winapi/com/ole32/filter_graph_events.cpp
        winapi/com/ole32/filter_graph_video.cpp
        winapi/com/ole32/filter_graph_guest_renderer.cpp
        winapi/com/ole32/filter_graph_pin_sample.cpp
        winapi/com/ole32/filter_graph_connect.cpp
        winapi/com/ole32/filter_graph_seeking.cpp
        winapi/com/ole32/dshow_host.cpp
        winapi/com/ole32/dshow_objects.cpp
        winapi/com/ole32/dshow_allocator.cpp
        winapi/com/ole32/dshow_clock.cpp
        winapi/com/ole32/dshow_async_reader.cpp
        winapi/com/ole32/dshow_audio_renderer.cpp
        winapi/com/ole32/wic.cpp
        winapi/com/ole32/windowscodecs_dll.cpp
        winapi/com/propsys/propsys.cpp
        winapi/com/rpcrt4/rpcrt4.cpp
        winapi/com/oleaut32/oleaut32.cpp
        winapi/com/oleaut32/oleaut32_variant_coerce.cpp
        winapi/com/oleaut32/oleaut32_safearray.cpp
        winapi/com/oleaut32/oleaut32_safearray_access.cpp
        winapi/com/oleaut32/oleaut32_ordinal_exports.cpp
        winapi/com/oleaut32/oleaut32_wire_marshal.cpp
        winapi/com/oleaut32/oleaut32_registration.cpp
        winapi/com/oleaut32/oleaut32_typelib.cpp
        winapi/com/oleaut32/typelib.cpp
        winapi/com/rpcrt4/ndr_proxy.cpp
        winapi/com/ole32/storage.cpp
        winapi/com/ole32/object_construction.cpp
        winapi/com/msxml/xml_dom.cpp
        winapi/com/msxml/xpath.cpp
        winapi/com/msxml/msxml_com.cpp
        winapi/com/ole32/stgcreatedocfile_stgopenstorage_stgisstoragefile.cpp
        winapi/com/ole32/storage_format.cpp
        winapi/com/combase/combase.cpp
        winapi/com/combase/combase_hstring.cpp
        winapi/com/combase/combase_inspectable.cpp
        winapi/com/combase/combase_activation.cpp
        winapi/com/combase/combase_globalization.cpp
        winapi/com/combase/combase_json_value.cpp
        winapi/com/combase/combase_json_object.cpp
        winapi/com/combase/combase_async.cpp
        winapi/com/combase/combase_bluetooth.cpp
        winapi/com/combase/combase_launcher.cpp
        winapi/com/combase/combase_winrt_statics.cpp
        winapi/com/combase/combase_smtc.cpp
        winapi/com/combase/combase_smtc_display.cpp
        winapi/com/combase/combase_thunks.cpp
    PUBLIC_DEPS
        vitreo_winapi_base
        vitreo_winapi_user32
        vitreo_winapi_media
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# --- vitreo_winapi_system: the remaining system DLLs: crypto, setup, shell, psapi, msi.
# shell32 needs comctl32 and ole32, which is what puts this above ui and com.
vitreo_add_module(
    NAME vitreo_winapi_system
    SOURCES
        winapi/system/setupapi/setupapi.cpp
        winapi/system/setupapi/setupapi_util.cpp
        winapi/system/setupapi/setupapi_device_tree.cpp
        winapi/system/setupapi/setupapi_classes.cpp
        winapi/system/setupapi/setupapi_device_info_set.cpp
        winapi/system/setupapi/setupapi_device_enum.cpp
        winapi/system/setupapi/setupapi_device_properties.cpp
        winapi/system/setupapi/setupapi_install_params.cpp
        winapi/system/setupapi/setupapi_driver_info.cpp
        winapi/system/setupapi/setupapi_device_install.cpp
        winapi/system/setupapi/setupapi_file_copy.cpp
        winapi/system/setupapi/setupapi_inf_install.cpp
        winapi/system/sysprog/regsvr32.cpp
        winapi/system/setupapi/inf_file.cpp
        winapi/system/setupapi/cfgmgr32.cpp
        winapi/system/setupapi/cfgmgr32_devnode.cpp
        winapi/system/setupapi/cfgmgr32_lists.cpp
        winapi/system/dbghelp/dbghelp.cpp
        winapi/system/dbghelp/msvc_demangle.cpp
        winapi/system/shell32/shell_paths.cpp
        winapi/system/shell32/classic_shell_namespace_com_interfaces.cpp
        winapi/system/shell32/shell_paths_registration.cpp
        winapi/system/shell32/notify_icons.cpp
        winapi/system/shell32/app_user_model_id.cpp
        winapi/system/shlwapi/shlwapi.cpp
        winapi/system/shlwapi/shlwapi_path_extras.cpp
        winapi/system/shlwapi/shlwapi_url.cpp
        winapi/system/shlwapi/shlwapi_url_core.cpp
        winapi/system/version/version.cpp
        winapi/system/amsi/amsi.cpp
        winapi/system/lz32/lz32.cpp
        winapi/system/imagehlp/imagehlp.cpp
        winapi/system/authz/authz.cpp
        winapi/system/cabinet/cabinet_fdi.cpp
        winapi/system/credui/credui.cpp
        winapi/system/cryptui/cryptui.cpp
        winapi/system/esent/esent.cpp
        winapi/system/psapi/psapi.cpp
        winapi/system/crypt32/authenticode.cpp
        winapi/system/crypt32/auth_root.cpp
        winapi/system/crypt32/x509_parse.cpp
        winapi/system/crypt32/pfx_is_pfx_blob.cpp
        winapi/system/crypt32/verify_certificate_signature.cpp
        winapi/system/crypt32/crypt32.cpp
        winapi/system/crypt32/cert_context.cpp
        winapi/system/crypt32/cert_properties.cpp
        winapi/system/crypt32/cert_chain.cpp
        winapi/system/crypt32/cert_find_names.cpp
        winapi/system/crypt32/cert_intended_key_usage.cpp
        winapi/system/crypt32/certaddcertificatecontexttostore.cpp
        winapi/system/crypt32/stub_implementations_unchanged_return_failure.cpp
        winapi/system/crypt32/dpapi_macos_keychain_commoncrypto.cpp
        winapi/system/crypt32/cryptstringtobinarya_cryptbinarytostringa_base64.cpp
        winapi/system/crypt32/wintrust_dll_authenticode_module_verification.cpp
        winapi/system/crypt32/certificate_stores_return_valid_looking.cpp
        winapi/system/crypt32/crypt32_registration.cpp
        winapi/system/crypt32/crypt_msg_signer.cpp
        winapi/system/bcrypt/bcrypt.cpp
        winapi/system/mscoree/mscoree_shim.cpp
        winapi/system/msi/msi_core.cpp
        winapi/system/msi/msi_sequence.cpp
        winapi/system/msi/msi_features.cpp
        winapi/system/msi/msi_assembly.cpp
        winapi/system/msi/msi_files.cpp
        winapi/system/msi/msi_search.cpp
        winapi/system/msi/msi_register.cpp
        winapi/system/msi/msi_register_db.cpp
        winapi/system/msi/msi_register_api.cpp
        winapi/system/msi/msi_register_com.cpp
        winapi/system/msi/msi_product.cpp
        winapi/system/msi/msi_customaction.cpp
        winapi/system/msi/msi_remote.cpp
        winapi/system/msi/msi_service.cpp
        winapi/system/msi/msi_database.cpp
        winapi/system/msi/msi_sql.cpp
        winapi/system/msi/msi_validation.cpp
        winapi/system/msi/msi_transform.cpp
        winapi/system/msi/msi_ui.cpp
        winapi/system/msi/msi_ui_dialog.cpp
        winapi/system/msi/msi_ui_runtime.cpp
        winapi/system/msi/msi_ui_win32.cpp
        winapi/system/msi/msi.cpp
        winapi/system/msi/msi_package.cpp
        winapi/system/msi/msi_engine.cpp
        winapi/system/msi/msi_registry.cpp
        winapi/system/msi/msi_install.cpp
        winapi/system/winspool/winspool.cpp
        winapi/system/wtsapi32/wtsapi32.cpp
        winapi/system/powrprof/powrprof.cpp
        winapi/system/ncrypt/ncrypt.cpp
        winapi/system/ktmw32/ktmw32.cpp
        winapi/system/wer/wer.cpp
        winapi/system/advpack/advpack.cpp
    PUBLIC_DEPS
        vitreo_winapi_base
        vitreo_winapi_com
        vitreo_winapi_ui
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# --- vitreo_winapi_networking: winsock, WinHTTP, WinINet, DNS, urlmon, secur32.
# secur32 needs crypt32, which is why this sits above system.
vitreo_add_module(
    NAME vitreo_winapi_networking
    SOURCES
        winapi/networking/ws2_32/ws2_32.cpp
        winapi/networking/ws2_32/ws2_socket_lifecycle.cpp
        winapi/networking/ws2_32/ws2_sockopt.cpp
        winapi/networking/ws2_32/ws2_netdb.cpp
        winapi/networking/ws2_32/ws2_recv_connect.cpp
        winapi/networking/ws2_32/ws2_select_send.cpp
        winapi/networking/ws2_32/ws2_32_helpers.cpp
        winapi/networking/ws2_32/ws2manager_implementation.cpp
        winapi/networking/ws2_32/wait_for_multiple_events.cpp
        winapi/networking/ws2_32/refresh_socket_events.cpp
        winapi/networking/ws2_32/begin_overlapped_recv.cpp
        winapi/networking/ws2_32/begin_overlapped_send.cpp
        winapi/networking/ws2_32/ws2_socket_diag.cpp
        winapi/networking/ws2_32/ws2_32_protocol.cpp
        winapi/networking/ws2_32/ws2_32_resolve.cpp
        winapi/networking/ws2_32/ws2_32_registration.cpp
        winapi/networking/ws2_32/ws2_32_address.cpp
        winapi/networking/ws2_32/ws2_32_overlapped.cpp
        winapi/networking/ws2_32/ws2_wsa_ioctl_connectex.cpp
        winapi/networking/ws2_32/ws2_wsa_send_recv.cpp
        winapi/networking/ws2_32/ws2_afd.cpp
        winapi/networking/ws2_32/ws2_msg_control.cpp
        winapi/networking/ws2_32/wsaconnect_connect_optional_send_recv.cpp
        winapi/networking/mswsock/mswsock.cpp
        winapi/networking/sspicli/sspicli.cpp
        winapi/networking/wkscli/wkscli.cpp
        winapi/networking/httpapi/httpapi.cpp
        winapi/networking/httpapi/httpapi_server.cpp
        winapi/networking/netutils/netutils.cpp
        winapi/networking/mpr/mpr.cpp
        winapi/networking/urlmon/urlmon.cpp
        winapi/networking/wsock32/wsock32.cpp
        winapi/networking/winhttp/winhttp.cpp
        winapi/networking/winhttp/proxy.cpp
        winapi/networking/winhttp/misc.cpp
        winapi/networking/winhttp/options.cpp
        winapi/networking/winhttp/url_helpers.cpp
        winapi/networking/winhttp/http_bridge.mm
        winapi/networking/wininet/wininet.cpp
        winapi/networking/wininet/session_connection_lifecycle.cpp
        winapi/networking/wininet/options.cpp
        winapi/networking/wininet/url_helpers.cpp
        winapi/networking/wininet/open_url.cpp
        winapi/networking/iphlpapi/iphlpapi.cpp
        winapi/networking/iphlpapi/tcp_table.cpp
        winapi/networking/iphlpapi/if_table.cpp
        winapi/networking/iphlpapi/adapter_info.cpp
        winapi/networking/iphlpapi/adapter_addresses.cpp
        winapi/networking/iphlpapi/connectivity.cpp
        winapi/networking/iphlpapi/address_notification.cpp
        winapi/networking/iphlpapi/interface_notification.cpp
        winapi/networking/iphlpapi/if_row2.cpp
        winapi/networking/iphlpapi/interface_convert.cpp
        winapi/networking/iphlpapi/icmp.cpp
        winapi/networking/iphlpapi/dhcpcsvc.cpp
        winapi/networking/iphlpapi/host_interfaces.cpp
        winapi/networking/tapi32/tapi32.cpp
        winapi/networking/dnsapi/dnsapi.cpp
        winapi/networking/secur32/secur32.cpp
        winapi/networking/secur32/schannel_tls.mm
    PUBLIC_DEPS
        vitreo_winapi_base
        vitreo_winapi_system
    PUBLIC_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)
# IP Helper reads DHCP leases, per-service DNS and interface kinds from
# SystemConfiguration (winapi/networking/iphlpapi/host_interfaces.cpp).
if(APPLE)
    target_link_libraries(vitreo_winapi_networking PUBLIC "-framework SystemConfiguration")
    # WinHTTP autoproxy: system proxy settings and PAC evaluation
    # (winapi/networking/winhttp/http_bridge.mm).
    target_link_libraries(vitreo_winapi_networking PUBLIC "-framework CFNetwork" "-framework JavaScriptCore")
endif()

if(APPLE)
    target_link_libraries(vitreo_winapi_base PUBLIC
        # FSEvents backs ReadDirectoryChangesW.
        "-framework CoreServices"
        "-framework Security"
        "-framework CoreFoundation"
        "-framework Foundation"
        "-framework CoreText"
        "-framework CoreGraphics"
        "-framework ImageIO"
        icucore
        # zlib: the GDI+ PNG codec (deflate/inflate).
        z
    )
endif()

# --- Facade -------------------------------------------------------------------
#
# Carries no objects. Everything outside WinAPI links this name and is
# unaffected by how the families are arranged behind it.
add_library(vitreo_winapi INTERFACE)
target_link_libraries(vitreo_winapi INTERFACE
    vitreo_winapi_base
    vitreo_winapi_msvcrt
    vitreo_winapi_graphics
    vitreo_winapi_user32
    vitreo_winapi_ui
    vitreo_winapi_media
    vitreo_winapi_com
    vitreo_winapi_system
    vitreo_winapi_networking
)

# ------------------------------------------------------------------------------
# Graphics Subsystem - Core Layer
# ------------------------------------------------------------------------------

add_library(vitreo_graphics_core STATIC
    graphics/common/active_backend.cpp
    graphics/common/hardware_solid_surface.cpp
    graphics/common/path_geometry_math.cpp
    graphics/common/image_codec_imageio.mm
    graphics/common/png_decode.cpp
    graphics/common/perf_diag.cpp
    graphics/common/pipeline_cache.cpp
    graphics/common/surface_presenter.cpp
    graphics/common/present_rate.cpp
    graphics/common/gpu_busy.cpp
    # The fixed-function generator emits MSL from plain state; it has none of
    # the SPIRV-Cross/glslang dependencies the rest of the shader pipeline
    # carries, and every frontend that draws fixed-function geometry needs it.
    graphics/shaders/ff_emulation.cpp
    graphics/shaders/ff_uniforms.cpp
    graphics/shaders/ff_vertex_layout.cpp
    graphics/shaders/sm3_to_msl.cpp
    graphics/shaders/sm3_to_msl_operands.cpp
    graphics/shaders/sm3_to_msl_declarations.cpp
    graphics/shaders/sm3_to_msl_subroutines.cpp
    graphics/shaders/sm3_to_msl_instructions.cpp
    graphics/shaders/sm3_to_msl_emit.cpp
    graphics/shaders/sm3_to_msl_state_folds.cpp
)

target_include_directories(vitreo_graphics_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vitreo_graphics_core PUBLIC
    vitreo_core
)
if(APPLE)
    target_link_libraries(vitreo_graphics_core PUBLIC
        "-framework CoreGraphics" "-framework ImageIO" "-framework CoreFoundation" z)
endif()

# ------------------------------------------------------------------------------
# Graphics Subsystem - Shader Compilation Pipeline
# ------------------------------------------------------------------------------

add_library(vitreo_graphics_shader STATIC
    graphics/shaders/spirv_to_msl.cpp
    graphics/shaders/glsl_to_spirv.cpp
    graphics/shaders/dxbc_parser.cpp
    graphics/shaders/dxbc_to_spirv.cpp
    graphics/shaders/dxbc_to_spirv_decoder.cpp
    graphics/shaders/dxbc_to_spirv_scan_stage.cpp
    graphics/shaders/dxbc_to_spirv_scan_resources.cpp
    graphics/shaders/dxbc_to_spirv_types.cpp
    graphics/shaders/dxbc_to_spirv_signature.cpp
    graphics/shaders/dxbc_to_spirv_resources.cpp
    graphics/shaders/dxbc_to_spirv_bindings.cpp
    graphics/shaders/dxbc_to_spirv_buffer_access.cpp
    graphics/shaders/dxbc_to_spirv_geometry.cpp
    graphics/shaders/dxbc_to_spirv_vertex_capture.cpp
    graphics/shaders/dxbc_to_spirv_tessellation.cpp
    graphics/shaders/dxbc_to_spirv_pixel_stats.cpp
    graphics/shaders/dxbc_to_spirv_function.cpp
    graphics/shaders/dxbc_to_spirv_subroutines.cpp
    graphics/shaders/dxbc_to_spirv_class_linkage.cpp
    graphics/shaders/dxbc_to_spirv_instructions.cpp
    graphics/shaders/dxbc_to_spirv_op_flow.cpp
    graphics/shaders/dxbc_to_spirv_op_integer.cpp
    graphics/shaders/dxbc_to_spirv_op_bitwise.cpp
    graphics/shaders/dxbc_to_spirv_op_float.cpp
    graphics/shaders/dxbc_to_spirv_op_convert.cpp
    graphics/shaders/dxbc_to_spirv_op_query.cpp
    graphics/shaders/dxbc_to_spirv_op_typed.cpp
    graphics/shaders/dxbc_to_spirv_op_buffer.cpp
    graphics/shaders/dxbc_to_spirv_op_atomic.cpp
    graphics/shaders/dxbc_to_spirv_op_sample.cpp
    graphics/shaders/dxbc_to_spirv_double.cpp
    graphics/shaders/dxbc_to_spirv_float_rules.cpp
    graphics/shaders/dxbc_to_spirv_minmax_sample.cpp
    graphics/shaders/dxbc_to_spirv_operands.cpp
    graphics/shaders/shader_cache.cpp
    graphics/shaders/shader_compiler.cpp
)

target_include_directories(vitreo_graphics_shader PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# Ensure FetchContent SPIRV-Cross headers take priority over any system install
target_include_directories(vitreo_graphics_shader BEFORE PUBLIC
    ${spirv_cross_SOURCE_DIR}
)

target_compile_definitions(vitreo_graphics_shader PUBLIC
    SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
)

target_link_libraries(vitreo_graphics_shader PUBLIC
    vitreo_core
    vitreo_graphics_core
    spirv-cross-msl
    SPIRV
    glslang
    glslang-default-resource-limits
)

# ------------------------------------------------------------------------------
# Graphics Subsystem - Metal Backend
# ------------------------------------------------------------------------------

add_library(vitreo_graphics_metal STATIC
    graphics/metal/metal_device.mm
    graphics/metal/metal_device_submission.mm
    graphics/metal/metal_device_sharing.mm
    graphics/metal/metal_device_copy.mm
    graphics/metal/metal_device_builtin.mm
    graphics/metal/metal_backend_buffer.mm
    graphics/metal/metal_backend_texture_2d.mm
    graphics/metal/metal_backend_texture_2d_transfer.mm
    graphics/metal/metal_backend_texture_2d_clear.mm
    graphics/metal/metal_backend_texture_3d.mm
    graphics/metal/metal_backend_pipeline.mm
    graphics/metal/metal_backend_compute.mm
    graphics/metal/metal_backend_draw.mm
    graphics/metal/metal_backend_render_pass.mm
    graphics/metal/metal_backend_swap_chain.mm
    graphics/metal/metal_swapchain.mm
    graphics/metal/metal_submit_queue.mm
    graphics/metal/metal_submit_queue_ownership.mm
    graphics/metal/metal_submit_queue_command_list.mm
    graphics/metal/metal_submit_queue_record_pass.mm
    graphics/metal/metal_submit_queue_record_ops.mm
    graphics/metal/metal_submit_queue_push_arena.mm
    graphics/metal/metal_submit_queue_delta_record.mm
    graphics/metal/metal_submit_queue_delta_apply.mm
    graphics/metal/metal_submit_queue_census.mm
    graphics/metal/metal_submit_queue_pass_merge.mm
    graphics/metal/metal_submit_queue_encode.mm
    graphics/metal/metal_submit_queue_encode_pass.mm
    graphics/metal/metal_submit_queue_encode_draw.mm
    graphics/metal/metal_submit_queue_present.mm
    graphics/metal/metal_resource_heap.mm
    graphics/metal/metal_buffer.mm
    graphics/metal/metal_buffer_pool.mm
    graphics/metal/metal_texture.mm
    graphics/metal/metal_tiled.mm
    graphics/metal/metal_shared_resource.mm
    graphics/metal/metal_pipeline.mm
    graphics/metal/metal_command.mm
    graphics/metal/metal_gpu_profile.mm
    graphics/metal/metal_gpu_watch.mm
    graphics/metal/metal_sync.mm
    graphics/metal/metal_surface_presenter.mm
)

target_include_directories(vitreo_graphics_metal PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_options(vitreo_graphics_metal PRIVATE
    -fobjc-arc          # ARC for Metal/ObjC bridging
    -Wno-old-style-cast # ObjC bridge casts are inherently C-style
)

target_link_libraries(vitreo_graphics_metal PUBLIC
    vitreo_core
    # vitreo/present_cpu.hpp: the [present-cpu] meter record_present feeds.
    vitreo_base
    vitreo_memory
    vitreo_graphics_core
    vitreo_windowing
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
    "-framework Foundation"
    "-framework IOSurface"
)


# ==============================================================================
# Feature facades
# ==============================================================================
#
# One name per feature area, each an INTERFACE target carrying no objects.
#
# They exist so that a consumer names what it needs -- "the PE loader", "the
# capsule", "graphics" -- instead of enumerating the targets that happen to
# implement it today. Splitting or merging a target behind a facade then costs
# one line here rather than an edit to every link list in the tree, which is
# what made the JIT and WinAPI splits above affordable.
#
# The rule for all of them: a facade never grows a source file. The moment it
# does it stops being a boundary and becomes another blob.

# --- PE loading ---------------------------------------------------------------
add_library(vitreo_pe INTERFACE)
target_link_libraries(vitreo_pe INTERFACE
    vitreo_pe_parser
    vitreo_pe_mapper
    vitreo_pe_reloc
    vitreo_pe_imports
    vitreo_pe_loader
)

# --- Capsule: the guest's filesystem, registry and environment -----------------
add_library(vitreo_capsule INTERFACE)
target_link_libraries(vitreo_capsule INTERFACE
    vitreo_capsule_vfs
    vitreo_capsule_registry
    vitreo_capsule_env
)

# --- Graphics: the host rendering backend and its shader pipeline --------------
#
# Deliberately does NOT include the WinAPI graphics frontends (d3d9, dxgi,
# ddraw...). Those are Windows contracts implemented over this; they live in
# vitreo_winapi_graphics and depend on this, not the other way round.
add_library(vitreo_graphics INTERFACE)
target_link_libraries(vitreo_graphics INTERFACE
    vitreo_graphics_core
    vitreo_graphics_shader
    vitreo_graphics_metal
)
