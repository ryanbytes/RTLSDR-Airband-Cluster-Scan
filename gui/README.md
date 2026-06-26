# RTLSDR-Airband Web GUI

This is a lightweight cross-platform browser GUI. `rtl_airband` writes
`status.json`; the static web page reads that file and renders the current
device, cluster, and active channel state.

Example config:

```conf
gui_status_filepath = "/path/to/gui/web/status.json";
gui_status_interval_ms = 1000;
```

Serve the directory with any static file server:

```sh
python3 gui/serve-airband-gui.py
```

Then open `http://127.0.0.1:12280/`.

The helper does not open a browser by default. Use `--open-browser` to also
open the system default browser.
