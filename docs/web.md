# Browser build

The SDL platform also compiles to WebAssembly, so the game can be tried on any
machine with a browser (macOS, Windows, Linux) without building anything there.

```sh
brew install emscripten        # or the emsdk on Windows
make web                       # -> build/web/index.html, index.js, index.wasm
python3 -m http.server 8765 --bind 127.0.0.1 --directory build/web
```

Open `http://localhost:8765/`, choose the `.tpl` library file (and its `.tp1` ...
volumes if it is split) and press Start. The file is read in place with
`File.slice()`: nothing is uploaded and nothing is loaded whole, so multi-gigabyte
libraries work.

For small test libraries, put the file next to `index.html` and open
`http://localhost:8765/?lib=library.tpl`. `&autoplay=1` starts in auto-play.

The three files in `build/web/` are static and can be copied to any web server.

## Differences from the desktop build

- **Audio latency.** Browsers buffer 20 to 100 ms and it varies by browser and
  device. Set the audio offset in the settings (or run the calibration). Fine for
  checking charts and skins, not for score attempts.
- **Frame rate** is capped at 60 fps whatever the display.
- **Settings and scores** are kept in the browser's `localStorage`.
- The music buffer is 24 MB instead of 64 MB; long uncompressed songs stream in
  as they play, as on the Pocket.

## The packer in a browser

Not built. It is feasible (the packer is plain Python with Pillow, both available in
Pyodide, and the audio decoder is a plug-in point in `taikopack/audio.py` that a
browser page could fill with `decodeAudioData`), but writing a multi-gigabyte
library from a web page only works in Chromium-based browsers and is slow. The
command-line packer runs unchanged on macOS and Windows and is the supported way.
