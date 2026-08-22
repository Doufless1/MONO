#!/usr/bin/env python3
"""
MONO - run the companion app against a simulated wallet
========================================================
Serves mono_app/index.html unmodified except for one injected <script> that
replaces navigator.bluetooth with a shim wired to mono_sim.exe. The app does
not know the difference: it pairs, subscribes to notifications and exchanges
the same text lines it would over a real Nordic UART link.

    python tests/app_sim_server.py          then open http://127.0.0.1:8765/

A bench panel in the corner stands in for the physical world - press a finger
to the sensor, shake the wallet, drop the phone link.

Build the simulator first:
    g++ -std=c++17 -x c++ -I tests/mocks -o tests/build/mono_sim.exe tests/mono_sim.cpp
"""

import os
import sys
import json
import threading
import subprocess
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
APP = os.path.join(ROOT, "mono_app", "index.html")
SIM = os.path.join(HERE, "build", "mono_sim.exe")
if not os.path.exists(SIM):
    SIM = os.path.join(HERE, "build", "mono_sim")

PORT = int(os.environ.get("MONO_SIM_PORT", "8765"))

# ------------------------------------------------------------ the wallet --
_lines = deque(maxlen=2000)
_seq = 0
_lock = threading.Lock()
_proc = None


def say(msg):
    """Console tracing must never take the bridge down. Piping this script
    into head/tail closes stdout, and an unguarded print would then kill the
    request that happened to be running."""
    try:
        print(msg, flush=True)
    except (BrokenPipeError, OSError, ValueError):
        pass


def start_sim():
    global _proc
    if not os.path.exists(SIM):
        sys.exit(f"simulator not built: {SIM}\n"
                 f"  g++ -std=c++17 -x c++ -I tests/mocks "
                 f"-o tests/build/mono_sim.exe tests/mono_sim.cpp")
    _proc = subprocess.Popen([SIM], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             text=True, bufsize=1)

    def pump():
        global _seq
        for line in _proc.stdout:
            line = line.rstrip("\r\n")
            if not line:
                continue
            with _lock:
                _seq += 1
                _lines.append((_seq, line))
            say("  wallet > " + line)
    threading.Thread(target=pump, daemon=True).start()


def tx(line):
    say("  app    > " + line)
    _proc.stdin.write(line + "\n")
    _proc.stdin.flush()


def since(n):
    with _lock:
        return _seq, [t for s, t in _lines if s > n]


