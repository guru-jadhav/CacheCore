# kv-store

An in-memory key-value store built from scratch in C++17. No external dependencies.

Implements LRU eviction and TTL expiry — designed as a Redis replacement for a URL Shortener backend.

**Stack:** C++17 · CMake 3.20 · Clang 18 / GCC 13 · pthreads · WSL (Ubuntu 24.04)

---

## Architecture

```
kv-store/
├── CMakeLists.txt
├── include/
│   ├── lru_store.h        # LRUStore, Node, TTLNode, LRUStoreConfig declarations
│   ├── tcp_server.h       # TCPServer — connection handling, thread pool
│   └── resp_parser.h      # RESPParser, RESPCommand, ParseStatus
└── src/
    ├── main.cpp
    ├── store/
    │   └── lru_store.cpp  # LRU + TTL + background eviction implementation
    ├── server/
    │   └── tcp_server.cpp # TCP server implementation (in progress)
    └── protocol/
        └── resp_parser.cpp # RESP protocol parser implementation (in progress)
```

---

## Core Components

### LRU Eviction

`unordered_map<string, Node*>` + doubly linked list with dummy sentinels.

- HashMap gives O(1) lookup
- DLL maintains recency order — MRU at head, LRU at tail
- On capacity overflow, tail node is evicted and erased from the map

### TTL Expiry

`std::priority_queue<TTLNode>` min-heap ordered by expiry time, with lazy deletion.

- `Node::expTime` holds `optional<time_point>` — `nullopt` means the key never expires
- Lazy expiry check on every GET/EXISTS before returning a value
- On SET or EXPIRE, a new heap entry is pushed — stale entries are discarded during active eviction by comparing heap entry expiry against the node's current expiry

### Background Eviction Thread

Active eviction runs on a background thread started in the constructor and stopped cleanly in the destructor.

- Instead of sleeping a fixed interval, the thread checks the TTL heap top and sleeps until the nearest key's expiry time. Falls back to `evictInterval` seconds when the heap is empty
- When a new key comes in with an earlier expiry than the current heap top, `cv.notify_one()` wakes the thread early to recalculate its next wakeup time
- Uses `cv.wait_until()` instead of `sleep_for` — so the destructor can wake it immediately on shutdown rather than waiting out the full sleep
- Two mutexes protect shared state: `mapMtx` for the store + DLL, `heapMtx` for the TTL heap. A third `evictionMtx` is used only with the condition variable — so the sleeping thread never blocks GET/SET operations

### TCP Server

Handles client connections over TCP on a configurable port.

- Binds to `0.0.0.0:port` — accepts connections from any network interface
- Thread pool of 10 worker threads pre-created at startup — bounded resource usage, no per-connection thread spawn overhead
- `accept()` loop runs on a dedicated thread — pushes client fds into a shared queue
- Worker threads block on `cv.wait()` — zero CPU when idle, exactly one worker wakes per new client via `notify_one()`
- Persistent connections — one worker thread serves one client for the lifetime of the connection
- `activeClients` atomic counter — 11th client gets `-ERR max clients reached\r\n` and is rejected
- `SO_REUSEADDR` set — server restarts immediately without waiting for OS `TIME_WAIT` to expire

### RESP Protocol Parser

Parses incoming RESP arrays into structured commands. Separate from the server layer — no socket knowledge.

- Protocol format: `*N, dbIndex, command, args...`
- `ParseStatus` enum — `OK`, `INCOMPLETE`, `INVALID_FORMAT`, `INVALID_DB_INDEX`, `UNKNOWN_COMMAND`
- `INCOMPLETE` lets `handleClient()` accumulate bytes across multiple `recv()` calls — handles TCP chunking correctly
- No pipelining — one command per parse call (sync clients send one command, wait for response)

### Configuration

Store is configured via `LRUStoreConfig` — a plain struct with sensible defaults:

```cpp
// production
LRUStoreConfig config = { .maxCapacity = 1000, .ttl = 60, .evictInterval = 60 };

// tests
LRUStoreConfig config = { .maxCapacity = 5, .ttl = 2, .evictInterval = 5 };

LRUStore store(config);
```

