#!/usr/bin/env python3
"""geosdisk.py IMAGE - make a .d64, .d71 or .d81 made by c1541 a GEOS disk, as GEOS's own "Convert it?" would.

GEOS asks "This is a NON-GEOS disk. Convert it?" for disks without its marker. Converting (SetGEOSDisk in the
GEOS 2.0 source, drv/drv1541.s) allocates an empty "border block" and writes its track/sector and the text
"GEOS format V1.0" into the disk header at offsets $AB and $AD. Seen byte for byte on a .d81 converted by
GEOS 128 2.0 in VICE: header 40/0 got 28 13 "GEOS format V1.0", block 40/19 became 00 FF, and the BAM of
track 40 lost that block. On a 1541 GEOS takes the first free block from track 19 on.
"""
import sys

def d64_off(t, s):
    spt = [21] * 17 + [19] * 7 + [18] * 6 + [17] * 5
    return (sum(spt[:t - 1]) + s) * 256

def d81_off(t, s):
    return ((t - 1) * 40 + s) * 256

def main(path):
    d = bytearray(open(path, "rb").read())
    if len(d) in (174848, 349696):                          # 1541, or a 1571's first side (same layout):
                                                            # header and BAM of tracks 1-35 in 18/0
        off, head = d64_off, d64_off(18, 0)
        nsec = lambda t: 21 if t <= 17 else 19 if t <= 24 else 18 if t <= 30 else 17
        tracks = list(range(19, 36)) + list(range(1, 18))   # (on a .d71 the border block stays on side 1)
        def bam(t): return head + 4 * t                      # free count, then 3 bitmap bytes
    elif len(d) == 819200:                                  # 1581: header 40/0, BAM 40/1 (1-40), 40/2 (41-80)
        off, head = d81_off, d81_off(40, 0)
        nsec = lambda t: 40
        tracks = [40] + list(range(41, 81)) + list(range(39, 0, -1))
        def bam(t): return d81_off(40, 1 if t <= 40 else 2) + 0x10 + 6 * ((t - 1) % 40)   # count + 5 bytes
    else:
        sys.exit(f"{path}: not a .d64, .d71 or .d81")
    if d[head + 0xAD:head + 0xBD] == b"GEOS format V1.0":
        return print(f"{path}: already a GEOS disk")
    for t in tracks:                                        # the first free block (1581: 40/19 like GEOS did)
        first = 19 if (len(d) == 819200 and t == 40) else 0
        for s in range(first, nsec(t)):
            b = bam(t)
            if d[b + 1 + s // 8] & (1 << (s % 8)):          # free
                d[b + 1 + s // 8] &= ~(1 << (s % 8)) & 0xFF
                d[b] -= 1
                d[off(t, s):off(t, s) + 256] = b"\0\xff" + bytes(254)
                d[head + 0xAB:head + 0xAD] = bytes([t, s])
                d[head + 0xAD:head + 0xBD] = b"GEOS format V1.0"
                open(path, "wb").write(d)
                return print(f"{path}: GEOS disk (border block {t}/{s})")
    sys.exit(f"{path}: no free block")

if __name__ == "__main__":
    main(sys.argv[1])
