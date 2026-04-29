<p align="center">
  <img src="assets/CacheCore_full.png" alt="CacheCore Logo" width="300">
</p>

# CacheCore

An in-memory key-value store built from scratch in C++17. Zero external dependencies.

Implements LRU eviction, TTL expiry with background cleanup, and a custom RESP-encoded wire protocol optimized for stateless multi-DB routing.

**Stack:** C++17 · CMake 3.20 · Clang 18 / GCC 13 · pthreads · Linux

---

## Architecture

```
CacheCore/
├── CMakeLists.txt              # Build config — C++17, pthread, recursive src glob
├── store.example.conf          # Template config — copy to store.conf before running
├── include/
│   ├── config.h               # Config struct — port (uint16_t) + vector<LRUStoreConfig>
│   ├── config_parser.h        # ConfigParser — static class, load() returns optional<Config>
│   ├── lru_store.h            # LRUStore, Node, TTLEntry, LRUStoreConfig
│   ├── tcp_server.h           # TCPServer, CommandDef, ErrMsg namespace
│   └── resp_parser.h          # RESPParser, RESPCommand, ParseStatus, ResponseType, ParseErr namespace
└── src/
    ├── main.cpp                # Entry point — config load, validation, signal handling, server lifecycle
    ├── config/
    │   └── config_parser.cpp  # Config file parser — .conf format, line-by-line validation
    ├── store/
    │   └── lru_store.cpp      # LRU + TTL + background eviction implementation
    ├── server/
    │   └── tcp_server.cpp     # TCP server — socket, thread pool, accept loop, command routing
    └── protocol/
        └── resp_parser.cpp    # RESP protocol — parse() and serialize() implementation
```

---

## How It Works — Component Breakdown

### 1. Configuration (`config.h` · `config_parser.h` · `config_parser.cpp`)

Server is started with a config file path:

```bash
cp store.example.conf store.conf   # copy template, edit as needed
./CacheCore ./store.conf
```

**Config file format (`.conf`):**
```properties
# CacheCore config
PORT=6948

# format: DB maxCapacity=<N> ttl=<N> evictInterval=<N>
DB maxCapacity=1000 ttl=60 evictInterval=1000
DB maxCapacity=500 ttl=150 evictInterval=80
DB maxCapacity=300 ttl=60 evictInterval=80
```

**Config structs (from code):**

| Struct | Field | Type | Description |
|---|---|---|---|
| `Config` | `port` | `uint16_t` | Server port. Default `0` (must be set). |
| `Config` | `dbConfig` | `vector<LRUStoreConfig>` | One entry per `DB` line in config. |
| `LRUStoreConfig` | `maxCapacity` | `size_t` | Max keys in this store. Default `100`. |
| `LRUStoreConfig` | `ttl` | `size_t` | Default TTL in seconds. Default `60`. |
| `LRUStoreConfig` | `evictInterval` | `size_t` | Background eviction fallback interval. Default `60`. |

**Parsing rules:**
- File must have `.conf` extension — validated via `std::filesystem::path::extension()`.
- File must exist and be a regular file — checked via `std::filesystem`.
- Lines starting with `#` → skipped (comments). Empty lines → skipped.
- Lines with leading spaces → spaces are consumed, then parsed normally.
- `PORT=` must be followed by digits only. Range: 1–65535. Parsed as `int`, cast to `uint16_t`.
- Each `DB` line is parsed field-by-field: `maxCapacity=`, then `ttl=`, then `evictInterval=`. Fields separated by spaces.
- If multiple `PORT=` lines exist, the last one wins.
- Any malformed line → `ConfigParser::load()` returns `std::nullopt`, server exits.
- After parsing, `main.cpp` additionally validates: `port != 0` and `dbConfig` is not empty.

**Error messages** (`fileParsingMessage` namespace in `config_parser.cpp`):

| Constant | Message |
|---|---|
| `NO_SUCH_FILE_FOUND` | `Error: provided file doesn't exist` |
| `PROVIDE_VALID_FILE` | `Error: please provide valid config file` |
| `INVALID_FORMAT` | `Error: invalid config file format` |
| `ERROR_WHILE_OPENING` | `Error opening file` |
| `ERROR_WHILE_IO` | `Error: I/O error while reading file` |

