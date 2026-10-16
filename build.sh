#!/bin/sh
# Build the single self-contained steam_api64.vfs.dll with mingw-w64 + MinHook.
# Works on Linux (cross) or in an msys2 mingw64 shell on Windows.
#
# Output: steam_api64.vfs.dll
#   - forwards every Steamworks export (steam_api64.def) to steam_api64_orig.dll
#   - on load, hooks the Win32 file APIs and remaps real->virtual paths (vfscore.c)
#   - depends only on kernel32 + msvcrt
#
# To retarget the forwarders to a game whose steam_api64 exports differ, regenerate
# steam_api64.def from that DLL first (needs python + pefile):
#
#   python - "PATH\to\steam_api64.dll" <<'PY'
#   import pefile,sys
#   p=pefile.PE(sys.argv[1],fast_load=True)
#   p.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
#   print("EXPORTS")
#   for s in p.DIRECTORY_ENTRY_EXPORT.symbols:
#       n=s.name.decode() if s.name else None
#       print(("  %s=steam_api64_orig.%s @%d"%(n,n,s.ordinal)) if n
#             else ("  steam_api64_orig.#%d @%d NONAME"%(s.ordinal,s.ordinal)))
#   PY
#
# The shipped steam_api64.def already carries a comprehensive, recent Steamworks
# export set, which covers current games without regenerating.
set -e
cd "$(dirname "$0")"

command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 || {
    echo "need x86_64-w64-mingw32-gcc (Debian/Ubuntu: apt install mingw-w64)" >&2; exit 1; }

[ -d minhook ] || git clone --depth 1 https://github.com/TsudaKageyu/minhook

x86_64-w64-mingw32-gcc -O2 -shared -s -static-libgcc -w -Wl,--exclude-all-symbols \
    -o steam_api64.vfs.dll \
    vfscore.c \
    minhook/src/hook.c minhook/src/buffer.c minhook/src/trampoline.c minhook/src/hde/hde64.c \
    -Iminhook/include \
    steam_api64.def

echo "built steam_api64.vfs.dll"
