# ------------------------------------------------------------------------------
# Vitreo architecture checks
# ------------------------------------------------------------------------------
#
# Turn the guardrails from things a person remembers to run into things the
# build can run.
#
# These are targets rather than steps in the release workflow on purpose.
# .github/workflows/release.yml builds a DMG and does not run them. Targets
# work from any CI, from a git hook, or by hand:
#
#     cmake --build build --target architecture-check   # fast, no apps
#     cmake --build build --target smoke-check          # runs applications
#     cmake --build build --target vitreo-check         # both
#
# None is wired into ALL. A failing ratchet should block a merge, not stop a
# developer from building.

include_guard(GLOBAL)

find_package(Python3 COMPONENTS Interpreter QUIET)

if(NOT Python3_Interpreter_FOUND)
    message(STATUS "Vitreo: python3 not found — architecture-check targets unavailable")
    return()
endif()

set(_vitreo_tools ${CMAKE_SOURCE_DIR}/tools)

if(NOT EXISTS "${_vitreo_tools}/architecture/check_forbidden_includes.py")
    return()
endif()

# --- Static checks: no build artifacts required, so these run anywhere -------

add_custom_target(check-layering
    COMMAND ${Python3_EXECUTABLE} ${_vitreo_tools}/architecture/check_forbidden_includes.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking layering (forbidden includes)"
    VERBATIM
    USES_TERMINAL)

add_custom_target(check-file-sizes
    COMMAND ${Python3_EXECUTABLE} ${_vitreo_tools}/architecture/report_file_sizes.py
            --baseline ${_vitreo_tools}/architecture/file_sizes_baseline.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking file-size ratchet"
    VERBATIM
    USES_TERMINAL)

# Not a ratchet — a report. Included because a new cycle passes every other
# gate: the layering check only catches what someone thought to forbid, and
# this derives the graph instead.
add_custom_target(report-cycles
    COMMAND ${Python3_EXECUTABLE} ${_vitreo_tools}/architecture/report_dependencies.py cycles
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Reporting module include cycles"
    VERBATIM
    USES_TERMINAL)

# --- Checks that need something built ----------------------------------------

# Compares every registered export — name, stdcall bytes, arity, ordinal —
# against the recorded baseline. The gate that makes WinAPI extraction safe.
# Requires VITREO_ENABLE_WIN_ORACLE=ON, which is what builds dump_thunks.
if(TARGET dump_thunks)
    add_custom_target(check-exports
        COMMAND ${CMAKE_COMMAND} -E env VITREO_BUILD_DIR=${CMAKE_BINARY_DIR}
                bash ${_vitreo_tools}/architecture/check_export_inventory.sh --check-names
        DEPENDS dump_thunks
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking guest export inventory"
        VERBATIM
        USES_TERMINAL)
else()
    add_custom_target(check-exports
        COMMAND ${CMAKE_COMMAND} -E echo
                "check-exports needs -DVITREO_ENABLE_WIN_ORACLE=ON (builds dump_thunks)"
        COMMENT "Export inventory check unavailable"
        VERBATIM)
endif()

# Runs real Windows applications. --no-gui keeps it to the console guests, which
# take under a second between them and open no windows; the full suite launches
# Solitaire and StarCraft and takes about half a minute.
add_custom_target(smoke-check
    COMMAND ${Python3_EXECUTABLE} ${_vitreo_tools}/compat/run_smoke_suite.py --no-gui
    DEPENDS vitreo
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running application smoke suite (headless subset)"
    VERBATIM
    USES_TERMINAL)

add_custom_target(smoke-check-all
    COMMAND ${Python3_EXECUTABLE} ${_vitreo_tools}/compat/run_smoke_suite.py
    DEPENDS vitreo
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running application smoke suite (all, opens windows)"
    VERBATIM
    USES_TERMINAL)

# --- Aggregates ---------------------------------------------------------------

# Ratchets, not reports: report-cycles above prints the graph, this fails the
# build when a cycle appears that is not in the baseline. The distinction is
# not academic -- the cycle count went 5 -> 8 while the report was green,
# because a report cannot fail.
add_custom_target(check-cycles
    COMMAND ${Python3_EXECUTABLE} ${_vitreo_tools}/architecture/report_dependencies.py
            cycles --baseline ${_vitreo_tools}/architecture/cycles_baseline.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking module include-cycle ratchet"
    VERBATIM
    USES_TERMINAL)

# Fails when a new target publishes ${CMAKE_SOURCE_DIR}/src, which is what
# makes every other layering rule advisory. Existing offenders are baselined
# debt.
add_custom_target(check-public-includes
    COMMAND ${Python3_EXECUTABLE} ${_vitreo_tools}/architecture/check_public_src_include.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking public include-path ratchet"
    VERBATIM
    USES_TERMINAL)

# The declared CMake target graph must be a DAG. This is a different question
# from report-cycles above, which derives cycles from include paths keyed by
# directory: five of those live inside a single target and cannot break a link.
# A cycle between two TARGETS is invisible in the build, because CMake papers
# over it by repeating archives on the link line.
add_custom_target(check-target-graph
    COMMAND ${Python3_EXECUTABLE} ${_vitreo_tools}/architecture/check_target_graph.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking that the target graph is acyclic"
    VERBATIM
    USES_TERMINAL)

add_custom_target(architecture-check)
add_dependencies(architecture-check
    check-layering check-file-sizes report-cycles
    check-cycles check-public-includes check-target-graph)

add_custom_target(vitreo-check)
add_dependencies(vitreo-check architecture-check check-exports smoke-check)
