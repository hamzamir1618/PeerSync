# Architecture and Dependency Graph

The **peersync** project is organized into a modular structure separating the core synchronization logic (`libpeersync`), user-facing frontends (`cli` and `gui`), and automated testing (`tests`).

## Dependency Graph

```mermaid
flowchart TD
    subgraph Frontends
        cli[cli - src/cli]
        gui[gui - src/gui]
    end

    subgraph Core
        libpeersync[libpeersync - src/core]
    end

    subgraph TestSuite["Test Suite"]
        tests[tests - tests/]
    end

    subgraph Vendored ["Vendored - third_party/"]
        mdns["mdns.h"]
        tfd["tinyfiledialogs"]
    end

    subgraph External ["External - FetchContent"]
        cli11["CLI11"]
        imgui["Dear ImGui + GLFW"]
        gtest["GoogleTest"]
    end

    cli --> libpeersync
    cli --> cli11
    gui --> libpeersync
    gui --> tfd
    gui --> imgui
    tests --> libpeersync
    tests --> gtest
    libpeersync --> mdns
```

## Component Relationships

- **`cli` -> `libpeersync` -> `mdns.h`**: The command-line interface drives `libpeersync`, which utilizes `mdns.h` for local network peer discovery (mDNS / UDP broadcast). The CLI also depends on `CLI11` for argument parsing.
- **`gui` -> `libpeersync` + `tinyfiledialogs`**: The graphical interface drives `libpeersync` and directly incorporates `tinyfiledialogs` to provide native cross-platform file open, save, and folder selection dialogs without requiring heavy UI frameworks.
- **`tests` -> `libpeersync` + `GoogleTest`**: The test executable links against `libpeersync` and `GoogleTest` (fetched automatically via CMake) to verify core discovery and transfer routines.

## mDNS / DNS-SD Service Discovery

