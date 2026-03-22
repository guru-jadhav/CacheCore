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
│   └── lru_store.h        # LRUStore, Node, TTLNode declarations
└── src/
    ├── main.cpp
    ├── store/
    │   └── lru_store.cpp  # LRU + TTL implementation
    ├── server/            # TCP socket server (in progress)
    └── protocol/          # Wire protocol parser (in progress)
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
- Lazy expiry check on every GET before returning a value
- On SET or EXPIRE, a new heap entry is pushed — stale entries are discarded during active eviction by comparing heap entry expiry against the node's current expiry
- Active eviction via background thread on a fixed interval (in progress)

---

## Commands

| Command | Signature | Description |
|---|---|---|
| `SET` | `SET key value [isExpires=true]` | Store a key. Expires after default TTL unless `isExpires=false` |
| `GET` | `GET key` | Returns value or `nullopt` if missing or expired |
| `DEL` | `DEL key` | Delete a key |
| `EXISTS` | `EXISTS key` | Returns true if key exists and is not expired |
| `EXPIRE` | `EXPIRE key seconds` | Set TTL for a key — effective value is `max(store_ttl, seconds)` |
| `CLEAR` | `CLEAR` | Delete all keys |

---

## Design Decisions

**`std::optional<string>` for GET** — empty string `""` is a valid value. Using it as a sentinel creates ambiguity. `std::optional` makes absence explicit.

**`expTime` stored in `Node`** — lazy expiry on GET requires O(1) check per key. The heap only exposes its top, so expiry time must be on the node itself.

**Lazy heap deletion** — `std::priority_queue` has no random access or in-place update. On TTL update, a new entry is pushed and the stale one is discarded during `removeExpKeys()`.

**`size_t` for capacity and TTL** — `store.size()` returns `size_t`. Mixing with `int` causes signed/unsigned comparison warnings and silent wraparound on negative values.

**Minimum TTL of 60 seconds** — enforced as `max(store_default_ttl, user_input)`. Sub-minute TTLs cause unnecessary churn in a cache use case.

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
- [ ] Background eviction thread
- [ ] Thread safety — mutex layer
- [ ] TCP socket server
- [ ] RESP-inspired wire protocol
- [ ] Integration with P2 — URL Shortener