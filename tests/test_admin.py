"""Admin shell, diagnostic processes, graceful shutdown and access logging tests."""

import os
import re
import shutil
import signal
import socket
import threading
import time
import unittest

from webshell_harness import (
    Conn,
    ServerProcess,
    child_processes,
    make_fixture_tree,
    open_fd_count,
    process_alive,
    request,
    sanitizer_problems,
)

LOG_LINE = re.compile(
    r'^\[(\d{2}/\w{3}/\d{4}:\d{2}:\d{2}:\d{2} [+-]\d{4})\] (\S+) "(\S+) (\S+)" (\d{3}|-) (\d+)$'
)


class ServerTestCase(unittest.TestCase):
    extra_args = ()

    def setUp(self):
        self.tmp, self.www = make_fixture_tree()
        self.log_path = os.path.join(self.tmp, "access.log")
        self.server = ServerProcess(self.www, self.tmp, list(self.extra_args))
        self.server.start()
        self.port = self.server.port

    def tearDown(self):
        if self.server.proc.poll() is None:
            self.server.kill()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def finish(self, how="command", timeout=20):
        """Stops the server, asserts a clean exit, and returns the shutdown statistics."""
        if how == "command":
            self.server.write_raw(b"shutdown\n")
            code = self.server.wait(timeout)
        else:
            code = self.server.stop(how, timeout)
        problems = sanitizer_problems(self.server.stderr)
        self.assertEqual(code, 0, self.server.stderr)
        self.assertEqual(problems, [], self.server.stderr)
        match = re.search(r"Stopped\. (.*)", self.server.stdout)
        self.assertIsNotNone(match, self.server.stdout)
        return {k: int(v) for k, v in (item.split("=") for item in match.group(1).split())}

    def get(self, path, method="GET"):
        conn = Conn(self.port)
        conn.send(request(method, path))
        r = conn.response(head=(method == "HEAD"))
        conn.close()
        return r

    def write(self, name, data):
        with open(os.path.join(self.www, name), "wb") as f:
            f.write(data)

    def log_lines(self):
        with open(self.log_path) as f:
            return f.read().splitlines()


class AdminShellTests(ServerTestCase):
    def test_status_reports_counters(self):
        for _ in range(3):
            self.assertEqual(self.get("/index.html").status, 200)
        time.sleep(0.2)
        m = self.server.command("status", r"\[status\] uptime=(\d+)\.\d+s .* active_clients=(\d+) "
                                          r"requests_served=(\d+) bytes_sent=(\d+)")
        self.assertEqual(int(m.group(2)), 0)
        self.assertEqual(int(m.group(3)), 3)
        self.assertGreater(int(m.group(4)), 3 * os.path.getsize(os.path.join(self.www, "index.html")))
        self.finish()

    def test_cache_stats_and_flush(self):
        m = self.server.command("cache stats", r"\[cache\] hits=(\d+) misses=(\d+) hit_ratio=(\S+)")
        self.assertEqual(m.groups(), ("0", "0", "n/a"))

        self.get("/index.html")
        self.get("/index.html")
        m = self.server.command("cache stats", r"hits=(\d+) misses=(\d+) hit_ratio=(\S+) frames_used=(\d+)/(\d+)")
        self.assertEqual(m.groups(), ("1", "1", "50.0%", "1", "64"))

        self.server.command("cache flush", r"\[cache\] flushed 1 frame\(s\); epoch is now 1")
        m = self.server.command("cache stats", r"hits=(\d+) misses=(\d+) .* frames_used=(\d+)/")
        self.assertEqual(m.groups(), ("1", "1", "0"), "flush empties frames but keeps lifetime counters")

        self.get("/index.html")
        m = self.server.command("cache stats", r"hits=(\d+) misses=(\d+)")
        self.assertEqual(m.groups(), ("1", "2"))
        self.finish()

    def test_partial_and_multiple_lines(self):
        self.server.write_raw(b"sta")
        time.sleep(0.2)
        self.server.write_raw(b"tus\ncache stats\n")
        self.server.expect(r"\[status\] uptime=")
        self.server.expect(r"\[cache\] hits=")
        self.server.write_raw(b"  cache    stats  \r\n")
        self.server.expect(r"\[cache\] hits=")
        self.finish()

    def test_invalid_input_does_not_break_server(self):
        self.server.command("bogus", r"unknown command: bogus")
        self.server.command("status now", r"unknown command: status now")
        self.server.command("exec", r"usage: exec <command>")
        self.server.command("x" * 2000, r"line too long")
        self.server.command("status", r"\[status\] uptime=")
        self.assertEqual(self.get("/data.json").status, 200)
        self.finish()

    def test_stdin_eof_keeps_serving(self):
        self.server.close_stdin()
        self.server.expect(r"stdin closed; still serving")
        self.assertEqual(self.get("/index.html").status, 200)
        self.finish(how=signal.SIGTERM)


