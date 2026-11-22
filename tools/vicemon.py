#!/usr/bin/env python3
"""vicemon.py - minimal client for the VICE binary monitor (VICE 3.x, API version 2).

Request:  STX(0x02) API(0x02) body_len(4, LE) request_id(4) command(1) body
Response: STX API body_len(4) response_type(1) error(1) request_id(4) body
While a command is being handled the emulation is stopped; EXIT (0xAA) lets it run again.
Unsolicited events (STOPPED 0x62, RESUMED 0x63, ...) carry request id 0xFFFFFFFF and are skipped.
"""
import socket, struct, time

CMD_MEM_GET, CMD_MEM_SET = 0x01, 0x02
CMD_BANKS = 0x82
CMD_EXIT = 0xAA
CMD_QUIT = 0xBB
CMD_RESET = 0xCC


class Monitor:
    def __init__(self, host="127.0.0.1", port=6502, timeout=60.0):
        end = time.time() + timeout
        while True:
            try:
                self.s = socket.create_connection((host, port), timeout=5)
                break
            except OSError:
                if time.time() > end:
                    raise
                time.sleep(0.2)
        self.s.settimeout(30)
        self.rid = 0
        self.banks = {}
        self.hits = []

    def _recv(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise ConnectionError("VICE closed the monitor connection")
            b += c
        return b

    def cmd(self, ctype, body=b""):
        self.rid += 1
        self.s.sendall(struct.pack("<BBIIB", 2, 2, len(body), self.rid, ctype) + body)
        while True:
            hdr = self._recv(12)
            stx, api, blen, rtype, err, rid = struct.unpack("<BBIBBI", hdr)
            data = self._recv(blen)
            if rid == 0xFFFFFFFF and rtype == 0x11:              # checkpoint hit (event)
                self.hits.append(data)
            if rid == self.rid:
                if err:
                    raise RuntimeError(f"monitor command {ctype:#x} error {err:#x}")
                return rtype, data

    def bank_list(self):
        _, d = self.cmd(CMD_BANKS)
        n = struct.unpack_from("<H", d, 0)[0]
        o = 2
        for _ in range(n):
            ilen = d[o]
            bid, nlen = struct.unpack_from("<HB", d, o + 1)
            self.banks[d[o + 4:o + 4 + nlen].decode()] = bid
            o += 1 + ilen
        return self.banks

    def read(self, addr, n, bank=0):
        _, d = self.cmd(CMD_MEM_GET, struct.pack("<BHHBH", 0, addr, addr + n - 1, 0, bank))
        ln = struct.unpack_from("<H", d, 0)[0]
        return d[2:2 + ln]

    def write(self, addr, data, bank=0):
        self.cmd(CMD_MEM_SET, struct.pack("<BHHBH", 0, addr, addr + len(data) - 1, 0, bank) + bytes(data))

    def checkpoint(self, start, end=None, op=4, stop=True):
        """op: 1 load, 2 store, 4 exec. Hits are collected in self.hits."""
        self.cmd(0x12, struct.pack("<HHBBBB", start, end if end is not None else start, 1 if stop else 0, 1, op, 0))

    def go(self):
        self.cmd(CMD_EXIT)

    def quit(self):
        try:
            self.cmd(CMD_QUIT)
        except Exception:
            pass
