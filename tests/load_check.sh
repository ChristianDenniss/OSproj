#!/usr/bin/env bash
# ApacheBench load check with admin commands issued during the load,
# descriptor counts before and after, and an access log line count.
# Usage: tests/load_check.sh [binary] [requests] [concurrency]
# Run from the project root.

BIN="${1:-./webshell}"
REQUESTS="${2:-10000}"
CONCURRENCY="${3:-50}"
PORT=18080
URL="http://127.0.0.1:${PORT}"
WORK=$(mktemp -d /tmp/webshell-load.XXXXXX)
LOG="$WORK/access.log"

mkfifo "$WORK/admin"
"$BIN" -p "$PORT" -r ./www -l "$LOG" < "$WORK/admin" > "$WORK/server.out" 2> "$WORK/server.err" &
PID=$!
exec 3> "$WORK/admin"
sleep 1

fd_count()
{
    ls "/proc/$PID/fd" | wc -l
}

admin()
{
    echo "$1" >&3
}

echo "descriptors at idle before load: $(fd_count)"

echo "--- ab -n $REQUESTS -c $CONCURRENCY (HTTP/1.0, connection per request)"
ab -q -n "$REQUESTS" -c "$CONCURRENCY" "$URL/index.html" > "$WORK/ab1.txt" 2>&1 &
AB=$!
sleep 0.3
admin "status"
admin "cache stats"
admin "exec sleep 1"
admin "cache flush"
wait "$AB"
grep -E "Complete requests|Failed requests|Non-2xx|Requests per second|Time per request.*mean\)" "$WORK/ab1.txt"

echo "--- ab -k -n $REQUESTS -c $CONCURRENCY (keep-alive)"
ab -q -k -n "$REQUESTS" -c "$CONCURRENCY" "$URL/logo.png" > "$WORK/ab2.txt" 2>&1 &
AB=$!
sleep 0.3
admin "status"
admin "exec echo diagnostic-ok"
wait "$AB"
grep -E "Complete requests|Failed requests|Non-2xx|Keep-Alive requests|Requests per second" "$WORK/ab2.txt"

sleep 1.5
admin "status"
admin "cache stats"
sleep 0.5
echo "descriptors at idle after load: $(fd_count)"
echo "child processes after load: $(ps --ppid "$PID" --no-headers | wc -l)"

admin "shutdown"
exec 3>&-
wait "$PID"
echo "server exit status: $?"
echo "access.log lines: $(wc -l < "$LOG") (expected $((REQUESTS * 2)))"
echo "--- server stdout"
cat "$WORK/server.out"
echo "--- server stderr (sanitizer reports would appear here)"
cat "$WORK/server.err"
rm -rf "$WORK"
