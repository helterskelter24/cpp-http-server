#!/usr/bin/env bash
# Benchmark matrix: pool mode vs epoll mode, measured with bench/loadgen.
#
#   bench/run.sh [--duration S=10] [--quick]
#
# Builds Release, then runs every scenario against a fresh server started on a
# kernel-chosen port and stopped with SIGTERM afterwards. Results go to
# bench/results/ (raw.csv, environment.txt, results.md via report.py).
# --quick is a smoke check: 2 s runs, connection counts {1, 64}, written to
# build/bench-quick/ so it never overwrites the published results.
#
# A run that fails (loadgen exit 1 or 2, a hang, a server that does not start)
# becomes a row with status "failed" and the reason; the sweep goes on.

set -euo pipefail

usage() {
    echo "usage: bench/run.sh [--duration SECONDS=10] [--quick]" >&2
    exit 2
}

DURATION=10
QUICK=0
while [ $# -gt 0 ]; do
    case "$1" in
        --duration) [ $# -ge 2 ] || usage; DURATION="$2"; shift 2 ;;
        --quick) QUICK=1; shift ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
done
[[ "$DURATION" =~ ^[0-9]+$ ]] && [ "$DURATION" -gt 0 ] || { echo "--duration must be a positive integer" >&2; exit 2; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
WARMUP=1

if [ "$QUICK" -eq 1 ]; then
    DURATION=2
    OUT="$BUILD/bench-quick"
    MAIN_CONNS=(1 64)
    NOKA_CONNS=(1 64)
else
    OUT="$ROOT/bench/results"
    MAIN_CONNS=(1 16 64 256 1024)
    NOKA_CONNS=(16 64 256)
fi
IDLE_ACTIVE=64
IDLE_COUNTS=(0 256 1024)
MODES=(pool epoll)

# Client and server share the machine's cores over loopback. The server gets
# one thread per core (pool workers or event loops), as it would in production.
# loadgen gets one thread: a single epoll loop drives all connections, and a
# second client thread would only take more CPU away from the server under test.
SERVER_THREADS="$(nproc)"
LOADGEN_THREADS=1
# Pool mode only: a deep accept queue so connections beyond the workers wait
# instead of getting 503 at once, and a 5 s keep-alive timeout so idle clients
# eventually give their worker back.
POOL_FLAGS=(--queue-capacity 4096 --keepalive-timeout 5)

# 1024 connections plus 1024 idle ones need far more descriptors than the
# usual soft limit of 1024. Raise to 8192, or as far as the hard limit allows;
# runs that still do not fit are skipped with the reason.
ulimit -n 8192 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
FD_LIMIT="$(ulimit -n)"
CLK_TCK="$(getconf CLK_TCK)"

echo "building Release in $BUILD" >&2
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j "$(nproc)" >/dev/null

mkdir -p "$OUT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CSV="$OUT/raw.csv"

# ---- environment.txt ----
{
    echo "date:            $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    commit="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    # The results directory itself is not part of what was measured.
    dirty="$(git -C "$ROOT" status --porcelain -- . ':!bench/results' 2>/dev/null | awk '{print $2}' | tr '\n' ' ' | sed 's/ *$//')"
    if [ -n "$dirty" ]; then
        echo "git commit:      $commit (uncommitted: $dirty)"
    else
        echo "git commit:      $commit"
    fi
    echo "cpu:             $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
    echo "cores (nproc):   $(nproc)"
    echo "memory:          $(awk '/MemTotal/ {printf "%.1f GiB", $2 / 1048576}' /proc/meminfo)"
    echo "kernel:          $(uname -sr)"
    echo "compiler:        $(c++ --version | head -1)"
    echo "cmake:           $(cmake --version | head -1)"
    echo "build type:      $(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$BUILD/CMakeCache.txt")"
    echo "server flags:    $(sed -n 's/^CXX_FLAGS = //p' "$BUILD/CMakeFiles/httpserver.dir/flags.make")"
    echo "ulimit -n:       $FD_LIMIT"
    for f in net/ipv4/tcp_rmem net/ipv4/tcp_wmem net/core/somaxconn net/ipv4/ip_local_port_range net/ipv4/tcp_tw_reuse; do
        if [ -r "/proc/sys/$f" ]; then
            printf '%-16s %s\n' "$(basename "$f"):" "$(tr '\t' ' ' < "/proc/sys/$f")"
        fi
    done
    echo "network:         loopback (127.0.0.1); client and server on the same machine, sharing its cores"
    echo "server:          --threads $SERVER_THREADS, access log off; pool mode adds ${POOL_FLAGS[*]}"
    echo "loadgen:         -t $LOADGEN_THREADS, --warmup $WARMUP, -d $DURATION, closed loop, --timeout 5 (default)"
} > "$OUT/environment.txt"

