# Getting Started

## Requirements
To build CacheCore from source, you need:
- **OS:** Linux (POSIX sockets + pthreads) or WSL (Ubuntu 24.04)
- **Compiler:** GCC 13+ or Clang 18+
- **Build System:** CMake 3.20+

## Build Instructions

```bash
# 1. Clone the repository
git clone https://github.com/guru-jadhav/CacheCore
cd CacheCore

# 2. Create the build directory
mkdir build && cd build

# 3. Generate and compile
cmake ..
cmake --build .
```

## Configuration (`.conf` format)

CacheCore requires a configuration file to start. You can find an example file at `store.example.conf` in the project root.

**How to use it:**
```bash
# From the build directory, copy the template
cp ../store.example.conf ../store.conf

# Start the server by passing the config file as an argument
./CacheCore ../store.conf
```

**Configuration Format (`store.conf`)**
The file defines the TCP `PORT` and initializes one or more logical Databases (`DB`).

```properties
# CacheCore config
PORT=6948

# Format: DB maxCapacity=<N> ttl=<N> evictInterval=<N>
# You can define multiple databases. The first one is DB 0, the second is DB 1, etc.
DB maxCapacity=1000 ttl=60 evictInterval=1000
DB maxCapacity=500 ttl=150 evictInterval=80
DB maxCapacity=300 ttl=60 evictInterval=80
```

| Field | Description | Minimum Value |
|---|---|---|
| `PORT` | The TCP port the server listens on | `1` to `65535` |
| `maxCapacity` | Max number of keys this specific database can hold before LRU eviction triggers | `0` (disables writes) |
| `ttl` | Default Time-To-Live in seconds for keys | `60` |
| `evictInterval` | How often the background thread checks for expired keys (seconds) | `10` |