---

### 2. LRU Store (`lru_store.h` · `lru_store.cpp`)

**Data structures:**

| Structure | Purpose |
|---|---|
| `unordered_map<string, Node*>` | O(1) key → node lookup |
| Doubly linked list (dummy head + tail sentinels) | Recency order — MRU at head, LRU at tail |
| `priority_queue<TTLEntry, vector<TTLEntry>, greater<TTLEntry>>` | Min-heap ordered by expiry time |

**`Node` fields:** `key`, `value`, `optional<steady_clock::time_point> expTime` (default `nullopt`), `prev`, `next`.

**`TTLEntry` fields:** `key`, `steady_clock::time_point expTime`. Has `operator>` for min-heap ordering.

**Constructor enforced minimums:**

| Parameter | Minimum | Code |
|---|---|---|
| `ttl` | 60 seconds | `std::max((size_t)60, config.ttl)` |
| `evictInterval` | 10 seconds | `std::max((size_t)10, config.evictInterval)` |

**Internal methods:**

| Method | What it does |
|---|---|
| `removeNode(Node*)` | Unlinks node from DLL (does not delete) |
| `linkToHead(Node*)` | Links node right after dummyHead |
| `moveToHead(Node*)` | Removes then links to head (skip if already head) |
| `deleteNode(Node*)` | Removes from DLL + erases from map + `delete` |
| `insertNode(key, val, isExpires)` | Evicts LRU if at capacity, creates new node, links to head, sets expiry |
| `setExpiry(Node*, isExpires, duration)` | If expiring: computes `now + max(ttl, duration)`, sets on node, schedules in heap. If not: sets `nullopt` (clears any existing TTL). |
| `scheduleTTL(key, expTime)` | Pushes to heap. If new expiry is earlier than heap top → `notify_one()` to wake eviction thread. |
| `isExpired(Node*)` | Returns `expTime.has_value() && expTime <= now` |
| `isAllDigits(string)` | Checks if string is all digits, **allows leading `-` for negative numbers** |
| `clearStore()` | Walks DLL deleting all nodes, relinks sentinels, drains heap |
| `evictionLoop()` | Background thread loop — see below |
| `purgeExpiredKeys()` | Drains expired entries from heap, then conditionally deletes from store |

---

### 3. Background Eviction Thread

Started in `LRUStore` constructor, stopped in destructor.

**Eviction loop logic (`evictionLoop`):**
1. Lock `evictionMtx` and `heapMtx`.
2. Compute next wakeup: if heap has entries → `ttlHeap.top().expTime`. If empty → `now + evictInterval`.
3. Unlock `heapMtx` (so GET/SET can push to heap while thread sleeps).
4. `evictionCv.wait_until(lock, nextEvictionTime, []{return stopEviction;})` — sleeps until wakeup time or notification.
5. If `stopEviction` → break. Otherwise → call `purgeExpiredKeys()`.

**`purgeExpiredKeys` — two-phase delete to avoid deadlock:**
1. **Phase 1:** Lock `heapMtx`. Pop all entries where `expTime <= now` into a `vector<string>`. Unlock `heapMtx`.
2. **Phase 2:** For each key: lock `storeMtx`, check if key still exists AND `expTime != nullopt` AND `expTime <= now` (stale check), then `deleteNode()`.

**Why not just call `DEL()`?** `purgeExpiredKeys` needs to verify the node's current expiry matches — a SET or EXPIRE may have updated it since the heap entry was created. `DEL()` deletes unconditionally.

**Shutdown sequence (destructor):**
1. `stopEviction = true` — set flag first.
2. `evictionCv.notify_one()` — wake thread immediately.
3. `evictionThread.join()` — block until thread exits.
4. Lock both mutexes, walk DLL deleting all nodes, drain heap.

---

### 3a. PING (Stateless Health Check)

`LRUStore::PING()` returns `"PONG"` — no mutex, no store access. Used by clients and load balancers to verify the server process is alive and the command pipeline is functional.

### 4. Thread Safety

**Three mutexes, each with a distinct purpose:**

