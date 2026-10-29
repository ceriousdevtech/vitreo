# ------------------------------------------------------------------------------
# Vitreo module and test helpers
# ------------------------------------------------------------------------------
#
# These exist so a module's dependencies are declared once, in one shape, and so
# that "what may this target see?" has an answer the build system enforces
# rather than a convention people remember.
#
# The rule they encode: a module publishes its own directory and nothing else.
# Vitreo's targets historically put ${CMAKE_SOURCE_DIR}/src on PUBLIC, which
# grants every downstream target access to every internal header and makes
# layering advisory. New modules must not do that.

include_guard(GLOBAL)

# vitreo_add_module(
#     NAME         <target>
#     SOURCES      <file>...
#     [PUBLIC_DEPS  <target>...]   # part of this module's interface
#     [PRIVATE_DEPS <target>...]   # implementation detail, not re-exported
#     [PUBLIC_INCLUDES  <dir>...]  # defaults to the module's include/ if present
#     [PRIVATE_INCLUDES <dir>...]
#     [COMPILE_OPTIONS <opt>...]
#     [OBJC_ARC]                   # compile Objective-C++ sources with ARC
# )
#
# Dependency direction is the point of the split between PUBLIC_DEPS and
# PRIVATE_DEPS: a dependency belongs in PUBLIC only when it appears in this
# module's own headers. Putting it in PRIVATE is what stops it leaking to
# dependents and keeps the graph honest.
function(vitreo_add_module)
    set(options OBJC_ARC)
    set(one_value NAME)
    set(multi_value
        SOURCES PUBLIC_DEPS PRIVATE_DEPS
        PUBLIC_INCLUDES PRIVATE_INCLUDES COMPILE_OPTIONS)
    cmake_parse_arguments(ARG "${options}" "${one_value}" "${multi_value}" ${ARGN})

    if(NOT ARG_NAME)
        message(FATAL_ERROR "vitreo_add_module: NAME is required")
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "vitreo_add_module(${ARG_NAME}): SOURCES is required")
    endif()
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "vitreo_add_module(${ARG_NAME}): unexpected arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    add_library(${ARG_NAME} STATIC ${ARG_SOURCES})

    # A module publishes its own include/ directory when it has one. Until a
    # module grows one, it publishes nothing and its dependents rely on the
    # global include path.
    if(ARG_PUBLIC_INCLUDES)
        target_include_directories(${ARG_NAME} PUBLIC ${ARG_PUBLIC_INCLUDES})
    elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_include_directories(${ARG_NAME} PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    endif()

    target_include_directories(${ARG_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${ARG_PRIVATE_INCLUDES})

    if(ARG_PUBLIC_DEPS)
        target_link_libraries(${ARG_NAME} PUBLIC ${ARG_PUBLIC_DEPS})
    endif()
    if(ARG_PRIVATE_DEPS)
        target_link_libraries(${ARG_NAME} PRIVATE ${ARG_PRIVATE_DEPS})
    endif()

    if(ARG_COMPILE_OPTIONS)
        target_compile_options(${ARG_NAME} PRIVATE ${ARG_COMPILE_OPTIONS})
    endif()

    if(ARG_OBJC_ARC)
        target_compile_options(${ARG_NAME} PRIVATE
            -fobjc-arc
            -Wno-old-style-cast  # ObjC bridging casts are inherently C-style
        )
    endif()
endfunction()

# vitreo_add_test(
#     NAME    <target>
#     SOURCES <file>...
#     [DEPS <target>...]
#     [FRAMEWORKS <name>...]       # Apple frameworks, linked only on APPLE
#     [COMPILE_DEFINITIONS <def>...]
#     [NO_DISCOVER]                # skip gtest_discover_tests
# )
#
# DEPS is deliberately the whole story: a test target that needs more than the
# module under test is telling you the module is not independently usable, and
# that signal is the reason the monolithic test binary is being split at all.
# Resist widening DEPS to make a link error go away — the link error is data.
function(vitreo_add_test)
    set(options NO_DISCOVER)
    set(one_value NAME)
    set(multi_value SOURCES DEPS FRAMEWORKS COMPILE_DEFINITIONS COMPILE_OPTIONS)
    cmake_parse_arguments(ARG "${options}" "${one_value}" "${multi_value}" ${ARGN})

    if(NOT ARG_NAME)
        message(FATAL_ERROR "vitreo_add_test: NAME is required")
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "vitreo_add_test(${ARG_NAME}): SOURCES is required")
    endif()
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "vitreo_add_test(${ARG_NAME}): unexpected arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    add_executable(${ARG_NAME} ${ARG_SOURCES})

    # On the Intel backend, test binaries need the same address-space link
    # options as the runtime: anything that maps guest memory -- or a
    # segment-addressed TEB -- must be able to reach below 4GB. Applied to
    # every test rather than a hand-maintained subset, because the failure
    # mode is a confusing mmap error far from the missing flag.
    if(VITREO_X64_GUEST_LINK_OPTIONS)
        target_link_options(${ARG_NAME} PRIVATE ${VITREO_X64_GUEST_LINK_OPTIONS})
    endif()

    target_link_libraries(${ARG_NAME} PRIVATE ${ARG_DEPS} GTest::gtest_main)

    target_include_directories(${ARG_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR})

    target_compile_options(${ARG_NAME} PRIVATE
        -Wno-old-style-cast   # GTest macros use old-style casts
        -Wno-sign-conversion  # GTest macros trigger this
        ${ARG_COMPILE_OPTIONS})

    if(ARG_COMPILE_DEFINITIONS)
        target_compile_definitions(${ARG_NAME} PRIVATE ${ARG_COMPILE_DEFINITIONS})
    endif()

    if(APPLE AND ARG_FRAMEWORKS)
        foreach(_fw IN LISTS ARG_FRAMEWORKS)
            target_link_libraries(${ARG_NAME} PRIVATE "-framework ${_fw}")
        endforeach()
    endif()

    if(NOT ARG_NO_DISCOVER)
        # Test names must stay stable across the split. They are the only
        # record that can prove an extraction changed no behavior.
        gtest_discover_tests(${ARG_NAME})
    endif()
endfunction()