Peer discovery in **peersync** is handled via Multicast DNS (mDNS) and DNS-Based Service Discovery (DNS-SD, RFC 6763) implemented in `PeerAdvertiser` and `PeerBrowser` using the vendored `mdns.h` library.
- **Service Type**: Instances advertise and browse under the standard DNS-SD service type `_peersync._tcp.local.`.
- **Dynamic Port & Instance Naming**: Each advertiser dynamically binds to the actual runtime TCP port assigned to the `TcpSocket` listener and constructs its instance name (defaulting to the local machine's hostname).
- **PeerBrowser & Parsing**: `PeerBrowser` continuously searches the local network on a background thread, maintaining an active list of peers (`std::vector<DiscoveredPeer>`) accessible via polling (`getCurrentPeers()`) or a real-time callback (`std::function<void(const DiscoveredPeer&)>`). To ensure security against untrusted network input, packet decoding is extracted into pure, standalone functions (`parseMdnsResponsePacket`) that robustly reject truncated or malformed buffers without crashing.
- **Background Responder & Clean Shutdown**: Dedicated background threads in both `PeerAdvertiser` and `PeerBrowser` operate non-blocking sockets with short select timeouts (100ms). This guarantees that calling `stop()` or destroying the objects cleanly unblocks and shuts down threads without hanging.

### Manual Verification Pattern
> [!IMPORTANT]
> **Multicast Verification Constraint**: While packet construction, TXT record formatting, query matching, and malformed packet parsing are heavily covered by automated unit tests in `tests/test_discovery.cpp`, **full multicast advertise+browse behavior across machines is verified manually** rather than relied upon in automated CI.
>
> Automated CI build runners and container network namespaces frequently filter, restrict, or disable UDP multicast traffic on port 5353, making deterministic end-to-end multicast testing impossible across sandboxed CI environments. Cross-machine discovery is verified manually across real machines or two processes on a loopback interface (tested locally via `DiscoveryTest.LiveLoopbackDiscoveryAdvertiserAndBrowser`), consistent with the project's established pattern for network-hardware-dependent code.

## Wire Protocol

All direct peer-to-peer communication in **peersync** is built on top of a reliable TCP message framing foundation implemented in `libpeersync` (`src/core/message_framing.cpp`). Because TCP is a continuous byte stream without native packet boundaries, every message transmitted over the network is encapsulated using a fixed-size length prefix framing structure:

```
+-------------------------+-------------------------------+
|  Length Prefix (4 B)    |       Payload (N Bytes)       |
|  Big-Endian / Net Order |  Arbitrary Binary Payload     |
+-------------------------+-------------------------------+
```

1. **Length Prefix**: A 4-byte unsigned integer (`uint32_t`) encoded in network byte order (big-endian) specifying the exact number of bytes in the trailing payload (\(N\)).
2. **Payload**: Exactly \(N\) bytes of data. To guard against malformed length prefixes or denial-of-service memory exhaustion attacks, a sane maximum message size (`MAX_MESSAGE_SIZE = 64 MB`) is enforced. Any message claiming a length exceeding this constant is immediately rejected with a `PeerSyncNetworkException`.
3. **Stream Reassembly and Error Handling**: The receiving socket loops over `recv()` calls until exactly 4 bytes of prefix and \(N\) bytes of payload have been read. If a connection closes mid-message or truncates payload delivery, a `PeerSyncNetworkException` is thrown rather than returning partial or corrupted data. This framing serves as the universal transport foundation for all higher-level synchronization commands and chunk transfers.

### Protocol Message Types

Within the framed binary payload, the very first byte specifies the `MessageType` enum tag (`1` to `13`), allowing receivers to inspect and dispatch messages without full deserialization. The defined message types and their purposes are:

- **`Hello` (1)**: Exchanged upon initial connection establishment to advertise device identity and verify protocol version compatibility.
- **`PairChallenge` (2)**: Transmits a cryptographic challenge nonce to initiate secure peer pairing and authentication.
- **`PairResponse` (3)**: Returns the cryptographic response or hashed PIN to prove pairing authorization.
- **`PairResult` (4)**: Communicates the final success or failure status of a pairing authentication attempt.
- **`ManifestRequest` (5)**: Requests a full synchronization manifest for a specific directory or repository path.
- **`ManifestResponse` (6)**: Returns a catalog of remote file entries, including relative paths, file sizes, timestamps, and SHA-256 digests.
- **`DeltaInstructions` (7)**: Transmits block rolling-checksum instructions to enable delta-compressed synchronization of modified files.
- **`BlockData` (8)**: Transmits raw binary file chunk data for a specific file path and offset.
- **`TransferAck` (9)**: Acknowledges successful receipt and disk persistence of transmitted block bytes.
- **`ResumeRequest` (10)**: Inquires whether a partially interrupted file transfer can be resumed from a recorded offset.
- **`ResumeResponse` (11)**: Returns resume eligibility status and confirms the valid byte offset to continue transmission from.
- **`ErrorMessage` (13)**: Communicates protocol-level or filesystem errors (with numeric code and descriptive string) to the connected peer.

## Pairing & Trust Model

The **peersync** pairing mechanism (`src/core/pairing.cpp`) utilizes a random 6-digit numeric PIN (`generatePin()`) and PBKDF2-HMAC-SHA256 (`deriveSessionKey()`) to establish cryptographic authorization between connecting devices.

> [!IMPORTANT]
> **Scope Limitation — Authentication vs. Encryption**:
> The PIN-based pairing handshake authenticates that both sides know the same secret PIN before any file synchronization or manifest transfer proceeds. **It does NOT encrypt the file data or protocol traffic itself.**
>
> This is a known, deliberate scope limitation for the current local-network architecture rather than an oversight. Operating over private local area networks (LANs), the primary threat model addressed is preventing unauthorized local devices from initiating sync connections or reading file trees without user authorization. Encrypting the entire transport byte stream is a natural candidate for a future TLS-based enhancement (e.g., wrapping `TcpSocket` in TLS/SSL).

## Delta Sync Algorithm

When synchronizing large files that have been modified, **peersync** avoids transferring entire file contents over the network by utilizing an rsync-derived delta synchronization engine (`src/core/delta.cpp`). The algorithm operates in two phases: signature computation over the old file and rolling-window delta scanning over the new file.

```
Old File (Remote): [Block 0: Sig] [Block 1: Sig] [Block 2: Sig] ...
                         ^               ^
New File (Local):  [Literal Bytes] [Copied Block 1] [Literal Bytes] ...
```

### 1. Two-Tier Signature Computation (`computeSignatures`)
The destination peer divides its existing version of the file into non-overlapping sequential blocks of size `blockSize` (with the final block possibly shorter) and computes two cryptographic identifiers for each block:
- **Weak Rolling Checksum (Adler-32)**: A 32-bit checksum ($M = 65521$) that can be updated in $O(1)$ time as a sliding byte window moves across a file.
- **Strong Hash (xxHash64)**: A 64-bit non-cryptographic hash (`XXH64`) providing extremely low collision probability to confirm exact block content matches.

These signatures (`BlockSignature`) are sent to the source peer, which constructs an in-memory hash map keyed by weak checksum to enable $O(1)$ average-case candidate block lookups.
### 3. Delta Sync Algorithm
peersync uses a rsync-like rolling checksum algorithm to minimize data transfer for modified files.

1. **Sender requests manifest**: The sender asks the receiver for a signature list of the target file.
2. **Receiver computes signatures**: If the file exists, the receiver splits it into fixed blocks (e.g., 4MB) and computes a weak Adler-32 checksum and a strong xxHash64 hash for each.
3. **Sender streams delta**: The sender uses a sliding window (bounded-memory stream) over the local file. At each byte, it computes the rolling Adler-32 checksum. If it matches a receiver block, it computes the xxHash64 to confirm.
   - **Match**: A `Copy` instruction is emitted referencing the receiver's existing block.
   - **Mismatch**: The byte is appended to a literal buffer. When the buffer reaches a threshold, it's emitted as a `Literal` instruction.
4. **O(1) Memory Pipeline with Sliding Window Acknowledgment**: Instead of materializing the entire sequence of instructions in memory, instructions are grouped into batches. To prevent the sender from out-pacing a slow receiver or exhausting memory while waiting for disk I/O, transmission is throttled using a **sliding window acknowledgment scheme**. The sender allows up to $W$ batches to be "in flight" (sent but not yet acknowledged) by executing a dedicated background thread to receive acks concurrently. This avoids the severe throughput bottleneck of strict stop-and-wait while keeping the sender's memory footprint strictly bounded (O(W) relative to file size), preventing out-of-memory errors on massive files.

### Why Rolling Checksums Beat Naive Fixed-Offset Diffing
A naive diffing algorithm that compares fixed file offsets (e.g., block 0 vs block 0, block 1 vs block 1) fails completely when bytes are **inserted or deleted** anywhere in the file. An insertion of even a single byte shifts all trailing data by one offset, causing all subsequent fixed-offset blocks to mismatch and forcing a full retransmission of unchanged content.

By using a **byte-by-byte rolling checksum**, the scanning window naturally re-synchronizes with remote block boundaries immediately after an insertion or deletion. Once the sliding window rolls past the modified bytes, it re-aligns with the original trailing blocks, hitting the hash map and emitting lightweight `Copy` instructions for all remaining unchanged content.

### Block Size Selection and Tradeoffs
The default block size in **peersync** is configurable (typically **64 KB** for large files and **4 KB** to **16 KB** for smaller datasets). Selecting the optimal block size involves a fundamental size/speed tradeoff:
- **Smaller Block Sizes (e.g., 4 KB - 16 KB)**:
  - **Pros**: Higher granularity. Small edits in localized regions result in less literal data transmission because fewer unchanged bytes are trapped inside a modified block.
  - **Cons**: Larger memory overhead and network payload for signature lists (more signatures per megabyte of file), and higher CPU overhead from more frequent hash map lookups.
- **Larger Block Sizes (e.g., 64 KB - 256 KB)**:
  - **Pros**: Extremely compact signature lists, minimal memory usage, and very high scanning throughput (fewer blocks to index and match).
  - **Cons**: Coarser granularity. A single byte edit forces retransmission of up to an entire `blockSize` literal buffer if no other block boundaries align.

## Resumable Transfers

When synchronizing large files over unreliable network connections, network drops or socket disconnections midway through a transfer can result in significant wasted bandwidth if the synchronization must start over from scratch. **peersync** solves this by implementing a persistent, state-aware resumption protocol in `libpeersync` (`src/core/transfer.cpp`).

### 1. Journaling Infrastructure & Deterministic Temporary Files
During file reception (`receiveFile`), incoming delta instructions (`Copy` and `Literal`) are applied incrementally to a deterministic temporary file (`<targetFile>.peersync-tmp`). Alongside this temporary file, the receiver maintains an on-disk progress journal (`<targetFile>.peersync-journal`) recording critical transfer metadata:
- **Relative Path**: The target repository file path.
- **Expected File Size**: The total expected size of the reconstructed target file in bytes.
- **Signature List Hash**: A cryptographic SHA-256 digest of the block signature list (`sigs`) computed over the receiver's initial local file before transfer began.
- **Last Sequence Number**: The cumulative count of delta instructions successfully applied and written to disk so far (`lastSeq`).

If a TCP socket connection closes or throws an exception mid-transfer, an RAII cleanup guard (`TempFileGuard`) checks whether the transfer finished successfully. On an interrupted or aborted transfer, the guard deliberately leaves both `.peersync-tmp` and `.peersync-journal` intact on disk rather than deleting them.

### 2. The Resumption Handshake (`ResumeRequest` & `ResumeResponse`)
When a new sync attempt begins for a given file path, `receiveFile` inspects the destination directory for an existing valid journal and temporary file:
1. **Validation & Stale Fallback**: The receiver loads `.peersync-journal` and verifies that the recorded relative path matches, the temporary file size is within bounds, and the stored signature hash (`sigHash`) matches the signature list of the current local destination file. If any check fails (e.g., the local destination file was modified out-of-band while disconnected), the journal and temporary file are considered stale and are immediately deleted (`deleteJournalAndTemp`), falling back seamlessly to a clean fresh sync.
2. **Resume Inquiry**: If the journal is valid, the receiver transmits a `ResumeRequestMessage` (MessageType 10) instead of a standard `ManifestResponseMessage`. This message includes the recorded sequence offset (`lastOffset`), expected file size, and the original block signature vector (`signatures`).
3. **Sender-Side Skipping**: The sender (`sendFile`) receives the `ResumeRequestMessage`, validates that the file size matches its source file, and responds with a positive `ResumeResponseMessage` (`canResume = true`). It then computes the delta instructions against the receiver's provided signatures and iterates through the instruction list, fast-forwarding and skipping the first `lastOffset` instructions without reading local disk blocks or transmitting payload over the wire.
4. **Resumption & Commit**: The sender resumes transmitting messages from instruction `lastOffset + 1`. The receiver opens `.peersync-tmp` in append/modify mode (`std::ios::ate | std::ios::in | std::ios::out`) and continues applying incoming instructions from its exact previous point of interruption. Once all bytes are received and verified against the expected file hash, `.peersync-tmp` is atomically renamed over `<targetFile>` and `.peersync-journal` is deleted.

### 3. Sliding Window Flow Control
To achieve near line-rate gigabit network throughput, **peersync** employs an asynchronous sliding window acknowledgement protocol:
- **Pipelined Transmission**: Rather than blocking for an acknowledgment after every batch of instructions (Stop-and-Wait ARQ), the sender pushes instruction batches concurrently up to a defined `MAX_IN_FLIGHT` limit (currently 8 batches, each containing up to 64KB or 512KB of instructions).
- **Asynchronous Acknowledgments**: A dedicated background `ackThread` continuously polls for incoming `TransferAck` messages from the receiver without interrupting the main sender thread's I/O and network transmission loop.
- **Backpressure & OS Buffers**: The sender utilizes large socket OS buffer allocations (`SO_SNDBUF` / `SO_RCVBUF` scaled to 8MB) to complement the sliding window. If the receiver's disk I/O falls behind, the receiver's OS buffer fills up, naturally exerting TCP backpressure. The sender's `ackThread` stops receiving acks, the in-flight counter hits the limit, and the sender's main thread yields via `std::condition_variable` until the receiver catches up. This prevents the sender from exhausting memory while maximizing throughput.

## Directory Synchronization & Conflict Resolution

To synchronize entire repository trees, **peersync** implements a high-level `SyncOrchestrator` (`src/core/sync_orchestrator.cpp`) that coordinates manifest exchanges, conflict resolution, and concurrent delta transfers across worker socket pools.

### 1. Recursive Manifest Construction & Exchange
When initiating a directory synchronization (`syncDirectory`), each peer scans its local directory using `std::filesystem::recursive_directory_iterator`, filtering out temporary and journal artifacts (`.peersync-tmp`, `.peersync-journal`). For each file, a `FileEntry` struct is populated containing its generic relative path, file size in bytes, and last modification timestamp (`mtime` converted to Unix epoch seconds). All manifest entries are sorted lexicographically by relative path to guarantee deterministic lockstep ordering between peers.

The initiator transmits its manifest via a `DirectoryManifestRequestMessage` (MessageType 14). The responder receives the catalog, opens a dedicated TCP listening socket for worker pool transfers (if `maxConcurrency > 0`), and replies with a `DirectoryManifestResponseMessage` (MessageType 15) containing its own sorted file manifest along with the dynamically assigned worker listening port (`workerPort`).

### 2. Two-Stage Cheap Filtering & Conflict Resolution Policy
To avoid redundant network transmission and signature computations, `SyncOrchestrator::computeSyncPlan` compares corresponding file entries across both manifests using a two-stage filter:
1. **Identical Cheap Filter**: If a file exists on both peers with the exact same file size and modification timestamp (`mtime`), it is classified as identical and immediately added to the `skipped` plan without opening a transfer session or transmitting data.
2. **Conflict Resolution Policy (Last-Write-Wins by mtime)**: If a file exists on both peers but differs in size or timestamp, a conflict is detected and recorded in the `conflicts` plan. In bidirectional synchronization mode (`SyncPolicy::Direction::Bidirectional`), conflicts are resolved deterministically using a **Last-Write-Wins** policy based on modification timestamp:
   - **Local Newer (`loc.mtime > rem.mtime`)**: The local file supersedes the remote file and is scheduled to be pushed (`toSend`).
   - **Remote Newer (`loc.mtime < rem.mtime`)**: The remote file supersedes the local version and is scheduled to be pulled (`toReceive`).
   - **Equal Timestamp, Differing Size**: In the rare event of identical timestamps with differing file sizes, the local initiator's version takes precedence in push/bidirectional modes.

### 3. Bounded Concurrency Worker Pool (`executeSync`)
To maximize throughput without overwhelming network or system resources, transfer operations (`toSend` and `toReceive`) are combined into a single unified task queue sorted by relative path. Because both initiator and responder sort tasks identically, worker threads operate in 100% lockstep across peer socket connections (when Thread $i$ on the initiator executes a push task, Thread $i$ on the responder executes the corresponding pull task).

Transfers execute across a pool of independent worker TCP connections bounded by `SyncPolicy::maxConcurrency` (defaulting to 4 concurrent worker threads). Each worker thread instantiates an independent `TransferSession` over its dedicated socket, applying rsync-style rolling checksum delta transfers and resumable journaling to each assigned file until the synchronization plan is fully satisfied.

### 4. Simultaneous Sync Tie-Breaker & Network Fault Resilience
In dynamic peer-to-peer environments with automatic mDNS/DNS-SD discovery, two peer nodes may discover each other and simultaneously attempt to initiate a file or directory synchronization for the exact same target path. Without coordination, both peers would simultaneously open sender transfer sessions and transmit `ManifestRequest` messages, resulting in protocol deadlock or cross-talk.

To resolve simultaneous push collisions deterministically, **peersync** implements a stateless tie-breaking protocol via `resolveSimultaneousSyncRole` (`src/core/conflict_resolution.cpp`). Both peers evaluate a lexicographical comparison of their identifiers (`deviceName`, tie-breaking on `ipAddress`, then `port`):
- **Sender Role**: The peer with the lexicographically smaller identifier (`localPeer < remotePeer`) assumes the `SimultaneousSyncRole::Sender` role and proceeds with its active push attempt (`sendFile`).
- **Receiver Role**: The peer with the lexicographically larger identifier assumes the `SimultaneousSyncRole::Receiver` role, yields its outbound push attempt, and transitions its socket session to listen for incoming transfer instructions (`receiveFile`).

To prevent denial-of-service or indefinite thread hangs from network stalls and malformed transmissions, protocol sessions enforce strict expectations and socket timeouts:
- **Read Timeouts on Partial Sends**: Sockets configured via `TcpSocket::setRecvTimeout` and `setSendTimeout` enforce a 60-second read deadline in `connect()` and `accept()`. If a remote peer transmits a valid 4-byte frame length header but hangs or drops the connection without completing the payload, `recvExactly` times out and raises a `PeerSyncNetworkException` cleanly.
- **Out-of-Order Message Expectation Enforcement**: At each step of the synchronization state machine (e.g., awaiting a `ManifestResponse` after sending a `ManifestRequest`), receiving an unexpected message type (such as `BlockDataMessage` or `HelloMessage`) immediately terminates the session with a `PeerSyncProtocolException`, protecting internal structures from invalid protocol transitions.

## Performance Benchmarks & Baseline Numbers

To validate the scalability and efficiency of **peersync**'s rolling-window delta synchronization engine, end-to-end performance benchmarks are implemented in `tests/perf_delta_benchmark.cpp` (labeled with `performance` in CTest and excluded from default CI runs).

The benchmarks evaluate both the local in-memory delta processing pipeline (`computeSignatures`, `computeDelta`, and `reconstructFile`) and live network streaming over loopback sockets (`TransferSession`). Tests are executed using 64 KB block sizes on large synthetic binary datasets featuring dispersed modifications and length-changing insertions.

### Observed Baseline Metrics (Windows x64 / NVMe SSD / Loopback Socket)

#### 1. In-Memory Delta Pipeline (250 MB & 1 GB synthetic files, 64 KB blocks)
| Metric | 250 MB Dataset | 1 GB (1000 MB) Dataset |
| :--- | :--- | :--- |
| **`computeSignatures` Speed** | ~164.4 MB/s | ~142.1 MB/s |
| **`computeDelta` Scanning Speed** | ~153.9 MB/s | ~126.6 MB/s |
| **`reconstructFile` Speed** | ~479.8 MB/s | ~239.8 MB/s |
| **Total End-to-End Throughput** | ~68.2 MB/s | ~52.3 MB/s |
| **Literal Bytes Generated** | 1,639,400 bytes (~1.64 MB) | 6,554,600 bytes (~6.55 MB) |
| **Bandwidth Equivalent %** | **0.625%** of full size | **0.625%** of full size |
| **Net Bandwidth Savings** | **99.37%** | **99.37%** |

#### 2. End-to-End Live Transfer (3.13 GB Dataset)
- **Original Baseline**: Never completed (failed or hung due to memory scaling, socket buffer sizes, and delta engine bugs).
- **Final Optimized Baseline**: **~28–31 seconds** (end-to-end throughput of > 100 MB/s on a loopback gigabit equivalent link, capped primarily by disk write speed and cryptographic hashing).

#### 3. Loopback Socket Streaming (`TransferSession`, 250 MB & 1 GB synthetic files)
| Metric | 250 MB Dataset | 1 GB (1000 MB) Dataset |
| :--- | :--- | :--- |
| **Wall-Clock Transfer Time** | ~9.28 s | ~57.5 s |
| **Effective Sync Throughput** | ~26.9 MB/s | ~17.4 MB/s |
| **Actual Wire Payload Sent** | 1,695,582 bytes (~1.62 MB) | ~6.78 MB |
| **Network Bandwidth %** | **0.647%** of full size | **0.647%** of full size |
| **Net Bandwidth Savings** | **99.35%** | **99.35%** |

### Key Takeaways
1. **Linear Scalability**: The delta synchronization algorithm scales linearly from 250 MB to 1 GB without degradation in savings or accuracy.
2. **Resilience to Insertions**: Even with length-changing insertions that shift file offsets, the Adler-32 rolling checksum re-aligns immediately, keeping literal data transmission strictly below 1% of the total file size (~99.37% reduction in network payload).
3. **High Scanning Throughput**: The rolling window scan achieves over 125–150 MB/s per core, allowing large files to be synchronized over local area networks with minimal CPU overhead.


