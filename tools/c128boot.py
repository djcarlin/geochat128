#!/usr/bin/env python3
"""c128boot.py IMAGE TITLE COMMAND - make a 1541 .d64 auto-boot on a C128: e.g.  c128boot.py x.d64 "geoChat 128" 'RUN"GEOS128"'

Boot sector (track 1 sector 0), C128 Programmer's Reference Guide "boot sector": 'CBM', load address lo/hi,
bank, block count (all 0: nothing more to load), title, 0, file name (empty), 0, then code, run at $0B00.
The code is LDX/LDY + JMP $AFA5, which executes a BASIC command string (C128/c128-autobooting.txt, Listing
One, which points X/Y one byte before the string). If another file uses track 1 sector 0, that block is moved
to a free sector first and the one pointer to it (found by d64refs.py) is updated; the BAM is updated.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from d64refs import off, refs, SPT, chain

def bam_entry(d, t):
    return off(18, 0) + 4 + 4 * (t - 1)

def is_free(d, t, s):
    e = bam_entry(d, t)
    return d[e + 1 + s // 8] & (1 << (s % 8)) != 0

def allocate(d, t, s):
    e = bam_entry(d, t)
    d[e + 1 + s // 8] &= ~(1 << (s % 8)) & 0xFF
    d[e] -= 1

def free_sector(d):
    for t in list(range(17, 0, -1)) + list(range(19, 36)):     # near the directory first, like the 1541 DOS
        for s in range(SPT[t - 1]):
            if is_free(d, t, s) and (t, s) != (1, 0): return t, s
    sys.exit("no free sector")

def main():
    # the C128 prints the title in its uppercase/graphics mode, where lowercase ASCII letters are graphics
    # characters ("geoChat" came out as symbols): capitals only
    path, title, command = sys.argv[1], sys.argv[2].upper().encode('latin1'), sys.argv[3].encode('latin1')
    d = bytearray(open(path, 'rb').read())
    if d[off(1, 0):off(1, 0) + 3] == b'CBM':
        print("already a boot sector; rewriting it")
    elif not is_free(d, 1, 0):
        users = refs(d, 1, 0)
        print("1/0 in use:", users)
        if len(users) != 1 or " link from " not in users[0]:
            sys.exit("only a single chain link can be moved; not touching this disk")
        frm = users[0].rsplit(" ", 1)[1]
        ft, fs = (int(x) for x in frm.split("/"))
        nt, ns = free_sector(d)
        d[off(nt, ns):off(nt, ns) + 256] = d[off(1, 0):off(1, 0) + 256]
        allocate(d, nt, ns)
        d[off(ft, fs)], d[off(ft, fs) + 1] = nt, ns
        print(f"moved block 1/0 to {nt}/{ns}, link at {ft}/{fs} updated")
    else:
        allocate(d, 1, 0)
    title = title[:16]
    sa = 0x0B00 + 15 + len(title)               # Listing One: SA = 2831 + LEN(title), one byte before the string
    code = bytes([0xA2, sa & 0xFF, 0xA0, sa >> 8, 0x4C, 0xA5, 0xAF])   # LDX #<sa, LDY #>sa, JMP $AFA5
    boot = b'CBM' + bytes(4) + title + b'\0' + b'\0' + code + command + b'\0'
    assert 0x0B00 + 16 + len(title) == 0x0B00 + len(boot) - len(command) - 1   # string right after the code
    d[off(1, 0):off(1, 0) + 256] = boot + bytes(256 - len(boot))
    open(path, 'wb').write(d)
    print(f"boot sector: BOOTING {title.decode()}... then {command.decode()}")

main()
