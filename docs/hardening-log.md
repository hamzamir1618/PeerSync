# PeerSync Hardening Log

This document records security hardening and defensive engineering enhancements applied to PeerSync, particularly around low-level memory management, network serialization, and algorithmic boundary conditions identified during ASan/UBSan code inspection and testing.

## 1. Network Socket Buffer Handling (`src/core/socket.cpp`)
- **Null Buffer Guarding**: Added early pointer validation in `TcpSocket::send` and `TcpSocket::recv`. Calling either method with a non-zero length and a `nullptr` buffer now immediately throws `PeerSyncNetworkException`, preventing undefined behavior when calling OS socket APIs (`send`/`recv`).
- **Syscall Chunk Sizing**: Capped individual OS `send` and `recv` calls to a maximum chunk size of 64 MB (`std::min<size_t>(..., 1024 * 1024 * 64)`). This prevents integer overflow and truncation on 64-bit platforms when downcasting `size_t` buffer lengths to signed 32-bit `int` (WinSock) or `ssize_t` (POSIX).

## 2. Message Framing & Protocol Serialization (`src/core/message_framing.cpp`, `src/core/protocol.cpp`)
- **Framed Payload Guards**: In `sendFramedMessage`, verified that payload data pointers are non-null before attempting transmission when payload size > 0. In `recvExactly`, added zero-length early returns and null-buffer checks before initiating socket reads.
- **Null-Pointer String/Vector UB Prevention**: In `BinaryReader::readString` and `BinaryReader::readBytes`, added explicit checks for `len == 0` returning empty strings/vectors immediately. In standard C++17, constructing `std::string(nullptr, 0)` or iterating from `nullptr` is undefined behavior; this early return guarantees safe construction when reading zero-length payloads from network buffers.
- **Overflow-Proof Bounds Checking**: Changed protocol buffer boundary checks from `m_offset + len > m_data.size()` to `len > m_data.size() - m_offset` across all primitive and compound readers (`readU16`, `readU32`, `readU64`, `readString`, `readBytes`). If a corrupted or malicious peer sends an oversized length prefix (e.g., `UINT32_MAX`), adding `m_offset + len` could wrap around on 32-bit architectures or truncated calculations; subtracting from `m_data.size()` eliminates integer overflow risks.
- **Serialization Size Validation**: Added checks in `writeString` and `writeBytes` asserting that container sizes do not exceed `UINT32_MAX` before truncating sizes to 32-bit length prefixes.

## 3. Rolling Checksum & Delta Window Arithmetic (`src/core/delta.cpp`)
- **Rolling Window Boundary Safe Condition**: In `computeDelta`, changed the rolling window continuation check from `i + blockSize < fileData.size()` to `fileData.size() - i > blockSize`. When `i` approaches the end of a very large buffer near `SIZE_MAX`, adding `i + blockSize` can cause unsigned integer wrap-around. Using subtraction guarantees safe index evaluation.
- **Copy Instruction Multiplication Overflow Guard**: In both `reconstructFile` (`src/core/delta.cpp`) and `receiveFile` (`src/core/transfer.cpp`), added explicit guards before calculating byte offsets from block indices:
  ```cpp
  if (blockSize > 0 && inst.blockIndex > UINT64_MAX / blockSize) {
      throw PeerSyncDeltaException("Copy instruction block index arithmetic overflow");
  }
  ```
  This prevents malicious or corrupted `DeltaInstruction::Copy` instructions from causing 64-bit integer overflow during byte offset calculation (`inst.blockIndex * blockSize`), which could bypass subsequent `offset >= oldFileSize` bounds checks.

## 4. Continuous Integration Sanitizer Job (`.github/workflows/ci.yml`)
- **ASan / UBSan CI Integration**: Added an automated GitHub Actions job (`asan-ubsan`) running on `ubuntu-latest` with `-DCMAKE_BUILD_TYPE=Debug` and `-DPEERSYNC_ENABLE_SANITIZERS=ON`.
- **Runtime Configuration**: Configured environment variables `ASAN_OPTIONS="detect_leaks=1:abort_on_error=1"` and `UBSAN_OPTIONS="print_stacktrace=1:abort_on_error=1"` to ensure CI jobs fail fast with stack traces upon any detection of memory errors, undefined behavior, or memory leaks.
- **Test Label Filtering**: Excluded `requires_multicast`, `performance`, and `stress` labeled tests (`-LE "requires_multicast|performance|stress"`) to keep ASan/UBSan CI runs fast and deterministic while exercising 100% of the unit and integration test suite.

## 5. Fuzz Testing & Deserialization Hardening (`fuzz/`, `src/core/protocol.cpp`, `src/core/delta.cpp`)
- **libFuzzer Targets**: Added two dedicated fuzzing targets gated behind the `PEERSYNC_BUILD_FUZZERS` CMake option (Clang-only):
  1. `fuzz_message_deserialization`: Feeds arbitrary byte streams into every protocol message deserialization function (`deserializeHelloMessage`, `deserializeManifestResponseMessage`, `deserializeDeltaInstructionsMessage`, `deserializeResumeRequestMessage`, `deserializeDirectoryManifestRequestMessage`, `deserializeDirectoryManifestResponseMessage`, etc.) and tests round-trip re-serialization.
  2. `fuzz_reconstruct_file`: Feeds arbitrary and malformed `DeltaInstruction` sequences (including extreme block indices, zero indices, and oversized literals) into `reconstructFile` to verify clean exception handling without out-of-bounds reads or crashes.
