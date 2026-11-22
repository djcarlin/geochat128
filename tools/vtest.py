#!/usr/bin/env python3
"""vtest.py - geoChat 128's self-test: GEOS 128 + a test build of geoChat 128 in headless VICE x128 (80 columns),
driven from Python through the VICE binary monitor.

    tools/vtest.py build/geochat128-test.cvt build/geochat128-test.map build/test.png [--reu 512] [--shot-at STEP]

The test build replaces the network driver with a mailbox (src/net_mbox.c, src/mbox.h) and adds a small agent
(src/agent.c) that types keys and clicks through GEOS itself. This script
  - builds a GEOS 128 boot disk with the app in place of the deskTop and a C128 boot sector,
  - starts x128 with the debug cartridge and the binary monitor, and waits for the agent,
  - plays the IRC server on the other side of the mailbox (FakeServer),
  - runs the scenario below and checks the app's variables (by name, from the linker map) and the 80-column
    screen (read straight from VDC memory),
  - ends with the agent writing the result to $D7FF: VICE exits with that code and saves the screenshot.
Exit code: 0 PASS, n = failure code (see the scenario), 2 harness error, 124 timeout.
"""
import argparse, hashlib, os, random, re, shutil, socket, struct, subprocess, sys, time, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vicemon import Monitor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEOS128 = os.environ.get("GEOS128", "GEOS128.D64")                # your own GEOS 128 2.0 disk (make passes it)

# ---- constants that must match the C sources ---------------------------------------------------------------
MSG_TOP, LINE_H, MSG_LINES, STATUS_Y, INPUT_Y = 16, 9, 18, 180, 190          # ui.h
FIELD_Y0, FIELD_DY = MSG_TOP + 22, 20                                       # settings.h
HINT_Y = FIELD_Y0 + 4 * FIELD_DY + 8
BTN_T = HINT_Y + 8
OK_L, OK_R, CAN_L = 120, 200, 220
X_ABOUT, X_SETTINGS, X_CONNECT, X_DISCONN, MENU_Y = 20, 68, 123, 190, 7       # menu bar (measured)
KEY_F1, KEY_F3, KEY_F4, KEY_F5, KEY_F7 = 1, 3, 4, 5, 14                       # cc65 gsprite.h
KEY_ENTER, KEY_TAB, KEY_UP, KEY_DOWN, KEY_CLEAR, KEY_ESC, KEY_DELETE = 13, 9, 16, 17, 19, 27, 29
KEY_LEFT, KEY_RIGHT = 8, 30                                        # GEOS KbdDecodeTab1/2 (cursor keys)
BOLDON, PLAINTEXT, REV_ON, REV_OFF = 24, 27, 18, 19
OP_KEYS, OP_CLICK, OP_REDRAW, OP_LAST, OP_FLOOD, OP_WALK, OP_INPUT, OP_FINDCFG, OP_SETLOAD, OP_EXIT, OP_SWITCH, \
    OP_DRAWROW, OP_TIME = range(1, 14)
WIN_SIZE = 35                                                               # struct uiwin: name[32] + 3 bytes
SPOS_NONE = 0xFFFFFFFF
NICK0, CHAN0 = "C128user", "#c64"                                           # Makefile test build defaults
CLOCK = "2026/11/21 13:45:00"                                    # what net_time returns in the test build
# mbox layout (src/mbox.h)
O_REQ, O_ACK, O_OP, O_RESULT, O_CONN, O_DROPREQ = 4, 5, 6, 12, 14, 16
O_RXHEAD, O_RXTAIL, O_TXHEAD, O_TXTAIL, O_TIME, O_TEXT, O_RX, O_TX = 18, 19, 20, 21, 22, 42, 298, 554
O_NOCABLE = 810


ALLOWED = set()
SKIP = set()                                                     # --skip: scenario parts left out (diagnosis)                                                  # --allow: checks reported but not fatal


class Fail(Exception):
    def __init__(self, code, why):
        super().__init__(f"FAIL {code}: {why}")
        self.code, self.why = code, why


