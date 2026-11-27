#!/usr/bin/env bash
# ApacheBench load check with descriptor counts before and after.
# Usage: tests/load_check.sh [binary] [requests] [concurrency]
# Run from the project root.

BIN="${1:-./webshell}"
REQUESTS="${2:-10000}"
CONCURRENCY="${3:-50}"
PORT=18080
URL="http://127.0.0.1:${PORT}"

"$BIN" -p "$PORT" -r ./www > /tmp/load_server.out 2> /tmp/load_server.err &
PID=$!
sleep 1

fd_count()
{
    ls "/proc/$PID/fd" | wc -l
}

echo "descriptors at idle before load: $(fd_count)"

echo "--- ab -n $REQUESTS -c $CONCURRENCY (HTTP/1.0, connection per request)"
ab -q -n "$REQUESTS" -c "$CONCURRENCY" "$URL/index.html" 2>&1 \
    | grep -E "Complete requests|Failed requests|Non-2xx|Requests per second|Time per request.*mean\)"

echo "--- ab -k -n $REQUESTS -c $CONCURRENCY (keep-alive)"
ab -q -k -n "$REQUESTS" -c "$CONCURRENCY" "$URL/logo.png" 2>&1 \
    | grep -E "Complete requests|Failed requests|Non-2xx|Keep-Alive requests|Requests per second"

sleep 1
echo "descriptors at idle after load: $(fd_count)"

kill -TERM "$PID"
wait "$PID"
echo "server exit status: $?"
echo "--- server stdout"
cat /tmp/load_server.out
echo "--- server stderr (sanitizer reports would appear here)"
cat /tmp/load_server.err