# ------------------------------------------------------------- the shim ---
SHIM = r"""
/* Web Bluetooth shim: same UUIDs, same notification mechanics, backed by
   mono_sim.exe instead of a radio. Loaded before the app's own script. */
(function () {
  "use strict";
  var NUS = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
  var RXU = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
  var TXU = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

  var seq = 0, poller = null, txChar = null, dev = null;

  function post(line) {
    return fetch("/tx", {method: "POST", body: line});
  }

  function startPolling() {
    if (poller) return;
    poller = setInterval(function () {
      fetch("/rx?since=" + seq).then(function (r) { return r.json(); })
        .then(function (d) {
          seq = d.seq;
          d.lines.forEach(function (l) {
            if (!txChar) return;
            txChar.value = new TextEncoder().encode(l + "\n");
            txChar.dispatchEvent(new Event("characteristicvaluechanged"));
          });
        }).catch(function () {});
    }, 120);
  }

  function Char(uuid) {
    var t = new EventTarget();
    t.uuid = uuid;
    t.value = null;
    t.writeValue = function (buf) {
      var s = new TextDecoder().decode(buf).replace(/\n$/, "");
      return post(s);
    };
    t.startNotifications = function () { txChar = t; startPolling(); return Promise.resolve(t); };
    return t;
  }

  function makeDevice() {
    var d = new EventTarget();
    d.name = "MONO";
    d.id = "sim-mono";
    var rx = Char(RXU), txc = Char(TXU);
    d.gatt = {
      connected: false,
      connect: function () {
        d.gatt.connected = true;
        return Promise.resolve({
          getPrimaryService: function (u) {
            if (u !== NUS) return Promise.reject(new Error("no such service"));
            return Promise.resolve({
              getCharacteristic: function (cu) {
                if (cu === RXU) return Promise.resolve(rx);
                if (cu === TXU) return Promise.resolve(txc);
                return Promise.reject(new Error("no such characteristic"));
              }
            });
          }
        });
      },
      disconnect: function () {
        d.gatt.connected = false;
        post("!link 0");
        d.dispatchEvent(new Event("gattserverdisconnected"));
      }
    };
    return d;
  }

  Object.defineProperty(navigator, "bluetooth", {
    configurable: true,
    value: {
      getAvailability: function () { return Promise.resolve(true); },
      requestDevice: function () {
        dev = makeDevice();
        post("!link 1");
        return Promise.resolve(dev);
      }
    }
  });

  // ------------------------------------------------------- bench panel --
  var BENCH = [
    ["Finger: enrolled #1", "!finger 1"],
    ["Finger: unknown",     "!finger new"],
    ["Press (enrol)",       "!press 7"],
    ["Lift finger",         "!lift"],
    ["Shake it",            "!tamper on"],
    ["Stop shaking",        "!tamper off"],
    ["Walk away",           "!link 0"],
    ["Come back",           "!link 1"],
    ["Battery 3.4 V",       "!batt 3.4"],
    ["Battery 4.1 V",       "!batt 4.1"],
    ["Peek internals",      "!peek"]
  ];

  window.addEventListener("DOMContentLoaded", function () {
    var p = document.createElement("div");
    p.id = "bench";
    p.style.cssText =
      "position:fixed;right:10px;bottom:10px;z-index:9999;width:172px;" +
      "background:var(--surface,#161B21);border:1px solid var(--rule,#28313A);" +
      "padding:9px;font:11px/1.4 'Fira Mono',monospace;border-radius:2px;" +
      "box-shadow:0 6px 24px rgba(0,0,0,.35);max-height:86vh;overflow:auto";
    var h = document.createElement("div");
    h.textContent = "BENCH - no hardware";
    h.style.cssText = "letter-spacing:.1em;color:var(--muted,#828F9C);" +
                      "margin-bottom:7px;text-transform:uppercase";
    p.appendChild(h);
    BENCH.forEach(function (b) {
      var btn = document.createElement("button");
      btn.textContent = b[0];
      btn.style.cssText = "display:block;width:100%;margin:0 0 4px;padding:5px 7px;" +
                          "font:11px 'Fira Mono',monospace;text-align:left;cursor:pointer";
      btn.onclick = function () { post(b[1]); };
      p.appendChild(btn);
    });
    document.body.appendChild(p);
  });
})();
"""


# ------------------------------------------------------------- the server --
class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, ctype, body):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/index"):
            html = open(APP, encoding="utf-8").read()
            tag = '<script src="/sim-shim.js"></script>'
            if "<head>" in html:
                html = html.replace("<head>", "<head>\n" + tag, 1)
            else:
                html = tag + html
            return self._send(200, "text/html; charset=utf-8", html)

        if self.path == "/sim-shim.js":
            return self._send(200, "application/javascript; charset=utf-8", SHIM)

        if self.path.startswith("/rx"):
            n = 0
            if "since=" in self.path:
                try:
                    n = int(self.path.split("since=")[1].split("&")[0])
                except ValueError:
                    n = 0
            s, lines = since(n)
            return self._send(200, "application/json",
                              json.dumps({"seq": s, "lines": lines}))

        return self._send(404, "text/plain", "not found")

    def do_POST(self):
        if self.path == "/tx":
            n = int(self.headers.get("Content-Length", 0))
            line = self.rfile.read(n).decode("utf-8").strip()
            if line:
                tx(line)
            return self._send(200, "application/json", '{"ok":true}')
        return self._send(404, "text/plain", "not found")


if __name__ == "__main__":
    start_sim()
    ThreadingHTTPServer.allow_reuse_address = False
    try:
        srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
    except OSError as e:
        sys.exit(f"port {PORT} is already in use ({e}). "
                 f"Stop the other instance, or set MONO_SIM_PORT.")
    print(f"\n  MONO app + simulated wallet on http://127.0.0.1:{PORT}/")
    print("  no board, no sensor, no battery. Ctrl-C to stop.\n")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if _proc:
            _proc.terminate()