class FakeServer:
    """The IRC server on the other side of the mailbox (one session per net_connect)."""
    BURST = [
        ":Alice!alice@retro.net PRIVMSG #c64 :Hi everyone! Anyone running GEOS 128 in 80 columns?",
        ":Bob!bob@retro.net PRIVMSG #c64 :This is a deliberately long message to test word wrapping on the eighty "
        "column screen. The VDC gives us 640 pixels across, so a line holds roughly a hundred characters of the "
        "GEOS system font, but IRC lines can be up to 512 bytes, so the client has to wrap them neatly at spaces "
        "and indent the continuation lines.",
        ":Carol!carol@retro.net PRIVMSG #c64 :\x01ACTION loads GEOS 128 from a 1581 and grins\x01",
        ":Dave!dave@retro.net NOTICE {me} :This is a notice sent directly to you",
        ":Eve!eve@retro.net PRIVMSG {me} :psst - this is a private message",
        ":Frank!frank@retro.net JOIN #c64",
        ":Frank!frank@retro.net NICK Frank128",
        ":Alice!alice@retro.net PART #c64 :brb, rebooting the 1571",
        ":Bob!bob@retro.net QUIT :Quit: power cut",
        ":Gina!gina@retro.net PRIVMSG #c64 :\x02bold\x02 then \x0304,01colour codes\x03 dropped and \x16reverse\x16 shown",
        ":Hank!hank@retro.net PRIVMSG #c64 :UTF-8: caf\xc3\xa9 \xe2\x9c\x93 na\xc3\xafve",
    ] + [f":Ivy!ivy@retro.net PRIVMSG #c64 :scroll test line {i:02}" for i in range(1, 9)] + [
        "PING :tok42",
        ":c64bot!bot@fake.irc PRIVMSG #c64 :burst done - say something!",
    ]

    def __init__(self):
        self.sent = []            # every line the client sent, all sessions
        self.session_sent = []    # this session only
        self.conn = None
        self.reset()

    def reset(self):
        self.out = bytearray()
        self.inbuf = bytearray()
        self.me = "you"
        self.registered = False
        self.burst_i = None       # None: not started; index into BURST
        self.done = False
        self.bot = True           # c64bot answers channel messages (not in the showcase)
        self.session_sent = []
        self.say(":fake.irc NOTICE * :*** Fake IRC server for geoChat 128 tests")

    def say(self, line):
        self.out += (line.encode("latin1") if isinstance(line, str) else line) + b"\r\n"

    def has(self, prefix, session=True):
        return any(l.startswith(prefix) for l in (self.session_sent if session else self.sent))

    def feed(self, data):
        self.inbuf += data
        while b"\n" in self.inbuf:
            line, _, self.inbuf = self.inbuf.partition(b"\n")
            line = line.rstrip(b"\r").decode("latin1")
            self.sent.append(line); self.session_sent.append(line)
            self.handle(line)

    def handle(self, l):
        me = self.me
        if l.startswith("NICK "):
            new = l[5:]
            if self.registered:
                self.say(f":{me}!user@c128 NICK :{new}")
            self.me = new
        elif l.startswith("USER "):
            self.registered = True
            for t in [f"001 {me} :Welcome to the fake IRC network, {me}", f"002 {me} :Your host is fake.irc",
                      f"375 {me} :- fake.irc Message of the day -", f"372 {me} :- Testing geoChat 128 on GEOS 128.",
                      f"376 {me} :End of /MOTD command."]:
                self.say(":fake.irc " + t)
        elif l.startswith("JOIN "):
            for ch in l[5:].split(" ")[0].split(","):
                self.say(f":{me}!user@c128 JOIN :{ch}")
                self.say(f":fake.irc 332 {me} {ch} :Commodore 128 fans - 80 columns or bust")
                self.say(f":fake.irc 353 {me} = {ch} :@c64bot Alice Bob Carol Dave Eve Gina Hank Ivy {me}")
                self.say(f":fake.irc 366 {me} {ch} :End of /NAMES list.")
            if self.burst_i is None:
                self.burst_i = 0
        elif l.startswith("PART "):
            ch = l[5:].split(" ")[0]
            self.say(f":{me}!user@c128 PART {ch} :bye")
        elif l.startswith("PRIVMSG #") and self.done and self.bot:
            target, _, text = l[8:].partition(" :")
            if not text.startswith("\x01"):
                self.say(f":c64bot!bot@fake.irc PRIVMSG {target} :{me}: got it ({len(text)} chars)")

    def tick(self):
        """called every pump: deliver the next burst line (paced, like a network)"""
        if self.burst_i is not None and self.burst_i < len(self.BURST) and len(self.out) < 100:
            self.say(self.BURST[self.burst_i].replace("{me}", self.me))
            self.burst_i += 1
            if self.burst_i == len(self.BURST):
                self.done = True


