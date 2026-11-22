# geoChat 128 - IRC client for GEOS 128, 80 columns.
#
#   make              release build: build/geochat128.cvt (GEOS convert file) + build/geochat128.d64
#   make NICK=x CHAN='#y'   ...with your own defaults
#   make test         self-test in VICE x128 (fake network): PASS/FAIL + build/test.png
#   make shot         like test but don't fail; just the screenshot
#   make clean

# GEOS 128 in 80 columns keeps its background screen at $6000-$7F3F and $A040-$BF7F (Hitchhiker's Guide).
# geoChat 128 draws to the foreground only and redraws itself through RecoverVector after menus and dialogs, so it
# uses that memory: see src/geochat128.cfg.
CL      = cl65 -t geos-cbm -O -C src/geochat128.cfg
# build-time defaults for the settings dialog: make NICK=yournick CHAN='#mychannel'
NICK   ?=
# (no default nick: every user sets their own in settings)
CHAN   ?= \#c64friends
# testing on real hardware: build the disks with CHAN='#yourtestchannel', so connect/quit tests don't disturb #c64friends
SERVER ?= irc.libera.chat
DEFS    = -DDEF_NICK='"$(NICK)"' -DDEF_CHAN='"$(CHAN)"' -DDEF_SERVER='"$(SERVER)"'
CORE    = src/main.c src/ui.c src/store.c src/settings.c src/irc.c src/fmt.c src/fastline.c src/hw.s src/fastdraw.s
# your own GEOS 128 2.0 system disk image (geoChat 128 doesn't include GEOS): make GEOS128=path/to/GEOS128.D64,
# or put the line "GEOS128 = path/to/GEOS128.D64" in local.mk (not in git)
-include local.mk
GEOS128 ?= GEOS128.D64
export GEOS128                              # tools/vtest.py and vtest.sh read it too

all: build/geochat128.cvt build/geochat128.d64 build/geochat128.d71 build/geochat128.d81

# a 1541 disk with the program, for real hardware (mount it next to a GEOS 128 boot disk)
# the same on a 1571 image (double-sided), for GEOS set up with a 1571 drive
build/geochat128.d71: build/geochat128.cvt
	rm -f $@
	c1541 -format "geochat 128,gc" d71 $@ -geoswrite build/geochat128.cvt >/dev/null
	python3 tools/geosdisk.py $@

# the same on a 1581 image, for GEOS set up with a 1581 drive
build/geochat128.d81: build/geochat128.cvt
	rm -f $@
	c1541 -format "geochat 128,gc" d81 $@ -geoswrite build/geochat128.cvt >/dev/null
	python3 tools/geosdisk.py $@

build/geochat128.d64: build/geochat128.cvt
	rm -f $@
	c1541 -format "geochat 128,gc" d64 $@ -geoswrite build/geochat128.cvt >/dev/null
	python3 tools/geosdisk.py $@

build:
	mkdir -p build

build/res.s: src/res.grc | build
	grc65 -t geos-cbm -s $@ $<
build/res-test.s: src/res-test.grc | build
	grc65 -t geos-cbm -s $@ $<

