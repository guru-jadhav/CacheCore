import socket
import random
import string
import threading
import time
import statistics
import subprocess
import os
import sys

HOST = "127.0.0.1"
PORT = 0 # Will be assigned dynamically
NUM_DBS = 0 # Will be assigned dynamically

NUM_THREADS = 15 # 10 to accept, 5 to reject
OPS_PER_THREAD = 50000
MAX_CLIENTS = 10

# Force 100% collision on a single key to test mutex deadlocks
HOT_KEY = "deadlock_key_1"

RED = "\033[91m"
GREEN = "\033[92m"
YELLOW = "\033[93m"
CYAN = "\033[96m"
RESET = "\033[0m"

# =========================================================
# SERVER LIFECYCLE & CONFIG
# =========================================================
def generate_config(conf_path):
    port = random.randint(10000, 60000)
    # Gemini tests multi-db isolation on DB 0 and 1, so we need at least 2 DBs
    num_dbs = random.randint(2, 5) 
    
    config_content = f"PORT={port}\n"
    for _ in range(num_dbs):
        cap = random.randint(100, 5000)
        ttl = random.randint(60, 300)
        evict = random.randint(10, 100)
        config_content += f"DB maxCapacity={cap} ttl={ttl} evictInterval={evict}\n"
    
    os.makedirs(os.path.dirname(conf_path), exist_ok=True)
    with open(conf_path, "w") as f:
        f.write(config_content)
    
    print(f"{CYAN}Generated dynamic config at {conf_path} (PORT={port}, DBs={num_dbs}){RESET}")
    return port, num_dbs

def start_server(conf_path):
    server_bin = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build", "CacheCore"))
    if not os.path.exists(server_bin):
        print(f"{RED}Error: {server_bin} not found. Build the project first.{RESET}")
        sys.exit(1)
    
    print(f"{CYAN}Starting CacheCore server...{RESET}")
    
    # ------------------ DESCRIPTION ------------------
    print(f"\n{YELLOW}========================================================{RESET}")
    print(f"{YELLOW}TEST: Deadlock & High Contention Stress Test{RESET}")
    print(f"{YELLOW}PURPOSE: Forces 100% collision on a single key across{RESET}")
    print(f"{YELLOW}         multiple threads to test the mutex hierarchy,{RESET}")
    print(f"{YELLOW}         ensuring no deadlocks occur under extreme load.{RESET}")
    print(f"{YELLOW}========================================================\n{RESET}")
    # -------------------------------------------------

    process = subprocess.Popen([server_bin, conf_path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1) # Give it a moment to bind and listen
    
    if process.poll() is not None:
        print(f"{RED}Server failed to start. Return code: {process.returncode}{RESET}")
        sys.exit(1)
        
    return process

# =========================================================
# METRICS
# =========================================================
class Metrics:
    def __init__(self):
        self.lock = threading.Lock()
        self.success = 0
        self.mismatch = 0
        self.dropped = 0
        self.tx_bytes = 0
        self.rx_bytes = 0
        self.latencies = []
        self.failures = []
        self.get_attempts = 0
        self.get_hits = 0

    def record_success(self, lat, tx, rx, cmd, act_type):
        with self.lock:
            self.success += 1
            self.latencies.append(lat)
            self.tx_bytes += tx
            self.rx_bytes += rx
            
            if cmd == "GET":
                self.get_attempts += 1
                if act_type == "[BULK]":
                    self.get_hits += 1

    def record_mismatch(self, tid, cmd, args, expected, actual_type, raw_resp):
        disp_resp = repr(raw_resp.strip())
        if len(disp_resp) > 150:
            disp_resp = disp_resp[:147] + "..."

        log = (
            f"[{RED}FAILED{RESET}] Thread {tid}\n"
            f"  CMD      : {cmd} {args}\n"
            f"  EXPECTED : {expected}\n"
            f"  ACTUAL   : {actual_type} => {disp_resp}\n"
            f"  {'-'*60}"
        )
        
        with self.lock:
            self.mismatch += 1
            if len(self.failures) < 15:
                self.failures.append(log)

    def record_drop(self):
        with self.lock:
            self.dropped += 1

metrics = Metrics()

# =========================================================
# STRICT CacheCore CLIENT
# =========================================================
class StrictCacheCore:
    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.connected = False

    def connect(self):
        try:
            self.sock.connect((self.host, self.port))
            self.reader = self.sock.makefile('rb')
            self.connected = True
            return True
        except Exception as e:
            return str(e)

    def _encode_request(self, db_index, command, *args):
        parts = [str(db_index), command] + [str(a) for a in args]
        req = f"*{len(parts)}\r\n"
        for part in parts:
            req += f"${len(part)}\r\n{part}\r\n"
        return req.encode('utf-8')

    def execute_strictly(self, db_index, command, *args):
        if not self.connected:
            return "[NO CONNECTION]", ""
        
        req_bytes = self._encode_request(db_index, command, *args)
        
        try:
            self.sock.sendall(req_bytes)
            
            prefix = self.reader.read(1)
            if not prefix:
                return "[DISCONNECTED]", "Server closed connection"

            line = self.reader.readline().rstrip(b'\r\n')
            decoded_line = line.decode('utf-8')

            if prefix == b'+':
                return "[STRING]", decoded_line
            elif prefix == b'-':
                return "[ERROR]", decoded_line
            elif prefix == b':':
                return "[INTEGER]", decoded_line
            elif prefix == b'$':
                length = int(line)
                if length == -1:
                    return "[NONE]", ""
                data = self.reader.read(length)
                self.reader.read(2)
                return "[BULK]", data.decode('utf-8')
            else:
                return "[INVALID]", f"{prefix.decode('utf-8')}{decoded_line}"
                
        except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError) as e:
            self.connected = False
            return "[DISCONNECTED]", str(e)
            
        return "[UNKNOWN]", "", req_bytes

    def close(self):
        if self.connected:
            try:
                self.sock.close()
            except:
                pass
            self.connected = False


