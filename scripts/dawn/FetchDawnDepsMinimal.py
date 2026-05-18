#!/usr/bin/env python3
"""
Trimmed wrapper around External/dawn/tools/fetch_dawn_dependencies.py.

Dawn's stock fetcher hard-codes an 18-submodule list (see
fetch_dawn_dependencies.py:98-118 in the Dawn tree). For Index we don't
need glfw3, googletest, or google_benchmark, so we re-use the same
DEPS-resolution machinery with a smaller required-submodules set.

Run BEFORE invoking CMake. SetupDawn.bat must omit
-DDAWN_FETCH_DEPENDENCIES=ON, otherwise CMake re-fetches the full list
and our trimming is wasted.
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DAWN_DIR = REPO_ROOT / "External" / "dawn"
TOOLS_DIR = DAWN_DIR / "tools"
FETCHER = TOOLS_DIR / "fetch_dawn_dependencies.py"

if not FETCHER.is_file():
    sys.exit(f"[ERROR] {FETCHER} not found. Clone Dawn first.")

sys.path.insert(0, str(TOOLS_DIR))
import fetch_dawn_dependencies as fdd  # noqa: E402

REQUIRED = [
    'third_party/abseil-cpp',
    'third_party/directx-shader-compiler/src',
    'third_party/directx-headers/src',
    'third_party/jinja2',
    'third_party/EGL-Registry/src',
    'third_party/OpenGL-Registry/src',
    'third_party/libprotobuf-mutator/src',
    'third_party/protobuf',
    'third_party/markupsafe',
    'third_party/glslang/src',
    'third_party/spirv-headers/src',
    'third_party/spirv-tools/src',
    'third_party/vulkan-headers/src',
    'third_party/vulkan-loader/src',
    'third_party/vulkan-utility-libraries/src',
    'third_party/webgpu-headers/src',
]


class _Args:
    directory = str(DAWN_DIR)
    git = "git"
    shallow = True


def main():
    print(f"[FetchDawnDepsMinimal] Fetching {len(REQUIRED)} of 18 Dawn deps "
          f"into {DAWN_DIR}")
    fdd.process_dir(_Args(), Path(_Args.directory).resolve(), REQUIRED)
    print("[FetchDawnDepsMinimal] Done.")


if __name__ == "__main__":
    main()