# release: the Ultimate network driver
build/geochat128.cvt: build/res.s $(CORE) src/net_uci.c src/*.h
	$(CL) $(DEFS) -m build/geochat128.map -o $@ build/res.s $(CORE) src/net_uci.c
	python3 tools/seticon.py $@

# the release program under the test header: in VICE (no Ultimate) it must report "No network interface"
build/geochat128-uci-test.cvt: build/res-test.s $(CORE) src/net_uci.c src/*.h
	$(CL) -DAUTOCONNECT -o $@ build/res-test.s $(CORE) src/net_uci.c
	python3 tools/seticon.py $@

# a GEOS 128 boot disk that starts geoChat 128 instead of the deskTop: mount it on drive 8 and reset (or switch on)
# with the 40/80 key down; it auto-boots. Quitting geoChat 128 restarts it.
build/geochat128-geos-test.cvt: build/res-test.s $(CORE) src/net_uci.c src/*.h
	$(CL) $(DEFS) -DLOAD_INPUT -m build/boot.map -o $@ build/res-test.s $(CORE) src/net_uci.c
	python3 tools/seticon.py $@
build/geochat128-boot.d64: build/geochat128-geos-test.cvt
	tools/mkboot.sh "$(GEOS128)" build/geochat128-geos-test.cvt mouse $@
build/geochat128-boot-joy.d64: build/geochat128-geos-test.cvt
	tools/mkboot.sh "$(GEOS128)" build/geochat128-geos-test.cvt joystick $@
boot-joy: build/geochat128-boot-joy.d64
# normal GEOS 128 (the real deskTop, the input driver it picks) with geoChat 128 on the disk: open it from the deskTop
build/geochat128-desk.d64: build/geochat128.cvt
	cp "$(GEOS128)" $@
	c1541 -attach $@ -geoswrite build/geochat128.cvt >/dev/null
	# joystick only (in port 1): without a mouse driver on the disk the deskTop keeps GEOS's joystick driver
	c1541 -attach $@ -delete "128 comm*" >/dev/null
	python3 tools/c128boot.py $@ "GEOS" 'RUN"GEOS128"'
desk: build/geochat128-desk.d64
boot: build/geochat128-boot.d64
	BOOT=1 CYCLES=250000000 tools/vtest.sh build/geochat128-geos-test.cvt build/boot.png || true

shot-uci: build/geochat128-uci-test.cvt
	CYCLES=250000000 tools/vtest.sh build/geochat128-uci-test.cvt build/uci.png || true

# the self-test: a test build (mailbox network + agent) driven by tools/vtest.py through the VICE binary monitor
TESTSRC = build/res-test.s $(CORE) src/net_mbox.c src/agent.c
build/geochat128-test.cvt: $(TESTSRC) src/*.h
	$(CL) -DSELFTEST -DRECONNECT_TICKS=30 -DDEF_NICK='"C128user"' -DDEF_CHAN='"#c64"' -m build/geochat128-test.map -Ln build/geochat128-test.lbl -o $@ $(TESTSRC)
	python3 tools/seticon.py $@

test: build/geochat128-test.cvt
	python3 tools/vtest.py --vdc 16 build/geochat128-test.cvt build/geochat128-test.map build/test.png
	python3 tools/vtest.py --vdc 64 --reu 512 --quiet build/geochat128-test.cvt build/geochat128-test.map build/test-reu.png

# the README's screenshot: a staged scene (tools/vtest.py --showcase) -> docs/geochat128.png
screenshot: build/geochat128-test.cvt
	python3 tools/vtest.py --quiet --showcase docs/geochat128.png build/geochat128-test.cvt build/geochat128-test.map build/showcase.png

# what a GitHub release carries (no GEOS: users make the boot disk from their own GEOS 128 disk with mkboot.sh)
release:                                    # always the public defaults, whatever was built before
	$(MAKE) -B NICK= CHAN='#c64friends' SERVER=irc.libera.chat build/geochat128.d64 build/geochat128.d71 build/geochat128.d81 build/geochat128-geos-test.cvt
	mkdir -p build/release
	cp build/geochat128.d64 build/release/geochat128.d64
	cp build/geochat128.d71 build/release/geochat128.d71
	cp build/geochat128.d81 build/release/geochat128.d81
	cp build/geochat128.cvt build/release/geochat128.cvt
	cp build/geochat128-geos-test.cvt build/release/geochat128-boot.cvt
	cp tools/mkboot.sh tools/c128boot.py tools/d64refs.py build/release/
	mkdir -p target
	cp build/release/geochat128.d64 build/release/geochat128.d71 build/release/geochat128.d81 build/release/geochat128.cvt build/release/geochat128-boot.cvt target/
	ls -l build/release

clean:
	rm -f build/*
.PHONY: all test boot boot-joy desk screenshot release shot-uci clean