class ProcessTests(ServerTestCase):
    def test_exec_is_asynchronous_and_reaped(self):
        m = self.server.command("exec sleep 2", r"\[exec\] started pid (\d+): sleep 2")
        child = int(m.group(1))
        start = time.monotonic()
        for _ in range(20):
            self.assertEqual(self.get("/data.json").status, 200)
        self.assertLess(time.monotonic() - start, 1.5, "HTTP must keep flowing while the child runs")
        self.server.command("status", r"children=1")

        self.server.expect(rf"\[exec\] pid {child} exited with status 0", timeout=6)
        time.sleep(0.1)
        self.assertEqual(child_processes(self.server.pid), [], "no zombie or leftover child")
        self.finish()

    def test_exec_failure_and_output(self):
        self.server.command("exec /no/such/program", r"exited with status 127")
        self.server.command("exec echo hello-from-child", r"hello-from-child")
        self.server.command("exec exit 3", r"exited with status 3")
        self.finish()

    def test_child_cannot_read_admin_console(self):
        # cat would hang forever if it inherited the admin stdin pipe.
        self.server.command("exec cat", r"\[exec\] pid \d+ exited with status 0", timeout=5)
        self.server.command("status", r"\[status\]")
        self.finish()

    def test_repeated_short_children(self):
        for _ in range(30):
            self.server.command("exec true", r"\[exec\] pid \d+ exited with status 0")
        time.sleep(0.1)
        self.assertEqual(child_processes(self.server.pid), [])
        self.finish()

    def test_burst_beyond_child_limit(self):
        self.server.write_raw(b"exec sleep 1\n" * 12)
        started = refused = 0
        for _ in range(12):
            m = self.server.expect(r"\[exec\] (started|refused)")
            if m.group(1) == "started":
                started += 1
            else:
                refused += 1
        self.assertEqual((started, refused), (8, 4))
        for _ in range(8):
            self.server.expect(r"exited with status 0", timeout=5)
        time.sleep(0.1)
        self.assertEqual(child_processes(self.server.pid), [])
        self.finish()

    def test_child_limit_and_shutdown_terminates_children(self):
        pids = []
        for _ in range(8):
            m = self.server.command("exec sleep 30", r"started pid (\d+)")
            pids.append(int(m.group(1)))
        self.server.command("exec sleep 30", r"refused: 8 diagnostic processes already running")

        start = time.monotonic()
        stats = self.finish()
        self.assertLess(time.monotonic() - start, 5)
        self.assertIn("terminating 8 diagnostic process(es)", self.server.stdout)
        self.assertEqual(self.server.stdout.count("terminated by signal 15"), 8)
        for pid in pids:
            self.assertFalse(process_alive(pid), f"child {pid} still running")
        self.assertEqual(stats["requests"], 0)

    def test_descriptor_hygiene(self):
        time.sleep(0.2)
        before = open_fd_count(self.server.pid)
        for _ in range(25):
            self.get("/index.html")
        for _ in range(5):
            self.server.command("exec true", r"exited with status 0")
        conns = [Conn(self.port) for _ in range(20)]
        for c in conns:
            c.close()
        time.sleep(0.5)
        self.assertEqual(open_fd_count(self.server.pid), before)
        self.finish()

    def test_commands_under_load(self):
        stop = threading.Event()
        errors = []
        counts = []
        with open(os.path.join(self.www, "index.html"), "rb") as f:
            expected = f.read()

        def load():
            n = 0
            try:
                conn = Conn(self.port, timeout=10)
                while not stop.is_set():
                    conn.send(request("GET", "/index.html"))
                    r = conn.response()
                    if r.status != 200 or r.body != expected:
                        errors.append(r.status)
                    n += 1
                conn.close()
            except Exception as exc:  # noqa: BLE001 - reported below
                errors.append(repr(exc))
            counts.append(n)

        threads = [threading.Thread(target=load) for _ in range(16)]
        for t in threads:
            t.start()
        try:
            time.sleep(0.3)
            first = int(self.server.command("status", r"requests_served=(\d+)").group(1))
            self.server.command("cache stats", r"\[cache\] hits=")
            self.server.command("exec sleep 1", r"started pid")
            self.server.command("cache flush", r"\[cache\] flushed")
            time.sleep(0.5)
            m = self.server.command("status", r"active_clients=(\d+) requests_served=(\d+)")
            self.assertEqual(int(m.group(1)), 16)
            self.assertGreater(int(m.group(2)), first, "requests keep completing between commands")
            self.server.expect(r"exited with status 0", timeout=5)
        finally:
            stop.set()
            for t in threads:
                t.join()
        self.assertEqual(errors, [])
        self.assertTrue(all(n > 0 for n in counts))
        self.finish()


