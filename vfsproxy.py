#!/usr/bin/env python3
"""
vfsproxy.py  --  install GameIsolatorVFS into a game's bin folder.

Places the prebuilt, COMPILED steam_api64 proxy + vfscore.dll next to the game's
real steam_api64.dll:

  1. Rename the real   steam_api64.dll -> steam_api64_orig.dll   (once)
  2. Copy the prebuilt steam_api64.proxy.dll -> steam_api64.dll   (forwards every
     call to steam_api64_orig and static-imports vfscore.dll)
  3. Copy vfscore.dll alongside it

The proxy is a normal compiled DLL shipped in this repo; nothing is built on your
machine and no Python packages are needed. (An earlier version generated the PE in
Python -- that produced a binary the Windows loader rejected, so it was dropped.)

Usage (run from the game's bin folder, or pass it):
    python vfsproxy.py
    python vfsproxy.py "E:\\...\\<Install>\\bin\\Win64_Shipping_Client"

Idempotent: re-run any time (e.g. after Steam restores steam_api64.dll). If the game
ships a steam_api64 whose exports the prebuilt proxy does not cover, rebuild the
proxy from that DLL -- see build.sh / README.
"""
import os, sys, shutil

PROXY_SRC = "steam_api64.proxy.dll"   # prebuilt compiled proxy in this repo
CORE_SRC  = "vfscore.dll"


def read_hijack(ini):
    hijack = "steam_api64.dll"
    if os.path.isfile(ini):
        for ln in open(ini, encoding="utf-8", errors="replace"):
            s = ln.strip()
            if s.lower().startswith("hijack_dll") and "=" in s:
                v = s.split("=", 1)[1].split(";", 1)[0].split("#", 1)[0]
                hijack = v.strip() or hijack
    return hijack


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    bin_dir = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.getcwd()
    ini = os.path.join(bin_dir, "vfsproxy.ini")

    hijack = read_hijack(ini)
    if hijack.lower() != "steam_api64.dll":
        print("[!] This prebuilt proxy is for steam_api64.dll. For '%s' you must rebuild" % hijack)
        print("    the proxy from that DLL's exports (see build.sh / README).")
        return
    base = "steam_api64"
    orig = base + "_orig.dll"
    tgt       = os.path.join(bin_dir, "steam_api64.dll")
    orig_path = os.path.join(bin_dir, orig)

    proxy_src = os.path.join(here, PROXY_SRC)
    core_src  = os.path.join(here, CORE_SRC)
    for p in (proxy_src, core_src):
        if not os.path.isfile(p):
            sys.exit("missing %s next to this script" % os.path.basename(p))

    if not os.path.isfile(orig_path):
        if not os.path.isfile(tgt):
            sys.exit("steam_api64.dll not found in %s" % bin_dir)
        shutil.move(tgt, orig_path)
        print("[+] renamed steam_api64.dll -> %s" % orig)
    else:
        print("[=] %s already present; refreshing proxy + vfscore" % orig)

    shutil.copy2(proxy_src, tgt)
    print("[+] installed steam_api64.dll proxy")
    shutil.copy2(core_src, os.path.join(bin_dir, CORE_SRC))
    print("[+] installed vfscore.dll")

    if not os.path.isfile(ini):
        print("[!] no vfsproxy.ini in %s - copy vfsproxy.ini.example to vfsproxy.ini" % bin_dir)
        print("    and set your real->virtual path pairs, or nothing is redirected.")
    print("[OK] done. Launch the game and check vfsproxy.log for the 'attach ... hooks enabled' lines.")


if __name__ == "__main__":
    main()
