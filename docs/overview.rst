Overview
========

The index is a SQLite database with one row per installed game, one row per
archive, and one row per file *and directory* inside each archive. Loose files
below each game root are recorded separately with their metadata; their bytes
are read from disk on demand. Directories are materialized so ``readdir()``
becomes an equality lookup on ``(archive_id, parent, name)`` instead of a
prefix scan. No inode/dentry split is needed: the namespace is read-only, and
archives cannot express hard links or renames.

The same three primitives serve every consumer:

- :cpp:func:`sauce::fs::core::Provider::get` for random access by path,
- :cpp:func:`sauce::fs::core::Provider::list` for folder browsing,
- :cpp:func:`sauce::fs::core::Provider::stat` for metadata.

Merged game views
-----------------

:cpp:class:`sauce::fs::core::IndexDatabase` can layer a game's content in the
order the engine would use:

- :cpp:func:`sauce::fs::core::IndexDatabase::engine_game_provider` follows the
  ``SearchPaths`` block from ``gameinfo.txt``. Earlier entries win, directory
  entries contribute loose files, VPK entries contribute archives, and map
  pakfiles are appended as the lowest-precedence layers.
- :cpp:func:`sauce::fs::core::IndexDatabase::garrysmod_game_provider` puts
  loose addon directories and workshop addons above local addons, then engine
  content, then games listed in ``cfg/mount.cfg``.
- :cpp:func:`sauce::fs::core::IndexDatabase::ordered_game_provider` selects one
  of these with :cpp:enum:`sauce::fs::core::GameOrder`; ``Auto`` uses Garry's
  Mod rules for a Garry's Mod row and engine search paths otherwise.

Search paths and Garry's Mod mount entries are persisted alongside the game
rows, so consumers do not need to parse ``gameinfo.txt`` or ``mount.cfg`` at
runtime. Loose-file metadata is refreshed by watch mode alongside archive
entries.

Building the index
------------------

.. code-block:: sh

   cmake --preset linux
   cmake --build --preset linux
   ./build/linux/sauce-fs-index --database index.db "/path/to/Half-Life 2"

Use ``--watch`` to keep the database current. Only archives whose size or
last-write time changed are hashed again, and a rewrite that produces the same
XXH3 digest keeps its existing entry rows. Updates use SQLite WAL, so a
long-lived reader can serve the previous snapshot while the watcher commits a
new one.

With no game folders, installed Source titles are discovered through Steam.
Garry's Mod additionally gets a GMA and legacy ``.bin`` addon crawler.

Embedding the library
---------------------

The core installs its public headers and a CMake package:

.. code-block:: sh

   cmake --install build/linux --prefix "$HOME/.local"

.. code-block:: cmake

   find_package(sauce-fs REQUIRED)
   target_link_libraries(my-tool PRIVATE sauce::core)

.. code-block:: cpp

   #include <sauce/core/core.hpp>

   using namespace sauce::fs::core;

   auto database = IndexDatabase::open("index.db");
   if (!database)
   {
       return 1;
   }
   auto root = (*database)->mount_provider();
   if (!root)
   {
       return 1;
   }
   auto games = (*root)->list("/");
   if (!games)
   {
       return 1;
   }
   for (const DirectoryEntry& game : *games)
   {
       // ...
   }

Only ``include/sauce/core/`` is public. Parsers, SQL text, SQLite helpers, and
the watcher implementation stay private to the library target, so the public
headers depend on nothing beyond the standard library.

To keep the index current inside an existing event loop, start the watcher on a
background thread and read semantic events:

.. code-block:: cpp

   ArchiveIndexWatcher watcher(options, watch);
   watcher.start();
   while (const auto event = watcher.waitEvent())
   {
       // IndexReady, ArchiveChanged(event->path), IndexUpdated, ...
   }
   watcher.stop();

Events are published only after a successful SQLite commit. The queue is bounded
by :cpp:member:`sauce::fs::core::ArchiveWatchOptions::max_queued_events`, which
reports overflow through an ``EventsDropped`` marker while never dropping
``IndexReady``, ``Failed``, or ``Stopped``.
:cpp:func:`sauce::fs::core::ArchiveIndexWatcher::run` remains available for
callers that simply want to block.

Logging
-------

The library never writes to stdout or stderr. Diagnostics go through
:cpp:struct:`sauce::fs::core::LogOptions`, carried by
:cpp:member:`sauce::fs::core::ArchiveIndexOptions::log` and
:cpp:member:`sauce::fs::fuse::ArchiveMountOptions::log`. An empty callback
means silent; the bundled command-line tools install a console sink.

Formatting is skipped when the callback is empty or a message is below the
configured level. Configure with ``-DSAUCE_FS_LOGGING=OFF`` to compile the call
sites and their formatting out entirely; the ``LogOptions`` type remains so the
API does not change.

Mounting
--------

.. code-block:: sh

   ./build/linux/sauce-fs-mount --database index.db /mnt/sauce-full

The mount root contains one directory per game. Below a game are its archive
trees and its loose files. Archive data is read on demand from the original
files; loose files are read directly from disk. Reads inflate DEFLATE and LZMA
payloads on demand from the original archive files.

The mount is read-only, but reported permission bits are configurable.
``--file-mode 0644 --dir-mode 0755`` keeps files copied out of the mount
editable by their owner.

The ``justfile`` wraps the whole workflow:

.. code-block:: sh

   just up      # build, index every discovered game, watch, and mount
   just status  # watcher/mount PIDs, database path, log directory
   just logs    # tail the watcher and mount logs
   just stop    # unmount and stop the watcher

Building this site
------------------

The site is generated with Doxygen, Breathe, and Sphinx:

.. code-block:: sh

   python3 -m venv build/docs-venv
   build/docs-venv/bin/pip install -r docs/requirements.txt
   cmake -S docs -B build/docs \
       -DSPHINX_EXECUTABLE="$PWD/build/docs-venv/bin/sphinx-build"
   cmake --build build/docs --target docs

The result is ``build/docs/html/index.html``. ``just docs`` runs the same
sequence using an activated virtualenv, or the path in ``SPHINX_EXECUTABLE``.

GitHub Actions builds the site on every push to ``main`` and deploys it to
GitHub Pages.
