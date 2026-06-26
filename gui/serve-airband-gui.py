#!/usr/bin/env python3
import argparse
import os
import socketserver
import threading
import time
import webbrowser
from functools import partial
from http.server import SimpleHTTPRequestHandler
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description="Serve the RTLSDR-Airband web GUI.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=12280, type=int)
    parser.add_argument("--open-browser", action="store_true")
    parser.add_argument("--directory", default=str(Path(__file__).resolve().parent / "web"))
    args = parser.parse_args()

    gui_dir = Path(args.directory).resolve()
    if not gui_dir.is_dir():
        raise SystemExit(f"GUI directory not found: {gui_dir}")

    handler = partial(SimpleHTTPRequestHandler, directory=str(gui_dir))
    socketserver.TCPServer.allow_reuse_address = True

    url = f"http://{args.host}:{args.port}/"
    if args.open_browser:
        threading.Thread(target=lambda: (time.sleep(1), webbrowser.open(url)), daemon=True).start()

    with socketserver.TCPServer((args.host, args.port), handler) as httpd:
        print(f"Serving RTLSDR-Airband web GUI at {url}", flush=True)
        httpd.serve_forever()


if __name__ == "__main__":
    main()
