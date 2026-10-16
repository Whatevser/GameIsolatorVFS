#!/bin/sh
# Build a steam_api64.vfs.dll that matches a SPECIFIC steam_api64.dll version.
#
# Point it at the game's real steam_api64.dll and it does everything:
#   1. reads that DLL's exports (gendef)
#   2. generates steam_api64.def  (every export forwarded to steam_api64_orig)
#   3. compiles vfscore.c + MinHook + that def  ->  steam_api64.vfs.dll
#
# Usage:
#   ./build.sh /path/to/steam_api64.dll        # the game's original DLL
#   ./build.sh                                 # looks for ./steam_api64_orig.dll
#                                              # or ./steam_api64.dll in this folder
#
# Needs mingw-w64 (the gcc cross-toolchain, which includes objdump) and gendef:
#   Debian/Ubuntu:  apt install mingw-w64 mingw-w64-tools
#   msys2 (on Win):  pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-tools-git
#
# The result is version-matched to that DLL. Steamworks is resolved by name, so the
# forwarders are name-based (ordinals need not match the original).
set -e
cd "$(dirname "$0")"

SRC="$1"
if [ -z "$SRC" ]; then
    if   [ -f steam_api64_orig.dll ]; then SRC=steam_api64_orig.dll
    elif [ -f steam_api64.dll ];      then SRC=steam_api64.dll
    else echo "usage: ./build.sh /path/to/steam_api64.dll" >&2; exit 1; fi
fi
[ -f "$SRC" ] || { echo "not found: $SRC" >&2; exit 1; }

command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 || {
    echo "need x86_64-w64-mingw32-gcc (apt install mingw-w64)" >&2; exit 1; }
command -v gendef >/dev/null 2>&1 || {
    echo "need gendef (apt install mingw-w64-tools)" >&2; exit 1; }

echo "[*] reading exports of $SRC"
gendef - "$SRC" > _raw.def 2>/dev/null

# turn each named export into a forwarder to steam_api64_orig.
# (Unnamed/ordinal-only exports can't be forwarded by name and mingw has no
#  ordinal-target forwarder syntax; Steamworks has none, so they're skipped.)
awk '
  BEGIN { print "EXPORTS" }
  /^EXPORTS/ { f=1; next }
  f && NF {
    if ($1 ~ /^ord_[0-9]+$/) { skipped++; next }
    print "  " $1 "=steam_api64_orig." $1
  }
  END { if (skipped) print "[!] skipped " skipped " unnamed (ordinal-only) export(s)" > "/dev/stderr" }
' _raw.def > steam_api64.def
rm -f _raw.def
echo "[*] generated steam_api64.def ($(grep -c '=' steam_api64.def) forwarders)"

[ -d minhook ] || git clone --depth 1 https://github.com/TsudaKageyu/minhook

x86_64-w64-mingw32-gcc -O2 -shared -s -static-libgcc -w -Wl,--exclude-all-symbols \
    -o steam_api64.vfs.dll \
    vfscore.c \
    minhook/src/hook.c minhook/src/buffer.c minhook/src/trampoline.c minhook/src/hde/hde64.c \
    -Iminhook/include \
    steam_api64.def

echo "[OK] built steam_api64.vfs.dll (matched to $SRC)"
