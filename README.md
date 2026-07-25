# peersync

> **Status: early development**

peersync finds another instance of itself on the local network automatically, transfers files directly with no internet, resumes interrupted transfers, and sends only the changed parts of a file.

## Features

- [ ] Automatic local network peer discovery (mDNS / UDP broadcast)
- [ ] Direct peer-to-peer file transfer without internet connection
- [ ] Resumable transfers for interrupted downloads and uploads
- [ ] Delta synchronization (transfers only changed file parts)
- [ ] Command-line interface (CLI)
- [ ] Graphical user interface (GUI)

## Dependencies

### Vendored (in `third_party/`)
- **mdns.h**: Single-header mDNS/DNS-SD library for local network peer discovery (Public Domain, pinned to tag `1.4.3`)
- **tinyfiledialogs**: Cross-platform native file and folder selection dialog library for the GUI (zlib License, pinned to `v2.9.3`)

### External (planned, via FetchContent)
- **GoogleTest**: C++ testing framework for unit tests
- **CLI11**: Command-line parser for the CLI frontend
- **Dear ImGui**: Immediate-mode GUI library for the GUI frontend
- **GLFW**: Windowing and input handling for Dear ImGui

## Build Instructions

### Prerequisites

- A C++17 compatible compiler (GCC, Clang, or MSVC)
- CMake 3.15 or higher

### Building from Source

```bash
# Clone the repository
git clone https://github.com/yourusername/peersync.git
cd peersync

# Configure the build with unit tests enabled
cmake -S . -B build -DPEERSYNC_BUILD_TESTS=ON

# Build the project
cmake --build build

# Run unit tests
ctest --test-dir build
```

To configure, build, and test end-to-end in a single command:
```bash
cmake -S . -B build -DPEERSYNC_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build
```

### CMake Options

- `PEERSYNC_BUILD_TESTS`: Build unit tests (Default: `ON`)
- `PEERSYNC_BUILD_GUI`: Build GUI frontend (Default: `ON`)
