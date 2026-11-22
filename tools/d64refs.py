#!/usr/bin/env python3
"""d64refs.py IMAGE T S - list everything on a 1541 .d64 (GEOS-aware) that points at track T sector S."""
import sys
SPT = [21]*17 + [19]*7 + [18]*6 + [17]*5

def off(t, s):
    return (sum(SPT[:t-1]) + s) * 256

def chain(d, t, s):
    seen = []
    while t and len(seen) < 700:
        seen.append((t, s))
        b = d[off(t, s):off(t, s)+2]
        t, s = b[0], b[1]
    return seen

def refs(d, tt, ss):
    out = []
    bam = d[off(18, 0):off(18, 0)+256]
    if (bam[0xAB], bam[0xAC]) == (tt, ss): out.append("BAM: GEOS border block pointer")
    for (t, s) in chain(d, 18, 1):
        blk = d[off(t, s):off(t, s)+256]
        for e in range(8):
            ent = blk[e*32:e*32+32]
            if ent[2] == 0: continue
            name = ent[5:21].rstrip(b'\xa0').decode('latin1')
            st, hd, vlir = (ent[3], ent[4]), (ent[0x15], ent[0x16]), ent[0x17] == 1
            if st == (tt, ss): out.append(f"{name}: directory entry start")
            if hd == (tt, ss): out.append(f"{name}: directory entry GEOS header")
            if vlir:
                idx = d[off(*st):off(*st)+256]
                for r in range(127):
                    rt, rs = idx[2+2*r], idx[3+2*r]
                    if rt == 0: continue
                    if (rt, rs) == (tt, ss): out.append(f"{name}: VLIR index, record {r}")
                    for (a, b) in chain(d, rt, rs):
                        nxt = d[off(a, b):off(a, b)+2]
                        if (nxt[0], nxt[1]) == (tt, ss): out.append(f"{name}: record {r} link from {a}/{b}")
            else:
                for (a, b) in chain(d, *st):
                    nxt = d[off(a, b):off(a, b)+2]
                    if (nxt[0], nxt[1]) == (tt, ss): out.append(f"{name}: link from {a}/{b}")
    return out

if __name__ == "__main__":
    d = open(sys.argv[1], 'rb').read()
    for r in refs(d, int(sys.argv[2]), int(sys.argv[3])): print(r)
