# sauce-fs

A read-only virtual filesystem with a cross-platform mount layer:

- **Linux**: FUSE3 (system `libfuse3` via pkg-config)
- **Windows**: WinFsp (installed MSI, located through `WINFSP_ROOT`)

The volume currently exposes a single generated file, `manifest.txt`, reporting
the versions of every linked library plus crc32/XXH3 digests of the report. The
same `snapshot()` source feeds both mount backends, so the volume content is
identical on every platform.

## Dependencies

Ported libraries come from [vcpkg](https://github.com/microsoft/vcpkg) via
`vcpkg.json`:

| Library | Use |
| --- | --- |
| `liblzma` | LZMA compression |
| `zlib` | DEFLATE compression and the standard `crc32()` |
| `sqlite3` | Embedded database |
| `xxhash` | XXH3/XXH64 hashing |
| `efsw` | Filesystem watching |

Not from vcpkg:

| Library | Platform | Why |
| --- | --- | --- |
| FUSE3 | Linux | must match the running kernel; vcpkg's `libfuse` port is FUSE2 |
| WinFsp | Windows | no vcpkg port; shipped as its own MSI |

## Prerequisites

- CMake 3.24+, Ninja, a C++20 compiler
- vcpkg, with `VCPKG_ROOT` exported:
  ```sh
  export VCPKG_ROOT=$HOME/vcpkg
  ```
- Linux: `libfuse3-dev` and `/dev/fuse`
- Windows: [WinFsp](https://winfsp.dev/) installed; optionally `WINFSP_ROOT`
  (defaults to `C:/Program Files (x86)/WinFsp`)

## Build

```sh
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
```

Windows: run the same commands with `--preset windows` from a developer prompt
(vcpkg triplet `x64-windows`).

## Run

```sh
./build/linux/sauce-fs --self-check     # link + round-trip each dependency
./build/linux/sauce-fs /mnt/sauce       # mount in the foreground
cat /mnt/sauce/manifest.txt
fusermount3 -u /mnt/sauce               # or Ctrl+C
```

On Windows, mount to a drive letter or directory:

```powershell
build\windows\sauce-fs.exe S:
type S:\manifest.txt
```

## Layout

```
CMakeLists.txt            dependency discovery, targets, per-platform backend
CMakePresets.json         vcpkg-based configure/build/test presets
vcpkg.json                manifest of ported dependencies
src/main.cpp              CLI: --self-check or mount
src/core/                 manifest report and volume contents (platform neutral)
src/backends/backend.h    mount()/backend_check() contract
src/backends/fuse3.cpp    Linux FUSE3 frontend
src/backends/winfsp.cpp   Windows WinFsp frontend
```

Adding a file to the volume means extending `snapshot()` in `src/core/volume.cpp`;
both backends pick it up without changes.
