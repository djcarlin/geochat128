# target

The built program, the same files as in the latest [release](https://github.com/djcarlin/geochat128/releases/latest).
`make release` updates them.

| File | What |
|---|---|
| `geochat128.d81` | disk image for a 1581 (any Ultimate can mount it) |
| `geochat128.d71` | disk image for a 1571 |
| `geochat128.d64` | disk image for a 1541 (also works in a 1571) |
| `geochat128.cvt` | the program alone, as a GEOS convert file, to copy onto a GEOS disk of your own |
| `geochat128-boot.cvt` | the boot-disk version, only for `tools/mkboot.sh` (see below) |

The disk images are GEOS disks: open them from the GEOS 128 deskTop, no "Convert it?" question. They contain
only geoChat 128, no GEOS.

**`geochat128-boot.cvt` is not for the deskTop.** It is named "128 DESKTOP" so that GEOS starts it in place of the
deskTop at the end of booting, and it loads the mouse or joystick driver itself (normally the deskTop does that).
Don't copy it onto an ordinary GEOS disk: it has the same name as that disk's deskTop. Use it only through `tools/mkboot.sh`,
which makes a separate boot disk from your own GEOS 128 2.0 system disk image (see the main README).
