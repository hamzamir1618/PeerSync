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
