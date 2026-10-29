cmake_minimum_required(VERSION 3.25)

# Vitreo supports macOS Ventura and later. Pin the Mach-O deployment target so
# builds made with a newer SDK remain launchable through LaunchServices on the
# oldest supported host instead of inheriting the build machine's OS version.
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING
        "Minimum supported macOS version")
endif()

project(vitreo 
    VERSION 1.0.0
    DESCRIPTION "Windows Runtime Engine for macOS ARM64"
    LANGUAGES CXX
)

# ------------------------------------------------------------------------------
# Build Configuration
# ------------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Vitreo module/test helpers (vitreo_add_module, vitreo_add_test)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(VitreoModule)

# Default to Release build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ------------------------------------------------------------------------------
# Compiler Flags
# ------------------------------------------------------------------------------

add_compile_options(
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Woverloaded-virtual
    -Wshadow
)

# Source-based coverage (-DVITREO_COVERAGE=ON).
#
# Not just "add the flags": the JIT executes generated code that faults ON
# PURPOSE. The linear-memory fast path stores directly to guest addresses and
# relies on a SIGBUS/SIGSEGV handler to convert a miss into a guest access
# violation, and the TSO fast path deliberately faults once per store site so
# the handler can backpatch it. Instrumenting that handler puts profile-counter
# updates inside signal context, and the process dies on the first such fault
# instead of recovering -- which is why the whole CPU suite used to SIGSEGV
# under coverage and JIT line coverage could not be measured at all.
#
# So: instrument everything except the fault-handling path itself.
option(VITREO_COVERAGE "Build with source-based coverage instrumentation" OFF)
if(VITREO_COVERAGE)
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate)
endif()

# Treat warnings as errors in CI
option(VITREO_WERROR "Treat warnings as errors" OFF)
if(VITREO_WERROR)
    add_compile_options(-Werror)
endif()

# Debug flags
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0)
    add_compile_definitions(VITREO_DEBUG=1)
else()
    add_compile_options(-O3)
    add_compile_definitions(NDEBUG)
endif()

# ------------------------------------------------------------------------------
# Platform Checks
# ------------------------------------------------------------------------------

if(NOT APPLE)
    message(WARNING "Vitreo is designed for macOS. Building on other platforms is unsupported.")
endif()

# ------------------------------------------------------------------------------
# Host Architecture / CPU Backend Selection
# ------------------------------------------------------------------------------
#
# Vitreo's guest CPU backend is host-architecture specific: the ARM64 backend
# translates x86/x64 to ARM64, while the Intel backend executes x86_64 guests
# natively. Everything above the backend seam -- loader, WinAPI, graphics,
# registry, capsule -- is shared and must never branch on host architecture.
#

# CMAKE_OSX_ARCHITECTURES wins when set (it is what actually reaches the
# compiler); otherwise fall back to the build host's processor. Universal
# builds are rejected because a single configure selects exactly one backend.
if(CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _vitreo_arch_count)
    if(_vitreo_arch_count GREATER 1)
        message(FATAL_ERROR
            "Vitreo cannot build a universal binary: the guest CPU backend is "
            "selected per-architecture. Configure one of arm64 or x86_64.")
    endif()
    set(VITREO_TARGET_ARCH "${CMAKE_OSX_ARCHITECTURES}")
    unset(_vitreo_arch_count)
else()
    set(VITREO_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(VITREO_TARGET_ARCH MATCHES "arm64|aarch64")
    set(VITREO_ARCH_ARM64  TRUE)
    set(VITREO_ARCH_X86_64 FALSE)
    add_compile_definitions(VITREO_ARCH_ARM64=1)
    message(STATUS "Vitreo CPU backend: ARM64 (x86/x64 -> ARM64 JIT + interpreter)")
elseif(VITREO_TARGET_ARCH MATCHES "x86_64|amd64|AMD64")
    set(VITREO_ARCH_ARM64  FALSE)
    set(VITREO_ARCH_X86_64 TRUE)
    add_compile_definitions(VITREO_ARCH_X86_64=1)
    message(STATUS "Vitreo CPU backend: x86_64 (native execution) -- IN DEVELOPMENT")

    # The guest entry/exit stubs are hand-written assembly: at a thunk
    # boundary every register belongs to the guest, so there is none left for
    # a compiler to use. Only the x86_64 backend has any.
    enable_language(ASM)

    # Link options every executable that hosts guest memory needs.
    #
    # A 64-bit Mach-O gets a 4GB __PAGEZERO by default, which owns every
    # address below 0x100000000 -- the entire 32-bit guest address space, and
    # the only place a segment-addressed TEB can live (descriptor bases are
    # 32-bit). Without this, mapping guest memory low fails with
    # KERN_INVALID_ADDRESS and looks like a kernel restriction rather than a
    # link-time choice.
    #
    # Shrinking __PAGEZERO then lets dyld place a PIE binary low, right on top
    # of the classic 0x400000 Win32 image base, so the executable is also
    # pinned above the guest address space.
    #
    # Cost: Vitreo's own text loses ASLR on this backend. Acceptable for a
    # diagnostic instrument, and stated rather than silent. -no_pie is
    # deprecated but functional. A future toolchain that drops it needs another
    # way to pin the image above the guest address space.
    set(VITREO_X64_GUEST_LINK_OPTIONS
        -Wl,-pagezero_size,0x1000
        -Wl,-no_pie
        -Wl,-image_base,0x400000000
        # Shrink the main thread stack. macOS anchors the stack TOP and grows
        # it down, so the default 16MB occupies 0x800000-0x1800000 -- right
        # where 32-bit Windows images live. A 0x400000-based image larger than
        # 4MB collides with it, which is every real game we care about
        # (RCT3 is 13.7MB, and a main EXE typically has no .reloc to rebase
        # with). -stack_addr is rejected for modern executables, so sizing is
        # the only lever. 4MB is ample for the host: guest code runs on its own
        # guest stack, and deep host recursion is not a thing Vitreo does.
        -Wl,-stack_size,0x400000
    )
else()
    message(FATAL_ERROR
        "Unsupported host architecture '${VITREO_TARGET_ARCH}'. "
        "Vitreo supports arm64 and x86_64.")
endif()

# ------------------------------------------------------------------------------
# Include Paths
# ------------------------------------------------------------------------------

include_directories(${CMAKE_SOURCE_DIR}/include)

# ------------------------------------------------------------------------------
# Third-Party Dependencies
# ------------------------------------------------------------------------------

include(FetchContent)

# License texts for these pins are in third_party/notices/runtime/.
# Moving a GIT_TAG requires replacing the matching notice file from the new
# tag before a binary is shipped. The build copies that directory next to
# the vitreo executable.
# SPIRV-Cross: SPIR-V → MSL shader translation
FetchContent_Declare(
    spirv_cross
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
    GIT_TAG        vulkan-sdk-1.3.283.0
    GIT_SHALLOW    TRUE
)

# glslang: GLSL → SPIR-V compilation
FetchContent_Declare(
    glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG        vulkan-sdk-1.3.283.0
    GIT_SHALLOW    TRUE
)

# Disable glslang extras we don't need
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)  # Needed for HLSL→SPIR-V path
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# Disable SPIRV-Cross CLI tools
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)

