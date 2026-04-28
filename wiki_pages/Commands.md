# Commands

CacheCore uses the standard Redis command structure. Since it uses the RESP protocol, you can interact with it using standard Redis client libraries or raw TCP sockets.

| Command | Arguments | What it does |
|---|---|---|
| `SET` | `key value isExpires` | Stores a key. `isExpires` must be `1` (apply TTL) or `0` (keep forever). |
| `GET` | `key` | Retrieves the value. Resets the LRU recency. |
| `DEL` | `key` | Deletes the key. |
| `EXISTS`| `key` | Returns `1` if key exists and is not expired, else `0`. |
| `EXPIRE`| `key seconds` | Updates a key's TTL. |
| `INCR`  | `key` | Increments an integer value. Initializes missing keys to `1`. |
| `CLEAR` | *(none)* | Wipes the entire database instantly. |
| `PING`  | *(none)* | Health check. Returns `PONG`. |

## How to use them via Python

Because CacheCore requires the **Database Index** as the first argument in the RESP array (which standard `redis-py` doesn't do by default), it is easiest to interact with the server using standard TCP sockets in Python.

Here is a usable copy-paste script to send commands to the server:

```python
import socket

def send_command(sock, db_index, command, *args):
    # Construct the RESP array
    parts = [str(db_index), command] + [str(a) for a in args]
    req = f"*{len(parts)}\r\n"
    for part in parts:
        req += f"${len(part)}\r\n{part}\r\n"
    
    # Send request and print raw response
    sock.sendall(req.encode('utf-8'))
    return sock.recv(1024).decode('utf-8')

# Connect to the server
s = socket.socket()
s.connect(("127.0.0.1", 6948))

# SET key="hello", value="world", isExpires=0 (no TTL) on DB 0
print(send_command(s, 0, "SET", "hello", "world", "0")) 
# Returns: :1\r\n

# GET key="hello" on DB 0
print(send_command(s, 0, "GET", "hello"))
# Returns: $5\r\nworld\r\n
```
