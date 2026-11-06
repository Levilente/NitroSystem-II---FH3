#!/usr/bin/env bash
# Builds dist/NitroSystemII.dll with an llvm-mingw clang++ (x86_64-w64-mingw32-clang++ on PATH).
set -e
cd "$(dirname "$0")/.."
mkdir -p dist
x86_64-w64-mingw32-clang++ -std=c++20 -O2 -shared -fms-extensions -fno-exceptions -fno-rtti -Wno-unknown-pragmas \
  -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE '-DFH3MOD_PLATFORM_VERSION="1.0.7"' \
  -Iinclude -Isrc src/nitro_mod.cpp -luser32 -lkernel32 -lgdi32 -lwinmm -static -o dist/NitroSystemII.dll
echo "BUILD COMPLETE: dist/NitroSystemII.dll"
