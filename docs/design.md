# Concurrent HTTP/1.1 Server: Design

Date: 2026-11-15
Status: approved

## 1. Intent

A concurrent HTTP/1.1 server written in C++17 directly on Linux TCP sockets, built to compare two concurrency models on identical HTTP code. Anyone should be able to clone it, build it with one command, run the tests and benchmarks, and check every capability below against code and measurements. Clarity beats cleverness throughout.

What the server must do:

1. HTTP/1.1 server from raw TCP sockets: request parsing, routing, connection handling, static and JSON responses.
2. Concurrent request processing with a thread pool; benchmarks of throughput and p50/p95/p99 latency under varying client loads.
3. Non-blocking I/O with Linux epoll; keep-alive connections, graceful shutdown, configurable request timeouts.

## 2. Goals, non-goals, anti-goals

**Goals**

- One binary, two concurrency models selected by `--mode=pool|epoll`, sharing the same parser, router, handlers and response code.
- Correct handling of partial reads, partial writes, pipelined requests, slow clients and abrupt disconnects in both modes.
- A reproducible benchmark that compares the two modes and shows where each one breaks down.
- Tests that run in CI, including under AddressSanitizer, UBSan and ThreadSanitizer.
- A README that explains the architecture, the decisions, the results and the limitations.

**Non-goals**

TLS, HTTP/2, chunked transfer encoding, compression, range requests, conditional requests (`If-Modified-Since`), `Expect: 100-continue`, virtual hosts, CGI, a config file, JSON request-body parsing, and any platform other than Linux.

**Anti-goals** (would make the repo a failure even if it works)

- Benchmark numbers that cannot be reproduced from the repo, or that hide the hardware they were run on.
- Any third-party HTTP, networking or JSON library. The standard library and Linux system calls only.
- Code that is hard to follow: heavy template metaprogramming, lock-free tricks without need.
- Claiming production readiness.

## 3. Constraints

- C++17, GCC 13 or Clang, CMake 3.16 or later, Linux only. Flags: `-Wall -Wextra -Wpedantic -Werror`.
- No fetched dependencies. Tests use a small in-repo assertion header and CTest; integration tests use Python 3 standard library only.
- The published benchmarks were run on a 2-core cloud VM with client and server on the same machine over loopback. The README states this next to the numbers.

## 4. Approaches considered

| | A. Two modes, one binary (chosen) | B. Single hybrid | C. epoll only, loop per thread |
|---|---|---|---|
| Shape | Blocking thread-pool server and an epoll server behind one flag | One epoll I/O thread hands parsed requests to a worker pool | One epoll loop per thread, no separate pool |
| Fit to the goals | Both models exist in their simplest form | Both present, but fused into one design | No thread-pool model at all |
| Benchmark story | Same workload, two designs, visible crossover | Only thread count to vary | Nothing to compare against |
| Cost | Two connection paths to write and test | Cross-thread handoff and locking | Least code |

**Chosen: A.** It keeps each model in its simplest form and makes the comparison measurable: the load at which thread-per-connection stops scaling and why epoll does not.

## 5. Architecture

```
                 +----------------------- shared -----------------------+
 bytes in  --->  | HttpParser -> Request -> Router -> Handler -> Response | ---> bytes out
                 +------------------------------------------------------+
                      ^                                         ^
        +-------------+--------------+           +--------------+-------------+
        | pool mode                  |           | epoll mode                 |
        | acceptor thread            |           | N event-loop threads       |
        | bounded queue of fds       |           | each: own listen socket    |
        | N workers, blocking I/O,   |           | (SO_REUSEPORT), own epoll, |
        | one connection per worker  |           | non-blocking I/O, many     |
        |                            |           | connections per thread     |
        +----------------------------+           +----------------------------+
```

### 5.1 Shared HTTP layer

**Parser.** An incremental state machine fed arbitrary byte chunks. It returns one of: need more data, request complete (with the number of bytes consumed, so leftover bytes start the next pipelined request), or error with an HTTP status. It never assumes a request arrives in one read.

- Request line: method, target, version. Accepts HTTP/1.1 and HTTP/1.0; other versions get 505.
- Headers: case-insensitive names, leading and trailing whitespace trimmed from values. HTTP/1.1 without `Host` gets 400.
- Body: `Content-Length` only. `Transfer-Encoding` present gets 501. Invalid or duplicate conflicting `Content-Length` gets 400.
- Limits: request line plus headers over `--max-header-bytes` (default 8192) gets 431; body over `--max-body-bytes` (default 1 MiB) gets 413.
- Target: split into path and query string; path is percent-decoded. Malformed encoding gets 400.