Multiple store instances can be created from a single config file — clients specify which instance to query by including a `dbIndex` in every request.

---

## Wire Protocol

Commands are sent as RESP arrays. Format: `*N\r\n` followed by N bulk strings `$len\r\ndata\r\n`.

Argument order: `dbIndex, command, args...`

```
SET on db 0:
*4\r\n$1\r\n0\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n

GET on db 1:
*3\r\n$1\r\n1\r\n$3\r\nGET\r\n$3\r\nfoo\r\n
```

Responses:
```
+OK\r\n          — success (SET, DEL, EXPIRE)
$3\r\nfoo\r\n    — bulk string (GET value)
$-1\r\n          — null (GET miss)
:1\r\n           — integer (EXISTS)
-ERR msg\r\n     — error
```

---

## Commands

| Command | Signature | Description |
|---|---|---|
| `SET` | `SET key value [isExpires=true]` | Store a key. Expires after default TTL unless `isExpires=false` |
| `GET` | `GET key` | Returns value or null if missing or expired |
| `DEL` | `DEL key` | Delete a key |
| `EXISTS` | `EXISTS key` | Returns 1 if key exists and is not expired |
| `EXPIRE` | `EXPIRE key seconds` | Set TTL for a key — effective value is `max(store_ttl, seconds)` |
| `CLEAR` | `CLEAR` | Delete all keys |

---

## Design Decisions

**`std::optional<string>` for GET** — empty string `""` is a valid value. Using it as a sentinel creates ambiguity. `std::optional` makes absence explicit.

**`expTime` stored in `Node`** — lazy expiry on GET requires O(1) check per key. The heap only exposes its top, so expiry time must be on the node itself.

**Lazy heap deletion** — `std::priority_queue` has no random access or in-place update. On TTL update, a new entry is pushed and the stale one is discarded during `removeExpKeys()`.

**`size_t` for capacity and TTL** — `store.size()` returns `size_t`. Mixing with `int` causes signed/unsigned comparison warnings and silent wraparound on negative values.

**Minimum TTL of 60 seconds** — enforced as `max(store_default_ttl, user_input)`. Sub-minute TTLs cause unnecessary churn in a cache use case.

**Two separate mutexes for store and heap** — `mapMtx` and `heapMtx` are kept separate so heap iteration during eviction doesn't block concurrent GET/SET on the store. A third `evictionMtx` is used only with the condition variable to avoid blocking GET/SET while the eviction thread is sleeping.

**`removeExpKeys()` releases `heapMtx` before touching the store** — holding both locks simultaneously would create a circular wait with SET/EXPIRE (which acquire `mapMtx` then `heapMtx`). Releasing `heapMtx` first eliminates the deadlock.

**Thread pool over one-thread-per-client** — bounded resource usage. One thread per client would allow unbounded thread creation — 1000 clients = 1000 threads = potential OOM. Pool of 10 caps memory usage and matches expected number of persistent app server connections.

**Persistent connections** — app servers connect once and reuse the connection for all requests. Avoids TCP 3-way handshake overhead on every command.

**RESP parser separate from TCPServer** — parser has no socket knowledge, server has no protocol knowledge. Each component has one job and can be tested independently.

---

## Build

```bash
git clone https://github.com/yourusername/kv-store
cd kv-store
mkdir build && cd build
cmake ..
cmake --build .
./kv-store
```

**Requirements:** GCC 13+ or Clang 18+, CMake 3.20+, Linux (POSIX)

---

## Roadmap

- [x] LRU eviction — HashMap + DLL
- [x] TTL expiry — min-heap with lazy deletion
- [x] EXPIRE command
- [x] Background eviction thread — smart wakeup via condition variable
- [x] Thread safety — two mutex design (mapMtx + heapMtx)
- [x] TCPServer skeleton — thread pool, accept loop, worker loop
- [x] RESPParser skeleton — parse() with INCOMPLETE/error detection
- [ ] serialize() — RESP response formatting
- [ ] handleCommand() — route parsed command to correct store instance
- [ ] stop() — clean shutdown
- [ ] Integration with P2 — URL Shortener