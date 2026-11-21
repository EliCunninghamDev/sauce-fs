# sauce-fs

Read-only access to Source engine content archives and loose game files: VPK
`_dir` trees and side archives, embedded BSP pakfiles, GMA addons, legacy `.bin`
addons, and files stored directly below a game root. Entry metadata is recorded
in SQLite and read back as raw bytes, inflating DEFLATE and LZMA payloads on
demand.

The project is split into two layers:

- **`core`** — a reusable indexing library (`sauce_fs_core`) that discovers
  installed games, scans archives, writes the SQLite index, watches for
  changes, and exposes the runtime provider API. Its public headers live in
  `include/sauce/core/`, and the build installs it as a CMake package. Nothing
  in this layer depends on a mount backend, so it can be embedded in another
  application.
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
| `fmt` | Type-safe formatting for CLI output and library diagnostics |

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
| `sauce_fs_core` / `sauce::core` | Reusable indexing library: discovery, scanning, SQLite, providers |
| `sauce_fs_fuse` / `sauce::fuse` | Read-only FUSE frontend over the index providers |
| `sauce-fs-index` | Archive scanner and indexer |
| `sauce-fs-get` | Reference consumer: get/list/stat/resolve |
| `sauce-fs-mount` | Read-only FUSE mount over the index |

### Using the index library

The core installs headers, a static library, and a CMake package:

```sh
cmake --install build/linux --prefix "$HOME/.local"
```

```cmake
find_package(sauce-fs REQUIRED)
target_link_libraries(my-tool PRIVATE sauce::core)
```

```cpp
#include <sauce/core/core.hpp>   // archive_access.hpp, archive_index.hpp, log.hpp

auto database = sauce::fs::core::IndexDatabase::open("index.db");
```

Only `include/sauce/core/` is public. Parsers, SQL text, SQLite helpers, and
the watcher implementation stay private to the library target, so the public
headers depend on nothing beyond the standard library.

Diagnostics never write to stdout or stderr from inside the library. Pass a
`LogOptions` callback through `ArchiveIndexOptions::log` (or
`ArchiveMountOptions::log` for the FUSE layer); the bundled CLIs use the
console sink in `src/util/console_log.hpp`. Formatting is skipped entirely when
the callback is empty or the message is below the level. Configure with
`-DSAUCE_FS_LOGGING=OFF` to compile the call sites and their formatting out;
the `LogOptions` type remains so the API does not change.

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
shift when an archive appears) is updated transactionally. Loose files are
diffed by path, size, and timestamp on the same pass. Archives are never
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

Files that are not archive containers are indexed as loose entries with their
metadata and materialized parent directories. Their bytes are read directly
from disk, so edits do not require reindexing. Watch mode diffs loose entries
alongside archives.

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
`--resolve` exercises the raw-pointer resolver. `--order` selects the merge
policy: `auto` (default), `engine`, `garrysmod`, or `index`:

```sh
./build/linux/sauce-fs-get --database index.db --game half-life-2 --list materials
./build/linux/sauce-fs-get --database index.db --game half-life-2 --order engine --list materials
./build/linux/sauce-fs-get --database index.db --resolve half-life-2/hl2/materials/foo.vmt
```

The default view is `game/path`: archive internals appear as
`game/archive-mount-path/internal-path`, and loose files appear at their real
`game/path`. With `--game` paths are relative to the game root. `engine`
follows the game's `gameinfo.txt` `SearchPaths`; `garrysmod` puts loose addons
and workshop content above local addons and applies `cfg/mount.cfg`. Both
include loose files.

### Mount

`sauce-fs-mount` mounts the index read-only. The root is `sourcegames`, then one
directory per game slug. Below a game are its archive trees and its loose files;
archive internals follow `archive-mount-path/internal-path`:

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

`include/sauce/core/archive_access.hpp` exposes a `Provider` with three
primitives:

```cpp
Result<NodeInfo> stat(std::string_view path) const;
Result<std::vector<DirectoryEntry>> list(std::string_view path) const;
Result<Blob> get(std::string_view path) const;   // Blob = shared_ptr<const Bytes>
```

`get` returns `std::expected<Blob, Error>`. Providers compose:

- `ArchiveProvider` — one archive; paths are archive-internal.
- `LooseFileProvider` — loose files below a game root; bytes come from disk.
- `LayeredProvider` — ordered overlay; first layer wins for `stat`/`get`, and
  `list` unions the layers. This is where merge resolution lives.
- `TreeProvider` — synthetic directories for game and archive levels.
- `IndexDatabase` — opens the database, owns the decoded-file cache, and builds
  providers through `archive_provider()`, `game_provider(slug, order)`,
  `engine_game_provider()`, `garrysmod_game_provider()`,
  `ordered_game_provider()`, and `mount_provider()`.