class Harness:
    def __init__(self, args):
        self.a = args
        self.syms = self.read_map(args.map)
        self.srv = FakeServer()
        self.vice = None

    @staticmethod
    def read_map(path):
        """symbols from the ld65 map, and from the VICE label file next to it (-Ln: lists every exported
        symbol, also those no other module imports, which the map leaves out)"""
        syms = {}
        txt = open(path).read()
        for m in re.finditer(r"(\w+)\s+([0-9A-F]{6})\s+R?[LE]A", txt):
            syms[m.group(1)] = int(m.group(2), 16)
        lbl = os.path.splitext(path)[0] + ".lbl"
        if os.path.exists(lbl):
            for m in re.finditer(r"^al ([0-9A-F]{6}) \.(\w+)$", open(lbl).read(), re.M):
                syms[m.group(2)] = int(m.group(1), 16)
        return syms

    def sym(self, name):
        if name not in self.syms:
            raise RuntimeError(f"symbol {name} not in the map")
        return self.syms[name]

    # ---- VICE -------------------------------------------------------------------------------------------
    def make_disk(self, disk):
        shutil.copy(GEOS128, disk)
        subprocess.run(["c1541", "-attach", disk, "-delete", "128 desktop", "-geoswrite", self.a.cvt],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "c128boot.py"), disk, "geoChat 128", 'RUN"GEOS128"'],
                       check=True, stdout=subprocess.DEVNULL)

    def start(self):
        disk = os.path.splitext(self.a.png)[0] + ".d64"
        self.make_disk(disk)
        port = random.randint(20000, 40000)
        cmd = ["x128", "-default", "-80col", "-minimized", "-silent", "-sounddev", "dummy", "-warp", "-debugcart",
               "-binarymonitor", "-binarymonitoraddress", f"ip4://127.0.0.1:{port}",
               "-exitscreenshot", os.path.abspath(self.a.png), "-8", disk]
        if self.a.reu:                                           # (after -default, which resets everything)
            cmd[2:2] = ["-reu", "-reusize", str(self.a.reu)]
        cmd[2:2] = [f"-VDC{self.a.vdc}KB"]
        # an empty keyboard map: if the VICE window pops up and takes focus, what someone types on this computer
        # must not reach the C128 (it did: stray letters in the input line). The test types through GEOS itself.
        keymap = os.path.abspath(os.path.join(os.path.dirname(self.a.png) or ".", "nokeys.vkm"))
        with open(keymap, "w") as f:
            f.write("# no host keys: nothing typed on the host reaches the emulated C128\n!CLEAR\n")
        cmd[2:2] = ["-keymap", "0", "-symkeymap", keymap]
        if os.path.exists(self.a.png):
            os.remove(self.a.png)
        self.log = open(os.path.splitext(self.a.png)[0] + ".vice.log", "w")
        self.vice = subprocess.Popen(cmd, stdout=self.log, stderr=subprocess.STDOUT)
        self.unplugged, self.void = False, bytearray()
        self.m = Monitor(port=port)
        self.m.bank_list()
        self.b1, self.b0, self.vdc = self.m.banks["ram01"], self.m.banks["ram00"], self.m.banks["vdc"]
        self.mb = self.sym("_mbox")

    # ---- memory -----------------------------------------------------------------------------------------
    def rd(self, name, n=1, off=0):
        return self.m.read(self.sym(name) + off, n, self.b1)

    def byte(self, name, off=0):
        return self.rd(name, 1, off)[0]

    def word(self, name, off=0):
        return struct.unpack("<H", self.rd(name, 2, off))[0]

    def dword(self, name):
        return struct.unpack("<I", self.rd(name, 4))[0]

    def cstr(self, name, n=48):
        return self.rd(name, n).split(b"\0")[0].decode("latin1")

    def win(self, i):
        d = self.m.read(self.sym("_ui_win") + i * WIN_SIZE, WIN_SIZE, self.b1)
        return {"name": d[:32].split(b"\0")[0].decode("latin1"), "used": d[32], "unread": d[33], "mention": d[34]}

    def win_find(self, name):
        for i in range(8):
            w = self.win(i)
            if w["used"] and w["name"].lower() == name.lower():
                return i
        return None

    def mouse_on(self):
        return self.m.read(0x30, 1, self.b0)[0]                 # mouseOn: GEOS zero page (bank 0)

    def park_mouse(self):
        self.m.write(0x3A, bytes([200, 0, 4]), self.b0)          # mousePos: x 200, y 4 (menu bar line)
        self.pump(3)

    def vdc_row(self, y, x0=0, x1=639):
        return self.m.read(y * 80 + (x0 + 7) // 8, (x1 + 1) // 8 - (x0 + 7) // 8, self.vdc)

    def solid(self, y, x0=0, x1=639):
        return all(b == 0xFF for b in self.vdc_row(y, x0, x1))

    def lines_ok(self):
        return self.solid(STATUS_Y) and self.solid(INPUT_Y)

    def screen_hash(self):
        return hashlib.md5(self.m.read(MSG_TOP * 80, MSG_LINES * LINE_H * 80, self.vdc)).hexdigest()

    # ---- the pump: run the C128 a little, then service the mailbox ------------------------------------------
    def unplug(self, out):
        self.unplugged = out
        self.void = bytearray()
        self.m.write(self.mb + O_NOCABLE, bytes([1 if out else 0]), self.b1)

    def pump(self, n=1):
        for _ in range(n):
            self.m.go()
            time.sleep(0.008)
            if self.vice.poll() is not None:
                raise ConnectionError("VICE exited")
            h = self.m.read(self.mb, 22, self.b1)
            if h[O_CONN] != self.srv.conn:                       # net_connect: a fresh server session
                if self.srv.conn is not None:                    # (client bytes are never discarded: what the
                    self.srv.reset()                             #  app sent after connecting is this session's)
                self.srv.conn = h[O_CONN]
            if h[O_TXHEAD] != h[O_TXTAIL]:                       # client -> server
                ring = self.m.read(self.mb + O_TX, 256, self.b1)
                t, data = h[O_TXTAIL], bytearray()
                while t != h[O_TXHEAD]:
                    data.append(ring[t]); t = (t + 1) & 255
                self.m.write(self.mb + O_TXTAIL, bytes([t]), self.b1)
                if self.unplugged:                               # pulled cable: the bytes go nowhere
                    self.void += data
                    continue
                self.srv.feed(bytes(data))
            if self.unplugged:                                   # ...and nothing comes back
                continue
            self.srv.tick()
            free = (h[O_RXTAIL] - h[O_RXHEAD] - 1) & 255         # server -> client
            if self.srv.out and free:
                chunk = self.srv.out[:min(free, 200)]
                del self.srv.out[:len(chunk)]
                head = h[O_RXHEAD]
                first = chunk[:256 - head]
                self.m.write(self.mb + O_RX + head, first, self.b1)
                if len(chunk) > len(first):
                    self.m.write(self.mb + O_RX, chunk[len(first):], self.b1)
                self.m.write(self.mb + O_RXHEAD, bytes([(head + len(chunk)) & 255]), self.b1)

    def check(self, ok, code, why):
        if not ok:
            if code in ALLOWED:
                print(f"  (allowed) FAIL {code}: {why}")
            else:
                raise Fail(code, why)

    def wait(self, cond, timeout, code, why):
        end = time.time() + timeout
        while not cond():
            if time.time() > end:
                raise Fail(code, why)
            self.pump()

    def agent(self, op, a1=0, x=0, y=0, text=None, timeout=30):
        if text is not None:
            b = (text.encode("latin1") if isinstance(text, str) else bytes(text)) + b"\0"
            self.m.write(self.mb + O_TEXT, b, self.b1)
        self.m.write(self.mb + O_OP, struct.pack("<BBHH", op, a1, x, y), self.b1)
        req = (self.m.read(self.mb + O_REQ, 1, self.b1)[0] + 1) & 255
        self.m.write(self.mb + O_REQ, bytes([req]), self.b1)
        end = time.time() + timeout
        while self.m.read(self.mb + O_ACK, 1, self.b1)[0] != req:
            if time.time() > end:
                raise Fail(99, f"agent op {op} not answered")
            self.pump()
        res = struct.unpack("<H", self.m.read(self.mb + O_RESULT, 2, self.b1))[0]
        txt = self.m.read(self.mb + O_TEXT, 256, self.b1).split(b"\0")[0].decode("latin1")
        return res, txt

    def keys(self, *parts):
        b = bytearray()
        for p in parts:
            b += bytes([p]) if isinstance(p, int) else p.encode("latin1")
        self.agent(OP_KEYS, text=bytes(b))

    def click(self, x, y):
        self.agent(OP_CLICK, x=x, y=y)
        self.pump(4)

    def last(self, w):
        return self.agent(OP_LAST, a1=w)[1]

    # ---- the scenario --------------------------------------------------------------------------------------
    def scenario(self):
        S, A = self.srv, self.agent
        step = self.step
        # 1-7: register, join, the burst, talk
        step("session")
        self.wait(lambda: S.done, 120, 7, "the scripted burst was not delivered")
        self.pump(20)
        if self.win_find(CHAN0) != self.byte("_ui_active"):
            raise Fail(70, "JOIN did not open and show the channel window")
        e = self.win_find("Eve")
        if e is None: raise Fail(71, "private message did not open a window")
        if not self.win(e)["mention"]: raise Fail(72, "private message window not marked")
        self.keys("hello from the 128", KEY_ENTER)
        self.keys("/me waves at the VDC", KEY_ENTER)
        self.pump(10)
        for code, p in [(1, "NICK " + NICK0), (2, "USER " + NICK0), (3, "JOIN " + CHAN0), (4, "PONG :tok42"),
                        (5, f"PRIVMSG {CHAN0} :hello from the 128"),
                        (6, f"PRIVMSG {CHAN0} :\x01ACTION waves at the VDC\x01")]:
            if not S.has(p): raise Fail(code, f"server never got {p!r}")
        if self.word("_irc_lines_in") < 30: raise Fail(7, "too few lines received")
        self.park_mouse()
        if not self.lines_ok(): raise Fail(50, "divider lines broken")
        h = self.screen_hash()
        A(OP_REDRAW); self.pump(3)
        self.check(self.screen_hash() == h, 73, "drawing from the store differs from live scrolling")

        if "about" not in SKIP:
            self.about()
        if "settings" not in SKIP:
            self.settings()
        self.rest()

    def showcase(self):
        """A staged scene for the README screenshot (make screenshot): a channel mid-conversation with a mention,
        bold, an action, joins/nick changes/quits, timestamps; an unread channel and a private message waiting."""
        S, A = self.srv, self.agent
        self.wait(lambda: S.done, 120, 7, "the scripted burst was not delivered")
        S.bot = False
        self.keys("/join #c128", KEY_ENTER)
        self.wait(lambda: self.win_find("#c128") is not None, 30, 70, "#c128 not opened")
        me = S.me
        def say(line, n=6):
            S.say(line); self.pump(n)
        say(":Sprite!s@retro.net JOIN #c128")
        say(":Jiffy!j@retro.net JOIN #c128")
        say(":Sprite!s@retro.net PRIVMSG #c128 :anyone else running GEOS 128 in 80 columns?")
        self.keys("right here - this whole IRC client runs on the VDC", KEY_ENTER); self.pump(6)
        say(":Sprite!s@retro.net PRIVMSG #c128 :\x01ACTION boots the 1571 and grins\x01")
        say(f":Basic7!b@retro.net PRIVMSG #c128 :{me}: how fast is switching between channels?")
        self.keys("instant for the last few: they are kept in the spare 48K of a 64K VDC", KEY_ENTER); self.pump(6)
        say(":Jiffy!j@retro.net PRIVMSG #c128 :\x02nice!\x02 and the scrollback?")
        self.keys("in the REU: thousands of lines, F1/F3 to page through them", KEY_ENTER); self.pump(6)
        say(":Basic7!b@retro.net NICK Basic8")
        say(":Raster!r@retro.net JOIN #c128")
        say(":Raster!r@retro.net PRIVMSG #c128 :timestamps come from the Ultimate's real-time clock, right?")
        self.keys("yes, and TAB completes nicks", KEY_ENTER); self.pump(6)
        say(":Jiffy!j@retro.net QUIT :Quit: off to load a game from tape")
        say(":Sprite!s@retro.net PRIVMSG #c64 :meanwhile in #c64 ...")              # an unread tab
        say(f":Nova!n@retro.net PRIVMSG {me} :hi! saw your post about geoChat 128")      # a private chat: !
        A(OP_SWITCH, a1=self.win_find("#c128"))
        self.m.write(0x3A, bytes([600 & 255, 600 >> 8, 110]), self.b0)   # pointer out of the way (blank area)
        self.pump(10)
        # the 80-column bitmap straight from VDC RAM, every scanline doubled (a monitor shows 640x200 with tall
        # pixels), black on white: VICE's own screenshot is 856x240 and looks squashed
        d = self.m.read(0, 16000, self.vdc)
        rows = []
        for y in range(200):
            r = bytes(255 - b for b in d[y * 80:(y + 1) * 80])
            rows += [r, r]
        raw = b"".join(b"\0" + r for r in rows)
        def chunk(t, b):
            return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
        with open(self.a.showcase, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 640, 400, 1, 0, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
        self.log_line(f"showcase: {self.a.showcase}")

    def about(self):
        A, step = self.agent, self.step
        step("about box")
        d0, r0 = self.byte("_agent_dlgs"), self.byte("_ui_redraws")
        self.click(X_ABOUT, MENU_Y)
        self.wait(lambda: self.byte("_agent_dlgs") == d0 + 1 and (self.mouse_on() & 0xC0) == 0xC0
                  and self.byte("_ui_redraws") > r0, 30, 10, "about box did not open and close")
        self.park_mouse()
        if not self.lines_ok(): raise Fail(13, "divider lines broken after the about box")

    def settings(self):
        S, A, step = self.srv, self.agent, self.step
        step("settings page")
        self.click(X_SETTINGS, MENU_Y)
        self.wait(lambda: self.word("_ui_overlay") != 0, 30, 20, "settings page did not open")
        n0 = self.word("_irc_lines_in")
        S.say(":Alice!alice@retro.net PRIVMSG #c64 :this arrives while the settings page is open")
        self.keys(KEY_CLEAR, "Tester", KEY_ENTER, KEY_CLEAR, "#test")
        self.click(300, FIELD_Y0 + 3 * FIELD_DY + 5)            # the port field
        self.keys(KEY_CLEAR, "70x00")                           # letters are refused
        self.pump(5)
        step("settings typed")                                  # (--shot-at "settings typed": the page)
        self.wait(lambda: self.word("_irc_lines_in") > n0, 30, 22, "no message arrived while the page was open")
        self.pump(10)
        if not self.solid(BTN_T, OK_L, OK_R): raise Fail(21, "the settings page was drawn over")
        self.click(OK_L + 20, BTN_T + 7)
        self.wait(lambda: self.word("_ui_overlay") == 0, 30, 23, "OK did not close the page")
        if self.cstr("_set_nick") != "Tester": raise Fail(24, "nick not taken")
        if self.cstr("_set_chan") != "#test": raise Fail(25, "channel not taken")
        if self.cstr("_set_server") != "irc.libera.chat": raise Fail(26, "server changed")
        if self.cstr("_set_port") != "7000": raise Fail(27, "port field accepted letters / not taken")
        self.park_mouse()                                       # (the save runs just after OK; GEOS hides the
        self.wait(lambda: (self.mouse_on() & 0xC0) == 0xC0 and self.lines_ok(), 30, 28,   # mouse during disk I/O)
                  "mouse or divider lines")
        end = time.time() + 30                                  # saved once the mouse button is up
        while A(OP_FINDCFG)[0] and time.time() < end:
            self.pump(5)
        self.check(A(OP_FINDCFG)[0] == 0, 60, "OK did not save the settings file")
        self.m.write(self.sym("_set_nick"), b"Nobody\0", self.b1)
        self.m.write(self.sym("_set_port"), b"1\0", self.b1)
        self.check(A(OP_SETLOAD)[0] == 0, 61, "settings file did not load")
        self.check(self.cstr("_set_nick") == "Tester" and self.cstr("_set_port") == "7000", 62, "loaded wrong values")
        self.m.write(self.sym("_set_nick"), b"Tester\0", self.b1)
        self.m.write(self.sym("_set_port"), b"7000\0", self.b1)
        self.click(X_SETTINGS, MENU_Y)
        self.wait(lambda: self.word("_ui_overlay") != 0, 30, 29, "settings page did not open again")
        self.keys("ZZ")
        self.click(CAN_L + 20, BTN_T + 7)
        self.wait(lambda: self.word("_ui_overlay") == 0, 30, 29, "Cancel did not close the page")
        if self.cstr("_set_nick") != "Tester": raise Fail(29, "Cancel changed the settings")

    def rest(self):
        S, A, step = self.srv, self.agent, self.step
        if "settings" in SKIP:                                  # what the settings page would have set
            self.m.write(self.sym("_set_nick"), b"Tester\0", self.b1)
            self.m.write(self.sym("_set_chan"), b"#test\0", self.b1)
        step("connect with the new settings")
        self.click(X_CONNECT, MENU_Y)
        self.wait(lambda: S.has("NICK Tester") and S.has("JOIN #test")
                  and self.win_find("#test") == self.byte("_ui_active"), 60, 30, "reconnect with new settings")
        if not S.has("JOIN #c64"): raise Fail(31, "open channel window not rejoined")
        S.say(":Ev\x01il!x@y PRIVMSG #test :ctl \x01\x07\x1d\x1f end")
        got = []                                                # the line the wait found: a notice may follow it
        self.wait(lambda: got.append(self.last(self.byte("_ui_active"))) or "ctl" in got[-1], 30, 51,
                  "control-code line not shown")
        t = got[-1]
        if any(ord(c) < 32 and ord(c) not in (BOLDON, PLAINTEXT, REV_ON, REV_OFF) for c in t):
            raise Fail(52, "control codes reached the screen: " + repr(t))
        if not re.match(r"\d\d:\d\d ", t): raise Fail(126, "no timestamp: " + repr(t))
        if not t.startswith(("13:45", "13:46")): raise Fail(127, "clock not set from the network side: " + repr(t[:5]))
        if self.m.read(0x8516, 3, self.b1) != bytes([26, 11, 21]): raise Fail(127, "date not set")

        step("typing and commands")
        self.keys("helloX", KEY_DELETE, KEY_ENTER)
        self.keys("junk", KEY_CLEAR, "ok", KEY_ENTER)
        self.pump(5)
        if not S.has("PRIVMSG #test :hello"): raise Fail(40, "DEL")
        if not S.has("PRIVMSG #test :ok"): raise Fail(41, "CLEAR")
        cmds = [("/nick Foo", "NICK Foo"), ("/msg bob hi there", "PRIVMSG bob :hi there"),
                ("/raw WHOIS bob", "WHOIS bob"), ("/notice bob psst", "NOTICE bob :psst"),
                ("/whois carol", "WHOIS carol"), ("/away gone fishing", "AWAY :gone fishing"), ("/away", "AWAY"),
                ("/topic", "TOPIC #test"), ("/topic new topic", "TOPIC #test :new topic"), ("/names", "NAMES #test"),
                ("/join #other", "JOIN #other")]
        for typed, expect in cmds:
            self.keys(typed, KEY_ENTER)
            self.pump(3)
            if not S.has(expect): raise Fail(42, f"{typed!r} did not send {expect!r}")
        self.wait(lambda: self.win_find("#other") == self.byte("_ui_active"), 30, 80, "/join did not open a window")
        self.keys("/part bye", KEY_ENTER)
        self.pump(3)
        if not S.has("PART #other :bye"): raise Fail(47, "/part")
        self.keys("/help", KEY_ENTER, "/bogus", KEY_ENTER, "/msg bob", KEY_ENTER)
        self.keys("".join(chr(97 + i % 26) for i in range(230)))        # overlong input
        if len(A(OP_INPUT)[1]) != 200: raise Fail(48, "input not capped at 200")
        self.park_mouse()
        if not self.lines_ok(): raise Fail(48, "divider lines with an overlong input")
        self.keys(KEY_CLEAR)
        self.wait(lambda: self.win_find("#other") is None and self.byte("_ui_active") == 0, 30, 81,
                  "PART did not close the window")

        step("fast line drawing")
        # src/fastdraw.s must give exactly the pixels GEOS's PutString gives: every character, the wide ones,
        # bold, reverse, both, an empty line and a full-width one, compared byte for byte in VDC memory
        B, RV, RO, PL = "\x18", "\x12", "\x13", "\x1b"
        lines = ["".join(chr(c) for c in range(32, 80)), "".join(chr(c) for c in range(80, 127)),
                 f"plain {B}bold MWmw_\\ text{PL} plain {RV}reverse MWmw_{RO} after {B}{RV}bold+rev Wm_{PL} end",
                 f"{RV} {RO}x{B}{RV}y{PL}z {B}i{PL}j {B}" + "".join(chr(c) for c in range(33, 90)), "", "W" * 63]
        if self.byte("_fl_ok") != 1: raise Fail(139, "fast line drawing not in use (fl_init failed)")
        row = 3
        y0 = 16 + row * 9                                       # MSG_TOP + row*LINE_H
        for i, t in enumerate(lines):
            A(OP_DRAWROW, a1=row, x=1, text=t)
            want = self.m.read(y0 * 80, 720, self.vdc)
            A(OP_DRAWROW, a1=row, x=1, text=RV + "M" * 62)        # junk the fast draw must clear
            A(OP_DRAWROW, a1=row, x=0, text=t)
            got = self.m.read(y0 * 80, 720, self.vdc)
            if got != want:
                k = next(j for j in range(720) if got[j] != want[j])
                raise Fail(140 + i, f"fast line {i} differs from PutString at scanline +{k // 80}, pixel "
                                    f"{(k % 80) * 8}: {got[k]:08b} instead of {want[k]:08b} ({t[:30]!r})")
        A(OP_REDRAW)

        step("windows")
        self.keys(KEY_F7); a = self.byte("_ui_active")
        self.keys(KEY_F7); b = self.byte("_ui_active")
        self.keys(KEY_F5); c = self.byte("_ui_active")
        if (a, b, c) != (1, 2, 1): raise Fail(82, f"F7/F7/F5 went to {a},{b},{c}")
        w = self.win_find("#test")
        self.click(self.word("_ui_tab_x", 2 * w) + 3, STATUS_Y + 5)
        if self.byte("_ui_active") != w: raise Fail(83, "clicking a tab did not switch")
        self.keys("/win 2", KEY_ENTER)
        if self.byte("_ui_active") != 1: raise Fail(84, "/win 2")
        self.keys("/query carol", KEY_ENTER)
        q = self.win_find("carol")
        if q is None or self.byte("_ui_active") != q: raise Fail(85, "/query")
        self.keys("hi carol", KEY_ENTER); self.pump(3)
        if not S.has("PRIVMSG carol :hi carol"): raise Fail(86, "typing in a query window")
        self.keys("/close", KEY_ENTER)
        if self.win_find("carol") is not None or self.byte("_ui_active") != 0: raise Fail(87, "/close")

        step("window cache")
        # with a 64 KB VDC, leaving a window copies its message area to spare VDC RAM and coming back copies it in
        # (src/ui.c, vc_save/ui_switch). Whatever comes back must be exactly what a full redraw draws.
        vc = self.byte("_ui_vcache")
        if vc != (3 if self.a.vdc == 64 else 0): raise Fail(150, f"{vc} cache slots with a {self.a.vdc} KB VDC")
        ch, tw = self.win_find(CHAN0), self.win_find("#test")
        area = lambda: self.m.read(16 * 80, 18 * 9 * 80, self.vdc)
        top = lambda: self.m.read(self.sym("_ui_top"), 4, self.b1)
        def same(code, why):
            got, t = area(), top()
            full = A(OP_REDRAW)[0]
            if area() != got: raise Fail(code, why + ": the screen differs from a full redraw")
            if top() != t: raise Fail(code, why + ": the scrollback position differs from a full redraw")
            return full
        self.park_mouse()
        A(OP_SWITCH, a1=ch); A(OP_SWITCH, a1=tw)
        t = A(OP_SWITCH, a1=ch)[0]
        full = same(151, "switching back")
        self.log_line(f"switch back: {t * 256 / 1022727:.3f} s; full redraw {full * 256 / 1022727:.3f} s")
        if vc and t * 256 / 1022727 > 0.3: raise Fail(152, f"cached switch took {t * 256 / 1022727:.2f} s")
        self.keys(KEY_F1); self.keys(KEY_ESC)                   # scrollback still works from the cached view
        same(153, "page up and back after a cached switch")
        A(OP_SWITCH, a1=tw)
        S.say(f":Bob!b@h PRIVMSG {CHAN0} :this arrived while you were in another window")
        self.wait(lambda: "another window" in self.last(ch), 30, 154, "message for the hidden window not stored")
        A(OP_SWITCH, a1=ch)
        same(154, "a window that got a message while hidden")
        self.keys(KEY_F1)                                       # leave while scrolled back, come back
        A(OP_SWITCH, a1=tw); A(OP_SWITCH, a1=ch)
        if self.dword("_ui_view") != 0xFFFFFFFF:
            raise Fail(155, "came back scrolled back")
        same(155, "leaving a window while scrolled back")

        step("scrollback")
        A(OP_SWITCH, a1=self.win_find(CHAN0))
        self.park_mouse()
        live = self.screen_hash()
        self.keys(KEY_F1)
        v1 = self.dword("_ui_view")
        if v1 == SPOS_NONE: raise Fail(90, "F1 did not scroll back")
        back = self.screen_hash()
        if back == live: raise Fail(91, "scrolled back but the screen did not change")
        self.keys(KEY_F1)
        v2 = self.dword("_ui_view")
        if v2 == SPOS_NONE or v2 >= v1: raise Fail(92, "second page up did not go further back")
        self.keys(KEY_F3)
        if self.dword("_ui_view") != v1 or self.screen_hash() != back: raise Fail(93, "F3 did not come back a page")
        self.keys(KEY_ESC)
        if self.dword("_ui_view") != SPOS_NONE: raise Fail(94, "ESC did not return to the newest")
        if self.screen_hash() != live: raise Fail(95, "the newest page looks different after scrolling")
        # page up after new lines have scrolled in live (the real C128: F1 in a window that had only been drawn
        # once, at start-up, did nothing): the page must come from just above the screen, not from where the
        # screen's top was at the last full redraw
        t0 = self.dword("_ui_top")
        for i in range(30):
            S.say(f":Gina!g@h PRIVMSG #c64 :scrolling in live, line {i}")
        self.wait(lambda: "line 29" in self.last(self.win_find(CHAN0)), 60, 88, "live lines not shown")
        self.keys(KEY_F1)
        v = self.dword("_ui_view")
        if v == SPOS_NONE or v <= t0: raise Fail(88, f"F1 after live scrolling went to {v:#x}, not above the screen")
        self.keys(KEY_ESC)
        live = self.screen_hash()
        self.keys(KEY_F1)
        back = self.screen_hash()
        S.say(":Gina!g@h PRIVMSG #c64 :this arrives while you are scrolled back")
        self.wait(lambda: self.byte("_ui_more") == 1, 30, 96, "new message while scrolled back not flagged")
        if self.screen_hash() != back: raise Fail(97, "new message drew over the scrolled-back page")
        self.keys(KEY_ESC)
        if self.byte("_ui_more") or self.screen_hash() == live: raise Fail(98, "ESC did not show the new message")

        step("message ring")
        banks = self.byte("_store_banks")
        reu = self.m.read(0x88C3, 1, self.b1)[0]                 # ramExpSize
        if reu > 1 and not banks: raise Fail(103, f"REU present ({reu} banks) but not used")
        A(OP_FLOOD, a1=0, x=2000, timeout=120)
        k, t = A(OP_WALK, a1=0, x=2000, timeout=120)
        if k < 1000: raise Fail(100, f"only {k} of 2000 messages reachable")
        if not t or ord(t[0]) != 1: raise Fail(101, "walking back ended in garbage")
        if banks and k != 2000: raise Fail(102, f"REU: only {k} of 2000 reachable")
        self.log_line(f"ring: {k} of 2000 reachable, REU banks {banks} (ramExpSize {reu})")

        step("history and nick completion")
        self.keys("alpha", KEY_ENTER, "beta", KEY_ENTER)
        for code, k_, want in [(110, KEY_UP, "beta"), (111, KEY_UP, "alpha"), (112, KEY_DOWN, "beta"), (113, KEY_DOWN, "")]:
            self.keys(k_)
            got = A(OP_INPUT)[1]
            if got != want: raise Fail(code, f"history: {got!r} instead of {want!r}")
        self.keys("Ali", KEY_TAB)
        if A(OP_INPUT)[1] != "Alice: ": raise Fail(114, "TAB at the start: " + A(OP_INPUT)[1])
        self.keys(KEY_CLEAR, "hi Ca", KEY_TAB)
        if A(OP_INPUT)[1] != "hi Carol ": raise Fail(115, "TAB in the line: " + A(OP_INPUT)[1])
        self.keys(KEY_CLEAR)
        # a big channel: 70 names in one NAMES reply. The first must still complete (a 40-nick pool lost it on
        # real hardware in a 70-person channel), and the list must not push out people who talked (Alice, Carol)
        names = ["Aardvark"] + [f"u{i:02d}" for i in range(68)] + ["Zyxel"]
        S.say(f":fake.irc 353 {S.me} = {CHAN0} :" + " ".join(names))
        S.say(f":fake.irc 366 {S.me} {CHAN0} :End of /NAMES list.")
        self.wait(lambda: "Aardvark" in self.last(self.win_find(CHAN0)), 30, 116, "big NAMES reply not shown")
        for code, pre, want in [(116, "Aard", "Aardvark: "), (117, "Zy", "Zyxel: "), (118, "Ali", "Alice: "),
                                (119, "aLI", "Alice: ")]:                           # case doesn't matter
            self.keys(pre, KEY_TAB)
            got = A(OP_INPUT)[1]
            self.keys(KEY_CLEAR)
            if got != want: raise Fail(code, f"TAB after a big NAMES list: {got!r} instead of {want!r}")

        # editing in the middle of the line: left twice, DEL removes "b", "X" goes in at the cursor, right twice
        self.keys(KEY_CLEAR, "abcd", KEY_LEFT, KEY_LEFT, KEY_DELETE, "X", KEY_RIGHT, KEY_RIGHT, "!")
        if A(OP_INPUT)[1] != "aXcd!": raise Fail(43, "cursor editing gave " + repr(A(OP_INPUT)[1]))
        self.keys(KEY_CLEAR)

        step("mentions, routing, CTCP, whois")
        A(OP_SWITCH, a1=0)
        beeps = self.byte("_ui_beeps")
        S.say(":Bob!b@h PRIVMSG #c64 :an ordinary line, no ding")
        self.wait(lambda: "no ding" in self.last(self.win_find(CHAN0)), 30, 128, "ordinary line not shown")
        if self.byte("_ui_beeps") != beeps: raise Fail(128, "an ordinary line made the SID ding")
        for l in [":Bob!b@h PRIVMSG #c64 :hey Foo are you there", ":Frank128!f@h QUIT :bye now",
                  ":Bob!b@h PRIVMSG Foo :\x01PING 12345\x01", ":Bob!b@h PRIVMSG Foo :\x01VERSION\x01",
                  ":fake.irc 311 Foo bob ~b host * :Bob Real"]:
            S.say(l)
        self.wait(lambda: "bob is ~b@host" in self.last(0), 30, 125, "whois reply not shown")
        c = self.win_find(CHAN0)
        if not self.win(c)["mention"]: raise Fail(120, "mention not marked")
        if self.byte("_ui_beeps") == beeps: raise Fail(128, "a mention did not make the SID ding")
        if "Frank128 has quit" not in self.last(c): raise Fail(121, "QUIT not shown where Frank was seen")
        if "Frank128" in self.last(self.win_find("#test")): raise Fail(122, "QUIT shown in a channel Frank wasn't in")
        if not S.has("NOTICE Bob :\x01PING 12345\x01"): raise Fail(123, "CTCP PING not answered")
        if not S.has("NOTICE Bob :\x01VERSION geoChat 128"): raise Fail(124, "CTCP VERSION not answered")

        step("automatic reconnect")
        conn = S.conn
        self.m.write(self.mb + O_DROPREQ, bytes([(self.m.read(self.mb + O_DROPREQ, 1, self.b1)[0] + 1) & 255]), self.b1)
        self.wait(lambda: self.byte("_irc_state") == 0 and self.word("_irc_retry") > 0, 30, 130, "drop not noticed")
        self.wait(lambda: S.conn != conn and S.has("NICK Foo") and S.has("JOIN #c64") and S.has("JOIN #test"),
                  120, 131, "did not reconnect and rejoin")

        step("pulled cable")
        # the real C128: with the Ethernet cable out for 6 minutes nothing arrives and nothing fails, and the
        # client never noticed. It must ping after a silence, give up on the connection, keep retrying while
        # connecting fails, and come back by itself once the cable is in again
        self.unplug(True)
        self.wait(lambda: b"PING :geochat128" in self.void, 120, 134, "no keepalive PING after a silence")
        self.wait(lambda: self.byte("_irc_state") == 0 and self.word("_irc_retry") > 0, 120, 135,
                  "the silent connection was not given up")
        self.wait(lambda: "Connection failed" in self.last(0) or "No network address" in self.last(0), 120, 136,
                  "no reconnect attempt while unplugged")
        if "No network address" not in self.last(0):
            raise Fail(138, "a failed connect with no address doesn't say so: " + self.last(0))
        self.wait(lambda: self.word("_irc_retry") > 0, 30, 136, "gave up retrying after a failed connect")
        conn = S.conn
        self.unplug(False)
        self.wait(lambda: S.conn != conn and S.has("NICK Foo") and S.has("JOIN #c64") and S.has("JOIN #test")
                  and self.byte("_irc_state") == 2, 180, 137, "did not come back after the cable was in again")

        step("many windows")
        # all 8 windows open, with long names: every tab must still be on the screen (640 pixels; text ends by 636),
        # and while scrolled back the [scrolled back: ESC] note too
        for name in ["#retrocomputing", "#commodore-128", "#c128-friends", "#geos-users"]:
            self.keys(f"/join {name}", KEY_ENTER)
            self.wait(lambda: self.win_find(name) is not None, 30, 156, f"{name} window not opened")
        used = sum(1 for i in range(8) if self.win(i)["used"])
        if used != 8: raise Fail(156, f"{used} windows open, wanted 8")
        if self.word("_ui_status_w") > 636: raise Fail(156, f"status line {self.word('_ui_status_w')} pixels wide")
        A(OP_SWITCH, a1=self.win_find(CHAN0))
        self.keys(KEY_F1)
        if self.word("_ui_status_w") > 636:
            raise Fail(157, f"scrolled back: status line {self.word('_ui_status_w')} pixels wide")
        self.keys(KEY_ESC)

        step("disconnect")
        self.pump(20)
        self.click(X_DISCONN, MENU_Y)
        self.wait(lambda: S.has("QUIT"), 30, 8, "disconnect menu did not send QUIT")
        self.pump(10)
        self.park_mouse()
        if (self.mouse_on() & 0xC0) != 0xC0: raise Fail(9, "menu left the mouse off")
        if not self.lines_ok(): raise Fail(49, "divider lines")
        self.pump(30)
        if self.word("_irc_retry") or self.byte("_irc_state"): raise Fail(133, "reconnecting after a user disconnect")
        # no nick set (the release has no default nick): connect must ask for one, not send "NICK" alone
        self.m.write(self.sym("_set_nick"), b"\0", self.b1)
        conn = S.conn
        self.click(X_CONNECT, MENU_Y)
        self.wait(lambda: "Set your nick" in self.last(self.byte("_ui_active")), 30, 32, "connect without a nick")
        self.pump(20)
        if S.conn != conn or self.byte("_irc_state"): raise Fail(32, "connected without a nick")

    def step(self, name):
        self.log_line("== " + name)
        if self.a.profile and name == self.a.profile:
            cpu = self.m.banks["cpu"]
            z = self.m.read(0x26, 8, cpu)
            base, sw, h = z[0], z[1] | z[2] << 8, z[3]
            idx = z[4] | z[5] << 8
            it = self.m.read(idx, 2 * 97, cpu)
            o = [it[2 * i] | it[2 * i + 1] << 8 for i in range(97)]
            print(f"font: baseline {base} setWidth {sw} height {h} index {idx:#06x} data {z[6] | z[7] << 8:#06x}")
            print("widths 32..127:", [o[i + 1] - o[i] for i in range(96)])
            for w in range(8):
                if self.win(w)["used"]:
                    A = self.agent
                    A(OP_SWITCH, a1=w)
                    r = A(OP_REDRAW)[0]
                    print(f"redraw window {w} {self.win(w)['name']}: {r * 256 / 1022727:.3f} s "
                          f"(CIA 2 counts the 1 MHz clock, also in 2 MHz mode)", flush=True)
            print(f"$D030 = {self.m.read(0xD030, 1, self.m.banks['io'])[0]:#04x} (bit 0 = 2 MHz)")
            A(OP_SWITCH, a1=0)
            t = A(OP_LAST, a1=0)[1]
            print(f"newest message of window 0 ({len(t)} chars): {t[:60]!r}")
            for part, name in [(1, "store_get"), (2, "store_prev"), (3, "wrap (one pass)"), (4, "fast draw")]:
                r = A(OP_TIME, a1=part)[0]
                print(f"  {name}: {r * 256 / 10 / 1022.727:.1f} ms each")
            raise Fail(0, "profile done")
        if self.a.shot_at and name == self.a.shot_at:
            raise Fail(0, "screenshot requested")

    def log_line(self, s):
        if not self.a.quiet:
            print(s, flush=True)

    def run(self):
        try:
            self.start()
            self.wait_agent()
            if self.a.showcase:
                self.showcase()
            else:
                self.scenario()
            code, why = 0, "all checks passed"
        except Fail as f:
            code, why = f.code, f.why
        except Exception as e:
            print(f"ERROR {type(e).__name__}: {e}")
            self.stop()
            return 2
        if code:
            print(f"FAIL {code}: {why}")
            print("  last lines the server got:", self.srv.sent[-4:])
            self.dump()
        try:                                                     # the app writes $D7FF: VICE exits
            self.m.write(self.mb + O_OP, struct.pack("<BBHH", OP_EXIT, code, 0, 0), self.b1)
            req = (self.m.read(self.mb + O_REQ, 1, self.b1)[0] + 1) & 255
            self.m.write(self.mb + O_REQ, bytes([req]), self.b1)
            self.m.go()
            self.vice.wait(timeout=60)
        except Exception:
            print("  (the app did not exit by itself: no screenshot)")
        self.stop()
        if not code:
            print(f"PASS (screenshot {self.a.png})")
        return code

    def dump(self):
        try:
            h = self.m.read(self.mb, 22, self.b1)
            print(f"  mbox req/ack {h[O_REQ]}/{h[O_ACK]} conn {h[O_CONN]} closed {h[15]} rx {h[O_RXHEAD]}/{h[O_RXTAIL]}"
                  f" tx {h[O_TXHEAD]}/{h[O_TXTAIL]}; irc_state {self.byte('_irc_state')} retry {self.word('_irc_retry')}"
                  f" active {self.byte('_ui_active')} overlay {self.word('_ui_overlay'):04x}")
            print("  windows:", [self.win(i)["name"] for i in range(8) if self.win(i)["used"]])
            _, d = self.m.cmd(0x31, b"\x00")                     # registers get (main memspace)
            print("  registers:", d.hex())
        except Exception as e:
            print("  (state dump failed:", e, ")")

    def wait_agent(self):
        end = time.time() + 240
        while self.m.read(self.mb, 4, self.b1) != b"MBX1":
            if time.time() > end:
                raise Fail(124, "the app never started (no mailbox)")
            self.m.go(); time.sleep(0.2)
        self.m.write(self.mb + O_TIME, CLOCK.encode() + b"\0", self.b1)   # the "Ultimate's clock" (net_time)

    def stop(self):
        if self.vice and self.vice.poll() is None:
            try:
                self.m.quit()
            except Exception:
                pass
            try:
                self.vice.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.vice.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cvt"); ap.add_argument("map"); ap.add_argument("png")
    ap.add_argument("--reu", type=int, default=0, help="REU size in KB (0 = none)")
    ap.add_argument("--shot-at", help="stop (PASS) when this step starts, for a screenshot")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--showcase", metavar="PNG", help="stage the README screenshot instead of the test, save it here")
    ap.add_argument("--vdc", type=int, choices=(16, 64), default=64, help="VDC RAM in KB (64: window cache)")
    ap.add_argument("--profile", help="at this step, time a redraw of every window (CIA 2 cycle count)")
    ap.add_argument("--allow", default="", help="comma-separated failure codes to report but not stop on")
    ap.add_argument("--skip", default="", help="diagnosis: leave out scenario parts (about, settings)")
    a = ap.parse_args()
    ALLOWED.update(int(c) for c in a.allow.split(",") if c)
    SKIP.update(c for c in a.skip.split(",") if c)
    sys.exit(Harness(a).run())


if __name__ == "__main__":
    main()
