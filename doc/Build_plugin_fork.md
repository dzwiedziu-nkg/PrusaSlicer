# Building and running this fork

This is a fork of PrusaSlicer `3.0.0-alpha11` that adds **slicing plugin hooks** — five
extension points that let a Lua plugin influence slicing itself, which the released
plugin system cannot do.

**Build `main`.** It carries every hook, so every plugin below works. The `pr*` branches
carry one hook each and are there to be read, not to be run. See `doc/Plugin_API.md` for the
API and the plugins that use it:

- [island-order-plugin](https://github.com/dzwiedziu-nkg/island-order-plugin) — the print order of a layer's islands
- [short-extrusion-plugin](https://github.com/dzwiedziu-nkg/short-extrusion-plugin) — drops extrusions not worth the travel to reach them
- [sequential-islands-plugin](https://github.com/dzwiedziu-nkg/sequential-islands-plugin) — prints an object's upper branches one at a time
- [radial-bridge-plugin](https://github.com/dzwiedziu-nkg/radial-bridge-plugin) — runs bridges over annular gaps as spokes
- [wipe-tower-cancel-plugin](https://github.com/dzwiedziu-nkg/wipe-tower-cancel-plugin) — lets the printer cancel the wipe tower mid print

`doc/Build.md` is upstream's build guide and still applies. This file adds the parts that
are specific to this fork, and the one thing upstream leaves out: **where the built
program actually is.**

## 1. Prerequisites — Linux Mint 22.x

Mint 22.x is built on Ubuntu 24.04 "noble", so the packages upstream lists work as they
are. This exact set is what the fork is built with on Mint 22.1:

```bash
sudo apt install git build-essential autoconf cmake ninja-build \
    libglu1-mesa-dev libgtk-3-dev libdbus-1-dev libwebkit2gtk-4.1-dev texinfo
```

`ninja-build` is optional; `make` is used by default. Nothing else was needed — no PPA, no
manually installed library.

## 2. Build the dependencies — once, and it is slow

```bash
cd deps && mkdir -p build && cd build
cmake ..
cmake --build .
```

**This takes hours**, and it only has to happen once. Run `cmake --build .` a second time
afterwards: it should finish almost instantly and say there is nothing to do. If it starts
building again, the first run did not finish and the next step will fail confusingly.

The result lands in `deps/build/destdir`. **Do not move or copy that directory** — dozens
of files inside it hold absolute paths, so it only works where it was built. If you have
built the same dependency set elsewhere, point at it in place instead of rebuilding.

## 3. Build the slicer

From the repository root:

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DSLIC3R_STATIC=ON \
         -DCMAKE_PREFIX_PATH="$PWD/../deps/build/destdir/usr/local"
cmake --build . -j "$(nproc)"
```

About 15 minutes cold on 16 threads, a few minutes for an incremental rebuild.

Optional, and worth it after changing anything in the engine:

```bash
ctest            # from build/ — should be 14/14
```

A `ctest` run leaves `src/slic3r-shared/start_gcode_*_debug.txt` behind in the *source*
tree. Delete them; they are not supposed to be committed.

## 4. Where the binary is

This is the part that costs newcomers time. `build/` ends up holding sixteen executables
and **fourteen of them are test binaries**. Of the other two, one is a translation helper.
The application is:

```
build/src/slic3r-app-launcher/slic3r-app-launcher
```

Nothing in the name says "slicer", and it is three directories down. There is no separate
CLI binary — the same launcher does both the GUI and the command line, chosen by whether
you pass `-g`. Anything ending in `-tests` is a unit test suite, not the program.

A symlink somewhere on your `PATH` saves a lot of typing:

```bash
ln -s "$PWD/build/src/slic3r-app-launcher/slic3r-app-launcher" ~/.local/bin/prusaslicer-fork
```

## 5. Running it

### GUI

```bash
build/src/slic3r-app-launcher/slic3r-app-launcher
```

Run it from a terminal. The default log level prints the "plugin in use" lines to the
console, which is how you confirm a plugin loaded.

### Command line

A 3MF carries its own configuration, so it needs nothing else:

```bash
slic3r-app-launcher -g model.3mf -o out.gcode --loglevel 4
```

An STL does not, so every preset has to be named:

```bash
slic3r-app-launcher \
  --printer-profile "Prusa CORE One 0.4 HF" \
  --print-profile   "0.20mm SPEED @COREONE 0.4HF" \
  --tool-print-profile "no tool" \
  --material-profile "Prusa PLA@COREONE HF0.4@COREONE 0.4" \
  --center 125,105 -g model.stl -o out.gcode
```

Getting those names right is easier than guessing — both of these print JSON listing the
valid values:

```bash
slic3r-app-launcher --query-printer-models
slic3r-app-launcher --printer-profile "<name>" --query-print-tool-filament-profiles
```

## 6. Installing a plugin

Because the version is an alpha, the data directory carries a `-dev` suffix:

```
~/.config/PrusaSlicer3-dev/
```

Plugins are bundle directories under `lua/` there, and **the directory name must match the
`id` in the bundle's `manifest.json`**. Symlinking keeps the plugin editable in its own
checkout:

```bash
ln -s ~/projects/island-order-plugin/com.github.dzwiedziu-nkg.island-order \
      ~/.config/PrusaSlicer3-dev/lua/
```

Editing a plugin's `.lua` or its `settings.lua` takes effect **on the next slice** — no
restart, no rescan. To disable one, remove the symlink.

`settings.lua` is the only way to configure a slicing plugin: **there is no UI for it.**
The _Plugins_ menu and its parameter dialog list plugins of type `project.plugin` only
(`PluginSystem.cpp` refuses anything else), and a slicing plugin is never invoked by the
user, so it does not appear there. Each plugin's README documents its keys. Note that the
file is loaded inside a `pcall`, so a syntax error in it is silent: the file is ignored and
the plugin's built-in defaults apply.

Slicing plugins have no menu entry; they are not user invoked. The log names each one at
the start of every slice:

```
[info] Island ordering plugin in use: com.github.dzwiedziu-nkg.island-order.island_order
[info] Extrusion filter plugin in use: com.github.dzwiedziu-nkg.short-extrusion.short_extrusion
[info] Fill planner plugin in use: com.github.dzwiedziu-nkg.radial-bridge.radial_bridge
[info] Object labels plugin in use: com.github.dzwiedziu-nkg.wipe-tower-cancel.wipe_tower_cancel
```

The same lines go to `~/.config/PrusaSlicer3-dev/shared_runtime/log.txt`. Note that the log
file always goes to the *default* data directory even when `--datadir` points elsewhere.

`--datadir <dir>` is the clean way to compare with and without a plugin: keep a second data
directory with no `lua/` in it and slice the same file into both.
