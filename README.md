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

## Build Instructions

### Prerequisites

- A C++17 compatible compiler (GCC, Clang, or MSVC)
- CMake 3.15 or higher

### Building from Source

```bash
# Clone the repository
git clone https://github.com/yourusername/peersync.git
cd peersync

# Configure the build
cmake -S . -B build

# Build the project
cmake --build build

# Run unit tests
ctest --test-dir build --output-on-failure
```

### CMake Options

- `PEERSYNC_BUILD_TESTS`: Build unit tests (Default: `ON`)
- `PEERSYNC_BUILD_GUI`: Build GUI frontend (Default: `ON`)
