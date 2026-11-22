#!/bin/sh
# vtest.sh APP.cvt SHOT.png - run a GEOS 128 app headless in VICE x128 (80 columns) and report its result.
# The app must be a test build named "128 DESKTOP" (src/res-test.grc) so GEOS starts it after booting.
# Exit/result: 0 PASS, 1-255 FAIL n (from the app's write to $D7FF), 124 TIMEOUT, 2 ERROR.
set -u
APP=$1; SHOT=$2
GEOS128=${GEOS128:-GEOS128.D64}
CYCLES=${CYCLES:-900000000}
[ -f "$APP" ] && [ -f "$GEOS128" ] || { echo "ERROR missing $APP or $GEOS128"; exit 2; }
DISK=build/test.d64; LOG=build/vtest.log
cp "$GEOS128" "$DISK"
c1541 -attach "$DISK" -delete "128 desktop" -geoswrite "$APP" >/dev/null 2>&1 || { echo "ERROR c1541"; exit 2; }
# BOOT=1: give the disk a C128 boot sector and just attach it (no autostart), so the C128 must boot by itself
START="-autostart $DISK"
if [ "${BOOT:-0}" = 1 ]; then
    python3 "$(dirname "$0")/c128boot.py" "$DISK" "geoChat 128" 'RUN"GEOS128"' >/dev/null || { echo "ERROR c128boot"; exit 2; }
    START="-8 $DISK"
fi
rm -f "$SHOT"
x128 -default -80col -minimized -silent -sounddev dummy -warp -debugcart -limitcycles "$CYCLES" \
     -exitscreenshot "$PWD/$SHOT" $START > "$LOG" 2>&1
CODE=$(sed -n 's/.*DBGCART: exit(\([0-9]*\)).*/\1/p' "$LOG" | tail -1)
if [ -z "$CODE" ]; then echo "TIMEOUT (no result; screenshot $SHOT)"; exit 124; fi
if [ "$CODE" = 0 ]; then echo "PASS (screenshot $SHOT)"; else echo "FAIL $CODE (screenshot $SHOT)"; fi
exit "$CODE"
