"""Sphinx configuration for the sauce-fs documentation.

Doxygen extracts the public headers into XML and Breathe renders those
compounds inside Sphinx. CMake supplies the XML location and version through
the environment so the same file works for the root build and the standalone
docs project.
"""

from __future__ import annotations

import os
from datetime import datetime
from pathlib import Path

SOURCE_DIR = Path(__file__).resolve().parent
CHECKOUT = SOURCE_DIR.parent

project = "sauce-fs"
author = "Elijah Cunningham"
copyright = f"{datetime.now().year}, Elijah Cunningham"

version = os.environ.get("SAUCE_FS_VERSION", "")
release = version or "0.1.0"

extensions = ["breathe"]

# CMake exports the XML path. The fallback keeps a manual
# `sphinx-build docs build/docs/html` working after `just docs-doxygen`.
_doxygen_xml = Path(
    os.environ.get(
        "SAUCE_FS_DOXYGEN_XML",
        CHECKOUT / "build" / "docs" / "doxygen" / "xml",
    )
)
breathe_projects = {"sauce-fs": str(_doxygen_xml)}
breathe_default_project = "sauce-fs"

exclude_patterns = ["_build", "Thumbs.db", ".DS_Store"]

html_theme = "furo"
html_title = f"sauce-fs {release}" if version else "sauce-fs"
html_baseurl = "https://elicunninghamdev.github.io/sauce-fs/"

highlight_language = "cpp"
primary_domain = "cpp"
