#!/usr/bin/env python3
"""
vfsproxy.py  --  generate a forwarding proxy DLL for any version of a hijack DLL
(default steam_api64.dll) so that vfscore.dll gets loaded into the game, which
then redirects the real->virtual paths from vfsproxy.ini.

What it does, in the game's bin folder (where the hijack DLL and this script sit):
  1. If not already done, rename   steam_api64.dll -> steam_api64_orig.dll
  2. Read every export of steam_api64_orig.dll
  3. Write a new steam_api64.dll that forwards every export (by name AND ordinal)
     to steam_api64_orig, and static-imports vfscore.dll (so it loads early).
  4. Make sure vfscore.dll is present next to it.

The proxy is a pure data PE (no code, no entry point): just an export table of
forwarders + a one-symbol import of vfscore.dll. The real DLL does all the work;
vfscore does the redirect. Idempotent: re-running detects the existing proxy.

Usage:
    python vfsproxy.py                 # uses ./vfsproxy.ini, operates in .
    python vfsproxy.py <bin_dir>       # operate in another folder
Requires: pefile  (pip install pefile)
"""
import os, sys, struct, shutil

try:
    import pefile
except ImportError:
    sys.exit("pefile is required:  pip install pefile")

IMAGE_BASE = 0x180000000
SECT_RVA   = 0x1000
FILE_ALIGN = 0x200
SECT_ALIGN = 0x1000


def read_exports(path):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
    d = getattr(pe, 'DIRECTORY_ENTRY_EXPORT', None)
    if not d:
        raise RuntimeError("no export table in " + path)
    out = []
    for s in d.symbols:
        out.append((s.ordinal, s.name.decode() if s.name else None))
    base = d.struct.Base
    pe.close()
    return out, base


