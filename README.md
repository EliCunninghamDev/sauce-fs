# sauce-fs

Read-only access to Source engine content archives: VPK `_dir` trees and side
archives, embedded BSP pakfiles, GMA addons, and legacy `.bin` addons. Entry
metadata is recorded in SQLite and read back as raw bytes, inflating DEFLATE
and LZMA payloads on demand.

The project is split into two layers:

- **`core`** — a reusable indexing library (`sauce_fs_core`) that discovers
  installed games, scans archives, writes the SQLite index, watches for
  changes, and exposes the runtime provider API. Nothing in this layer depends
  on a mount backend, so it can be embedded in another application.
- **`fuse`** — the filesystem layer (`sauce_fs_fuse`), a read-only FUSE3 mount
  that walks the index through the provider API.

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

## Prerequisites

- CMake 3.24+, Ninja, a C++23 compiler (`std::expected` is used by the archive
  access layer)
- vcpkg, with `VCPKG_ROOT` exported:
  ```sh
  export VCPKG_ROOT=$HOME/vcpkg
  ```
  The checked-in presets read only this environment variable. Keep
  machine-specific paths in a `CMakeUserPresets.json` (git-ignored, loaded
  automatically) that inherits from `linux`.
- Linux, for the mount: `libfuse3-dev` and `/dev/fuse`

## Build

```sh
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
```

Targets:

| Target | What it is |
| --- | --- |
| `sauce_fs_core` | Reusable indexing library: discovery, scanning, SQLite, providers |
| `sauce_fs_fuse` | Read-only FUSE frontend over the index providers |
| `sauce-fs-index` | Archive scanner and indexer |
| `sauce-fs-get` | Reference consumer: get/list/stat/resolve |
| `sauce-fs-mount` | Read-only FUSE mount over the index |

### Quick start

With [just](https://github.com/casey/just) installed, the full workflow is one
command:

```sh
just up
```

This builds the project, indexes every discovered Source game into
`build/linux/sauce-full.db`, keeps the index fresh with a watcher, and mounts
it at `/mnt/sauce-full`. `just up` is idempotent: re-running it unmounts the
old mount, stops the previous watcher, and builds everything again.

```sh
just status   # what is running and where the database and logs live
just logs     # tail the indexer and mount logs
just stop     # unmount and stop the watcher
just index    # rebuild the index once without starting the watcher
just mount    # remount the existing index
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

Files are `0444` and directories `0555` by default; both are always read-only
through the mount. `--file-mode` and `--dir-mode` change only the reported
bits, which matters when copying out because `cp` makes the copy inherit the
source mode. `just up` mounts with `0644`/`0755` so extracted files stay
editable.

Because archives are namespaced per
archive rather than merged per game, colliding paths stay distinct — for
example `half-life-2/maps/c1a0/materials/...` and
`half-life-2/hl2_misc/materials/...`.

### API

`src/core/archive_access.hpp` exposes a `Provider` with three primitives:

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

Everything in the indexing library lives in `sauce::fs::core`. The FUSE layer
lives in `sauce::fs::fuse` and only depends on the public `core` API.

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

- The mount is Linux/FUSE3 only; `sauce_fs_core` itself is portable.
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
CMakeLists.txt            dependency discovery and targets
CMakePresets.json         vcpkg-based configure/build/test presets
justfile                  one-command build, full index, and mount workflow
vcpkg.json                manifest of ported dependencies
src/core/                 indexing library: discovery, scanning, SQLite, providers
src/fuse/                 read-only FUSE frontend over the core providers
src/parsers/              VPK, BSP, GMA, Steam, and gameinfo readers
src/util/format.hpp       fmt wrapper shared by the CLIs and the mount
tests/                    index watcher and GMA metadata integration tests
```