# =========================================================
# PHASE 0 — CORRECTNESS TESTS (single-threaded, sequential)
# =========================================================

correctness_passed = 0
correctness_failed = 0
correctness_failures = []

def assert_eq(test_name, actual_type, actual_msg, expected_type, expected_msg=None):
    global correctness_passed, correctness_failed
    ok = actual_type == expected_type
    if expected_msg is not None:
        ok = ok and actual_msg == expected_msg
    if ok:
        correctness_passed += 1
        print(f"  {GREEN}✓{RESET} {test_name}")
    else:
        correctness_failed += 1
        exp = f"{expected_type}" + (f" => {expected_msg}" if expected_msg else "")
        act = f"{actual_type} => {actual_msg}"
        msg = f"  {RED}✗{RESET} {test_name}  |  expected: {exp}  |  actual: {act}"
        print(msg)
        correctness_failures.append(msg)

def run_correctness_tests():
    global correctness_passed, correctness_failed

    print("=" * 80)
    print(f"{CYAN}PHASE 0: CORRECTNESS TESTS (sequential){RESET}")
    print("=" * 80)

    c = StrictCacheCore(HOST, PORT)
    result = c.connect()
    if result is not True:
        print(f"{RED}Cannot connect for correctness tests: {result}{RESET}")
        return False

    # clean slate
    for i in range(NUM_DBS):
        c.execute_strictly(i, "CLEAR")

    # ------ PING ------
    print(f"\n{YELLOW}[PING]{RESET}")
    t, m = c.execute_strictly(0, "PING")
    assert_eq("PING returns +PONG", t, m, "[STRING]", "PONG")

    # ------ SET / GET basic ------
    print(f"\n{YELLOW}[SET / GET]{RESET}")
    t, m = c.execute_strictly(0, "SET", "k1", "hello", "0")
    assert_eq("SET k1=hello → :1", t, m, "[INTEGER]", "1")

    t, m = c.execute_strictly(0, "GET", "k1")
    assert_eq("GET k1 → hello", t, m, "[BULK]", "hello")

    # overwrite
    t, m = c.execute_strictly(0, "SET", "k1", "world", "0")
    assert_eq("SET k1=world (overwrite) → :1", t, m, "[INTEGER]", "1")

    t, m = c.execute_strictly(0, "GET", "k1")
    assert_eq("GET k1 → world (overwritten)", t, m, "[BULK]", "world")

    # GET miss
    t, m = c.execute_strictly(0, "GET", "nonexistent")
    assert_eq("GET miss → $-1", t, m, "[NONE]")

    # empty key and empty value
    t, m = c.execute_strictly(0, "SET", "", "emptykey", "0")
    assert_eq("SET ''='emptykey' → :1", t, m, "[INTEGER]", "1")

    t, m = c.execute_strictly(0, "GET", "")
    assert_eq("GET '' → emptykey", t, m, "[BULK]", "emptykey")

    t, m = c.execute_strictly(0, "SET", "emptyval", "", "0")
    assert_eq("SET emptyval='' → :1", t, m, "[INTEGER]", "1")

    t, m = c.execute_strictly(0, "GET", "emptyval")
    assert_eq("GET emptyval → '' (empty string)", t, m, "[BULK]", "")

    # ------ DEL ------
    print(f"\n{YELLOW}[DEL]{RESET}")
    c.execute_strictly(0, "SET", "delme", "bye", "0")
    t, m = c.execute_strictly(0, "DEL", "delme")
    assert_eq("DEL existing key → +OK", t, m, "[STRING]", "OK")

    t, m = c.execute_strictly(0, "GET", "delme")
    assert_eq("GET after DEL → $-1", t, m, "[NONE]")

    # DEL idempotent
    t, m = c.execute_strictly(0, "DEL", "never_existed")
    assert_eq("DEL non-existent → +OK (idempotent)", t, m, "[STRING]", "OK")

    # ------ EXISTS ------
    print(f"\n{YELLOW}[EXISTS]{RESET}")
    c.execute_strictly(0, "SET", "exkey", "val", "0")
    t, m = c.execute_strictly(0, "EXISTS", "exkey")
    assert_eq("EXISTS present key → :1", t, m, "[INTEGER]", "1")

    c.execute_strictly(0, "DEL", "exkey")
    t, m = c.execute_strictly(0, "EXISTS", "exkey")
    assert_eq("EXISTS after DEL → :0", t, m, "[INTEGER]", "0")

    t, m = c.execute_strictly(0, "EXISTS", "never_existed_2")
    assert_eq("EXISTS non-existent → :0", t, m, "[INTEGER]", "0")

    # ------ INCR ------
    print(f"\n{YELLOW}[INCR]{RESET}")

    # INCR missing key → creates "1"
    c.execute_strictly(0, "DEL", "counter")
    t, m = c.execute_strictly(0, "INCR", "counter")
    assert_eq("INCR missing key → :1", t, m, "[INTEGER]", "1")

    t, m = c.execute_strictly(0, "INCR", "counter")
    assert_eq("INCR again → :2", t, m, "[INTEGER]", "2")

    t, m = c.execute_strictly(0, "INCR", "counter")
    assert_eq("INCR again → :3", t, m, "[INTEGER]", "3")

    # INCR on non-numeric → error
    c.execute_strictly(0, "SET", "strkey", "abc", "0")
    t, m = c.execute_strictly(0, "INCR", "strkey")
    assert_eq("INCR non-numeric → ERROR", t, m, "[ERROR]", "ERR value is not an integer or out of range")

    # INCR negative number (valid per README)
    c.execute_strictly(0, "SET", "negkey", "-5", "0")
    t, m = c.execute_strictly(0, "INCR", "negkey")
    assert_eq("INCR negative (-5) → :-4", t, m, "[INTEGER]", "-4")

    # INCR at LLONG_MAX → error (overflow guard)
    c.execute_strictly(0, "SET", "maxkey", "9223372036854775807", "0")
    t, m = c.execute_strictly(0, "INCR", "maxkey")
    assert_eq("INCR at LLONG_MAX → ERROR", t, m, "[ERROR]", "ERR value is not an integer or out of range")

    # ------ EXPIRE ------
    print(f"\n{YELLOW}[EXPIRE]{RESET}")

    # EXPIRE on missing key → still +OK
    t, m = c.execute_strictly(0, "EXPIRE", "ghost_key", "120")
    assert_eq("EXPIRE non-existent key → +OK", t, m, "[STRING]", "OK")

    # EXPIRE invalid TTL
    t, m = c.execute_strictly(0, "EXPIRE", "k1", "notanumber")
    assert_eq("EXPIRE invalid TTL → ERROR", t, m, "[ERROR]", "ERR invalid TTL value")

    # ------ SET TTL clears on re-SET ------
    print(f"\n{YELLOW}[SET TTL clear]{RESET}")
    # SET with TTL, then re-SET without TTL — key should persist
    c.execute_strictly(0, "SET", "ttlclear", "v1", "1")  # with expiry
    c.execute_strictly(0, "SET", "ttlclear", "v2", "0")  # without expiry — clears TTL
    t, m = c.execute_strictly(0, "GET", "ttlclear")
    assert_eq("SET clears TTL: GET returns v2", t, m, "[BULK]", "v2")

    # ------ CLEAR ------
    print(f"\n{YELLOW}[CLEAR]{RESET}")
    c.execute_strictly(0, "SET", "c1", "a", "0")
    c.execute_strictly(0, "SET", "c2", "b", "0")
    c.execute_strictly(0, "SET", "c3", "c", "0")
    t, m = c.execute_strictly(0, "CLEAR")
    assert_eq("CLEAR → +OK", t, m, "[STRING]", "OK")

    t, m = c.execute_strictly(0, "GET", "c1")
    assert_eq("GET after CLEAR → $-1", t, m, "[NONE]")
    t, m = c.execute_strictly(0, "EXISTS", "c2")
    assert_eq("EXISTS after CLEAR → :0", t, m, "[INTEGER]", "0")

    # ------ ERROR PATHS ------
    print(f"\n{YELLOW}[Error paths]{RESET}")

    # Invalid DB index
    t, m = c.execute_strictly(99, "GET", "k1")
    assert_eq("Invalid DB index (99) → ERROR", t, m, "[ERROR]")

    # Unknown command
    t, m = c.execute_strictly(0, "FOOBAR", "k1")
    assert_eq("Unknown command FOOBAR → ERROR", t, m, "[ERROR]")

    # Wrong arg count — GET expects 1 arg, send 2
    t, m = c.execute_strictly(0, "GET", "k1", "extra")
    assert_eq("GET with 2 args (expects 1) → ERROR", t, m, "[ERROR]")

    # Wrong arg count — SET expects 3 args, send 1
    t, m = c.execute_strictly(0, "SET", "k1")
    assert_eq("SET with 1 arg (expects 3) → ERROR", t, m, "[ERROR]")

    # Wrong arg count — CLEAR expects 0 args, send 1
    t, m = c.execute_strictly(0, "CLEAR", "extra")
    assert_eq("CLEAR with 1 arg (expects 0) → ERROR", t, m, "[ERROR]")

    # ------ MULTI-DB ISOLATION ------
    print(f"\n{YELLOW}[Multi-DB isolation]{RESET}")
    c.execute_strictly(0, "CLEAR")
    c.execute_strictly(1, "CLEAR")

    c.execute_strictly(0, "SET", "shared", "db0val", "0")
    c.execute_strictly(1, "SET", "shared", "db1val", "0")

    t, m = c.execute_strictly(0, "GET", "shared")
    assert_eq("DB0 has its own value", t, m, "[BULK]", "db0val")

    t, m = c.execute_strictly(1, "GET", "shared")
    assert_eq("DB1 has its own value", t, m, "[BULK]", "db1val")

    c.execute_strictly(0, "DEL", "shared")
    t, m = c.execute_strictly(1, "GET", "shared")
    assert_eq("DEL on DB0 doesn't affect DB1", t, m, "[BULK]", "db1val")

    # cleanup
    for i in range(NUM_DBS):
        c.execute_strictly(i, "CLEAR")

    c.close()

    # ------ SUMMARY ------
    print(f"\n{'-'*60}")
    total = correctness_passed + correctness_failed
    print(f"Correctness: {GREEN}{correctness_passed}{RESET}/{total} passed, {RED}{correctness_failed}{RESET} failed")

    if correctness_failures:
        print(f"\n{RED}FAILURES:{RESET}")
        for f in correctness_failures:
            print(f)

    return correctness_failed == 0


