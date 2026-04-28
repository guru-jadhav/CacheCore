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
- Used as a health-check / liveness probe. Redis-compatible.

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