class ShutdownTests(ServerTestCase):
    def test_shutdown_drains_active_transfer(self):
        big = os.urandom(8 * 1024 * 1024)
        self.write("big.bin", big)
        slow = Conn(self.port, timeout=20, rcvbuf=4096)
        slow.send(request("GET", "/big.bin"))
        time.sleep(0.3)

        self.server.write_raw(b"shutdown\n")
        self.server.expect(r"\[shutdown\] shutdown command: stopped accepting, closed 0 idle connection\(s\), "
                           r"draining 1 response")
        with self.assertRaises(ConnectionRefusedError):
            socket.create_connection(("127.0.0.1", self.port), timeout=1)

        r = slow.response()
        self.assertEqual(r.header("content-length"), str(len(big)))
        self.assertEqual(r.body, big, "the in-progress response is delivered in full")
        self.assertTrue(slow.is_closed(2.0))
        slow.close()

        self.assertEqual(self.server.wait(10), 0)
        self.assertEqual(sanitizer_problems(self.server.stderr), [])
        self.assertIn("Stopped.", self.server.stdout)
        self.assertNotIn("aborting", self.server.stdout)
        self.assertEqual(len(self.log_lines()), 1)

    def test_drain_deadline_aborts_stalled_client(self):
        big = os.urandom(16 * 1024 * 1024)
        self.write("big.bin", big)
        stalled = Conn(self.port, rcvbuf=4096)
        stalled.send(request("GET", "/big.bin"))
        time.sleep(0.3)

        start = time.monotonic()
        stats = self.finish()
        elapsed = time.monotonic() - start
        stalled.close()
        self.assertGreater(elapsed, 4.0)
        self.assertLess(elapsed, 9.0)
        self.assertIn("drain deadline reached; aborting 1 connection(s)", self.server.stdout)
        self.assertEqual(stats["requests"], 0)

        lines = self.log_lines()
        self.assertEqual(len(lines), 1)
        m = LOG_LINE.match(lines[0])
        self.assertEqual(m.group(5), "200")
        self.assertLess(int(m.group(6)), len(big), "aborted transfer logs only the bytes actually sent")

    def test_idle_keep_alive_closed_immediately(self):
        conn = Conn(self.port)
        conn.send(request("GET", "/index.html"))
        conn.response()
        start = time.monotonic()
        self.server.write_raw(b"shutdown\n")
        self.assertTrue(conn.is_closed(2.0))
        conn.close()
        self.assertEqual(self.server.wait(10), 0)
        self.assertLess(time.monotonic() - start, 2.0)
        self.assertIn("closed 1 idle connection(s)", self.server.stdout)

    def test_sigint_and_sigterm_are_graceful(self):
        self.get("/index.html")
        stats = self.finish(how=signal.SIGINT)
        self.assertIn("termination signal", self.server.stdout)
        self.assertEqual(stats["requests"], 1)
        self.assertEqual(len(self.log_lines()), 1, "final flush persists the record")


