API reference
=============

The reference is generated from the public headers in ``include/sauce/core/``.
Everything else is an implementation detail of the static library.

Results and errors
------------------

.. doxygenenum:: sauce::fs::core::ErrorCode

.. doxygenstruct:: sauce::fs::core::Error
   :members:

.. doxygentypedef:: sauce::fs::core::Result

.. doxygentypedef:: sauce::fs::core::Bytes
.. doxygentypedef:: sauce::fs::core::Blob

Providers
---------

.. doxygenclass:: sauce::fs::core::Provider
   :members:

.. doxygentypedef:: sauce::fs::core::ProviderPtr

.. doxygenstruct:: sauce::fs::core::NodeInfo
   :members:

.. doxygenstruct:: sauce::fs::core::DirectoryEntry
   :members:

.. doxygenstruct:: sauce::fs::core::ResolvedEntry
   :members:

.. doxygenclass:: sauce::fs::core::TreeProvider
   :members:

.. doxygenclass:: sauce::fs::core::LayeredProvider
   :members:

.. doxygenclass:: sauce::fs::core::ArchiveProvider
   :members:

Index database
--------------

.. doxygenclass:: sauce::fs::core::IndexDatabase
   :members:

Building an index
-----------------

.. doxygenenum:: sauce::fs::core::ArchiveVariant

.. doxygenenum:: sauce::fs::core::AssetType

.. doxygenstruct:: sauce::fs::core::GameRecord
   :members:

.. doxygenstruct:: sauce::fs::core::ArchiveRecord
   :members:

.. doxygenstruct:: sauce::fs::core::ArchiveContent
   :members:

.. doxygenstruct:: sauce::fs::core::ArchiveIndexOptions
   :members:

.. doxygenstruct:: sauce::fs::core::ArchiveIndexSummary
   :members:

.. doxygenstruct:: sauce::fs::core::ArchiveIndexResult
   :members:

.. doxygenfunction:: sauce::fs::core::build_archive_index

.. doxygenfunction:: sauce::fs::core::archive_variant_name
.. doxygenfunction:: sauce::fs::core::archive_type_id
.. doxygenfunction:: sauce::fs::core::asset_type_name

Watching for changes
--------------------

.. doxygenstruct:: sauce::fs::core::ArchiveWatchOptions
   :members:

.. doxygenclass:: sauce::fs::core::ArchiveIndexWatcher
   :members:

Logging
-------

.. doxygenenum:: sauce::fs::core::LogLevel

.. doxygentypedef:: sauce::fs::core::LogCallback

.. doxygenstruct:: sauce::fs::core::LogOptions
   :members:

FUSE mount
----------

The mount layer is Linux-only and is not part of the installed ``core``
package. Its public entry points are documented here because they are the
intended way to run the filesystem layer.

.. doxygenstruct:: sauce::fs::fuse::ArchiveMountOptions
   :members:

.. doxygenfunction:: sauce::fs::fuse::run_archive_mount
