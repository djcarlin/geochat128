#!/bin/sh
# mkboot.sh GEOS128.D64 geochat128-boot.cvt mouse|joystick OUT.d64
#
# Makes a GEOS 128 disk that boots straight into geoChat 128 (no deskTop), from YOUR OWN GEOS 128 2.0 system disk
# image: geoChat 128 is not allowed to distribute GEOS itself. Needs VICE's c1541 and Python 3 (no cc65).
#   - geochat128-boot.cvt is the build of geoChat 128 that takes the deskTop's place ("128 DESKTOP"): GEOS starts it at
#     the end of booting. It loads the first 128 input driver on the disk itself (the deskTop normally does).
#   - mouse:    keep "128 comm 1351" (1351 mouse, port 1), remove the joystick driver
#     joystick: keep "128 joystick" (port 1), remove the 1351 drivers
#   - then a C128 boot sector is added: with the 40/80 key down, switching on (or reset) boots it.
# On an Ultimate, put OUT.d64 on the SD card / USB stick (so saved settings survive) and mount it on drive 8.
set -eu
[ $# -eq 4 ] || { echo "usage: $0 GEOS128.D64 geochat128-boot.cvt mouse|joystick OUT.d64" >&2; exit 2; }
GEOS=$1; CVT=$2; INPUT=$3; OUT=$4
case "$INPUT" in mouse|joystick) ;; *) echo "$0: input must be mouse or joystick" >&2; exit 2;; esac
command -v c1541 >/dev/null || { echo "$0: c1541 (from VICE) not found" >&2; exit 2; }
cp "$GEOS" "$OUT"
c1541 -attach "$OUT" -delete "128 desktop" -geoswrite "$CVT" >/dev/null
if [ "$INPUT" = mouse ]; then
    c1541 -attach "$OUT" -delete "128 joystick" >/dev/null
else
    c1541 -attach "$OUT" -delete "128 comm*" >/dev/null
fi
python3 "$(dirname "$0")/c128boot.py" "$OUT" "geoChat 128" 'RUN"GEOS128"'
echo "made $OUT ($INPUT)"