# =========================================================
# PHASE 1 — STRESS TEST (unchanged logic, added PING)
# =========================================================

def build_extreme_cmd(iteration):
    if random.random() < 0.01:
        big_val = ''.join(random.choices(string.ascii_letters, k=1024 * 50))
        return ["SET", 0, "SET", HOT_KEY, big_val, 0]
        
    if random.random() < 0.01:
        return ["SET", 0, "SET", "", "", 0]

    cmd = random.choice(["SET", "GET", "DEL", "EXISTS", "EXPIRE", "INCR", "PING"])
    db = 0

    if cmd == "PING":
        return [cmd, db, cmd]
    if cmd == "SET":
        return [cmd, db, cmd, HOT_KEY, str(iteration), random.randint(0, 1)]
    if cmd == "EXPIRE":
        return [cmd, db, cmd, HOT_KEY, random.randint(0, 5)]
    if cmd in ["GET", "DEL", "EXISTS", "INCR"]:
        return [cmd, db, cmd, HOT_KEY]

def is_valid(cmd, resp_type, resp):
    if "max clients" in resp: return True
    if cmd == "PING": return resp_type == "[STRING]"
    if cmd == "SET": return resp_type == "[INTEGER]"
    if cmd in ["DEL", "CLEAR"]: return resp_type == "[STRING]"
    if cmd == "EXPIRE": return resp_type in ["[STRING]", "[ERROR]"]
    if cmd == "GET": return resp_type in ["[BULK]", "[NONE]"]
    if cmd == "EXISTS": return resp_type == "[INTEGER]"
    if cmd == "INCR": return resp_type in ["[INTEGER]", "[ERROR]"]
    return False

