sauce-fs
========

Read-only access to Source engine content archives. VPK directory trees and
their side archives, embedded BSP pakfiles, GMA addons, and legacy ``.bin``
addons are indexed into SQLite and served through one provider API.

The project is split into two layers:

``core``
    A reusable indexing library (``sauce_fs_core`` / ``sauce::core``). It
    discovers installed games, scans archives, writes the SQLite index, watches
    for changes, and exposes the runtime provider API. Nothing in this layer
    depends on a mount backend, so it can be embedded in another program.

``fuse``
    A read-only FUSE3 mount (``sauce_fs_fuse`` / ``sauce::fuse``) that walks the
    index through the provider API.

.. toctree::
   :maxdepth: 2
   :caption: Guide

   overview

.. toctree::
   :maxdepth: 2
   :caption: Reference

   api