# spirv-cross: Vitreo uses -fno-exceptions, so map SPIR-V errors to abort()
# inside the library. Callers must validate SPIR-V (or isolate compile).
set(SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(spirv_cross glslang)

# Suppress warnings in third-party targets
foreach(_tgt spirv-cross-core spirv-cross-msl spirv-cross-glsl spirv-cross-cpp spirv-cross-reflect spirv-cross-util)
    if(TARGET ${_tgt})
        target_compile_options(${_tgt} PRIVATE -w)
    endif()
endforeach()

foreach(_tgt glslang SPIRV glslang-default-resource-limits)
    if(TARGET ${_tgt})
        target_compile_options(${_tgt} PRIVATE -w)
    endif()
endforeach()

# ------------------------------------------------------------------------------
# Subdirectories
# ------------------------------------------------------------------------------

add_subdirectory(src)

# ------------------------------------------------------------------------------
# Third-party notices beside the binaries that contain those works
# ------------------------------------------------------------------------------

set(_vitreo_runtime_notices
    spirv-cross-LICENSE
    glslang-LICENSE.txt
    khronos-spirv-header-LICENSE
    microsoft-d3d11-tessellator-LICENSE
)
foreach(_notice IN LISTS _vitreo_runtime_notices)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/notices/runtime/${_notice}")
        message(FATAL_ERROR
            "Missing third-party notice third_party/notices/runtime/${_notice}")
    endif()
endforeach()

add_custom_target(vitreo_third_party_notices ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:vitreo>/third-party-notices"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/third_party/notices/runtime"
            "$<TARGET_FILE_DIR:vitreo>/third-party-notices"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/LICENSE"
            "$<TARGET_FILE_DIR:vitreo>/LICENSE"
    COMMENT "Place the Vitreo license and third-party notices next to the vitreo binary"
    VERBATIM
)
add_dependencies(vitreo vitreo_third_party_notices)

# ------------------------------------------------------------------------------
# Testing
# ------------------------------------------------------------------------------

option(VITREO_BUILD_TESTS "Build tests" ON)
if(VITREO_BUILD_TESTS)
    enable_testing()
    
    # Fetch GoogleTest
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    # Disable install for gtest
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/notices/tests/googletest-LICENSE")
        message(FATAL_ERROR
            "Missing third-party notice third_party/notices/tests/googletest-LICENSE")
    endif()
    add_custom_target(vitreo_test_third_party_notices ALL
        COMMAND ${CMAKE_COMMAND} -E rm -rf
                "${CMAKE_BINARY_DIR}/tests/third-party-notices"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/third_party/notices/tests"
                "${CMAKE_BINARY_DIR}/tests/third-party-notices"
        COMMENT "Place the GoogleTest notice with the test binaries"
        VERBATIM
    )

    add_subdirectory(tests)
endif()

if(VITREO_BUILD_FUZZ AND EXISTS "${CMAKE_SOURCE_DIR}/tools/fuzz/CMakeLists.txt")
    add_subdirectory(tools/fuzz)
endif()

option(VITREO_BUILD_CHECKPOINT_TOOLS
       "Build checkpoint inspection tools" ON)
if(VITREO_BUILD_CHECKPOINT_TOOLS)
    if(EXISTS "${CMAKE_SOURCE_DIR}/tools/checkpoint/CMakeLists.txt")
        add_subdirectory(tools/checkpoint)
    endif()
endif()

option(VITREO_BUILD_JIT_BENCH
       "Build standalone in-process JIT microbenchmarks" OFF)
if(VITREO_BUILD_JIT_BENCH AND VITREO_ARCH_ARM64)
    if(EXISTS "${CMAKE_SOURCE_DIR}/tools/jit_bench/CMakeLists.txt")
        add_subdirectory(tools/jit_bench)
    endif()
endif()

# ------------------------------------------------------------------------------
# Architecture checks
# ------------------------------------------------------------------------------
# Declared last so the targets they depend on already exist.

include(VitreoChecks)

# ------------------------------------------------------------------------------
# Installation (future)
# ------------------------------------------------------------------------------

# install(TARGETS vitreo DESTINATION bin)
# install(DIRECTORY include/vitreo DESTINATION include)