def get_expected_str(cmd):
    mapping = {
        "PING": "STRING (+PONG)",
        "SET": "INTEGER (:1 or :0)",
        "DEL": "STRING (+OK)",
        "CLEAR": "STRING (+OK)",
        "EXPIRE": "STRING (+OK) or ERROR (-ERR)",
        "GET": "BULK ($...) or NONE ($-1)",
        "EXISTS": "INTEGER (:1 or :0)",
        "INCR": "INTEGER (:...) or ERROR (-ERR)"
    }
    return mapping.get(cmd, "UNKNOWN")

def extreme_worker(tid, client):
    for i in range(OPS_PER_THREAD):
        cmd_payload = build_extreme_cmd(i)
        cmd_name = cmd_payload[0]
        db = cmd_payload[1]
        actual_payload = cmd_payload[2:]
        
        start = time.time()

        act_type, act_msg = client.execute_strictly(db, cmd_name, *actual_payload[1:])
        latency = (time.time() - start) * 1000

        if act_type == "[DISCONNECTED]":
            metrics.record_drop()
            break 

        if is_valid(cmd_name, act_type, act_msg):
            metrics.record_success(latency, 50, len(act_msg) + 5, cmd_name, act_type)
        else:
            expected_str = get_expected_str(cmd_name)
            metrics.record_mismatch(tid, cmd_name, actual_payload[1:], expected_str, act_type, act_msg)
            
    client.close()

