#!/usr/bin/env python3
"""seticon.py FILE.cvt - put geoChat 128's deskTop icon into a GEOS convert (.cvt) file.

A .cvt file is the directory entry + signature (254 bytes), then the GEOS header block without its 2 link bytes.
The header starts with the icon: width in bytes (3), height (21), $BF (= $80 + 63: 63 bytes follow uncompressed),
then the 24x21 picture, 3 bytes per row, bit 7 = leftmost pixel (Hitchhiker's Guide to GEOS, "File Header Block";
seen in grc65's output: 03 15 bf + a default empty box).
"""
import sys

ICON = """
.######################.
#......................#
#......................#
#......................#
#..####..#####...####..#
#...##...##..##.##..##.#
#...##...##..##.##.....#
#...##...#####..##.....#
#...##...##.##..##.....#
#...##...##..##.##..##.#
#..####..##..##..####..#
#......................#
#......................#
#......................#
.####..################.
....#..#................
....#.#.................
....##..................
....#...................
........................
........................
""".split()

assert len(ICON) == 21 and all(len(r) == 24 for r in ICON), "icon must be 24x21"
pic = bytes(int(r[i:i + 8].replace("#", "1").replace(".", "0"), 2) for r in ICON for i in (0, 8, 16))

path = sys.argv[1]
d = bytearray(open(path, "rb").read())
h = 254                                         # the header block in the .cvt
if d[30:58] != b"PRG formatted GEOS file V1.0" or d[h:h + 3] != b"\x03\x15\xbf":
    sys.exit(f"{path}: not a GEOS .cvt with an uncompressed 24x21 icon")
d[h + 3:h + 3 + 63] = pic
open(path, "wb").write(d)
