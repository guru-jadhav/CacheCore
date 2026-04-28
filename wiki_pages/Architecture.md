# Architecture

```text
CacheCore/
├── include/
│   ├── config.h               # Config struct
│   ├── config_parser.h        # .conf file parser
│   ├── lru_store.h            # Thread-safe LRU + TTL store
│   ├── tcp_server.h           # Socket + Thread pool management
│   └── resp_parser.h          # Redis protocol parser
└── src/
    ├── main.cpp                # Server lifecycle
    ├── config/config_parser.cpp 
    ├── store/lru_store.cpp      
    ├── server/tcp_server.cpp    
    └── protocol/resp_parser.cpp   
```

## Component Overview
- **ConfigParser:** Reads the `.conf` file and validates the setup.
- **TCPServer:** Manages a pool of 10 persistent worker threads that accept connections and route requests.
- **RESPParser:** Decodes incoming byte streams from clients into command arrays.
- **LRUStore:** The core thread-safe database engine handling data storage and eviction.

## LRU Store
The store utilizes two primary data structures:
1. `unordered_map<string, Node*>` for O(1) key lookups.
2. A **Doubly Linked List** to maintain recency order (Most Recently Used at the head, Least Recently Used at the tail). When `maxCapacity` is reached, the tail node is evicted.

## Background Eviction
Instead of just relying on lazy eviction (deleting expired keys when they are requested), CacheCore uses a dedicated background thread.
- Keys with TTLs are added to a **Min-Heap** (`std::priority_queue`).
- The background thread sleeps using a `condition_variable`. 
- It wakes up intelligently based on the expiration time of the key at the top of the heap, or via a fallback `evictInterval`.
- When it wakes, it purges all expired keys.

## Thread Safety
The system uses a strict 3-tier mutex hierarchy to ensure O(1) reads/writes are never blocked by background maintenance:
1. `storeMtx`: Protects the HashMap and Linked List (used by client threads).
2. `heapMtx`: Protects the TTL Min-Heap.
3. `evictionMtx`: Used solely for the background thread's sleep condition.

*Lock ordering rule: A thread must always acquire `storeMtx` before `heapMtx` to prevent deadlocks.*