def run_extreme_stress():
    print("\n" + "="*80)
    print(f"{YELLOW}PHASE 1: STRICT SYNCHRONOUS EXTREME STRESS & PROFILING{RESET}")
    print("="*80)

    clients = []
    for i in range(NUM_THREADS):
        c = StrictCacheCore(HOST, PORT)
        if c.connect() is True:
            act_type, act_msg = c.execute_strictly(0, "EXISTS", "probe")
            if "max clients" in act_msg:
                print(f"[T{i}] {YELLOW}Rejected cleanly by server.{RESET}")
                c.close()
            else:
                clients.append((i, c))
                print(f"[T{i}] {GREEN}Connected.{RESET}")
        else:
            print(f"[T{i}] {RED}TCP Connect failed.{RESET}")

    print(f"\n{CYAN}Spawning {len(clients)} aggressive blocking workers...{RESET}")
    start_time = time.perf_counter()

    threads = []
    for tid, client in clients:
        t = threading.Thread(target=extreme_worker, args=(tid, client))
        t.start()
        threads.append(t)

    for t in threads:
        t.join()

    duration = time.perf_counter() - start_time

    print("\n" + "="*80)
    print("PERFORMANCE PROFILING & METRICS")
    print("="*80)

    total_ops = metrics.success + metrics.mismatch + metrics.dropped
    hit_ratio = (metrics.get_hits / metrics.get_attempts * 100) if metrics.get_attempts > 0 else 0

    print(f"Execution Time:    {duration:.3f} s")
    print(f"Throughput:        {(total_ops / duration):.2f} ops/sec  -> Total request-response cycles completed per second.")
    print(f"Bandwidth (Tx):    {(metrics.tx_bytes / 1024 / 1024):.2f} MB       -> Data sent from client to server.")
    print(f"Bandwidth (Rx):    {(metrics.rx_bytes / 1024 / 1024):.2f} MB       -> Data received from server to client.")
    print(f"Cache Hit Ratio:   {hit_ratio:.1f}%          -> Percentage of GET requests that successfully returned a value.")
    
    print("-" * 80)
    print(f"Total Attempted:   {total_ops}")
    print(f"Clean Success:     {GREEN}{metrics.success}{RESET}")
    print(f"Protocol Mismatch: {RED}{metrics.mismatch}{RESET}")
    print(f"Socket Drops:      {RED}{metrics.dropped}{RESET}")
    
    if metrics.latencies:
        lats = sorted(metrics.latencies)
        print("-" * 80)
        print(f"{CYAN}LATENCY DISTRIBUTION (ms){RESET}")
        print(f"Min:   {lats[0]:.2f} -> The absolute fastest an operation completed (Baseline network/system overhead).")
        print(f"p50:   {statistics.quantiles(lats, n=100)[49]:.2f} -> Median latency. 50% of requests are faster than this.")
        print(f"p90:   {statistics.quantiles(lats, n=100)[89]:.2f} -> 90% of requests are faster than this.")
        print(f"p95:   {statistics.quantiles(lats, n=100)[94]:.2f} -> 95% of requests are faster. Spikes here indicate thread/mutex queuing.")
        print(f"p99:   {statistics.quantiles(lats, n=100)[98]:.2f} -> 99% of requests are faster. Crucial metric for worst-case user experience.")
        print(f"Max:   {lats[-1]:.2f} -> The absolute slowest operation (Likely OS context switch, GC, or eviction pause).")

    if metrics.mismatch > 0:
        print("\n" + RED + "FAILURE SAMPLES:" + RESET)
        for f in metrics.failures:
            print(f)

    print("\n" + "="*80)
    if metrics.mismatch == 0 and metrics.dropped == 0 and len(clients) == MAX_CLIENTS:
        print(f"{GREEN}SYSTEM PASSED. Mutex hierarchy handles 100% contention smoothly.{RESET}")
    else:
        print(f"{RED}SYSTEM CRACKED. Check C++ logs.{RESET}")

if __name__ == "__main__":
    conf_path = os.path.join(os.path.dirname(__file__), ".configs", "deadlock_stress.conf")
    PORT, NUM_DBS = generate_config(conf_path)
    
    server_process = start_server(conf_path)
    
    try:
        ok = run_correctness_tests()
        if not ok:
            print(f"\n{RED}ABORTING stress test — correctness failures detected.{RESET}")
        else:
            run_extreme_stress()
    finally:
        print(f"\n{CYAN}Shutting down server...{RESET}")
        server_process.terminate()
        server_process.wait()
