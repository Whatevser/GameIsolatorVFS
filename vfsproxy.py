#!/usr/bin/env python3
"""
vfsproxy.py  --  install GameIsolatorVFS into a game's bin folder.

Single self-contained DLL, no dependencies to chase:

  1. Rename the real  steam_api64.dll -> steam_api64_orig.dll   (once)
  2. Copy the shipped steam_api64.vfs.dll -> steam_api64.dll

steam_api64.dll then forwards every Steamworks call to steam_api64_orig.dll
(Steam/playtime/overlay unchanged) and, on load, remaps the real->virtual paths
from vfsproxy.ini. It depends only on kernel32 + msvcrt.

Nothing is built or generated on your machine and no Python packages are needed
(you can just do the two file operations by hand). This replaces an earlier
two-DLL design whose dependency load failed on Windows (0x800701E7).

Usage (run from the game's bin folder, or pass it):
    python vfsproxy.py
    python vfsproxy.py "E:\\...\\<Install>\\bin\\Win64_Shipping_Client"

Idempotent: re-run any time (e.g. after Steam restores steam_api64.dll).
"""
import os, sys, shutil

SINGLE = "steam_api64.vfs.dll"   # the self-contained DLL shipped in this repo


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    bin_dir = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.getcwd()

    src = os.path.join(here, SINGLE)
    if not os.path.isfile(src):
        sys.exit("missing %s next to this script" % SINGLE)

    tgt       = os.path.join(bin_dir, "steam_api64.dll")
    orig_path = os.path.join(bin_dir, "steam_api64_orig.dll")

    if not os.path.isfile(orig_path):
        if not os.path.isfile(tgt):
            sys.exit("steam_api64.dll not found in %s" % bin_dir)
        shutil.move(tgt, orig_path)
        print("[+] renamed steam_api64.dll -> steam_api64_orig.dll")
    else:
        print("[=] steam_api64_orig.dll already present; refreshing the proxy")

    shutil.copy2(src, tgt)
    print("[+] installed self-contained steam_api64.dll")

    # clean up the old two-DLL leftover if present
    stale = os.path.join(bin_dir, "vfscore.dll")
    if os.path.isfile(stale):
        try:
            os.remove(stale)
            print("[+] removed obsolete vfscore.dll")
        except OSError:
            print("[!] could not remove old vfscore.dll - delete it manually")

    if not os.path.isfile(os.path.join(bin_dir, "vfsproxy.ini")):
        print("[!] no vfsproxy.ini in %s - copy vfsproxy.ini.example to vfsproxy.ini" % bin_dir)
        print("    and set your real->virtual path pairs, or nothing is redirected.")
    print("[OK] done. Launch the game and check vfsproxy.log for 'attach ... hooks enabled'.")


if __name__ == "__main__":
    main()
