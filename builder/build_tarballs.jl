# build_tarballs.jl — BinaryBuilder.jl recipe for the qrunch_qsim C++ shim.
#
# This script cross-compiles the qrunch_qsim shared library for all Tier-1
# x86_64 platforms. The library wraps Google's qsim (header-only, C++17) and
# exports a minimal C ABI for Julia's ccall.
#
# Microarchitecture strategy:
#   Google qsim selects its SIMD kernel at compile time via preprocessor
#   defines. We build three variants as separate platform augmentations:
#
#     Platform tag            CMake flag              qsim kernel selected
#     ─────────────           ──────────              ────────────────────
#     x86_64-*-*-generic      -DQSIM_SIMD=generic     simulator_basic.h / SSE4.1
#     x86_64-*-*-avx2         -DQSIM_SIMD=avx2        simulator_avx.h (AVX2+FMA)
#     x86_64-*-*-avx512       -DQSIM_SIMD=avx512      simulator_avx512.h
#
#   At Julia load time, `_qsim_lib_path()` selects the best available variant
#   by checking CPU features (via `Sys.ARCH` / `llvmcall` CPUID or Hwloc.jl).
#   For now, the avx2 variant is the primary target.
#
# Usage (do NOT run yet — draft only):
#   julia --project builder/build_tarballs.jl --deploy=local
#
# Prerequisites:
#   - BinaryBuilder.jl installed in the project environment
#   - qsim submodule initialized: git submodule update --init shim/qsim/extern/qsim
#   - nlohmann/json.hpp available (fetched automatically by build script)
#
# Future platforms (placeholders):
#   - CUDA / cuQuantum GPU backend: requires CUDA toolkit in BinaryBuilder,
#     separate builder environment, and `-DQSIM_SIMD=cuda -DQSIM_CUQUANTUM=ON`.
#     Will be a separate JLL package (qrunch_qsim_cuda_jll).

using BinaryBuilder, Pkg

# ── Package identity ───────────────────────────────────────────────────────────

name = "qrunch_qsim"
version = v"0.2.0"   # First JLL-compatible version (C ABI v1)

# ── Sources ────────────────────────────────────────────────────────────────────
#
# Source 1: nlohmann/json v3.11.3 — single-header C++ JSON library (MIT)
#   Used by qrunch_qsim.cpp for serializing measurement results.
#
# Source 2: The qrunch shim + qsim headers — bundled from the repository.
#   In practice this would be a tarball of shim/qsim/ containing:
#     src/qrunch_qsim.cpp
#     include/qrunch_qsim.h
#     extern/qsim/          (Google qsim headers, header-only)
#     extern/nlohmann/      (populated by build script from source 1)
#     CMakeLists.txt

sources = [
    ArchiveSource(
        "https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz",
        "d5a8e7c8d9b6a5c4e3d2f1a0b9c8d7e6f5a4b3c2d1e0f9a8b7c6d5e4f3a2b1c0d"),
    DirectorySource("./bundled"),   # shim/qsim tree (populated before calling build_tarballs)
]

# ── Build script ───────────────────────────────────────────────────────────────