class AccessLogTests(ServerTestCase):
    def test_format_fields_and_order(self):
        index_size = os.path.getsize(os.path.join(self.www, "index.html"))
        self.get("/index.html")
        self.get("/data.json", method="HEAD")
        self.get("/missing.html")
        conn = Conn(self.port)
        conn.send(request("POST", "/form"))
        conn.response()
        conn.close()
        self.finish()

        parsed = [LOG_LINE.match(line) for line in self.log_lines()]
        self.assertTrue(all(parsed), self.log_lines())
        fields = [(m.group(2), m.group(3), m.group(4), m.group(5), int(m.group(6))) for m in parsed]
        self.assertEqual(fields, [
            ("127.0.0.1", "GET", "/index.html", "200", index_size),
            ("127.0.0.1", "HEAD", "/data.json", "200", 0),
            ("127.0.0.1", "GET", "/missing.html", "404", len(b"404 Not Found\n")),
            ("127.0.0.1", "POST", "/form", "405", len(b"405 Method Not Allowed\n")),
        ])

    def test_periodic_flush_while_running(self):
        self.get("/index.html")
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if os.path.exists(self.log_path) and os.path.getsize(self.log_path) > 0:
                break
            time.sleep(0.1)
        self.assertEqual(len(self.log_lines()), 1, "record reaches access.log before shutdown")
        self.server.command("status", r"log_pending=0 log_written=1")
        self.finish()

    def test_log_injection_is_escaped(self):
        self.get('/a"b')
        conn = Conn(self.port)
        conn.send(b'GET /x HTTP/1.1\r\nHost: x\r\nBad\x01Header: y\r\n\r\n')
        conn.response()
        conn.close()
        self.finish()
        lines = self.log_lines()
        self.assertEqual(len(lines), 2)
        self.assertIn('"GET /a\\x22b" 404', lines[0])
        self.assertTrue(all(LOG_LINE.match(line) for line in lines))


class LogBackpressureTests(ServerTestCase):
    extra_args = ("-q", "4")

    def test_ring_wraparound_without_loss(self):
        for i in range(100):
            self.assertEqual(self.get(f"/data.json?seq={i}").status, 200)

        errors = []

        def burst(worker):
            try:
                conn = Conn(self.port, timeout=10)
                for i in range(25):
                    conn.send(request("GET", f"/index.html?w={worker}&i={i}"))
                    if conn.response().status != 200:
                        errors.append("status")
                conn.close()
            except Exception as exc:  # noqa: BLE001 - reported below
                errors.append(repr(exc))

        threads = [threading.Thread(target=burst, args=(w,)) for w in range(20)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])

        stats = self.finish()
        targets = [LOG_LINE.match(line).group(4) for line in self.log_lines()]
        self.assertEqual(len(targets), 600)
        self.assertEqual(len(set(targets)), 600, "no duplicated records")
        self.assertEqual(targets[:100], [f"/data.json?seq={i}" for i in range(100)], "FIFO order")
        self.assertEqual(stats["log_written"], 600)
        self.assertEqual(stats["log_lost"], 0)
        self.assertGreater(stats["log_deferred"], 0, "a 4-slot ring must have applied backpressure")


if __name__ == "__main__":
    unittest.main()
