# Benchmark results

Date: 2026-11-15 16:09:05 UTC. Commit: 356bdda (uncommitted: bench/report.py bench/run.sh). Machine, kernel, compiler and flags: [environment.txt](environment.txt).

Server `--threads 2`, loadgen `-t 1` on the same machine over loopback; 10 s measured after 1 s of warm-up; closed loop (each connection waits for its response before sending the next request). Pool mode runs with `--queue-capacity 4096 --keepalive-timeout 5`. Raw data: [raw.csv](raw.csv).

## Keep-alive, `GET /health`

Each connection sends requests back to back on one keep-alive connection; the response is a small JSON body.

| mode | connections | idle | req/s | p50 (ms) | p95 (ms) | p99 (ms) | max (ms) | errors |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| pool | 1 | 0 | 30590 | 0.030 | 0.041 | 0.062 | 14.3 | 0 |
| pool | 16 | 0 | 73769 | 0.023 | 0.045 | 0.064 | 296.4 | 0 |
| pool | 64 | 0 | 76374 | 0.023 | 0.043 | 0.061 | 930.5 | 0 |
| pool | 256 | 0 | 76320 | 0.023 | 0.043 | 0.063 | 3477.5 | 0 |
| pool | 1024 | 0 | 76538 | 0.023 | 0.044 | 0.063 | 5003.6 | 629 (timeout 629) |
| epoll | 1 | 0 | 28108 | 0.031 | 0.052 | 0.070 | 6.772 | 0 |
| epoll | 16 | 0 | 111930 | 0.132 | 0.253 | 0.363 | 8.404 | 0 |
| epoll | 64 | 0 | 129250 | 0.463 | 1.037 | 1.382 | 8.562 | 0 |
| epoll | 256 | 0 | 127256 | 1.556 | 5.385 | 6.040 | 17.6 | 0 |
| epoll | 1024 | 0 | 129737 | 6.978 | 15.1 | 17.9 | 40.2 | 0 |

## Keep-alive, `GET /file16k.bin`

Same as above, serving a 16 KiB static file from memory.

| mode | connections | idle | req/s | p50 (ms) | p95 (ms) | p99 (ms) | max (ms) | errors |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| pool | 1 | 0 | 22440 | 0.039 | 0.065 | 0.089 | 4.125 | 0 |
| pool | 16 | 0 | 44480 | 0.035 | 0.083 | 0.124 | 476.6 | 0 |
| pool | 64 | 0 | 48602 | 0.033 | 0.075 | 0.112 | 1386.4 | 0 |
| pool | 256 | 0 | 50010 | 0.033 | 0.072 | 0.102 | 5009.7 | 5 (timeout 5) |
| pool | 1024 | 0 | 43383 | 0.035 | 0.086 | 0.135 | 5008.9 | 843 (timeout 843) |
| epoll | 1 | 0 | 23512 | 0.039 | 0.054 | 0.079 | 12.9 | 0 |
| epoll | 16 | 0 | 85054 | 0.176 | 0.308 | 0.487 | 8.396 | 0 |
| epoll | 64 | 0 | 88498 | 0.711 | 1.366 | 1.958 | 8.835 | 0 |
| epoll | 256 | 0 | 97004 | 2.748 | 4.612 | 5.752 | 19.9 | 0 |
| epoll | 1024 | 0 | 95702 | 9.142 | 21.1 | 24.6 | 43.3 | 0 |

## New connection per request, `GET /health`

Every request opens a new TCP connection; latency includes connect and accept.

| mode | connections | idle | req/s | p50 (ms) | p95 (ms) | p99 (ms) | max (ms) | errors |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| pool | 16 | 0 | 39055 | 0.289 | 0.552 | 0.800 | 7.487 | 0 |
| pool | 64 | 0 | 38821 | 1.160 | 2.426 | 3.436 | 15.6 | 0 |
| pool | 256 | 0 | 44192 | 4.349 | 7.752 | 10.3 | 37.4 | 0 |
| epoll | 16 | 0 | 42779 | 0.262 | 0.555 | 0.886 | 6.373 | 0 |
| epoll | 64 | 0 | 49306 | 0.976 | 1.847 | 2.761 | 8.056 | 0 |
| epoll | 256 | 0 | 44644 | 4.092 | 8.362 | 11.1 | 38.1 | 0 |

## Idle clients, `GET /health`

64 active keep-alive connections, plus idle connections that are opened before the run and never send anything.

| mode | connections | idle | req/s | p50 (ms) | p95 (ms) | p99 (ms) | max (ms) | errors |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| pool | 64 | 0 | 75402 | 0.023 | 0.044 | 0.059 | 930.5 | 0 |
| pool | 64 | 256 (117 closed by server) |  |  |  |  |  | failed: no successful request (64 errors: timeout 64) |
| pool | 64 | 1024 (118 closed by server) |  |  |  |  |  | failed: no successful request (64 errors: timeout 64) |
| epoll | 64 | 0 | 131559 | 0.454 | 1.029 | 1.405 | 8.503 | 0 |
| epoll | 64 | 256 (256 closed by server) | 129966 | 0.454 | 1.043 | 1.434 | 9.658 | 0 |
| epoll | 64 | 1024 (1024 closed by server) | 127182 | 0.461 | 1.043 | 1.471 | 8.508 | 0 |
