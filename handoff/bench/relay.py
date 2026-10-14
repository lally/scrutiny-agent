#!/usr/bin/env python3
"""stdio relay adding a fixed one-way delay in each direction (order-preserving)."""
import os, sys, time, threading, subprocess, collections
delay = float(sys.argv[1]); cmd = sys.argv[2:]
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
totals = {}
def pump(rfd, wfd, close_w, name=None):
    q = collections.deque(); cv = threading.Condition(); done = [False]
    def reader():
        while True:
            try: b = os.read(rfd, 65536)
            except OSError: b = b''
            totals[name] = totals.get(name, 0) + len(b)
            if os.environ.get("RELAY_STATS"):
                open(os.environ["RELAY_STATS"], "w").write("%d %d\n" % (totals.get("up", 0), totals.get("down", 0)))
            with cv:
                q.append((time.monotonic() + delay, b)); cv.notify()
            if not b: return
    def writer():
        while True:
            with cv:
                while not q: cv.wait()
                due, b = q.popleft()
            dt = due - time.monotonic()
            if dt > 0: time.sleep(dt)
            if not b:
                try: close_w()
                except OSError: pass
                return
            try:
                off = 0
                while off < len(b): off += os.write(wfd, b[off:])
            except OSError: return
    threading.Thread(target=reader, daemon=True).start()
    t = threading.Thread(target=writer, daemon=True); t.start(); return t
t1 = pump(0, p.stdin.fileno(), p.stdin.close, "up")
t2 = pump(p.stdout.fileno(), 1, lambda: os.close(1), "down")
t2.join()
