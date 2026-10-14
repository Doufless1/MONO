#!/usr/bin/env python3
"""
Serve the repository for local development
==========================================
    python tools/serve.py            then open http://localhost:8080/

    /                  the project page (site/index.html)
    /mono_app/         the ONYX app        (?demo for the simulated wallet)
    /sim/              the wallet as WebAssembly

Why not `python -m http.server`: on Windows it takes MIME types from the
registry, which often maps .js to text/plain, and browsers refuse to run an
ES module served as text. This pins the types and turns caching off so an
edit shows up on reload. localhost counts as a secure origin, so Web
Bluetooth and WebCrypto both work here.
"""

import argparse
import functools
import http.server
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json",
    ".webmanifest": "application/manifest+json",
    ".wasm": "application/wasm",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".glb": "model/gltf-binary",
    ".stl": "model/stl",
    ".md": "text/markdown; charset=utf-8",
}


class Handler(http.server.SimpleHTTPRequestHandler):
    def guess_type(self, path):
        return TYPES.get(os.path.splitext(path)[1].lower()) or super().guess_type(path)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        if self.path in ("/", "/index.html") and os.path.exists(os.path.join(ROOT, "site", "index.html")):
            self.path = "/site/index.html"
        return super().do_GET()

    def log_message(self, fmt, *args):
        if os.environ.get("MONO_SERVE_QUIET") != "1":
            super().log_message(fmt, *args)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", 8080)))
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()
    handler = functools.partial(Handler, directory=ROOT)
    with http.server.ThreadingHTTPServer((args.host, args.port), handler) as srv:
        print(f"\n  MONO dev server on http://localhost:{args.port}/   (Ctrl-C to stop)")
        print(f"  app: http://localhost:{args.port}/mono_app/   demo: http://localhost:{args.port}/mono_app/?demo\n")
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
