"""Page cache and I/O worker integration tests.

Each test starts its own server so the cache counters it reads back at
shutdown come only from that test's requests.
"""

import os
import re
import shutil
import threading
import time
import unittest

from webshell_harness import Conn, ServerProcess, make_fixture_tree, request, sanitizer_problems

PAGE = 4096


def stop_stats(server):
    code = server.stop()
    problems = sanitizer_problems(server.stderr)
    if code != 0 or problems:
        raise AssertionError(f"server exit {code}, sanitizer output:\n{server.stderr}")
    match = re.search(r"Stopped\. (.*)", server.stdout)
    if match is None:
        raise AssertionError("no shutdown statistics in server output:\n" + server.stdout)
    return {k: int(v) for k, v in (item.split("=") for item in match.group(1).split())}


class CacheIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.tmp, self.www = make_fixture_tree()
        self.two_pages = os.urandom(PAGE + 1)
        self.four_pages = os.urandom(3 * PAGE + 17)
        self.write("two.bin", self.two_pages)
        self.write("four.bin", self.four_pages)
        self.server = None

    def tearDown(self):
        if self.server is not None and self.server.proc.poll() is None:
            self.server.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write(self, name, data):
        with open(os.path.join(self.www, name), "wb") as f:
            f.write(data)

    def start(self, frames, jobs=None):
        args = ["-c", str(frames)] + (["-j", str(jobs)] if jobs else [])
        self.server = ServerProcess(self.www, self.tmp, args)
        self.server.start()
        return self.server.port

    def get(self, port, path):
        conn = Conn(port)
        conn.send(request("GET", path))
        r = conn.response()
        conn.close()
        return r

    def test_repeat_request_hits_cache(self):
        port = self.start(frames=8)
        for _ in range(3):
            self.assertEqual(self.get(port, "/two.bin").body, self.two_pages)
        stats = stop_stats(self.server)
        self.assertEqual(stats["cache_misses"], 2, "first request reads both pages from disk")
        self.assertEqual(stats["cache_hits"], 4, "later requests are served from cached frames")
        self.assertEqual(stats["cache_evictions"], 0)

    def test_head_does_not_touch_cache(self):
        port = self.start(frames=8)
        conn = Conn(port)
        conn.send(request("HEAD", "/four.bin"))
        r = conn.response(head=True)
        conn.close()
        self.assertEqual(r.header("content-length"), str(len(self.four_pages)))
        stats = stop_stats(self.server)
        self.assertEqual(stats["cache_hits"] + stats["cache_misses"], 0)

    def test_lru_eviction_under_small_capacity(self):
        port = self.start(frames=2)
        self.assertEqual(self.get(port, "/four.bin").body, self.four_pages)
        self.assertEqual(self.get(port, "/four.bin").body, self.four_pages)
        stats = stop_stats(self.server)
        # Four pages through two frames: every lookup misses, and each insert
        # after the first two evicts the least recently used page.
        self.assertEqual(stats["cache_hits"], 0)
        self.assertEqual(stats["cache_misses"], 8)
        self.assertEqual(stats["cache_evictions"], 6)

    def test_modified_file_is_not_served_stale(self):
        port = self.start(frames=8)
        self.write("change.txt", b"version one\n")
        self.assertEqual(self.get(port, "/change.txt").body, b"version one\n")
        time.sleep(0.01)
        self.write("change.txt", b"version two, longer\n")
        self.assertEqual(self.get(port, "/change.txt").body, b"version two, longer\n")
        stop_stats(self.server)

    def test_job_pool_saturation(self):
        """64 clients streaming through a single worker job must all complete correctly."""
        big = os.urandom(512 * 1024)
        self.write("big.bin", big)
        port = self.start(frames=16, jobs=1)
        errors = []

        def worker():
            try:
                conn = Conn(port, timeout=30)
                for _ in range(2):
                    conn.send(request("GET", "/big.bin"))
                    if conn.response().body != big:
                        errors.append("body mismatch")
                conn.close()
            except Exception as exc:  # noqa: BLE001 - reported below
                errors.append(repr(exc))

        threads = [threading.Thread(target=worker) for _ in range(64)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])
        stats = stop_stats(self.server)
        self.assertEqual(stats["requests"], 128)
        self.assertGreater(stats["io_deferred"], 0, "the deferral path should have been exercised")

    def test_disconnect_while_read_in_flight(self):
        big = os.urandom(2 * 1024 * 1024)
        self.write("big.bin", big)
        port = self.start(frames=4)
        for _ in range(30):
            conn = Conn(port)
            conn.send(request("GET", "/big.bin"))
            conn.close()
        self.assertEqual(self.get(port, "/two.bin").body, self.two_pages)
        stop_stats(self.server)


if __name__ == "__main__":
    unittest.main()
