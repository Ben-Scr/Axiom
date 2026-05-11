#!/usr/bin/env bash
# ============================================================================
# SetupDawn.sh  ---  Linux/macOS counterpart to SetupDawn.bat.
#
# See SetupDawn.bat for full prose. Builds Dawn's monolithic webgpu_dawn
# static library under External/dawn/build/. Run once; subsequent engine
# builds link the result.
# ============================================================================
set -euo pipefail

cd "$(dirname "$0")/.."

DAWN_DIR="External/dawn"
DAWN_BUILD_DIR="${DAWN_DIR}/build"

echo "============================================"
echo "  Dawn WebGPU Setup"
echo "============================================"
echo "Target: ${DAWN_DIR}"
echo

# --- Tool checks -------------------------------------------------------------
for tool in git cmake python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[ERROR] $tool is not on PATH."
        exit 1
    fi
done

# --- Clone -------------------------------------------------------------------
if [[ ! -f "${DAWN_DIR}/CMakeLists.txt" ]]; then
    echo "[1/3] Cloning Dawn (this may take a few minutes)..."
    git clone --depth=1 https://dawn.googlesource.com/dawn "${DAWN_DIR}"
else
    echo "[1/3] Dawn already present at ${DAWN_DIR} (skipping clone)."
fi

# --- Configure ---------------------------------------------------------------
echo "[2/3] Configuring Dawn via CMake..."
cmake -S "${DAWN_DIR}" -B "${DAWN_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DDAWN_FETCH_DEPENDENCIES=ON \
    -DDAWN_BUILD_MONOLITHIC_LIBRARY=ON \
    -DDAWN_ENABLE_INSTALL=OFF \
    -DDAWN_BUILD_SAMPLES=OFF \
    -DTINT_BUILD_TESTS=OFF \
    -DTINT_BUILD_CMD_TOOLS=OFF

# --- Build -------------------------------------------------------------------
echo "[3/3] Building webgpu_dawn..."
cmake --build "${DAWN_BUILD_DIR}" --target webgpu_dawn -j"$(getconf _NPROCESSORS_ONLN || echo 4)"

echo
echo "============================================"
echo "  Dawn build complete."
echo "============================================"
echo "Headers : ${DAWN_DIR}/include"
echo "Headers : ${DAWN_BUILD_DIR}/gen/include"
echo "Library : ${DAWN_BUILD_DIR}/src/dawn/native/libwebgpu_dawn.a"
echo
echo "Next: regenerate the engine project files with --rhi=webgpu, e.g."
echo "  premake5 gmake2 --rhi=webgpu"
echo