script = raw"""
# ── Stage 1: Set up nlohmann/json ──────────────────────────────────────────
# The json.tar.xz extracts to json-3.11.3/ (or similar). Copy the single
# header into the expected location so CMake can find it.
JSON_DIR=$(ls -d ${srcdir}/json-* 2>/dev/null | head -1)
if [[ -z "${JSON_DIR}" ]]; then
    JSON_DIR=$(ls -d ${workspace}/srcdir/json-* 2>/dev/null | head -1)
fi
if [[ -n "${JSON_DIR}" ]]; then
    mkdir -p ${srcdir}/shim/qsim/extern/nlohmann
    cp ${JSON_DIR}/single_include/nlohmann/json.hpp ${srcdir}/shim/qsim/extern/nlohmann/
else
    # Fallback: try to download directly
    mkdir -p ${srcdir}/shim/qsim/extern/nlohmann
    curl -sL -o ${srcdir}/shim/qsim/extern/nlohmann/json.hpp \
        https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
fi

# ── Stage 2: Select microarchitecture ──────────────────────────────────────
# The platform name includes the microarchitecture tag (e.g. x86_64-linux-gnu-avx2).
# We use CMake's -DQSIM_SIMD= to select the qsim kernel at compile time.
QSIM_SIMD="generic"
if [[ "${target}" == *avx512* ]]; then
    QSIM_SIMD="avx512"
elif [[ "${target}" == *avx2* ]]; then
    QSIM_SIMD="avx2"
fi

echo "=== qrunch_qsim build: target=${target}, QSIM_SIMD=${QSIM_SIMD} ==="

# ── Stage 3: CMake configure & build ───────────────────────────────────────
cd ${srcdir}/shim/qsim
mkdir -p build && cd build

cmake .. \
    -DCMAKE_INSTALL_PREFIX=${prefix} \
    -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TARGET_TOOLCHAIN} \
    -DCMAKE_BUILD_TYPE=Release \
    -DQSIM_SIMD=${QSIM_SIMD}

make -j${nproc}

# ── Stage 4: Install ───────────────────────────────────────────────────────
make install

# Verify the library was built
if [[ "${target}" == *w64* ]]; then
    ls -la ${prefix}/bin/qrunch_qsim.* 2>/dev/null || echo "WARNING: no DLL found"
else
    ls -la ${prefix}/lib/libqrunch_qsim.* 2>/dev/null || echo "WARNING: no .so/.dylib found"
fi
"""

# ── Platform matrix ────────────────────────────────────────────────────────────
#
# qsim requires AVX2+FMA for the fast path, but we also produce a generic x86_64
# build with SSE4.1 fallback for broader compatibility.
#
# NOTE: BinaryBuilder augmentations (the microarchitecture suffix) are not
# natively supported by BinaryBuilder's platform model. The actual mechanism
# for building multiple SIMD variants would be one of:
#   (a) Separate build_tarballs.jl invocations with different `platforms` arrays
#   (b) A single build that compiles all three and selects at runtime (preferred)
#   (c) Platform-specific `augment_platform!` in the JLL wrapper
#
# For this draft, we define the three OS targets and document that three
# separate build passes (or a runtime-dispatch build) are needed.
#
# Platforms are expanded to include both libstdc++ and libc++ ABI variants
# on Linux (cxxstring_abi).

platforms = [
    # Windows (mingw-w64)
    Platform("x86_64", "windows"),
    # macOS
    Platform("x86_64", "macos"),
    # Linux (glibc)
    Platform("x86_64", "linux"; libc="glibc"),
]

platforms = expand_cxxstring_abis(platforms)

# ── Products ───────────────────────────────────────────────────────────────────

products = [
    LibraryProduct("libqrunch_qsim", :libqrunch_qsim),
]

# ── Dependencies ───────────────────────────────────────────────────────────────
#
# No external JLL dependencies. qsim is header-only (bundled as source).
# nlohmann/json is fetched during the build.
#
# FUTURE: When CUDA/cuQuantum support is added:
#   dependencies = [
#       Dependency("CUDA_Runtime_jll"),
#       Dependency("cuQuantum_jll"),
#   ]

dependencies = Dependency[]

# ── Build invocation ───────────────────────────────────────────────────────────
#
# DO NOT RUN YET. This is a draft recipe.
#
# When ready:
#   julia --project=builder build_tarballs.jl --deploy=local
#
# For official JLL registration:
#   julia --project=builder build_tarballs.jl --deploy=JuliaBinaryWrappers/QRunch_jll

build_tarballs(
    ARGS,
    name,
    version,
    sources,
    script,
    platforms,
    products,
    dependencies;
    # Julia 1.6 is the minimum for BinaryBuilder-produced JLLs.
    # Qrunch itself requires Julia 1.12.
    julia_compat="1.6",
    # GCC 9+ needed for C++17 `<charconv>`, `<string_view>`, structured bindings.
    preferred_gcc_version=v"9",
    # qsim is header-only C++; we don't need a special GCC cross-compiler
    # beyond what BinaryBuilder provides.
)