- **Size-Prefixed Container Count Validation**: Identified and mitigated a potential Denial-of-Service / Out-of-Memory vulnerability where a malicious peer sends a small payload (within `MAX_MESSAGE_SIZE`) containing a massive 32-bit element count (e.g., `count = 1000000000`) for container fields (`files`, `signatures`, `instructions`). Calling `.reserve(count)` directly on deserialized vectors would attempt to allocate gigabytes of heap memory or throw `std::bad_alloc`. Added `BinaryReader::validateCount(uint32_t count, size_t minElemSize)` to verify that `count` does not exceed the maximum possible number of elements that could physically fit in the remaining unparsed bytes of the payload before invoking `.reserve(count)`.
- **Block Size Capping & Copy Instruction Refinement**: In `computeSignatures`, `computeDelta`, and `reconstructFile`, enforced a strict block size validity bounds check (`1 <= blockSize <= 64 MB`). In `reconstructFile`, simplified and hardened Copy instruction boundary checks so that referencing any offset >= `oldFileSize` (or copying from an empty file) cleanly throws `PeerSyncDeltaException` before any file seeking or reading is attempted.

## 6. Fault-Injection & Protocol Hardening (`tests/test_fault_injection.cpp`, `src/core/socket.cpp`, `src/core/conflict_resolution.cpp`)
- **Socket Read/Write Timeouts on Partial Sends**: To prevent receiver threads from hanging indefinitely when a peer opens a connection, sends a valid 4-byte frame length prefix, but fails to transmit the remaining payload bytes (partial send / idle connection), `TcpSocket` now provides `setRecvTimeout(int timeoutMs)` and `setSendTimeout(int timeoutMs)`. Both `connect()` and `accept()` configure a default 60-second timeout, ensuring all socket reads (`recvExactly`) will time out and raise a `PeerSyncNetworkException` cleanly upon network stall.
- **Out-of-Order Message Expectation Enforcement**: Hardened protocol state machines across `TransferSession` and `SyncOrchestrator`. When a specific protocol step is expected (e.g., expecting `ManifestResponse` after sending `ManifestRequest`), receiving an unexpected message type (such as `BlockDataMessage` or `HelloMessage`) is rejected immediately with a clear `PeerSyncProtocolException("Expected <MessageType>, got type <N>")` rather than misinterpreting payloads or causing undefined behavior.
- **Simultaneous Sync Tie-Breaker (Conflict Avoidance)**: In automatic peer-to-peer discovery environments where two nodes may attempt to initiate a file or directory synchronization to each other simultaneously for the same path, leaving role resolution undefined risks both sides talking past each other or deadlocking. Implemented `resolveSimultaneousSyncRole(const PeerIdentifier& local, const PeerIdentifier& remote)` in `include/peersync/conflict_resolution.h`. This deterministic tie-breaker compares lexicographical keys (`deviceName`, then `ipAddress`, then `port`), assigning the smaller peer the `Sender` role and the larger peer the `Receiver` role.
- **Targeted Fault-Injection Test Suite**: Created `tests/test_fault_injection.cpp` exercising all three fault scenarios (`PartialSendReadTimeout`, `UnexpectedMessageAfterPairingHandshake`, and `SimultaneousSyncConflictResolution`) over real loopback sockets, verified with 100% green sanitizer CI passes.

## 7. Performance & Correctness at Scale (3.13 GB Investigation)
During the end-to-end testing of large files (3.13 GB dataset), a massive performance regression investigation identified and fixed a series of distinct correctness, scaling, and throughput bugs:
1. **Missing fast-path for new-file transfers**: The delta engine was performing a full rolling-checksum scan on files the receiver didn't have at all, rather than taking an immediate fast-path.
2. **O(file size) memory buildup**: The original build-then-send architecture materialized the full instruction list in memory before sending, which crashed on gigabyte-sized files. It was refactored into a streaming, sliding-window architecture.
3. **Nagle's algorithm delays**: Split prefix+payload socket sends were triggering Nagle's algorithm delays. The payload and prefix are now serialized together.
4. **End-of-transfer socket race condition**: A concurrency race between the background `ackThread` and the main thread both reading from the socket corrupted the end-of-transfer handshake sequence.
5. **Resumability corruption**: A misscoped `saveJournal()` call was writing `bytesApplied=0` every batch, silently corrupting the resumability sequence guarantees (a correctness bug, not just perf).
6. **OS socket buffer starvation**: Default OS socket send/receive buffers (64KB) were too small to hold the 8-batch/4MB application-level sliding window, causing `sock.send()` to block synchronously. Increased buffers to 8MB.
7. **Per-batch journal disk I/O dominating receiver time**: The per-batch `saveJournal()+flush()` file open/close cost (~1.3ms x tens of thousands of calls) added over 60 seconds of latency to the transfer. Replaced with a strict 100-batch / 500ms periodic throttle, explicitly tested for idempotent resumption safety.
8. **Slow-path chunk truncation**: An off-by-one (`<` vs `<=`) in the slow-path chunking loop silently truncated output at exact 4MB chunk-size boundaries.
9. **Redundant hash computation**: A redundant duplicate whole-file hash computation left over from diagnostic instrumentation was removed, instantly saving several seconds.