echo "scenario,mode,path,server_threads,loadgen_threads,label,connections,idle,keepalive,requests,errors,duration_s,rps,p50_ms,p95_ms,p99_ms,max_ms,status,error_kinds,idle_closed,server_cpu_s,loadgen_cpu_s,server_exit,note" > "$CSV"

# Commas would shift CSV columns; newlines would split rows.
clean() { tr ',\n' '; ' | sed 's/ *$//'; }

# Sets CHILD_CPU to the user+system CPU seconds of this shell's waited-for
# children. `times` must run in this shell: in a $(...) or pipeline subshell
# its counters would start at zero, so the result goes through a variable.
children_cpu() {
    times > "$TMP/times"
    CHILD_CPU="$(tail -1 "$TMP/times" | awk '{
        s = 0
        for (i = 1; i <= 2; i++) { split($i, a, /[ms]/); s += a[1] * 60 + a[2] }
        printf "%.2f", s
    }')"
}

# CPU seconds a running process has used so far (utime + stime).
process_cpu() {
    sed 's/^.*) //' "/proc/$1/stat" | awk -v hz="$CLK_TCK" '{printf "%.2f", ($12 + $13) / hz}'
}

RUN=0
# run_one SCENARIO MODE PATH CONNECTIONS IDLE KEEPALIVE(1|0)
run_one() {
    local scenario="$1" mode="$2" path="$3" conns="$4" idle="$5" keepalive="$6"
    local name="${path##*/}"
    local label="$scenario-$mode-${name%%.*}-c$conns-i$idle"
    RUN=$((RUN + 1))
    local prefix="$scenario,$mode,$path,$SERVER_THREADS,$LOADGEN_THREADS,$label"
    local empty_numbers="$conns,$idle,$keepalive,,,$DURATION,,,,,"

    # Each side holds one descriptor per connection; the margin covers
    # listening sockets, epoll/event fds, logs and the standard streams.
    local needed=$((conns + idle + 64))
    if [ "$needed" -gt "$FD_LIMIT" ]; then
        echo "$prefix,$empty_numbers,skipped,,,,,,needs about $needed file descriptors but ulimit -n is $FD_LIMIT" >> "$CSV"
        echo "[$RUN] $label: skipped (ulimit -n $FD_LIMIT)" >&2
        return
    fi

    local server_args=(--mode "$mode" --bind 127.0.0.1 --port 0 --root "$ROOT/www" --threads "$SERVER_THREADS")
    [ "$mode" = pool ] && server_args+=("${POOL_FLAGS[@]}")
    local log="$TMP/server-$RUN.log"
    "$BUILD/httpserver" "${server_args[@]}" > /dev/null 2> "$log" &
    local spid=$!

    local port="" i
    for i in $(seq 100); do
        port="$(sed -n 's/.*listening on [^ ]*:\([0-9][0-9]*\) mode=.*/\1/p' "$log")"
        [ -n "$port" ] && break
        kill -0 "$spid" 2>/dev/null || break
        sleep 0.05
    done
    if [ -z "$port" ]; then
        kill -KILL "$spid" 2>/dev/null || true
        wait "$spid" 2>/dev/null || true
        echo "$prefix,$empty_numbers,failed,,,,,,server did not start: $(tail -1 "$log" | clean)" >> "$CSV"
        echo "[$RUN] $label: server did not start" >&2
        return
    fi

    local ka_flag=()
    [ "$keepalive" -eq 0 ] && ka_flag=(--no-keepalive)
    local idle_flag=()
    [ "$idle" -gt 0 ] && idle_flag=(--idle "$idle")
    # A run normally ends after warm-up + duration (+ up to 5 s of setup);
    # anything past 30 s more is a hang.
    local limit=$((WARMUP + DURATION + 30))
    local cpu_before rc=0
    children_cpu
    cpu_before="$CHILD_CPU"
    timeout -k 5 "$limit" "$BUILD/loadgen" --host 127.0.0.1 --port "$port" --path "$path" \
        -c "$conns" "${idle_flag[@]}" "${ka_flag[@]}" -t "$LOADGEN_THREADS" \
        -d "$DURATION" --warmup "$WARMUP" --csv --label "$label" \
        > "$TMP/loadgen.out" 2> "$TMP/loadgen.err" || rc=$?
    local loadgen_cpu
    children_cpu
    loadgen_cpu="$(awk -v a="$CHILD_CPU" -v b="$cpu_before" 'BEGIN {printf "%.2f", a - b}')"

    local server_cpu="" server_exit
    server_cpu="$(process_cpu "$spid" 2>/dev/null || true)"
    kill -TERM "$spid" 2>/dev/null || true
    # Shutdown grace is 5 s; allow 15 s before calling it stuck.
    for i in $(seq 300); do
        kill -0 "$spid" 2>/dev/null || break
        sleep 0.05
    done
    local note=""
    if kill -0 "$spid" 2>/dev/null; then
        kill -KILL "$spid" 2>/dev/null || true
        note="server did not exit within 15 s of SIGTERM; "
    fi
    server_exit=0
    wait "$spid" 2>/dev/null || server_exit=$?

    local kinds idle_closed problems
    kinds="$(sed -n 's/^loadgen: errors: //p' "$TMP/loadgen.err" | clean)"
    idle_closed="$(sed -n 's/^loadgen: idle_closed=//p' "$TMP/loadgen.err")"
    [ -n "$idle_closed" ] || idle_closed=0
    # Server warnings and errors (e.g. accept failures) are worth a look.
    problems="$(grep -E '\[(WARN|ERROR)\]' "$log" | head -3 | clean || true)"
    [ -n "$problems" ] && note="${note}server log: $problems; "

    local status numbers
    case "$rc" in
        0)
            status=ok
            numbers="$(cut -d, -f2- "$TMP/loadgen.out")"
            ;;
        1)
            status=failed
            numbers="$(cut -d, -f2- "$TMP/loadgen.out")"
            note="${note}no successful request"
            ;;
        2)
            status=failed
            numbers="$empty_numbers"
            note="${note}$(grep -v '^loadgen: errors: \|^loadgen: idle_closed=' "$TMP/loadgen.err" | tail -1 | sed 's/^loadgen: //' | clean)"
            ;;
        124|137)
            status=failed
            numbers="$empty_numbers"
            note="${note}loadgen hung and was killed after $limit s"
            ;;
        *)
            status=failed
            numbers="$empty_numbers"
            note="${note}loadgen exited with status $rc: $(tail -1 "$TMP/loadgen.err" | clean)"
            ;;
    esac
    [ -n "$numbers" ] || numbers="$empty_numbers"
    note="$(printf '%s' "$note" | sed 's/; *$//' | clean)"
    echo "$prefix,$numbers,$status,$kinds,$idle_closed,$server_cpu,$loadgen_cpu,$server_exit,$note" >> "$CSV"
    echo "[$RUN] $label: $status $(cut -d, -f5,8,11 "$TMP/loadgen.out" 2>/dev/null) ${kinds:+($kinds)} ${note:+[$note]}" >&2

    # Lets the previous run's sockets settle before the next server starts.
    sleep 1
}

for mode in "${MODES[@]}"; do
    for path in /health /file16k.bin; do
        for c in "${MAIN_CONNS[@]}"; do
            run_one keepalive "$mode" "$path" "$c" 0 1
        done
    done
done
for mode in "${MODES[@]}"; do
    for c in "${NOKA_CONNS[@]}"; do
        run_one no-keepalive "$mode" /health "$c" 0 0
    done
done
for mode in "${MODES[@]}"; do
    for idle in "${IDLE_COUNTS[@]}"; do
        run_one idle "$mode" /health "$IDLE_ACTIVE" "$idle" 1
    done
done

python3 "$ROOT/bench/report.py" "$OUT"
echo "results in $OUT" >&2