| Mutex | Protects | Used by |
|---|---|---|
| `storeMtx` | `store` map + DLL | GET, SET, DEL, EXISTS, CLEAR, EXPIRE, INCR, PING (no lock — stateless), purgeExpiredKeys (phase 2) |
| `heapMtx` | `ttlHeap` | scheduleTTL, purgeExpiredKeys (phase 1), evictionLoop (wakeup calc), CLEAR |
| `evictionMtx` | condition variable wait | evictionLoop only |

**Lock ordering rule:** `storeMtx` → `heapMtx` (SET/EXPIRE acquire store then call `scheduleTTL` which acquires heap). `purgeExpiredKeys` releases `heapMtx` before acquiring `storeMtx` to avoid circular wait.

---

### 5. TCP Server (`tcp_server.h` · `tcp_server.cpp`)

**Architecture:**

| Component | Detail |
|---|---|
| Socket | `AF_INET`, `SOCK_STREAM`, `INADDR_ANY`, `SO_REUSEADDR` |
| Accept thread | Dedicated `std::thread` running `acceptLoop()` |
| Worker pool | 10 `std::thread`s, pre-created at startup |
| Client queue | `std::queue<int>` protected by `queueMtx` + `clientCv` |
| Max clients | `activeClients` atomic counter, capped at 10 |
| Connection model | Persistent — one worker serves one client for connection lifetime |
| Backlog | `SOMAXCONN` (OS default max) |

**Connection flow:**
1. `acceptLoop()` calls `accept()` in a loop.
2. If `activeClients >= 10` → send raw `-ERR max clients reached\r\n` and close fd.
3. Otherwise → push fd to queue, increment `activeClients`, `notify_one()`.
4. `workerLoop()` wakes, pops fd, calls `handleClient(fd)`.
5. `handleClient()` loops: `recv()` → accumulate → `parser.parse()` → if OK: `handleCommand()` → `send()`. If INCOMPLETE: keep accumulating. If error: send error, reset buffer.
6. On `recv() == 0` (client disconnect) or `recv() < 0` (error) → decrement `activeClients`, close fd, return.

**Shutdown (`stop()`):**
1. Set `shouldStop = true`.
2. `shutdown(serverFd, SHUT_RDWR)` — breaks blocking `accept()`.
3. `close(serverFd)`.
4. `clientCv.notify_all()` — wake all sleeping workers.
5. Join accept thread + all worker threads.

**Command routing — O(1) via `commandRegistry`:**

`std::unordered_map<string, CommandDef>` where `CommandDef = {int expectedArgs, function<string(RESPCommand&)> handler}`.

`handleCommand()` flow:
1. Validate DB index (0 ≤ `dbIndex` < `stores.size()`).
2. Look up command in registry. Not found → error.
3. Validate arg count matches `expectedArgs`. Mismatch → error with specific message: `"WRONG NUMBER OF ARGUMENTS — SET expects 3 arg(s) but received 1"`.
4. Call handler lambda.

**Error messages (`ErrMsg` namespace in `tcp_server.h`):**

| Constant | Value |
|---|---|
| `INVALID_DB_INDEX` | `INVALID DB INDEX` |
| `INVALID_ARGS` | `WRONG NUMBER OF ARGUMENTS` |
| `INVALID_TTL` | `TTL MUST BE A VALID INTEGER` |
| `MAX_CLIENTS` | `MAX CLIENTS REACHED, TRY AGAIN LATER` |
| `UNKNOWN_COMMAND` | `UNKNOWN COMMAND` |

---

### 6. RESP Protocol Parser (`resp_parser.h` · `resp_parser.cpp`)

> [!NOTE]
> **Custom Protocol Schema:** While CacheCore uses standard RESP (REdis Serialization Protocol) syntax, it employs a custom stateless command structure. Unlike Redis (which uses stateful connections via `SELECT`), CacheCore requires every command array to pass the target Database Index as its first argument to enable stateless, O(1) multi-DB routing. Standard Redis clients (like `redis-cli`) will not work out-of-the-box.

**Request format:** RESP array of bulk strings.

```
*N\r\n
$len\r\ndata\r\n    ← bulk string 0: dbIndex
$len\r\ndata\r\n    ← bulk string 1: command
$len\r\ndata\r\n    ← bulk string 2+: args
```

**Parsing rules:**
- Bulk string 0 → `parsedRequest.dbIndex` (parsed via `std::stoi`, catch → `INVALID_DB_INDEX`).
- Bulk string 1 → `parsedRequest.command`.
- Bulk strings 2+ → pushed to `parsedRequest.args`.

