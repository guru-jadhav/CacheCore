import socket
import random
import string
import threading
import time
import statistics
from collections import defaultdict
import subprocess
import os
import sys

HOST = "127.0.0.1"
PORT = 0 # Will be assigned dynamically
NUM_DBS = 0 # Will be assigned dynamically

NUM_THREADS = 15
OPS_PER_THREAD = 5000
MAX_CLIENTS = 10

commands = ["SET", "GET", "DEL", "EXISTS", "EXPIRE", "INCR", "CLEAR", "PING"]
HOT_KEYS = ["hot1", "hot2", "hot3"]

lock = threading.Lock()

accepted_clients = []
rejected_clients = []

stats = {
    "success": 0,
    "mismatch": 0,
    "dropped": 0,
    "latencies": [],
    "per_cmd_lat": defaultdict(list)
}

failures = []

# =========================
# COLORS
# =========================
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
    # GPT tests multi-db isolation on DB 0, 1, and 2, so we need at least 3 DBs
    num_dbs = random.randint(3, 6) 
    
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
    print(f"{YELLOW}TEST: Multi-DB Broad Workload Stress Test{RESET}")
    print(f"{YELLOW}PURPOSE: Spreads random CRUD commands across multiple{RESET}")
    print(f"{YELLOW}         databases and keys to test data isolation,{RESET}")
    print(f"{YELLOW}         general throughput, and memory stability.{RESET}")
    print(f"{YELLOW}========================================================\n{RESET}")
    # -------------------------------------------------

    process = subprocess.Popen([server_bin, conf_path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1) # Give it a moment to bind and listen
    
    if process.poll() is not None:
        print(f"{RED}Server failed to start. Return code: {process.returncode}{RESET}")
        sys.exit(1)
        
    return process

# =========================
# RESP
# =========================
def to_resp(arr):
    res = f"*{len(arr)}\r\n"
    for x in arr:
        x = str(x)
        res += f"${len(x)}\r\n{x}\r\n"
    return res.encode()

def recv_full(sock):
    first = sock.recv(1)
    if not first:
        return ""

    data = first

    if first in [b'+', b'-', b':']:
        while not data.endswith(b"\r\n"):
            data += sock.recv(1)

    elif first == b'$':
        while not data.endswith(b"\r\n"):
            data += sock.recv(1)
        length = int(data[1:-2])
        if length != -1:
            body = b""
            while len(body) < length + 2:
                body += sock.recv(length + 2 - len(body))
            data += body

    return data.decode()

def classify(resp):
    if not resp: return "NONE"
    if resp.startswith("+"): return "STRING"
    if resp.startswith("-"): return "ERROR"
    if resp.startswith(":"): return "INTEGER"
    if resp.startswith("$-1"): return "NULLBULK"
    if resp.startswith("$"): return "BULK"
    return "INVALID"

def extract_value(resp):
    """Extract the actual value from a RESP response."""
    t = classify(resp)
    if t == "STRING": return resp[1:].strip()
    if t == "ERROR": return resp[1:].strip()
    if t == "INTEGER": return resp[1:].strip()
    if t == "NULLBULK": return None
    if t == "BULK":
        lines = resp.split("\r\n")
        return lines[1] if len(lines) > 1 else ""
    return resp

# =========================
# GENERATION
# =========================
def rand_key():
    return random.choice(HOT_KEYS) if random.random() < 0.7 else ''.join(random.choices(string.ascii_letters, k=5))

def rand_val():
    return str(random.randint(0,1000)) if random.random() < 0.5 else ''.join(random.choices(string.ascii_letters, k=5))

def build_cmd():
    cmd = random.choice(commands)
    db = random.choice(range(NUM_DBS))

    if cmd == "PING": return cmd, [db, cmd]
    if cmd == "SET": return cmd, [db, cmd, rand_key(), rand_val(), random.randint(0,1)]
    if cmd == "GET": return cmd, [db, cmd, rand_key()]
    if cmd == "DEL": return cmd, [db, cmd, rand_key()]
    if cmd == "EXISTS": return cmd, [db, cmd, rand_key()]
    if cmd == "EXPIRE": return cmd, [db, cmd, rand_key(), random.randint(0,200)]
    if cmd == "INCR": return cmd, [db, cmd, rand_key()]
    if cmd == "CLEAR": return cmd, [db, cmd]

# =========================
# VALIDATION (per README)
# =========================
def is_valid(cmd, t, resp):
    if resp.startswith("-ERR"): return True
    if cmd == "PING": return t == "STRING"
    if cmd == "SET": return t == "INTEGER"
    if cmd in ["DEL","CLEAR"]: return t == "STRING"
    if cmd == "EXPIRE": return t in ["STRING","ERROR"]
    if cmd == "GET": return t in ["BULK", "NULLBULK"]
    if cmd == "EXISTS": return t == "INTEGER"
    if cmd == "INCR": return t in ["INTEGER","ERROR"]
    return False

# =========================
# CORRECTNESS TESTS
# =========================
correctness_passed = 0
correctness_failed = 0
correctness_failures_list = []

def send_cmd(sock, db, cmd, *args):
    arr = [db, cmd] + list(args)
    sock.sendall(to_resp(arr))
    return recv_full(sock)

def assert_resp(test_name, resp, expected_type, expected_value=None):
    global correctness_passed, correctness_failed
    t = classify(resp)
    val = extract_value(resp)
    ok = t == expected_type
    if expected_value is not None:
        ok = ok and val == expected_value
    if ok:
        correctness_passed += 1
        print(f"  {GREEN}✓{RESET} {test_name}")
    else:
        exp = f"{expected_type}" + (f" => {expected_value}" if expected_value else "")
        act = f"{t} => {val}"
        msg = f"  {RED}✗{RESET} {test_name}  |  expected: {exp}  |  actual: {act}"
        print(msg)
        correctness_failed += 1
        correctness_failures_list.append(msg)

def run_correctness_tests():
    global correctness_passed, correctness_failed

    print("=" * 80)
    print(f"{CYAN}PHASE 0: CORRECTNESS & EDGE CASE TESTS{RESET}")
    print("=" * 80)

    s = socket.socket()
    try:
        s.connect((HOST, PORT))
    except Exception as e:
        print(f"{RED}Cannot connect: {e}{RESET}")
        return False

    # clean slate
    for i in range(NUM_DBS):
        send_cmd(s, i, "CLEAR")

    # --- PING ---
    print(f"\n{YELLOW}[PING]{RESET}")
    r = send_cmd(s, 0, "PING")
    assert_resp("PING → +PONG", r, "STRING", "PONG")

    # --- SET/GET correctness ---
    print(f"\n{YELLOW}[SET/GET correctness]{RESET}")
    r = send_cmd(s, 0, "SET", "fruit", "apple", "0")
    assert_resp("SET fruit=apple → :1", r, "INTEGER", "1")
    r = send_cmd(s, 0, "GET", "fruit")
    assert_resp("GET fruit → apple", r, "BULK", "apple")

    # overwrite
    r = send_cmd(s, 0, "SET", "fruit", "banana", "0")
    assert_resp("SET fruit=banana (overwrite) → :1", r, "INTEGER", "1")
    r = send_cmd(s, 0, "GET", "fruit")
    assert_resp("GET fruit → banana", r, "BULK", "banana")

    # GET miss
    r = send_cmd(s, 0, "GET", "ghost")
    assert_resp("GET miss → $-1", r, "NULLBULK")

    # --- DEL ---
    print(f"\n{YELLOW}[DEL]{RESET}")
    send_cmd(s, 0, "SET", "temp", "x", "0")
    r = send_cmd(s, 0, "DEL", "temp")
    assert_resp("DEL existing → +OK", r, "STRING", "OK")
    r = send_cmd(s, 0, "GET", "temp")
    assert_resp("GET after DEL → $-1", r, "NULLBULK")
    r = send_cmd(s, 0, "DEL", "never_was")
    assert_resp("DEL non-existent → +OK (idempotent)", r, "STRING", "OK")

    # --- EXISTS ---
    print(f"\n{YELLOW}[EXISTS]{RESET}")
    send_cmd(s, 0, "SET", "alive", "yes", "0")
    r = send_cmd(s, 0, "EXISTS", "alive")
    assert_resp("EXISTS present → :1", r, "INTEGER", "1")
    send_cmd(s, 0, "DEL", "alive")
    r = send_cmd(s, 0, "EXISTS", "alive")
    assert_resp("EXISTS after DEL → :0", r, "INTEGER", "0")
    r = send_cmd(s, 0, "EXISTS", "never_was_2")
    assert_resp("EXISTS non-existent → :0", r, "INTEGER", "0")

    # --- INCR ---
    print(f"\n{YELLOW}[INCR]{RESET}")
    send_cmd(s, 0, "DEL", "cnt")
    r = send_cmd(s, 0, "INCR", "cnt")
    assert_resp("INCR missing → :1 (auto-create)", r, "INTEGER", "1")
    r = send_cmd(s, 0, "INCR", "cnt")
    assert_resp("INCR → :2", r, "INTEGER", "2")

    send_cmd(s, 0, "SET", "word", "hello", "0")
    r = send_cmd(s, 0, "INCR", "word")
    assert_resp("INCR non-numeric → ERROR", r, "ERROR")

    send_cmd(s, 0, "SET", "neg", "-10", "0")
    r = send_cmd(s, 0, "INCR", "neg")
    assert_resp("INCR negative (-10) → :-9", r, "INTEGER", "-9")

    send_cmd(s, 0, "SET", "big", "9223372036854775807", "0")
    r = send_cmd(s, 0, "INCR", "big")
    assert_resp("INCR at LLONG_MAX → ERROR", r, "ERROR")

    # --- EXPIRE ---
    print(f"\n{YELLOW}[EXPIRE]{RESET}")
    r = send_cmd(s, 0, "EXPIRE", "ghost_key", "100")
    assert_resp("EXPIRE non-existent → +OK", r, "STRING", "OK")
    r = send_cmd(s, 0, "EXPIRE", "fruit", "notnum")
    assert_resp("EXPIRE invalid TTL → ERROR", r, "ERROR")

    # --- SET TTL clear ---
    print(f"\n{YELLOW}[SET TTL clear]{RESET}")
    send_cmd(s, 0, "SET", "ttltest", "v1", "1")
    send_cmd(s, 0, "SET", "ttltest", "v2", "0")  # clears TTL
    r = send_cmd(s, 0, "GET", "ttltest")
    assert_resp("SET with isExpires=0 clears TTL", r, "BULK", "v2")

    # --- CLEAR ---
    print(f"\n{YELLOW}[CLEAR]{RESET}")
    send_cmd(s, 0, "SET", "x1", "a", "0")
    send_cmd(s, 0, "SET", "x2", "b", "0")
    r = send_cmd(s, 0, "CLEAR")
    assert_resp("CLEAR → +OK", r, "STRING", "OK")
    r = send_cmd(s, 0, "GET", "x1")
    assert_resp("GET after CLEAR → $-1", r, "NULLBULK")
    r = send_cmd(s, 0, "EXISTS", "x2")
    assert_resp("EXISTS after CLEAR → :0", r, "INTEGER", "0")

    # --- ERROR PATHS ---
    print(f"\n{YELLOW}[Error paths]{RESET}")
    r = send_cmd(s, 99, "GET", "k1")
    assert_resp("Invalid DB index (99) → ERROR", r, "ERROR")
    r = send_cmd(s, 0, "XYZZY", "k1")
    assert_resp("Unknown command → ERROR", r, "ERROR")
    r = send_cmd(s, 0, "GET", "k1", "extra")
    assert_resp("GET wrong arg count → ERROR", r, "ERROR")
    r = send_cmd(s, 0, "SET", "k1")
    assert_resp("SET wrong arg count → ERROR", r, "ERROR")
    r = send_cmd(s, 0, "CLEAR", "junk")
    assert_resp("CLEAR wrong arg count → ERROR", r, "ERROR")
    r = send_cmd(s, 0, "INCR")
    assert_resp("INCR no args → ERROR", r, "ERROR")

    # --- MULTI-DB ISOLATION ---
    print(f"\n{YELLOW}[Multi-DB isolation]{RESET}")
    send_cmd(s, 0, "CLEAR")
    send_cmd(s, 1, "CLEAR")
    send_cmd(s, 2, "CLEAR")

    send_cmd(s, 0, "SET", "iso", "db0", "0")
    send_cmd(s, 1, "SET", "iso", "db1", "0")
    send_cmd(s, 2, "SET", "iso", "db2", "0")

    r = send_cmd(s, 0, "GET", "iso")
    assert_resp("DB0 isolated value", r, "BULK", "db0")
    r = send_cmd(s, 1, "GET", "iso")
    assert_resp("DB1 isolated value", r, "BULK", "db1")
    r = send_cmd(s, 2, "GET", "iso")
    assert_resp("DB2 isolated value", r, "BULK", "db2")

    send_cmd(s, 0, "DEL", "iso")
    r = send_cmd(s, 1, "GET", "iso")
    assert_resp("DEL on DB0 doesn't affect DB1", r, "BULK", "db1")
    r = send_cmd(s, 2, "GET", "iso")
    assert_resp("DEL on DB0 doesn't affect DB2", r, "BULK", "db2")

    send_cmd(s, 1, "CLEAR")
    r = send_cmd(s, 2, "EXISTS", "iso")
    assert_resp("CLEAR on DB1 doesn't affect DB2", r, "INTEGER", "1")

    # --- EMPTY KEY/VALUE ---
    print(f"\n{YELLOW}[Empty key/value]{RESET}")
    r = send_cmd(s, 0, "SET", "", "emptykey", "0")
    assert_resp("SET empty key → :1", r, "INTEGER", "1")
    r = send_cmd(s, 0, "GET", "")
    assert_resp("GET empty key → emptykey", r, "BULK", "emptykey")
    r = send_cmd(s, 0, "SET", "emptyval", "", "0")
    assert_resp("SET empty value → :1", r, "INTEGER", "1")
    r = send_cmd(s, 0, "GET", "emptyval")
    assert_resp("GET empty value → ''", r, "BULK", "")

    # --- LARGE VALUE ---
    print(f"\n{YELLOW}[Large payload]{RESET}")
    big = "X" * 50000
    r = send_cmd(s, 0, "SET", "bigkey", big, "0")
    assert_resp("SET 50KB value → :1", r, "INTEGER", "1")
    r = send_cmd(s, 0, "GET", "bigkey")
    t = classify(r)
    v = extract_value(r)
    ok = t == "BULK" and v == big
    if ok:
        correctness_passed += 1
        print(f"  {GREEN}✓{RESET} GET 50KB value matches")
    else:
        correctness_failed += 1
        msg = f"  {RED}✗{RESET} GET 50KB value mismatch (got {len(v) if v else 0} bytes)"
        print(msg)
        correctness_failures_list.append(msg)

    # cleanup
    for i in range(NUM_DBS):
        send_cmd(s, i, "CLEAR")
        
    s.close()

    # --- SUMMARY ---
    print(f"\n{'-'*60}")
    total = correctness_passed + correctness_failed
    print(f"Correctness: {GREEN}{correctness_passed}{RESET}/{total} passed, {RED}{correctness_failed}{RESET} failed")
    if correctness_failures_list:
        print(f"\n{RED}FAILURES:{RESET}")
        for f in correctness_failures_list:
            print(f)

    return correctness_failed == 0

# =========================
# CONNECTION PHASE
# =========================
def connect_phase(tid):
    try:
        s = socket.socket()
        s.connect((HOST, PORT))

        probe = [0, "PING"]
        s.sendall(to_resp(probe))
        resp = recv_full(s)

        if resp.startswith("-ERR"):
            with lock:
                rejected_clients.append((tid, resp.strip()))
            s.close()
            print(f"[T{tid}] ❌ Rejected → {resp.strip()}")
        else:
            with lock:
                accepted_clients.append((tid, s))
            print(f"[T{tid}] ✅ Connected (PING → {resp.strip()})")

    except Exception as e:
        with lock:
            rejected_clients.append((tid, str(e)))
        print(f"[T{tid}] ❌ TCP FAIL → {e}")

# =========================
# WORKER
# =========================
def worker(tid, sock):
    for _ in range(OPS_PER_THREAD):
        cmd, arr = build_cmd()
        start = time.perf_counter()

        try:
            sock.sendall(to_resp(arr))
            resp = recv_full(sock)
            latency = (time.perf_counter() - start) * 1000

            t = classify(resp)

            with lock:
                stats["latencies"].append(latency)
                stats["per_cmd_lat"][cmd].append(latency)

                if is_valid(cmd, t, resp):
                    stats["success"] += 1
                else:
                    stats["mismatch"] += 1
                    if len(failures) < 15:
                        failures.append(f"[T{tid}] {cmd} → {t} = {repr(resp[:100])}")

        except:
            with lock:
                stats["dropped"] += 1
            break

    sock.close()

# =========================
# METRICS PRINT
# =========================
def print_latency_stats(lat):
    lat.sort()

    def p(x): return lat[int(len(lat)*x)]

    print(f"\nLatency Breakdown (ms):")
    print(f"Min   : {lat[0]:.2f}  → fastest possible execution (best-case path)")
    print(f"P50   : {p(0.50):.2f} → typical request (median user experience)")
    print(f"P90   : {p(0.90):.2f} → mild contention starts appearing")
    print(f"P95   : {p(0.95):.2f} → tail latency (important for SLAs)")
    print(f"P99   : {p(0.99):.2f} → rare slow ops (contention / locks)")
    print(f"Max   : {lat[-1]:.2f} → worst-case spike (blocking / eviction / OS)")
    print(f"StdDev: {statistics.stdev(lat):.2f} → jitter (stability of system)")

    # Histogram
    buckets = [0,1,2,5,10,50,100,999]
    hist = {b:0 for b in buckets}

    for v in lat:
        for b in buckets:
            if v <= b:
                hist[b]+=1
                break

    print("\nLatency Distribution:")
    for b in buckets:
        print(f"≤ {b:>3} ms : {hist[b]}")

# =========================
# RUN
# =========================
def run():
    # Phase 0 — correctness
    ok = run_correctness_tests()
    if not ok:
        print(f"\n{RED}ABORTING stress test — correctness failures detected.{RESET}")
        return

    # Phase 1 — connection test
    print("\n" + "="*80)
    print("PHASE 1: CONNECTION TEST")
    print("="*80)

    threads = []
    for i in range(NUM_THREADS):
        t = threading.Thread(target=connect_phase, args=(i,))
        t.start()
        threads.append(t)

    for t in threads:
        t.join()

    print("\nSUMMARY:")
    print("Accepted:", [tid for tid,_ in accepted_clients])
    print("Rejected:", rejected_clients)

    # Phase 2 — stress
    print("\n" + "="*80)
    print("PHASE 2: STRESS TEST")
    print("="*80)

    start = time.perf_counter()

    workers = []
    for tid, sock in accepted_clients:
        t = threading.Thread(target=worker, args=(tid, sock))
        t.start()
        workers.append(t)

    for t in workers:
        t.join()

    duration = time.perf_counter() - start
    total_ops = len(accepted_clients)*OPS_PER_THREAD

    print("\nRESULTS")
    print("="*80)

    print(f"Throughput        : {total_ops/duration:.2f} ops/sec → system capacity")
    print(f"Per-client TPS    : {(total_ops/duration)/len(accepted_clients):.2f} → fairness/load per client")

    error_rate = (stats["mismatch"] + stats["dropped"]) / total_ops
    print(f"Error Rate        : {error_rate*100:.4f}% → reliability")

    print(f"Success           : {stats['success']}")
    print(f"Mismatch          : {stats['mismatch']}")
    print(f"Dropped           : {stats['dropped']}")

    if stats["latencies"]:
        print_latency_stats(stats["latencies"])

    print("\nPer Command Avg Latency:")
    for cmd, vals in stats["per_cmd_lat"].items():
        if vals:
            print(f"{cmd:<8}: {sum(vals)/len(vals):.2f} ms")

    if failures:
        print(f"\n{RED}FAILURE SAMPLES:{RESET}")
        for f in failures:
            print(f)

    print("\nVERDICT:",
          f"{GREEN}SOLID 🚀{RESET}" if stats["mismatch"]==0 and stats["dropped"]==0 else f"{RED}INVESTIGATE ❌{RESET}")

if __name__ == "__main__":
    conf_path = os.path.join(os.path.dirname(__file__), ".configs", "multidb_stress.conf")
    PORT, NUM_DBS = generate_config(conf_path)
    
    server_process = start_server(conf_path)
    
    try:
        run()
    finally:
        print(f"\n{CYAN}Shutting down server...{RESET}")
        server_process.terminate()
        server_process.wait()
