#!/usr/bin/env bash
# Smoke test for a server binary listening on 127.0.0.1:8080.
# Usage: tests/baseline_smoke.sh <server-binary>
# Run from the project root so the server finds ./www.

BIN="${1:?usage: $0 <server-binary>}"
URL="http://127.0.0.1:8080"

"$BIN" > /tmp/smoke_server.out 2>&1 &
PID=$!
sleep 1

echo "--- GET /"
curl -si "$URL/"
echo
echo "--- HEAD /"
curl -sI "$URL/"
echo "--- direct traversal"
curl -si --path-as-is "$URL/../secret.txt"
echo
echo "--- encoded traversal"
curl -si --path-as-is "$URL/%2e%2e/secret.txt"
echo
echo "--- missing file"
curl -si "$URL/nope.html"
echo
echo "--- POST"
curl -si -X POST "$URL/"
echo
echo "--- keep-alive reuse"
curl -sv "$URL/" "$URL/index.html" 2>&1 | grep -E "Connected to|Re-using|HTTP/1.1 200"
echo "--- ab -n 200 -c 16"
ab -n 200 -c 16 "$URL/index.html" 2>&1 | grep -E "Complete requests|Failed requests|Non-2xx"

kill -INT "$PID"
sleep 1
kill -0 "$PID" 2>/dev/null && echo "server still alive after SIGINT" && kill -KILL "$PID"
wait "$PID" 2>/dev/null
echo "--- server exit status: $?"
echo "--- server output"
cat /tmp/smoke_server.out