**`ParseStatus` enum:**

| Value | Meaning |
|---|---|
| `OK` | Fully parsed, ready for execution |
| `INCOMPLETE` | Not enough bytes yet — accumulate more from `recv()` |
| `INVALID_FORMAT` | Malformed RESP — send error, reset buffer |
| `INVALID_DB_INDEX` | DB index is not a valid number |
| `UNKNOWN_COMMAND` | (defined but not used by parser — routing happens in TCPServer) |

**Parse error constants (`ParseErr` namespace):**

| Constant | Value |
|---|---|
| `EMPTY_REQUEST` | `empty request` |
| `EXPECTED_ARRAY` | `expected '*' at start of RESP array` |
| `INVALID_ARRAY_COUNT` | `non-numeric character in array count after '*'` |
| `MISSING_CRLF` | `expected \r\n terminator not found` |
| `EXPECTED_BULK` | `expected '$' at start of bulk string` |
| `INVALID_BULK_LEN` | `non-numeric character in bulk string length after '$'` |
| `BULK_DATA_SHORT` | `bulk string data shorter than declared length` |
| `INVALID_DB_INDEX` | `db index must be a valid number` |

**`ResponseType` enum + `serialize()`:**

| Type | Wire format | Example |
|---|---|---|
| `OK` | `+OK\r\n` | Success for DEL, EXPIRE, CLEAR |
| `ERROR` | `-ERR <msg>\r\n` | Any error |
| `BULK` | `$<len>\r\n<data>\r\n` | GET hit |
| `NULLBULK` | `$-1\r\n` | GET miss |
| `INTEGER` | `:<value>\r\n` | SET, EXISTS, INCR |
| `SIMPLE_STRING` | `+<msg>\r\n` | PING response (`+PONG\r\n`) |

---

## Commands

