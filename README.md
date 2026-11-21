# sauce-fs

Read-only, cross-platform access to Source engine content. Two layers live here:

- **`sauce-fs`** — the original virtual filesystem. It exposes a generated
  `manifest.txt` reporting the versions of every linked library plus
  crc32/XXH3 digests of the report. Linux uses FUSE3 and Windows uses WinFsp;
  the same `snapshot()` source feeds both, so the volume content is identical.
- **Archive index** — a scanner plus runtime access layer for the archives
  inside installed Source games: VPK `_dir` trees and side archives, embedded
  BSP pakfiles, GMA addons, and legacy `.bin` addons. It records entry metadata
  in SQLite and reads entries back as raw bytes, inflating DEFLATE and LZMA
  payloads on demand.

The index is consumed three ways through one provider API: random lookup by
path, folder browsing, and a read-only mount that walks every archive.

## Dependencies

Ported libraries come from [vcpkg](https://github.com/microsoft/vcpkg) via
`vcpkg.json`:

| Library | Use |
| --- | --- |
| `liblzma` | LZMA compression (BSP pakfiles, some VPK entries) |
| `zlib` | DEFLATE compression and the standard `crc32()` |
| `sqlite3` | Archive index |
| `xxhash` | XXH3/XXH64 hashing |
| `efsw` | Filesystem watching |
| `fmt` | Type-safe formatting for CLI output |

Not from vcpkg:

| Library | Platform | Why |
| --- | --- | --- |
| FUSE3 | Linux | must match the running kernel; vcpkg's `libfuse` port is FUSE2 |
| WinFsp | Windows | no vcpkg port; shipped as its own MSI |

## Prerequisites

- CMake 3.24+, Ninja, a C++23 compiler (`std::expected` is used by the archive
  access layer)
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

Targets:

| Target | What it is |
| --- | --- |
| `sauce-fs` | Manifest VFS with the platform mount backend |
| `sauce-fs-index` | Archive scanner and indexer |
| `sauce-fs-get` | Reference consumer: get/list/stat/resolve |
| `sauce-fs-mount` | Read-only FUSE mount over the index |

## Manifest volume

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

## Archive index

`sauce-fs-index` discovers installed Source games through the Steam libraries
and writes a SQLite database. Pass explicit game folders to skip discovery:

```sh
./build/linux/sauce-fs-index --database index.db
./build/linux/sauce-fs-index --database index.db "/path/to/steamapps/common/Half-Life 2"
```

The database defaults to a file named `test` in the current directory. A full
install set (21 games, 1,080 archives, ~2M files) indexes in a few seconds and
produces roughly 400 MB.

Each game is scanned on its own thread, with a separate Garry's Mod crawler for
`addons/` and workshop items. Archives are hashed with XXH3-64 over the whole
file; entry contents are not hashed. The database is written once all threads
join.

### Watching for changes

`--watch` keeps the indexer running after the initial build. efsw watches every
scanned game root (plus Garry's Mod workshop roots) and a burst of events is
debounced into one update:

```sh
./build/linux/sauce-fs-index --database index.db --watch
./build/linux/sauce-fs-index --database index.db --watch --debounce-ms 1000 \
    "/path/to/steamapps/common/Half-Life 2"
```

Only archives whose size or last-write time changed are hashed again. If a
rewrite produced the same XXH3 digest, the existing entry rows are kept, so a
touched but unchanged VPK is cheap. Added archives are parsed and inserted,
removed archives are deleted, and a changed mount path (collision suffixes can
shift when an archive appears) is updated transactionally. Archives are never
reparsed just because a sibling changed. Watch roots are selected from the same
game folders as the initial build; restart the watcher when a whole new game
folder appears.

Updates use SQLite WAL, so a long-lived `sauce-fs-mount` process (or another
user of the access API) can read the previous snapshot while the watcher
commits a new one. The runtime access layer checks `PRAGMA data_version` before
each lookup, reloads metadata and decoded-file caches when it changes, and the
FUSE mount rebuilds its provider tree when a game or archive is added or
removed. New and replaced archives therefore appear in the mounted tree
without remounting.

### What gets indexed

| Variant | Source |
| --- | --- |
| `vpk` | `*_dir.vpk` trees, including side archives and preload bytes |
| `bsp-pak` | Embedded `pakfile` ZIP lumps inside `.bsp` maps |
| `gma` | Garry's Mod addons |
| `gma-bin` | Legacy GMA payloads wrapped in `.bin` |

VPK side archives (`*_000.vpk`, …) are not separate rows; their entries are
resolved through the parent `_dir` archive. BSPs embed a ZIP, so their entries
carry absolute offsets and method 0/8/14. Legacy `.bin` offsets are relative to
the decoded GMAD payload.

`archives.archive_type_id` references the `archive_type` lookup table. GMAD
archives also store the header metadata: addon name, description (the raw
`addon.json` string), author, addon version, SteamID64, and pack timestamp.

### CLI

`sauce-fs-get` is a reference consumer of the runtime access library:

```sh
./build/linux/sauce-fs-get --database index.db --list /
./build/linux/sauce-fs-get --database index.db --list half-life-2/hl2
./build/linux/sauce-fs-get --database index.db --stat half-life-2/hl2/materials/foo.vmt
./build/linux/sauce-fs-get --database index.db half-life-2/hl2/materials/foo.vmt > foo.vmt
```

`--game SLUG` switches from the per-archive view to a merged game view, and
`--resolve` exercises the raw-pointer resolver:

```sh
./build/linux/sauce-fs-get --database index.db --game half-life-2 --list materials
./build/linux/sauce-fs-get --database index.db --resolve half-life-2/hl2/materials/foo.vmt
```

The default view is `game/archive-mount-path/internal-path`; with `--game` paths
are relative to the game root.

### Mount

`sauce-fs-mount` mounts the index read-only. The root is `sourcegames`, then one
directory per game slug, one directory per archive, then the archive's internal
tree:

```sh
mkdir -p /mnt/sourcegames
./build/linux/sauce-fs-mount --database index.db -f /mnt/sourcegames
ls /mnt/sourcegames/half-life-2/hl2
cat /mnt/sourcegames/half-life-2/hl2/materials/foo.vmt
fusermount3 -u /mnt/sourcegames
```

Files are `0444` and directories `0555`. Because archives are namespaced per
archive rather than merged per game, colliding paths stay distinct — for
example `half-life-2/maps/c1a0/materials/...` and
`half-life-2/hl2_misc/materials/...`.

### API

`src/index/archive_access.hpp` exposes a `Provider` with three primitives:

```cpp
Result<NodeInfo> stat(std::string_view path) const;
Result<std::vector<DirectoryEntry>> list(std::string_view path) const;
Result<Blob> get(std::string_view path) const;   // Blob = shared_ptr<const Bytes>
```

`get` returns `std::expected<Blob, Error>`. Providers compose:

- `ArchiveProvider` — one archive; paths are archive-internal.
- `LayeredProvider` — ordered overlay; first layer wins for `stat`/`get`, and
  `list` unions the layers. This is where merge resolution lives.
- `TreeProvider` — synthetic directories for game and archive levels.
- `IndexDatabase` — opens the database, owns the decoded-file cache, and builds
  providers through `archive_provider()`, `game_provider(slug, order)`, and
  `mount_provider()`.

For consumers that cannot handle `Result<T>` or exceptions, `Provider::resolve`
is a `noexcept` variant returning `bool` and filling a `ResolvedEntry` with a
raw pointer plus the owning `Blob`:

```cpp
ResolvedEntry entry;
if (provider->resolve("materials/foo.vmt", entry))
{
    consume(entry.data, entry.size);   // entry.blob keeps it alive
}
```

### Database schema

- `games(id, slug, title, game_path)` — one row per detected game. `slug` is
  derived from the title (`half-life-2`) because folder names collide.
- `archive_type(id, name)` — lookup table for the archive container kind
  (`vpk`, `bsp-pak`, `gma`, `gma-bin`).
- `archives(id, game_id, path, mount_path, name, stem, archive_type_id, hash,
  size, modified, addon_*)` — one row per archive, `UNIQUE(game_id,
  mount_path)`, XXH3-64 `hash`, and the physical size/mtime used by watch mode
  to skip unchanged files. GMAD rows carry `addon_name`, `addon_description`,
  `addon_author`, `addon_version`, `steamid64`, and `timestamp`.
- `archive_entries(archive_id, path, parent, name, kind, offset, chunk, length,
  size, method, preload_offset, preload_length, assettype)` — primary key
  `(archive_id, path)`, `WITHOUT ROWID`, indexed by
  `(archive_id, parent, name)`.

Directories are materialized as rows, so listing is a parent lookup. `size` is
the logical file size, `length` the stored byte count, `method` is 0 (stored),
8 (DEFLATE), or 14 (LZMA), and `assettype` is one of `Unknown`, `Model`,
`Material`, `Texture`, `Sound`, `Map`, `Script`.

## Limitations

- Archive mounts are Linux/FUSE3 only; the Windows backend currently mounts the
  manifest volume, not the archive index.
- FUSE `open` materializes the whole logical file, so opening a very large entry
  allocates its full size. The decoded-file cache is 256 MB; legacy `.bin`
  payloads are pinned separately.
- `readdir` ignores offsets, so very large directories are served in one fill
  pass.
- Synthetic `stat` reports `st_nlink = 2` for directories and `df` reports all
  blocks used.
- The access layer requires C++23.

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
src/parsers/              VPK, BSP, GMA, Steam, and gameinfo readers
src/index/                archive indexer, SQLite schema, runtime providers
src/mount/                read-only FUSE frontend over the index providers
tests/                    index watcher and GMA metadata integration tests
```

Adding a file to the manifest volume means extending `snapshot()` in
`src/core/volume.cpp`; both backends pick it up without changes.