`engine_game_provider()` layers content in ``gameinfo.txt`` search-path order;
`garrysmod_game_provider()` applies loose addons, workshop addons, local
addons, engine content, and `cfg/mount.cfg` games in that order. `GameOrder`
selects a policy explicitly, and `Auto` picks Garry's Mod rules for a Garry's
Mod row.

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

### Watching for changes

`ArchiveIndexWatcher` keeps the database current. `run(result)` builds once and
then blocks; that is what the CLI uses. Embedders that own an event loop can
call `start()` and consume semantic events instead:

```cpp
ArchiveIndexWatcher watcher(options, watch);
watcher.start();

for (;;)
{
    const auto event = watcher.waitEvent();
    if (!event)
    {
        break;   // stopped and the queue is empty
    }
    switch (event->kind)
    {
    case WatchEventKind::IndexReady:     /* build caches now */ break;
    case WatchEventKind::ArchiveChanged: /* invalidate event->path */ break;
    case WatchEventKind::Failed:         /* report event->message */ break;
    default: break;
    }
}
watcher.stop();
```

`start()` is asynchronous: `IndexReady` means the initial full build and the
post-watch reconcile are both committed, so a consumer does not need a separate
one-shot build. Each incremental commit publishes per-archive `ArchiveAdded`,
`ArchiveChanged`, and `ArchiveRemoved` events, followed by an `IndexUpdated`
summary. Events are emitted only after a successful SQLite commit and carry
values, so no watcher lock is held while a consumer processes one.

The queue is bounded by `ArchiveWatchOptions::max_queued_events`; on overflow,
repeated `(kind, path)` events are coalesced, one `EventsDropped` marker is
queued, and generations show the gap. `IndexReady`, `Failed`, and `Stopped` are
always delivered. `pollEvent()`, `waitEvent()`, `drainEvents()`, and `running()`
are safe to call from any thread.

Everything in the indexing library lives in `sauce::fs::core`. The FUSE layer
lives in `sauce::fs::fuse` and only depends on the public `core` API.

### Database schema

- `games(id, slug, title, game_path, gameinfo, garrys_mod)` — one row per
  detected game. `slug` is derived from the title (`half-life-2`) because
  folder names collide. `gameinfo` records the parsed file, and `garrys_mod`
  selects the addon ordering policy.
- `game_search_paths(game_id, ordinal, types, raw, resolved, kind)` — resolved
  `SearchPaths` entries in file order; kind is `dir`, `vpk`, `file`, or
  `missing`.
- `game_mounts(game_id, ordinal, name, path, source)` — Garry's Mod
  `cfg/mount.cfg` entries.
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
- `loose_entries(root, key, path, parent_key, name, kind, size, modified,
  assettype)` — primary key `(root, case-folded key)`, `WITHOUT ROWID`,
  indexed by `(root, parent_key, name)`. `path` preserves the on-disk case;
  `key` is used for case-insensitive lookups.

Directories are materialized as rows, so listing is a parent lookup. `size` is
the logical file size, `length` the stored byte count, `method` is 0 (stored),
8 (DEFLATE), or 14 (LZMA), and `assettype` is one of `Unknown`, `Model`,
`Material`, `Texture`, `Sound`, `Map`, `Script`.

## Documentation

The API reference and guide are built with Doxygen, Breathe, and Sphinx:

```sh
python3 -m venv build/docs-venv
build/docs-venv/bin/pip install -r docs/requirements.txt
just docs        # needs doxygen; uses sphinx-build from PATH or the venv
```

The site is written to `build/docs/html/index.html`. `docs/` is also a
standalone CMake project, so CI can build it without the vcpkg toolchain:

```sh
cmake -S docs -B build/docs -DSPHINX_EXECUTABLE="$PWD/build/docs-venv/bin/sphinx-build"
cmake --build build/docs --target docs
```

Every push to `main` builds the same site in GitHub Actions and publishes it to
<https://elicunninghamdev.github.io/sauce-fs/>. The job lives in
`.github/workflows/docs.yml`; select "GitHub Actions" as the Pages source once
in the repository settings.

## Limitations

- The mount is Linux/FUSE3 only; `sauce_fs_core` itself is portable.
- Loose files are indexed below each game root. Search-path directories outside
  every indexed game (for example a shared `platform/` folder) are recorded but
  have no loose rows to serve.
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
cmake/                    installed package config and consumer test driver
docs/                     Doxygen/Breathe/Sphinx guide and API reference
justfile                  one-command build, index, mount, and docs workflow
vcpkg.json                manifest of ported dependencies
include/sauce/core/       public indexing library headers
src/core/                 indexing library implementation and private headers
src/fuse/                 read-only FUSE frontend over the core providers
src/parsers/              VPK, BSP, GMA, Steam, and gameinfo readers
src/util/                 fmt and console logging helpers shared by the CLIs
tests/                    index watcher, GMA metadata, and package consumption tests
.github/workflows/        GitHub Pages documentation build
```
