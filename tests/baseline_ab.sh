#!/usr/bin/env bash
# Baseline ApacheBench and keep-alive checks against a server on 127.0.0.1:8080.
# Usage: tests/baseline_ab.sh <server-binary>

BIN="${1:?usage: $0 <server-binary>}"
URL="http://127.0.0.1:8080"

pkill -x "$(basename "$BIN")" 2>/dev/null
sleep 0.5
"$BIN" > /dev/null 2>&1 &
PID=$!
sleep 1

echo "--- ab -k -n 200 -c 16 (persistent)"
ab -s 5 -k -n 200 -c 16 "$URL/index.html" 2>&1 | grep -E "Complete requests|Failed requests|Keep-Alive requests|timeout"
echo "--- ab -n 20 -c 4 (HTTP/1.0, no keep-alive)"
ab -s 5 -n 20 -c 4 "$URL/index.html" 2>&1 | grep -E "Complete requests|Failed requests|timeout"
echo "--- curl connection reuse"
curl -sv "$URL/" "$URL/index.html" 2>&1 | grep -iE "connected|reus|left intact"

kill -TERM "$PID"
wait "$PID"
echo "--- server exit status after SIGTERM: $?"
