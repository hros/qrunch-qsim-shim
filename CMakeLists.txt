cmake_minimum_required(VERSION 3.16)
project(qrunch_qsim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── External dependencies ──────────────────────────────────────────────────────

set(QSIM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/qsim")
if(NOT EXISTS "${QSIM_DIR}/lib/circuit.h")
    message(FATAL_ERROR
        "qsim not found at ${QSIM_DIR}.\n"
        "Run: git submodule update --init extern/qsim\n"
        "  (maps to: https://github.com/quantumlib/qsim)")
endif()

set(JSON_SINGLE_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/extern/nlohmann/json.hpp")
if(NOT EXISTS "${JSON_SINGLE_HEADER}")
    message(FATAL_ERROR
        "nlohmann/json.hpp not found at ${JSON_SINGLE_HEADER}.\n"
        "Download from: https://github.com/nlohmann/json/releases\n"
        "  or: cmake -DFETCH_JSON=ON .")
endif()

find_package(Threads REQUIRED)

# ── Library target ─────────────────────────────────────────────────────────────

add_library(qrunch_qsim SHARED src/qrunch_qsim.cpp)

target_include_directories(qrunch_qsim
    PRIVATE
        include
        "${QSIM_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/extern"   # for nlohmann/json.hpp
)

target_link_libraries(qrunch_qsim PRIVATE Threads::Threads)

# Symbol visibility and SIMD flags.
# Supports three compilers:
#   clang++ (GNU frontend, -mavx2 flags)  — preferred, used by build.ps1
#   clang-cl (MSVC frontend, /arch:AVX2)  — when MSVC=TRUE and Clang ID
#   cl.exe (MSVC)                          — NMake fallback
if(MSVC)
    # cl.exe or clang-cl: use MSVC-style flags
    target_compile_options(qrunch_qsim PRIVATE /O2 /arch:AVX2)
    target_compile_definitions(qrunch_qsim PRIVATE QRUNCH_QSIM_USE_AVX2)
else()
    # clang++ or GCC: use GCC-style flags
    target_compile_options(qrunch_qsim PRIVATE -fvisibility=hidden -O3)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-mavx2" COMPILER_HAS_AVX2)
    if(COMPILER_HAS_AVX2)
        target_compile_options(qrunch_qsim PRIVATE -mavx2)
        target_compile_definitions(qrunch_qsim PRIVATE QRUNCH_QSIM_USE_AVX2)
    else()
        check_cxx_compiler_flag("-msse4.1" COMPILER_HAS_SSE41)
        if(COMPILER_HAS_SSE41)
            target_compile_options(qrunch_qsim PRIVATE -msse4.1)
            target_compile_definitions(qrunch_qsim PRIVATE QRUNCH_QSIM_USE_SSE)
        endif()
    endif()
endif()

# ── Install ────────────────────────────────────────────────────────────────────

install(TARGETS qrunch_qsim
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin   # .dll on Windows
)
install(FILES include/qrunch_qsim.h DESTINATION include)

# ── Optional: auto-fetch nlohmann/json ────────────────────────────────────────

option(FETCH_JSON "Download nlohmann/json.hpp if not present" OFF)
if(FETCH_JSON)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
        DOWNLOAD_NO_EXTRACT TRUE
        DOWNLOAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/nlohmann"
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()
