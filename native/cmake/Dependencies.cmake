include(FetchContent)

option(VOID_USE_LOCAL_DEPS
       "Use existing local dependency source directories instead of locked FetchContent downloads"
       OFF)

option(BUILD_ANALYSIS "Build native analysis cache, CLI, and overlay feature" ON)

set(VOID_ZSTD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/analysis/vendor/zstd")
if(BUILD_ANALYSIS AND EXISTS "${VOID_ZSTD_DIR}/build/cmake/CMakeLists.txt" AND NOT TARGET libzstd_static)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
    set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
    set(ZSTD_USE_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
    add_subdirectory("${VOID_ZSTD_DIR}/build/cmake"
                     "${CMAKE_BINARY_DIR}/_deps/zstd-build"
                     EXCLUDE_FROM_ALL)
endif()

function(void_link_zstd target_name)
    if(TARGET libzstd_static)
        target_link_libraries(${target_name} PRIVATE libzstd_static)
    else()
        message(FATAL_ERROR "Vendored zstd is missing at ${VOID_ZSTD_DIR}")
    endif()
endfunction()

# spdlog (header-only): locked FetchContent by default; local cache is explicit opt-in.
set(SPDLOG_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/windows/x64/_deps/spdlog-src")
if(VOID_USE_LOCAL_DEPS AND EXISTS "${SPDLOG_LOCAL_DIR}/include/spdlog/spdlog.h")
    message(STATUS "Using local spdlog from: ${SPDLOG_LOCAL_DIR}")
    add_library(spdlog_header_only INTERFACE)
    target_include_directories(spdlog_header_only INTERFACE "${SPDLOG_LOCAL_DIR}/include")
    add_library(spdlog::spdlog_header_only ALIAS spdlog_header_only)
else()
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG 48bcf39a661a13be22666ac64db8a7f886f2637e
    )
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(spdlog)
endif()

option(BUILD_TESTS "Build tests" ON)
option(BUILD_ANALYSIS_TESTS "Build native analysis tests that require external analysis tools" ON)
if(NOT BUILD_ANALYSIS AND BUILD_ANALYSIS_TESTS)
    message(STATUS "BUILD_ANALYSIS=OFF disables BUILD_ANALYSIS_TESTS")
    set(BUILD_ANALYSIS_TESTS OFF CACHE BOOL "" FORCE)
endif()
if(BUILD_TESTS)
    set(CATCH2_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_deps/catch2-src")
    if(VOID_USE_LOCAL_DEPS AND EXISTS "${CATCH2_LOCAL_DIR}/CMakeLists.txt")
        message(STATUS "Using local Catch2 from: ${CATCH2_LOCAL_DIR}")
        add_subdirectory("${CATCH2_LOCAL_DIR}" "${CMAKE_BINARY_DIR}/_deps/catch2-build")
    else()
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG 56809e5282f104c5c8b570e7c2996cdc352d94f1
        )
        FetchContent_MakeAvailable(Catch2)
    endif()
endif()

option(BUILD_PYTHON "Build Python bindings" ON)
if(BUILD_PYTHON)
    find_package(Python3 COMPONENTS Interpreter Development QUIET)
    find_package(pybind11 CONFIG QUIET)
    if(Python3_FOUND AND pybind11_FOUND)
        message(STATUS "Python: ${Python3_VERSION}")
        message(STATUS "pybind11: ${pybind11_VERSION}")
    else()
        message(STATUS "Python/pybind11 not found, skipping Python bindings")
        set(BUILD_PYTHON OFF)
    endif()
endif()
