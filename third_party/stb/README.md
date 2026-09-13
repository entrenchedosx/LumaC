# stb_image (vendored)

Single-file image decoder in C (PNG, JPEG, and others).

- Upstream: https://github.com/nothings/stb (`stb_image.h`)
- Revision: master as of 2026-09-12 (file unmodified, 283010 bytes)
- License: MIT (see `LICENSE` in this directory; dual
  MIT/public-domain, used here under MIT)
- Used by: `assets/src/stb_image_impl.c` (sole
  `STB_IMAGE_IMPLEMENTATION` translation unit) and
  `assets/src/texture.c` (decode API use only)

No package-manager, network, or OS image-API dependency: builds are
reproducible from this checkout alone.
