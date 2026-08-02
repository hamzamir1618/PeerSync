# Vendored Third-Party Libraries

This directory contains single-header and drop-in libraries that are directly vendored into the repository.

## mjansson/mdns (`mdns.h`)

- **Source URL**: [https://github.com/mjansson/mdns](https://github.com/mjansson/mdns)
- **License**: Public Domain / Unlicense
- **Pinned Version**: Tag `1.4.3` (Commit hash: `1727be0602941a714cb6048a737f0584b1cebf3c`)
- **Purpose**: Cross-platform mDNS and DNS-SD library in C. Used by libpeersync for automatic peer discovery on the local network without requiring external daemon dependencies.

## tinyfiledialogs (`tinyfiledialogs.h`, `tinyfiledialogs.c`)

- **Source URL**: [http://tinyfiledialogs.sourceforge.net](http://tinyfiledialogs.sourceforge.net) / [https://sourceforge.net/projects/tinyfiledialogs/](https://sourceforge.net/projects/tinyfiledialogs/)
- **License**: zlib License
- **Pinned Version**: `v2.9.3`
- **Purpose**: Cross-platform C/C++ library for native file open, save, and folder selection dialogs. Used by the GUI frontend for file and directory selection without requiring heavy GUI frameworks.

## Font Awesome 6 Free Solid (`fa-solid-900.ttf` / `IconsFontAwesome6.h`)

- **Source URL**: [https://fontawesome.com/](https://fontawesome.com/) and [https://github.com/juliettef/IconFontCppHeaders](https://github.com/juliettef/IconFontCppHeaders)
- **License**: 
  - Icons (Glyphs): CC BY 4.0 License (Attribution required)
  - Fonts: SIL OFL 1.1 License
  - `IconsFontAwesome6.h`: zlib License
- **Pinned Version**: 6.5.2 (Free)
- **Purpose**: Provides highly scalable vector icons for the GUI frontend. The TTF is compressed into a C-array (`fa_solid_900_compressed.h`) and embedded at compile-time to guarantee rendering consistency across deployments.

## Cryptographic Algorithms (SHA-256 / HMAC / PBKDF2)

- **Status**: Implemented directly in `src/core/pairing.cpp` (no third-party dependency vendored).
- **Purpose**: Provides standard SHA-256 (RFC 6234), HMAC-SHA256 (RFC 2104), and PBKDF2 (RFC 2898) for PIN verification and session key derivation. Implemented directly as a compact, self-contained C++17 module to eliminate dependency overhead and licensing complexity while maintaining mathematical compliance with standard RFC test vectors.
