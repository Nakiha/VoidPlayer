include(FetchContent)

# spdlog (header-only): try the Flutter build cache first, fallback to FetchContent.
set(SPDLOG_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../build/windows/x64/_deps/spdlog-src")
if(EXISTS "${SPDLOG_LOCAL_DIR}/include/spdlog/spdlog.h")
    message(STATUS "Using local spdlog from: ${SPDLOG_LOCAL_DIR}")
    add_library(spdlog_header_only INTERFACE)
    target_include_directories(spdlog_header_only INTERFACE "${SPDLOG_LOCAL_DIR}/include")
    add_library(spdlog::spdlog_header_only ALIAS spdlog_header_only)
else()
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
    )
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(spdlog)
endif()

option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    set(CATCH2_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_deps/catch2-src")
    if(EXISTS "${CATCH2_LOCAL_DIR}/CMakeLists.txt")
        message(STATUS "Using local Catch2 from: ${CATCH2_LOCAL_DIR}")
        add_subdirectory("${CATCH2_LOCAL_DIR}" "${CMAKE_BINARY_DIR}/_deps/catch2-build")
    else()
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.8.1
            GIT_SHALLOW TRUE
            SOURCE_DIR "${CATCH2_LOCAL_DIR}"
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