**Router.** Registered as (method, pattern, handler). Patterns support literal segments and named parameters (`/api/users/:id`). No match gets 404; path matches but method does not gets 405 with an `Allow` header. `HEAD` is served by the `GET` handler with the body dropped. Unsupported methods get 501. The handler signature is `Response(const Request&)`; handlers are synchronous.

**Response.** Status, headers, body. The serializer always adds `Content-Length`, `Date`, `Server` and `Connection`. Error responses carry a small JSON body.

**Keep-alive rules.** HTTP/1.1 is persistent unless the request says `Connection: close`. HTTP/1.0 is persistent only with `Connection: keep-alive`. The connection is closed after `--max-keepalive-requests` (default 1000), after any parse error, and during shutdown.

**Static files.** Served from `--root` for any `GET`/`HEAD` path no route claims. After decoding, the path is normalized and resolved with `realpath`; anything that resolves outside the root gets 403 or 404. Directories serve `index.html` if present. Content type comes from a small extension table. Files are read fully into the response buffer; files over 64 MiB are refused with a 500 and a log line. `sendfile` is left as a documented extension.

**JSON.** A small writer that builds objects and arrays and escapes strings correctly. Built-in endpoints:

| Route | Purpose |
|---|---|
| `GET /health` | `{"status":"ok"}`; the small-response benchmark target |
| `GET /api/stats` | uptime, mode, threads, active connections, total requests, counts by status class |
| `GET /api/time` | current server time |
| `POST /api/echo` | returns method, path, headers count, body length and body as a JSON string |
| `GET /api/users/:id` | demonstrates path parameters |

### 5.2 Pool mode (blocking)

- The acceptor thread waits with `poll` on the listen socket and a shutdown `eventfd`, accepts, and pushes the fd into a bounded queue (default capacity 1024).
- If the queue is full, the acceptor writes a short `503 Service Unavailable` and closes the socket. This keeps overload visible instead of letting connections hang in the kernel backlog.
- `--threads` workers (default: hardware concurrency) each pop an fd and own that connection until it closes: read, parse, handle, write, repeat while keep-alive holds.
- Timeouts use `SO_RCVTIMEO`/`SO_SNDTIMEO` plus a wall-clock check of the request deadline.
- Known and intended weakness: one idle keep-alive client occupies one worker. The benchmark demonstrates this.

The `ThreadPool` is a standalone class (mutex, condition variable, task queue, clean join on destruction) with its own unit tests.

### 5.3 Epoll mode (non-blocking)

- `--threads` event-loop threads. Each opens its own listening socket with `SO_REUSEPORT`, so the kernel spreads new connections across threads and no fd is ever handed between threads. A connection lives and dies on one thread, so per-connection state needs no locks.
- Edge-triggered (`EPOLLET`) with `EPOLLRDHUP`. On readable: `accept4`/`read` until `EAGAIN`. On writable: `write` until done or `EAGAIN`.
- Per-connection state: fd, read buffer, parser, write buffer and offset, deadline, request count, and a phase (reading, writing, closing).
- Writes are attempted immediately; `EPOLLOUT` interest is added only when the kernel buffer is full and removed once the write buffer drains.
- Pipelined requests are answered in order: the next request is parsed only after the previous response is fully queued.
- Handlers run inline on the loop thread. They are short CPU work; the one blocking call is the static-file disk read, which is accepted and documented.
- Timers: each loop keeps connections in a list ordered by deadline. Because every deadline of a given kind is "now plus a fixed duration", updating one is a move to the back, O(1). `epoll_wait` sleeps until the earliest deadline.

### 5.4 Timeouts (both modes)

| Flag | Default | Meaning | On expiry |
|---|---|---|---|
| `--request-timeout` | 10 s | From first byte of a request to the full request received. Defends against slow-header clients. | `408 Request Timeout`, close |
| `--keepalive-timeout` | 5 s | Idle time allowed between requests on a persistent connection | close silently |
| `--write-timeout` | 10 s | No progress writing a response | close |

### 5.5 Graceful shutdown (both modes)

