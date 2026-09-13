# cgltf (vendored)

Single-file glTF 2.0 parser in C99.

- Upstream: https://github.com/jkuhlmann/cgltf
- Version: **v1.15** (exact tag; file `cgltf.h` unmodified, 202865 bytes)
- License: MIT (see `LICENSE` in this directory)
- Used by: `assets/src/cgltf_impl.c` (sole `CGLTF_IMPLEMENTATION`
  translation unit) and `assets/src/gltf_import.c` (API use only)

No package-manager or network dependency: builds are reproducible
from this checkout alone. Custom file I/O is wired through
`cgltf_options.file` to the asset filesystem helper (`la_fs_*`),
so all file access stays centralized.
