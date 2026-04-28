# Wire Protocol (RESP)

CacheCore communicates using the **Redis Serialization Protocol (RESP)**. 
Every request sent to the server must be formatted as a RESP Array of Bulk Strings.

## Request Structure
```text
*<Number of Arguments>\r\n
$<Length of DB Index>\r\n<DB Index>\r\n
$<Length of Command>\r\n<Command>\r\n
$<Length of Arg 1>\r\n<Arg 1>\r\n
...
```

*(Note: CacheCore requires the first argument to ALWAYS be the target Database Index, which is a slight modification from standard Redis to allow connection-less multiplexing).*

## Examples

**1. PING on DB 0:**
```text
*2\r\n$1\r\n0\r\n$4\r\nPING\r\n
→ +PONG\r\n
```

**2. SET "foo" = "bar" with TTL on DB 0:**
```text
*5\r\n$1\r\n0\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n$1\r\n1\r\n
→ :1\r\n
```

**3. SET "foo" = "bar" without TTL on DB 0:**
```text
*5\r\n$1\r\n0\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n$1\r\n0\r\n
→ :1\r\n
```

**4. GET "foo" on DB 0:**
```text
*3\r\n$1\r\n0\r\n$3\r\nGET\r\n$3\r\nfoo\r\n
→ $3\r\nbar\r\n
```

**5. GET miss (key not found):**
```text
→ $-1\r\n
```

**6. EXPIRE "foo" 120s on DB 0:**
```text
*4\r\n$1\r\n0\r\n$6\r\nEXPIRE\r\n$3\r\nfoo\r\n$3\r\n120\r\n
→ +OK\r\n
```
