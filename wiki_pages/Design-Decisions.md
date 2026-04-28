# Design Decisions

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
| **Config file, not hardcoded** | Different environments (prod/test) use different files. No code changes needed. Fails fast on malformed config — no silent defaults. |
| **`INCR` initializes missing/expired keys to `"1"`** | Redis-compatible. Missing key is treated as `0`, incremented to `1`, stored with no TTL. Caller explicitly requested increment — no guessing. |
| **`SET` clears TTL when `isExpires=false`** | If a key had a TTL and is re-SET without one, `expTime` becomes `nullopt`. Without this, the background thread would eventually delete a key the user intended to keep forever. |
