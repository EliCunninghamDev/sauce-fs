#pragma once

#include "core/manifest.h"

namespace sauce::fs
{

// Mount the volume. argv[0] is the mount point; the rest is forwarded to the
// platform mount layer (FUSE options on Linux).
int mount(int argc, char** argv);

// Verifies the platform mount backend is present and usable.
Check backend_check();

} // namespace sauce::fs
