# tools

Everything here needs Python 3; the disk tools also need `c1541` from VICE, the test tools `x128`. None of them
need cc65.

## Making a boot disk (also in a release download)

| Tool | What |
|---|---|
| `mkboot.sh` | a GEOS 128 disk that starts straight into geoChat 128, made from your own GEOS 128 2.0 system disk |
| `c128boot.py` | adds a C128 boot sector to a .d64, so the C128 runs a BASIC command when switched on (used by `mkboot.sh`) |
| `d64refs.py` | lists what points at a disk block; `c128boot.py` uses it to move a file out of the boot sector's way |

```
./mkboot.sh GEOS128.D64 geochat128-boot.cvt mouse my-geochat128.d64      # or: joystick
python3 c128boot.py disk.d64 "geoChat 128" 'RUN"GEOS128"'
python3 d64refs.py disk.d64 1 0
```

## Building

| Tool | What |
|---|---|
| `seticon.py` | puts geoChat 128's deskTop icon into a built `.cvt` file (the Makefile runs it after every link) |
| `geosdisk.py` | makes a `.d64`, `.d71` or `.d81` from `c1541` a GEOS disk, so GEOS doesn't ask to convert it |

## Testing

| Tool | What |
|---|---|
| `vtest.py` | the self-test: boots a test build in VICE, plays the IRC server, drives the program and checks it |
| `vicemon.py` | a small client for VICE's binary monitor (used by `vtest.py`) |
| `vtest.sh` | runs a test build in VICE until it writes its result to `$D7FF` (used by `make boot`) |

Normally run through the Makefile: `make test` (twice: a 16 KB VDC; a 64 KB VDC with a 512 KB REU) and
`make screenshot`. `vtest.py` directly:

```
python3 tools/vtest.py [options] build/geochat128-test.cvt build/geochat128-test.map build/test.png
```

| Option | |
|---|---|
| `--vdc 16` / `--vdc 64` | VDC memory (default 64; 16 has no window cache) |
| `--reu 512` | an REU of that many KB (default none) |
| `--quiet` | print only the result |
| `--shot-at "STEP"` | stop with PASS when that step starts; the screenshot shows that moment |
| `--allow 91,97` | report these failure codes but keep going |
| `--skip about,settings` | leave out parts of the scenario (for narrowing down a failure) |
| `--profile "STEP"` | at that step, time a full redraw of every window (the per-part timings need a build with `-DPROFILE`) |
| `--showcase PNG` | instead of the test, stage the README's screenshot and save it as PNG |

The result is `PASS` or `FAIL n: what went wrong`, with the exit code to match; each check is a `Fail(n, ...)`
line in `vtest.py`, so searching for the number finds it. VICE's log is next to the screenshot
(`build/test.vice.log`). The GEOS 128 disk comes from `GEOS128` (the Makefile passes it on; see the main
README). VICE gets an empty keyboard map, so typing on this computer while its window has focus can't disturb a
test.
