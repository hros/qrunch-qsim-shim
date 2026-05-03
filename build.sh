#!/usr/bin/env bash
# Quick local build script. On Windows use build.ps1 instead.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Fetch qsim submodule if missing
if [ ! -f extern/qsim/lib/circuit.h ]; then
    git submodule update --init extern/qsim
fi

# Fetch nlohmann/json if missing
if [ ! -f extern/nlohmann/json.hpp ]; then
    mkdir -p extern/nlohmann
    curl -fsSL -o extern/nlohmann/json.hpp \
        https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
fi

BUILD_DIR="${1:-build}"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --parallel

echo ""
echo "Built: $BUILD_DIR/libqrunch_qsim.so (or .dylib)"
echo "Set in Julia: ENV[\"QRUNCH_QSIM_LIB\"] = \"$(pwd)/$BUILD_DIR/libqrunch_qsim.so\""
