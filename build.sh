#!/bin/sh
# Rebuild vfscore.dll (64-bit) with mingw-w64 + MinHook.
# Works on Linux (cross) or in an msys2 mingw64 shell on Windows.
set -e
cd "$(dirname "$0")"

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "need x86_64-w64-mingw32-gcc (Debian/Ubuntu: apt install mingw-w64)" >&2
    exit 1
fi

if [ ! -d minhook ]; then
    git clone --depth 1 https://github.com/TsudaKageyu/minhook
fi

x86_64-w64-mingw32-gcc -O2 -shared -s -static-libgcc -w -o vfscore.dll \
    vfscore.c \
    minhook/src/hook.c minhook/src/buffer.c minhook/src/trampoline.c \
    minhook/src/hde/hde64.c \
    -Iminhook/include

echo "built vfscore.dll"
