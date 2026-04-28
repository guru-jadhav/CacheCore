# CacheCore Tests

This folder holds the automated test scripts I use to verify that CacheCore works correctly and can actually handle heavy concurrency without crashing or deadlocking. 

## Requirements
- Python 3
- Make sure you've built the project first (the scripts expect to find the compiled `kv-store` binary in the `../build/` directory).

## The Scripts

### 1. `test_multidb_stress.py` (Multi-DB Broad Load)
**What it does:** This script spins up a server with a random number of databases (between 3 and 6). Then it spawns a bunch of threads that bombard the server with random CRUD commands across all those databases.
**Why:** It proves that our multi-DB state isolation actually works, measures overall throughput, and checks memory stability under a varied, normal-ish workload.

### 2. `test_deadlock_stress.py` (High Contention & Deadlocks)
**What it does:** Forces 100% collision. Every single thread aggressively reads and writes to the exact same key (`deadlock_key_1`) at the same time.
**Why:** This is a brutal test for our 3-tier mutex hierarchy. If there's a race condition or a deadlock in the locking logic, this script will definitely break it.

## Running the tests

You don't need to manually start the C++ server. The Python scripts handle everything—they generate a dynamic config file on the fly, pick an open port, spin up the server in the background, run the tests, and then cleanly kill the server when they finish.

Just run them like normal python scripts:

```bash
python3 tests/test_multidb_stress.py
python3 tests/test_deadlock_stress.py
```

## How they work under the hood

Both scripts run in two phases to make sure we don't waste time profiling broken code:

- **Phase 0 (Correctness):** A single-threaded run that goes through edge cases, TTL logic, basic commands (`PING`, `SET`, `GET`, `DEL`, `EXISTS`, `INCR`, `EXPIRE`), and checks error handling. If anything here fails, the script aborts immediately.
- **Phase 1 (Stress):** If Phase 0 passes, we unleash the multi-threaded assault. This phase spits out metrics like throughput, latency percentiles (p50, p90, p99), cache hits, and network bandwidth.