| Command | Wire Args | `expectedArgs` | Store Method | Return Type | Response |
|---|---|---|---|---|---|
| `SET` | `key value isExpires(0/1)` | 3 | `bool SET(key, value, isExpires=="1")` | `INTEGER` | `:1\r\n` on success, `:0\r\n` if maxCapacity is 0 |
| `GET` | `key` | 1 | `optional<string> GET(key)` | `BULK` / `NULLBULK` | `$<len>\r\n<value>\r\n` or `$-1\r\n` |
| `DEL` | `key` | 1 | `void DEL(key)` | `OK` | `+OK\r\n` (always, even if key didn't exist) |
| `EXISTS` | `key` | 1 | `bool EXISTS(key)` | `INTEGER` | `:1\r\n` if exists and not expired, `:0\r\n` otherwise |
| `EXPIRE` | `key seconds` | 2 | `void EXPIRE(key, stoi(seconds))` | `OK` / `ERROR` | `+OK\r\n` or `-ERR invalid TTL value\r\n` |
| `INCR` | `key` | 1 | `optional<string> INCR(key)` | `INTEGER` / `ERROR` | `:<newValue>\r\n` or `-ERR value is not an integer or out of range\r\n` |
| `CLEAR` | _(none)_ | 0 | `void CLEAR()` | `OK` | `+OK\r\n` |
| `PING` | _(none)_ | 0 | `string PING()` | `SIMPLE_STRING` | `+PONG\r\n` |

### Command Behavior Details

**SET:**
- If key exists → update value, move to head, update expiry.
- If key doesn't exist → evict LRU if at capacity, insert new node at head.
- `isExpires` is the 3rd arg: `"1"` → apply TTL (`max(store_ttl, 0)`), `"0"` → no expiry (`nullopt`).
- If key previously had a TTL and is re-SET with `isExpires=0`, the TTL is **cleared** (`nullopt`).
- Returns `false` only if `maxCapacity == 0`.

**GET:**
- If key exists but expired → lazy delete (remove from DLL + map), return `nullopt`.
- If key exists and valid → move to head (update recency), return value.

**DEL:**
- Idempotent. If key doesn't exist, silently returns.

**EXISTS:**
- If key exists but expired → lazy delete, return `false`.

**EXPIRE:**
- If key doesn't exist → silently returns (still sends `+OK`).
- Effective TTL = `max(store_default_ttl, user_seconds)`. The store's `ttl` is already clamped to `max(60, config_ttl)`.
- `seconds` arg is parsed via `std::stoi` in `tcp_server.cpp` — non-numeric → caught, returns `-ERR invalid TTL value`.

**INCR:**
- Missing or expired key → delete if expired, insert `"1"` with no TTL, return `"1"`.
- Existing key with non-numeric value → return error. **Negative integers are valid** (leading `-` is accepted by `isAllDigits()`).
- Value at `LLONG_MAX` → return error (overflow guard, no wrap).
- On success → increment, update value in-place, move to head, return new value as integer.

**CLEAR:**
- Acquires both `storeMtx` and `heapMtx`. Walks DLL deleting all nodes, relinks sentinels, drains heap, clears map.

**PING:**
- Stateless — returns `"PONG"` immediately. No mutex acquired, no store interaction.
- Used as a health-check / liveness probe.

---

## Wire Protocol Examples

```
SET "foo" = "bar" with TTL on db 0:
*5\r\n$1\r\n0\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n$1\r\n1\r\n
→ :1\r\n

SET "foo" = "bar" without TTL on db 0:
*5\r\n$1\r\n0\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n$1\r\n0\r\n
→ :1\r\n

GET "foo" on db 0:
*3\r\n$1\r\n0\r\n$3\r\nGET\r\n$3\r\nfoo\r\n
→ $3\r\nbar\r\n

GET miss:
→ $-1\r\n

DEL "foo" on db 0:
*3\r\n$1\r\n0\r\n$3\r\nDEL\r\n$3\r\nfoo\r\n
→ +OK\r\n

EXISTS "foo" on db 0:
*3\r\n$1\r\n0\r\n$6\r\nEXISTS\r\n$3\r\nfoo\r\n
→ :1\r\n  or  :0\r\n

EXPIRE "foo" 120s on db 0:
*4\r\n$1\r\n0\r\n$6\r\nEXPIRE\r\n$3\r\nfoo\r\n$3\r\n120\r\n
→ +OK\r\n

INCR "counter" on db 0:
*3\r\n$1\r\n0\r\n$4\r\nINCR\r\n$7\r\ncounter\r\n
→ :1\r\n  (if new)  or  :2\r\n  (if was "1")

CLEAR db 0:
*2\r\n$1\r\n0\r\n$5\r\nCLEAR\r\n
→ +OK\r\n

PING on db 0:
*2\r\n$1\r\n0\r\n$4\r\nPING\r\n
→ +PONG\r\n
```

---

## Design Decisions

| Decision | Rationale |
|---|---|
| **`std::optional<string>` for GET/INCR return** | Empty string `""` is a valid value. `nullopt` makes absence explicit without sentinel ambiguity. |
| **`expTime` stored in `Node`, not just heap** | Lazy expiry on GET/EXISTS/INCR needs O(1) per-key check. Heap only exposes its top. |
| **Lazy heap deletion** | `std::priority_queue` has no random-access delete or in-place update. On TTL change, push new entry; stale ones are discarded in `purgeExpiredKeys()` via expiry comparison. |
| **`size_t` for capacity, TTL, evictInterval** | `store.size()` returns `size_t`. Mixing with `int` → signed/unsigned warnings and silent wraparound on negative values. |
| **Minimum TTL = 60s, minimum evictInterval = 10s** | Enforced in constructor: `max(60, config.ttl)`, `max(10, config.evictInterval)`. Sub-second TTLs and aggressive eviction intervals cause unnecessary background thread wakeups and lock contention that degrade throughput, so we enforce sensible minimums at the store level. |
| **Three separate mutexes** | `storeMtx` for store+DLL, `heapMtx` for TTL heap, `evictionMtx` for condition variable. Eviction thread sleeping never blocks GET/SET. |
| **`purgeExpiredKeys()` releases heap lock before store lock** | Prevents circular wait. SET/EXPIRE acquire `storeMtx` → `heapMtx`. Purge acquires `heapMtx` → releases → `storeMtx`. No deadlock. |
| **Thread pool (10 workers) over thread-per-client** | Bounded resource usage. 1000 clients ≠ 1000 threads. Pool of 10 matches expected persistent app server connections. |
| **Persistent connections** | App servers connect once, reuse forever. Avoids TCP 3-way handshake per command. One worker per client for connection lifetime. |
| **RESP parser decoupled from TCPServer** | Parser has zero socket knowledge. Server has zero protocol knowledge. Independently testable. |
| **O(1) command routing via `commandRegistry`** | `unordered_map<string, CommandDef>` with arg count + lambda. Adding a command = one map entry. No if-chain. Arg count validated centrally before handler is called. |
| **Config file, not hardcoded** | Different environments (prod/test) use different files. No code changes needed. Fails fast on malformed config — no silent defaults. `store.example.conf` committed; `store.conf` gitignored. |
| **`ConfigParser` as static class, not namespace** | Class boundary provides access control — `parsePort`, `parseTTL`, etc. are `private`. A namespace can't hide helpers. |
| **`INCR` initializes missing/expired keys to `"1"`** | Redis-compatible. Missing key is treated as `0`, incremented to `1`, stored with no TTL. Caller explicitly requested increment — no guessing. |
| **`INCR` overflow guard at `LLONG_MAX`** | Returns error instead of wrapping. Checked *before* `++`, not after. |
| **`SET` clears TTL when `isExpires=false`** | If a key had a TTL and is re-SET without one, `expTime` becomes `nullopt`. Without this, the background thread would eventually delete a key the user intended to keep forever. |
| **`vector<unique_ptr<LRUStore>>` over `vector<LRUStore>`** | `LRUStore` contains `std::mutex`, `std::condition_variable`, and `std::atomic` — all non-copyable and non-movable. `vector` needs to move elements during reallocation, so `vector<LRUStore>` won't compile. `unique_ptr` heap-allocates each store at a fixed address (never moved) and only the 8-byte pointer is stored in the vector (movable). Automatic cleanup via RAII — no manual `new`/`delete`. |
| **`SO_REUSEADDR`** | Server restarts immediately without waiting for OS `TIME_WAIT` (≈60s) to expire. |
| **`shutdown(SHUT_RDWR)` before `close()` on stop** | Actively breaks the blocking `accept()` call instead of waiting for the next connection to arrive. |

---

## Startup Flow (`main.cpp`)

```
1. Parse CLI args — require config file path
2. ConfigParser::load(path) — returns optional<Config>
3. Validate: port != 0, dbConfig not empty
4. Print loaded config (port + all DB instances)
5. Register SIGINT handler → sets running = false
6. Construct TCPServer(config) → creates LRUStore instances + command registry
7. server.start() → socket, bind, listen, spawn accept thread + 10 workers
8. Main thread polls `running` every 100ms
9. On SIGINT → server.stop() → shutdown socket, join all threads
```

---

## Build

```bash
git clone https://github.com/guru-jadhav/CacheCore
cd CacheCore
mkdir build && cd build
cmake ..
cmake --build .
cp ../store.example.conf ../store.conf
# edit store.conf as needed
./CacheCore ../store.conf
```

**Requirements:** GCC 13+ or Clang 18+, CMake 3.20+, Linux (POSIX sockets + pthreads)

**CMake details:** `GLOB_RECURSE` on `src/*.cpp`, include path set to `include/`, links `pthread`.

---

## Testing

CacheCore includes a fully automated Python testing suite that manages dynamic server configurations, verifies correctness, and performs extreme high-contention stress testing. 

For full details and execution instructions, see the [tests/README.md](tests/README.md).

---

## Roadmap

- [x] LRU eviction — HashMap + DLL
- [x] TTL expiry — min-heap with lazy deletion
- [x] EXPIRE command
- [x] INCR command — with missing/expired key initialization + overflow guard
- [x] PING command — stateless health check, `SIMPLE_STRING` response type
- [x] Background eviction thread — smart wakeup via condition variable
- [x] Thread safety — three mutex design (storeMtx + heapMtx + evictionMtx)
- [x] TCPServer — thread pool, accept loop, worker loop, O(1) command routing
- [x] RESPParser — parse() + serialize() with full error messages
- [x] Config file driven startup — `.conf` format, full validation
- [x] Move implementations to .cpp files
- [x] End-to-end test with Python RESP client (Automated multi-DB and deadlock stress testing)
- [ ] Development of a client library ecosystem