1. `SIGINT` or `SIGTERM` handler does only async-signal-safe work: sets an atomic flag and writes to an `eventfd` that wakes every thread.
2. Listening sockets close; no new connections.
3. Idle keep-alive connections close immediately.
4. Requests already in flight finish; their responses carry `Connection: close`.
5. After `--shutdown-grace` (default 5 s), remaining connections are force-closed.
6. Threads join, final stats are logged, exit code 0.

`SIGPIPE` is ignored and sends use `MSG_NOSIGNAL`.

### 5.6 Configuration and logging

CLI flags only: `--mode`, `--port` (8080), `--bind`, `--threads`, `--root`, `--backlog`, `--queue-capacity`, the three timeouts, `--max-keepalive-requests`, `--max-header-bytes`, `--max-body-bytes`, `--shutdown-grace`, `--log-level`, `--access-log`. Invalid flags print usage and exit non-zero.

A minimal thread-safe logger writes to stderr. The access log is off by default so it does not distort benchmarks.

## 6. Benchmarking

**Load generator (`bench/loadgen`, C++).** Epoll-based so the client is not the bottleneck. Closed loop: each of `-c` connections sends a request, waits for the full response, records the latency in nanoseconds, repeats for `-d` seconds after a warm-up. Options: connections, duration, client threads, keep-alive on or off, path, and `--idle N` to hold N extra silent keep-alive connections open. Output: requests per second, p50/p95/p99/max latency, error and timeout counts, as text and as a CSV row.

**Matrix (`bench/run.sh`).**

- Modes: pool, epoll.
- Connections: 1, 16, 64, 256, 1024.
- Workloads: `GET /health` (tiny JSON), a 16 KiB static file.
- Keep-alive on for the main sweep; one sweep with a new connection per request.
- Idle-client scenario: 64 active connections plus 0, 256, 1024 idle ones.

Results land in `bench/results/` as CSV plus a generated Markdown table that the README embeds. A chart is generated if a plotting library is available; otherwise the tables stand alone.

**Honesty notes that go in the README:** hardware and kernel, loopback with client and server sharing cores, and that a closed-loop generator measures service time and understates tail latency under overload (coordinated omission).

## 7. Testing

- **Unit (C++, CTest):** parser fed whole, byte by byte, and with pipelined input; every limit and error status; router matching, parameters, 404/405; path normalization and traversal attempts; response serialization; JSON escaping; thread pool.
- **Integration (Python 3 stdlib, run against both modes):** keep-alive reuse, pipelining, `Connection: close`, HEAD, large body, 400/404/405/408/413/431/501/505, idle timeout, slow-header client, client disconnect mid-request, traversal, pool-mode 503 on overload, graceful shutdown with a request in flight.
- **Sanitizer builds:** ASan plus UBSan, and TSan, each running the full suite.
- **CI:** GitHub Actions on `ubuntu-latest`: release build and tests, then the two sanitizer jobs.

## 8. Repository layout

```
CMakeLists.txt
README.md  LICENSE  .clang-format  .gitignore
.github/workflows/ci.yml
src/
  main.cpp  config.{h,cpp}  logger.{h,cpp}
  http/    request.h  response.{h,cpp}  parser.{h,cpp}  router.{h,cpp}  mime.{h,cpp}  json.{h,cpp}
  net/     socket.{h,cpp}  (RAII fd, listen helpers)
  server/  server.h  thread_pool.{h,cpp}  pool_server.{h,cpp}  epoll_server.{h,cpp}  stats.h
  app/     routes.{h,cpp}  static_files.{h,cpp}
bench/     loadgen.cpp  run.sh  results/
tests/     unit/  integration/
www/       sample static site
docs/      design.md
```

## 9. Choices that could reasonably go the other way

Each can be changed without affecting the rest:

- Edge-triggered rather than level-triggered epoll.
- `SO_REUSEPORT` listener per loop rather than one acceptor handing off fds.
- Pool mode answers 503 when its queue is full rather than blocking the acceptor.
- Whole-file reads for static files rather than `sendfile`.
- In-repo test header rather than GoogleTest, to keep the build free of fetched dependencies.
- All default values in sections 5.1 to 5.6.
- MIT license; repo name `cpp-http-server`.

## 10. Left open for implementation

Internal class boundaries beyond the layout above, buffer types and growth policy, the percentile method in the load generator (sorted samples or histogram), behaviour on `EMFILE` (log and back off), README wording and diagram, and whether a chart can be generated.
