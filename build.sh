#!/bin/sh
# Rebuild both binaries with mingw-w64 + MinHook.
# Works on Linux (cross) or in an msys2 mingw64 shell on Windows.
#
#   vfscore.dll            - the redirector
#   steam_api64.proxy.dll  - forwarder proxy (imports vfscore, forwards to
#                            steam_api64_orig) built from steam_api64.def
#
# To retarget the proxy to a different game whose steam_api64 has different
# exports, regenerate steam_api64.def from that DLL first:
#
#   python - <<'PY'
#   import pefile
#   p=pefile.PE("steam_api64.dll",fast_load=True)
#   p.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
#   print("EXPORTS")
#   for s in p.DIRECTORY_ENTRY_EXPORT.symbols:
#       n=s.name.decode() if s.name else None
#       print(("  %s=steam_api64_orig.%s @%d"%(n,n,s.ordinal)) if n
#             else ("  steam_api64_orig.#%d @%d NONAME"%(s.ordinal,s.ordinal)))
#   PY
#
set -e
cd "$(dirname "$0")"

command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 || {
    echo "need x86_64-w64-mingw32-gcc (Debian/Ubuntu: apt install mingw-w64)" >&2; exit 1; }

[ -d minhook ] || git clone --depth 1 https://github.com/TsudaKageyu/minhook

MH="minhook/src/hook.c minhook/src/buffer.c minhook/src/trampoline.c minhook/src/hde/hde64.c -Iminhook/include"

# 1) vfscore.dll
x86_64-w64-mingw32-gcc -O2 -shared -s -static-libgcc -w -o vfscore.dll vfscore.c $MH
echo "built vfscore.dll"

# 2) steam_api64.proxy.dll  (needs an import lib for vfscore!VfsInit)
printf 'EXPORTS\n  VfsInit\n' > vfscore.vfsdef
x86_64-w64-mingw32-dlltool -d vfscore.vfsdef -l libvfscore.a -D vfscore.dll
x86_64-w64-mingw32-gcc -O2 -shared -s -static-libgcc -w -Wl,--exclude-all-symbols \
    -o steam_api64.proxy.dll proxy.c steam_api64.def -L. -lvfscore
rm -f vfscore.vfsdef libvfscore.a
echo "built steam_api64.proxy.dll"
