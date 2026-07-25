# peersync

[![CI](https://github.com/<owner>/<repo>/actions/workflows/ci.yml/badge.svg)](https://github.com/<owner>/<repo>/actions/workflows/ci.yml)
<!-- NOTE: Replace <owner>/<repo> in the badge URL above with your actual GitHub username and repository name -->

> **Status: early development**

peersync finds another instance of itself on the local network automatically, transfers files directly with no internet, resumes interrupted transfers, and sends only the changed parts of a file.

## Features

- [x] Automatic local network peer discovery (mDNS / DNS-SD)
- [x] PIN-based pairing before first sync
- [x] Direct peer-to-peer file transfer without internet connection
- [x] Resumable transfers for interrupted downloads and uploads
- [x] Delta synchronization (transfers only changed file parts)
- [x] Directory synchronization (with Last-Write-Wins conflict resolution by modification timestamp)
- [x] Command-line interface (CLI)
- [ ] Graphical user interface (GUI)

## How Pairing Works

Before any file transfer or directory synchronization can occur between two discovered devices, **peersync** requires a one-time PIN-based authorization handshake:

1. **PIN Generation & Display**: The device listening for incoming sync requests generates a random 6-digit numeric PIN (e.g., `042918`) and displays it on screen.
2. **Out-of-Band Entry**: The user on the initiating device is prompted to enter the displayed PIN into their CLI or GUI prompt.
3. **Cryptographic Handshake**: Both peers independently derive a session key via PBKDF2-HMAC-SHA256 and perform an HMAC challenge-response handshake (`PairChallenge` -> `PairResponse` -> `PairResult`). The PIN itself is never transmitted over the wire.

## Directory Sync & Conflict Policy

When synchronizing entire repository trees, **peersync** exchanges sorted directory manifests and classifies files using a two-stage filter:
1. **Identical Cheap Filter**: Files with identical size and modification timestamp (`mtime`) are skipped immediately without network IO or signature calculation.
2. **Conflict Resolution Policy (Last-Write-Wins)**: When files differ in content or timestamp across peers, conflicts are resolved deterministically by comparing modification timestamps:
   - The version with the **newer modification timestamp** automatically overwrites the older version via rolling-checksum delta transfer.
   - All transfers within a synchronization session execute concurrently across a bounded pool of worker sockets (defaulting to 4 concurrent worker threads).


## CLI Usage Examples

The `peersync` command-line interface provides intuitive subcommands for local network peer discovery and synchronization.

### 1. Listen for Peers (`listen`)
Start an mDNS service advertiser and TCP listener on a specified device name and port. If `--name` is omitted, the local machine's hostname is used. If `--port 0` is specified, an ephemeral port is assigned automatically.

```bash
# Listen with a custom device name on an ephemeral port
peersync listen --name my-laptop --port 0
```
**Sample Output:**
```
Listening as my-laptop on port 54321, waiting for peers...
```
Pressing `Ctrl+C` cleanly shuts down the mDNS service announcement and listening socket without hanging or killing the process abruptly.

### 2. Discover Peers (`discover`)
Search the local network for advertised `peersync` instances over mDNS/DNS-SD for a specified timeout duration (defaulting to 3 seconds).

```bash
# Browse for local peers for 5 seconds
peersync discover --timeout 5
```
**Sample Output:**
```
Browsing for peers for 5 seconds...

Discovered Peers:
Name                            IP Address          Port      
--------------------------------------------------------------
my-laptop                       192.168.1.50        54321     
desktop-workstation             192.168.1.102       48192     
--------------------------------------------------------------
```


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

## Contributing

We welcome contributions! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for details on pull request requirements, testing guidelines, and local development.