def is_vfs_proxy(path):
    """True if 'path' already imports vfscore.dll (i.e. it's our proxy)."""
    try:
        pe = pefile.PE(path, fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
        imps = getattr(pe, 'DIRECTORY_ENTRY_IMPORT', [])
        r = any(e.dll and e.dll.decode().lower() == 'vfscore.dll' for e in imps)
        pe.close()
        return r
    except Exception:
        return False


def build_proxy(exports, base, orig_stem, export_dll_name,
                import_dll="vfscore.dll", import_sym="VfsInit"):
    """Return bytes of a pure-forwarder proxy PE64."""
    ords = [o for o, _ in exports]
    omin, omax = min(ords), max(ords)
    base = min(base, omin)
    eat_count = omax - base + 1
    named = sorted([(n, o) for o, n in exports if n], key=lambda x: x[0].encode())
    nnamed = len(named)

    # --- lay out the single section; offsets are relative to section start ---
    off = 0
    def alloc(n, al=1):
        nonlocal off
        if al > 1 and off % al:
            off += al - (off % al)
        r = off
        off += n
        return r

    o_expdir = alloc(40, 4)
    o_eat    = alloc(4 * eat_count, 4)
    o_enpt   = alloc(4 * nnamed, 4)
    o_ordtbl = alloc(2 * nnamed, 2)
    o_impdesc= alloc(20 * 2, 4)          # vfscore + null terminator
    o_ilt    = alloc(8 * 2, 8)
    o_iat    = alloc(8 * 2, 8)
    o_hint   = alloc(2 + len(import_sym) + 1, 2)

    # string blobs
    def addstr(s):
        return alloc(len(s) + 1)
    o_expdll = addstr(export_dll_name)
    o_impdll = addstr(import_dll)
    name_off = {}
    for n, _ in named:
        name_off[n] = addstr(n)
    fwd_off = {}
    for o, n in exports:
        tgt = ("%s.%s" % (orig_stem, n)) if n else ("%s.#%d" % (orig_stem, o))
        fwd_off[o] = addstr(tgt)

    sect_vsize = off
    rva = lambda x: SECT_RVA + x
    sect = bytearray(sect_vsize)

    def put(o, data):
        sect[o:o + len(data)] = data

    # export directory
    put(o_expdir, struct.pack('<IIHHIIIIII',
        0, 0, 0, 0,
        rva(o_expdll), base, eat_count, nnamed,
        rva(o_eat), rva(o_enpt)) + struct.pack('<I', rva(o_ordtbl)))
    # EAT (forwarder RVAs; 0 for gaps)
    for o, n in exports:
        put(o_eat + 4 * (o - base), struct.pack('<I', rva(fwd_off[o])))
    # ENPT + ordinal table (names sorted; ordinal index = ord - base)
    for i, (n, o) in enumerate(named):
        put(o_enpt + 4 * i, struct.pack('<I', rva(name_off[n])))
        put(o_ordtbl + 2 * i, struct.pack('<H', o - base))

    # import descriptor for vfscore.dll
    put(o_impdesc, struct.pack('<IIIII', rva(o_ilt), 0, 0, rva(o_impdll), rva(o_iat)))
    # ILT / IAT: one import-by-name thunk -> hint/name, then null
    put(o_ilt, struct.pack('<Q', rva(o_hint)))
    put(o_iat, struct.pack('<Q', rva(o_hint)))
    put(o_hint, struct.pack('<H', 0) + import_sym.encode() + b'\x00')

    # strings
    put(o_expdll, export_dll_name.encode() + b'\x00')
    put(o_impdll, import_dll.encode() + b'\x00')
    for n, _ in named:
        put(name_off[n], n.encode() + b'\x00')
    for o, n in exports:
        tgt = ("%s.%s" % (orig_stem, n)) if n else ("%s.#%d" % (orig_stem, o))
        put(fwd_off[o], tgt.encode() + b'\x00')

    raw_size = (sect_vsize + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1)
    sect += b'\x00' * (raw_size - sect_vsize)
    size_of_image = SECT_RVA + ((sect_vsize + SECT_ALIGN - 1) & ~(SECT_ALIGN - 1))

    # ---- headers ----
    IMPORT_RVA, IMPORT_SZ = rva(o_impdesc), 40
    EXPORT_RVA, EXPORT_SZ = rva(o_expdir), sect_vsize - o_expdir  # generous
    IAT_RVA, IAT_SZ = rva(o_iat), 16

    opt = struct.pack('<HBBIIIII',
        0x20B, 0, 0, 0, raw_size, 0, 0, 0)                  # magic..BaseOfCode (PE32+: no BaseOfData)
    opt += struct.pack('<Q', IMAGE_BASE)
    opt += struct.pack('<IIHHHHHHIIIIHH',
        SECT_ALIGN, FILE_ALIGN, 6, 0, 0, 0, 6, 0, 0,
        size_of_image, FILE_ALIGN, 0, 2, 0x100)             # subsys=GUI, dllchar=NX
    opt += struct.pack('<QQQQ', 0x100000, 0x1000, 0x100000, 0x1000)
    opt += struct.pack('<II', 0, 16)                        # LoaderFlags, NumRvaAndSizes
    dirs = [(0, 0)] * 16
    dirs[0] = (EXPORT_RVA, EXPORT_SZ)
    dirs[1] = (IMPORT_RVA, IMPORT_SZ)
    dirs[12] = (IAT_RVA, IAT_SZ)
    for r, s in dirs:
        opt += struct.pack('<II', r, s)

    # COFF: relocs stripped, executable image, large-address-aware, DLL
    coff = struct.pack('<HHIIIHH', 0x8664, 1, 0, 0, 0, len(opt),
                       0x0001 | 0x0002 | 0x0020 | 0x2000)
    sechdr = struct.pack('<8sIIIIIIHHI', b'.rdata', sect_vsize, SECT_RVA,
                         raw_size, FILE_ALIGN, 0, 0, 0, 0, 0xC0000040)  # init|read|write

    pe_at = 0x40
    hdr = bytearray(FILE_ALIGN)
    hdr[0:2] = b'MZ'
    struct.pack_into('<I', hdr, 0x3C, pe_at)
    off2 = pe_at
    hdr[off2:off2 + 4] = b'PE\x00\x00'; off2 += 4
    hdr[off2:off2 + len(coff)] = coff; off2 += len(coff)
    hdr[off2:off2 + len(opt)] = opt; off2 += len(opt)
    hdr[off2:off2 + len(sechdr)] = sechdr; off2 += len(sechdr)

    return bytes(hdr) + bytes(sect)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    bin_dir = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.getcwd()
    ini = os.path.join(bin_dir, "vfsproxy.ini")
    hijack = "steam_api64.dll"
    if os.path.isfile(ini):
        for ln in open(ini, encoding="utf-8", errors="replace"):
            s = ln.strip()
            if s.lower().startswith("hijack_dll"):
                hijack = s.split("=", 1)[1].strip()
    base_name = hijack[:-4] if hijack.lower().endswith(".dll") else hijack
    orig = base_name + "_orig.dll"
    tgt = os.path.join(bin_dir, hijack)
    orig_path = os.path.join(bin_dir, orig)

    if not os.path.isfile(tgt) and not os.path.isfile(orig_path):
        sys.exit("hijack DLL not found: %s" % tgt)

    # rename real -> _orig once; skip if proxy already in place
    if os.path.isfile(tgt) and is_vfs_proxy(tgt):
        print("[=] %s is already a vfs proxy; rebuilding from %s" % (hijack, orig))
    elif not os.path.isfile(orig_path):
        shutil.move(tgt, orig_path)
        print("[+] renamed %s -> %s" % (hijack, orig))

    exports, base = read_exports(orig_path)
    data = build_proxy(exports, base, base_name + "_orig", hijack)
    with open(tgt, "wb") as f:
        f.write(data)
    print("[+] wrote %s : %d exports forwarded to %s" % (hijack, len(exports), orig))

    # ship vfscore.dll next to it
    src_core = os.path.join(here, "vfscore.dll")
    dst_core = os.path.join(bin_dir, "vfscore.dll")
    if os.path.isfile(src_core) and os.path.abspath(src_core) != os.path.abspath(dst_core):
        shutil.copy2(src_core, dst_core)
        print("[+] copied vfscore.dll into %s" % bin_dir)
    elif not os.path.isfile(dst_core):
        print("[!] vfscore.dll not found next to this script; copy it into %s yourself" % bin_dir)

    if not os.path.isfile(ini):
        print("[!] no vfsproxy.ini in %s - create one (see README) or nothing is redirected" % bin_dir)
    print("[OK] done.")


if __name__ == "__main__":
    main()
