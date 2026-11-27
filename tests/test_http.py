"""HTTP engine tests: framing, persistence, pipelining, MIME, errors, path security, streaming.

Run from the project root with `make test`.
"""

import os
import shutil
import socket
import threading
import time
import unittest

from webshell_harness import (
    SECRET,
    Conn,
    ConnectionClosed,
    ServerProcess,
    make_fixture_tree,
    request,
    sanitizer_problems,
)

PAGE = 4096
BINARY_SIZES = [1, PAGE - 1, PAGE, PAGE + 1, 3 * PAGE + 17, 1024 * 1024]


class HttpEngineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp, cls.www = make_fixture_tree()
        cls.blobs = {}
        for size in BINARY_SIZES:
            data = os.urandom(size)
            cls.blobs[size] = data
            with open(os.path.join(cls.www, f"bin_{size}.bin"), "wb") as f:
                f.write(data)
        cls.big = os.urandom(4 * 1024 * 1024)
        with open(os.path.join(cls.www, "big.bin"), "wb") as f:
            f.write(cls.big)

        cls.server = ServerProcess(cls.www, cls.tmp)
        cls.server.start()
        cls.port = cls.server.port

    @classmethod
    def tearDownClass(cls):
        code = cls.server.stop()
        shutil.rmtree(cls.tmp, ignore_errors=True)
        problems = sanitizer_problems(cls.server.stderr)
        if code != 0 or problems:
            raise AssertionError(f"server exit {code}, sanitizer output:\n{cls.server.stderr}")

    def fetch(self, path, method="GET", version="1.1", headers=None):
        conn = Conn(self.port)
        try:
            conn.send(request(method, path, version, headers))
            return conn.response(head=(method == "HEAD"))
        finally:
            conn.close()

    def read_www(self, name):
        with open(os.path.join(self.www, name), "rb") as f:
            return f.read()

    # Basic retrieval and MIME types

    def test_root_maps_to_index(self):
        r = self.fetch("/")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body, self.read_www("index.html"))
        self.assertEqual(r.header("content-length"), str(len(r.body)))

    def test_mime_types(self):
        expected = {
            "/index.html": "text/html",
            "/style.css": "text/css",
            "/app.js": "text/javascript",
            "/data.json": "application/json",
            "/logo.png": "image/png",
            "/bin_1.bin": "application/octet-stream",
        }
        for path, mime in expected.items():
            with self.subTest(path=path):
                r = self.fetch(path)
                self.assertEqual(r.status, 200)
                self.assertTrue(r.header("content-type").startswith(mime), r.header("content-type"))
                self.assertEqual(r.body, self.read_www(path.lstrip("/")))

    def test_png_is_byte_exact(self):
        r = self.fetch("/logo.png")
        self.assertTrue(r.body.startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertEqual(r.body, self.read_www("logo.png"))

    def test_query_string_ignored_for_lookup(self):
        r = self.fetch("/data.json?refresh=1&x=%20")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body, self.read_www("data.json"))

    def test_subdirectory_and_directory_index(self):
        self.assertEqual(self.fetch("/sub/page.html").body, b"<p>sub page</p>\n")
        self.assertEqual(self.fetch("/sub/").body, b"<p>sub index</p>\n")

    def test_percent_decoding(self):
        with open(os.path.join(self.www, "with space.txt"), "wb") as f:
            f.write(b"spaced\n")
        r = self.fetch("/with%20space.txt")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body, b"spaced\n")

    # HEAD

    def test_head_matches_get_without_body(self):
        conn = Conn(self.port)
        conn.send(request("HEAD", "/index.html") + request("GET", "/index.html"))
        head = conn.response(head=True)
        get = conn.response()
        conn.close()
        self.assertEqual(head.status, 200)
        self.assertEqual(head.header("content-length"), get.header("content-length"))
        self.assertEqual(head.header("content-type"), get.header("content-type"))
        self.assertEqual(head.body, b"")
        self.assertEqual(get.body, self.read_www("index.html"))

    def test_head_error_has_no_body(self):
        conn = Conn(self.port)
        conn.send(request("HEAD", "/missing.html") + request("GET", "/index.html"))
        head = conn.response(head=True)
        self.assertEqual(head.status, 404)
        self.assertGreater(int(head.header("content-length")), 0)
        follow = conn.response()
        self.assertEqual(follow.status, 200, "bytes after a HEAD 404 must be the next response")
        conn.close()

    # Persistence, HTTP/1.0 and pipelining

    def test_sequential_keep_alive(self):
        conn = Conn(self.port)
        for _ in range(5):
            conn.send(request("GET", "/index.html"))
            r = conn.response()
            self.assertEqual(r.status, 200)
            self.assertEqual(r.header("connection"), "keep-alive")
        self.assertFalse(conn.is_closed(0.3))
        conn.close()

    def test_pipelined_requests_in_order(self):
        conn = Conn(self.port)
        paths = ["/style.css", "/data.json", "/missing", "/app.js", "/bin_4097.bin"]
        conn.send(b"".join(request("GET", p) for p in paths))
        statuses = []
        for path in paths:
            r = conn.response()
            statuses.append(r.status)
            if r.status == 200:
                self.assertEqual(r.body, self.read_www(path.lstrip("/")))
        self.assertEqual(statuses, [200, 200, 404, 200, 200])
        conn.close()

    def test_many_pipelined_requests(self):
        conn = Conn(self.port)
        count = 100
        conn.send(request("GET", "/data.json") * count)
        for _ in range(count):
            self.assertEqual(conn.response().status, 200)
        self.assertFalse(conn.is_closed(0.3))
        conn.close()

    def test_connection_close_is_honoured(self):
        conn = Conn(self.port)
        conn.send(request("GET", "/", headers=["Connection: close"]))
        r = conn.response()
        self.assertEqual(r.header("connection"), "close")
        self.assertTrue(conn.is_closed())
        conn.close()

    def test_connection_header_tokens_case_insensitive(self):
        conn = Conn(self.port)
        conn.send(request("GET", "/", headers=["Connection: Upgrade, CLOSE"]))
        conn.response()
        self.assertTrue(conn.is_closed())
        conn.close()

    def test_http10_closes_by_default(self):
        conn = Conn(self.port)
        conn.send(request("GET", "/", version="1.0", host=False))
        r = conn.response()
        self.assertEqual(r.status, 200)
        self.assertEqual(r.header("connection"), "close")
        self.assertTrue(conn.is_closed())
        conn.close()

    def test_http10_keep_alive_opt_in(self):
        conn = Conn(self.port)
        for _ in range(3):
            conn.send(request("GET", "/", version="1.0", host=False, headers=["Connection: keep-alive"]))
            r = conn.response()
            self.assertEqual(r.header("connection"), "keep-alive")
        self.assertFalse(conn.is_closed(0.3))
        conn.close()

    def test_fragmented_request(self):
        conn = Conn(self.port)
        for byte in request("GET", "/data.json"):
            conn.send(bytes([byte]))
            time.sleep(0.005)
        r = conn.response()
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body, self.read_www("data.json"))
        conn.close()

    def test_leading_empty_lines_ignored(self):
        conn = Conn(self.port)
        conn.send(b"\r\n\r\n" + request("GET", "/data.json"))
        self.assertEqual(conn.response().status, 200)
        conn.close()

    # Error handling

    def test_missing_file_keeps_connection(self):
        conn = Conn(self.port)
        conn.send(request("GET", "/nope.html"))
        r = conn.response()
        self.assertEqual(r.status, 404)
        conn.send(request("GET", "/data.json"))
        self.assertEqual(conn.response().status, 200)
        conn.close()

    def test_unsupported_method(self):
        conn = Conn(self.port)
        conn.send(request("POST", "/", headers=["Content-Length: 0"]))
        r = conn.response()
        self.assertEqual(r.status, 405)
        self.assertEqual(r.header("allow"), "GET, HEAD")
        self.assertTrue(conn.is_closed())
        conn.close()

    def assert_rejected(self, raw, status):
        conn = Conn(self.port)
        conn.send(raw)
        r = conn.response()
        self.assertEqual(r.status, status, raw[:80])
        self.assertEqual(r.header("connection"), "close")
        self.assertTrue(conn.is_closed())
        conn.close()

    def test_malformed_requests(self):
        cases = [
            (b"GARBAGE\r\n\r\n", 400),
            (b"GET /\r\nHost: x\r\n\r\n", 400),
            (b"GET  / HTTP/1.1\r\nHost: x\r\n\r\n", 400),
            (b"GET / HTTP/1.1\r\nHost: x\r\nBad Header\r\n\r\n", 400),
            (b"GET / HTTP/1.1\r\nHost: x\r\n folded: value\r\n\r\n", 400),
            (b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400),
            (b"GET / HTTP/1.1\r\n\r\n", 400),
            (b"GET http://example.com/ HTTP/1.1\r\nHost: x\r\n\r\n", 400),
            (b"GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505),
            (b"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n", 400),
        ]
        for raw, status in cases:
            with self.subTest(raw=raw):
                self.assert_rejected(raw, status)

    def test_request_bodies_rejected(self):
        self.assert_rejected(request("GET", "/", headers=["Content-Length: 5"]), 413)
        self.assert_rejected(request("GET", "/", headers=["Transfer-Encoding: chunked"]), 501)

    def test_uri_too_long(self):
        self.assert_rejected(request("GET", "/" + "a" * 600), 414)

    def test_header_too_large(self):
        raw = b"GET / HTTP/1.1\r\nHost: x\r\nX-Fill: " + b"a" * 9000
        self.assert_rejected(raw[:8192], 431)

    # Path security

    def assert_not_exposed(self, path, allowed=(400, 403, 404)):
        conn = Conn(self.port)
        conn.send(request("GET", path))
        r = conn.response()
        conn.close()
        self.assertIn(r.status, allowed, path)
        self.assertNotIn(SECRET, r.body, path)

    def test_direct_traversal(self):
        for path in ["/../secret.txt", "/sub/../../secret.txt", "/sub/../index.html", "/./index.html"]:
            with self.subTest(path=path):
                self.assert_not_exposed(path)

    def test_encoded_traversal(self):
        for path in ["/%2e%2e/secret.txt", "/%2E%2E/secret.txt", "/.%2e/secret.txt",
                     "/%2e%2e%2fsecret.txt", "/sub%2f..%2f..%2fsecret.txt", "/..%5csecret.txt"]:
            with self.subTest(path=path):
                self.assert_not_exposed(path)

    def test_malformed_escapes_and_control_bytes(self):
        for path in ["/%", "/%2", "/%zz", "/index.html%00.png", "/%0d%0a", "/a%7f"]:
            with self.subTest(path=path):
                self.assert_not_exposed(path, allowed=(400,))

    def test_symlinks_rejected(self):
        for path in ["/link_secret.txt", "/linkdir/secret.txt", "/inner_link.html"]:
            with self.subTest(path=path):
                self.assert_not_exposed(path, allowed=(403, 404))

    def test_special_files_rejected(self):
        for path in ["/fifo.txt", "/sub"]:
            with self.subTest(path=path):
                self.assert_not_exposed(path, allowed=(403,))

    # Body streaming

    def test_empty_file(self):
        r = self.fetch("/empty.txt")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.header("content-length"), "0")
        self.assertEqual(r.body, b"")

    def test_binary_page_boundaries(self):
        conn = Conn(self.port, timeout=10)
        for size in BINARY_SIZES:
            with self.subTest(size=size):
                conn.send(request("GET", f"/bin_{size}.bin"))
                r = conn.response()
                self.assertEqual(r.header("content-length"), str(size))
                self.assertEqual(r.body, self.blobs[size])
        conn.close()

    def test_slow_reader_does_not_stall_others(self):
        slow = Conn(self.port, timeout=30, rcvbuf=4096)
        slow.send(request("GET", "/big.bin"))
        time.sleep(0.5)

        start = time.monotonic()
        r = self.fetch("/index.html")
        elapsed = time.monotonic() - start
        self.assertEqual(r.status, 200)
        self.assertLess(elapsed, 1.0, "a fast client must not wait behind a slow reader")

        received = bytearray()
        r = None
        while True:
            try:
                r = slow.response()
                break
            except socket.timeout:
                self.fail("slow transfer timed out")
        received += r.body
        slow.close()
        self.assertEqual(bytes(received), self.big)

    def test_concurrent_clients(self):
        errors = []
        expected = self.read_www("index.html")

        def worker():
            try:
                conn = Conn(self.port, timeout=10)
                for _ in range(20):
                    conn.send(request("GET", "/index.html"))
                    r = conn.response()
                    if r.status != 200 or r.body != expected:
                        errors.append(r.status)
                conn.close()
            except Exception as exc:  # noqa: BLE001 - collected and reported below
                errors.append(repr(exc))

        threads = [threading.Thread(target=worker) for _ in range(40)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])

    def test_idle_client_does_not_block_others(self):
        idle = Conn(self.port)
        idle.send(b"GET /index.html HTTP/1.1\r\nHost:")
        r = self.fetch("/data.json")
        self.assertEqual(r.status, 200)
        idle.close()

    def test_abrupt_disconnect_mid_transfer(self):
        conn = Conn(self.port, rcvbuf=4096)
        conn.send(request("GET", "/big.bin"))
        conn.sock.recv(1024)
        conn.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
        conn.close()
        time.sleep(0.2)
        self.assertEqual(self.fetch("/data.json").status, 200)


if __name__ == "__main__":
    unittest.main()
