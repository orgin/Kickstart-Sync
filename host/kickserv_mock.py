#!/usr/bin/env python3
"""
Minimal stand-in for the KickServ commodity, speaking the same protocol.
Serves the files of a local directory - handy for testing KickSync
without an Amiga.   usage: kickserv_mock.py DIR [PORT]
"""
import datetime
import os
import socketserver
import sys

AMIGA_EPOCH = datetime.datetime(1978, 1, 1)


def verstring(path):
    with open(path, "rb") as f:
        data = f.read()
    i = data.find(b"$VER:")
    if i < 0:
        return ""
    s = data[i + 5:i + 205]
    for t in (b"\0", b"\r", b"\n"):
        s = s.split(t)[0]
    return s.decode("latin-1").replace("\t", " ").strip()


class Handler(socketserver.StreamRequestHandler):
    def send(self, line):
        self.wfile.write(line.encode("latin-1") + b"\n")

    def handle(self):
        root = self.server.root
        self.send("KICKSERV\t1")
        for raw in self.rfile:
            line = raw.rstrip(b"\r\n").decode("latin-1")
            cmd, _, arg = line.partition(" ")
            cmd = cmd.upper()
            if cmd == "QUIT":
                self.send("BYE")
                return
            if cmd == "LIST":
                self.send("OK")
                for n in sorted(os.listdir(root)):
                    p = os.path.join(root, n)
                    if os.path.isfile(p):
                        self.send("%d\t%d\t%s\t%s" % (os.path.getsize(p), self.date(p), n, verstring(p)))
                self.send(".")
                continue
            if cmd in ("INFO", "GET"):
                if not arg or ":" in arg or arg.startswith("/") or ".." in arg or "//" in arg:
                    self.send("ERR\tinvalid name")
                    continue
                # AmigaOS file systems are case-insensitive
                match = [n for n in os.listdir(root) if n.lower() == arg.lower()]
                p = os.path.join(root, match[0] if match else arg)
                if not os.path.isfile(p):
                    self.send("ERR\tnot found")
                    continue
                if cmd == "INFO":
                    self.send("OK\t%d\t%d\t%s" % (os.path.getsize(p), self.date(p), verstring(p)))
                else:
                    with open(p, "rb") as f:
                        data = f.read()
                    self.send("OK\t%d" % len(data))
                    self.wfile.write(data)
                continue
            self.send("ERR\tunknown command")

    @staticmethod
    def date(p):
        dt = datetime.datetime.fromtimestamp(os.path.getmtime(p))
        return int((dt - AMIGA_EPOCH).total_seconds())


if __name__ == "__main__":
    srv = socketserver.ThreadingTCPServer(("", int(sys.argv[2]) if len(sys.argv) > 2 else 6800), Handler)
    srv.root = sys.argv[1]
    srv.allow_reuse_address = True
    print("serving %s on port %d" % (srv.root, srv.server_address[1]))
    srv.serve_forever()
