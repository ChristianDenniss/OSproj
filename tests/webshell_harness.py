"""Shared helpers for the webshell test suite (Python standard library only)."""

import os
import queue
import re
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.environ.get("WEBSHELL_BIN", os.path.join(PROJECT_ROOT, "webshell"))
SECRET = b"RESTRICTED_ACCESS_SECRET"


class ConnectionClosed(Exception):
    pass


class Response:
    def __init__(self, status, reason, headers, body):
        self.status = status
        self.reason = reason
        self.headers = headers
        self.body = body

    def header(self, name):
        return self.headers.get(name.lower())


class Conn:
    """A raw TCP connection with a receive buffer, so pipelined replies can be split."""

    def __init__(self, port, timeout=5.0, rcvbuf=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf is not None:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.settimeout(timeout)
        self.sock.connect(("127.0.0.1", port))
        self.buf = b""

    def send(self, data):
        self.sock.sendall(data)

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise ConnectionClosed()
        self.buf += chunk

    def response(self, head=False):
        while b"\r\n\r\n" not in self.buf:
            self._fill()
        head_bytes, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = head_bytes.decode("latin-1").split("\r\n")
        version, status, reason = lines[0].split(" ", 2)
        assert version == "HTTP/1.1", lines[0]
        headers = {}
        for line in lines[1:]:
            name, value = line.split(":", 1)
            headers[name.strip().lower()] = value.strip()
        length = 0 if head else int(headers.get("content-length", "0"))
        while len(self.buf) < length:
            self._fill()
        body, self.buf = self.buf[:length], self.buf[length:]
        return Response(int(status), reason, headers, body)

    def is_closed(self, wait=1.0):
        """True if the server closes the connection within `wait` seconds."""
        self.sock.settimeout(wait)
        try:
            data = self.sock.recv(1)
        except socket.timeout:
            return False
        except (ConnectionResetError, BrokenPipeError):
            return True
        if data:
            self.buf += data
            return False
        return True

    def close(self):
        self.sock.close()


def request(method="GET", path="/", version="1.1", headers=None, host=True):
    lines = [f"{method} {path} HTTP/{version}"]
    if host:
        lines.append("Host: localhost")
    lines.extend(headers or [])
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class ServerProcess:
    """Runs ./webshell, drives its admin console on stdin and collects its output."""

    def __init__(self, doc_root, cwd, extra_args=()):
        self.doc_root = doc_root
        self.cwd = cwd
        self.extra_args = list(extra_args)
        self.port = free_port()
        self.proc = None
        self.stdout = ""
        self.stderr = ""
        self._lines = queue.Queue()
        self._stdout_lines = []
        self._stderr_chunks = []
        self._threads = []

    def start(self):
        self.proc = subprocess.Popen(
            [BINARY, "-p", str(self.port), "-r", self.doc_root] + self.extra_args,
            cwd=self.cwd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        self._threads = [
            threading.Thread(target=self._pump_stdout, daemon=True),
            threading.Thread(target=self._pump_stderr, daemon=True),
        ]
        for t in self._threads:
            t.start()

        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                self._join()
                raise RuntimeError("webshell exited during startup: " + "".join(self._stderr_chunks))
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("webshell did not start listening")

    def _pump_stdout(self):
        for raw in iter(self.proc.stdout.readline, b""):
            line = raw.decode(errors="replace").rstrip("\n")
            self._stdout_lines.append(line)
            self._lines.put(line)

    def _pump_stderr(self):
        for chunk in iter(lambda: self.proc.stderr.read(4096), b""):
            self._stderr_chunks.append(chunk.decode(errors="replace"))

    def _join(self):
        for t in self._threads:
            t.join(timeout=10)
        for pipe in (self.proc.stdin, self.proc.stdout, self.proc.stderr):
            try:
                pipe.close()
            except OSError:
                pass
        self.stdout = "\n".join(self._stdout_lines)
        self.stderr = "".join(self._stderr_chunks)

    @property
    def pid(self):
        return self.proc.pid

    def write_raw(self, data):
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def close_stdin(self):
        self.proc.stdin.close()

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.wait()

    def expect(self, pattern, timeout=5.0):
        """Waits for a stdout line matching the regex and returns the match."""
        deadline = time.monotonic() + timeout
        seen = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"no output matching {pattern!r} within {timeout}s; saw: {seen}")
            try:
                line = self._lines.get(timeout=remaining)
            except queue.Empty:
                continue
            seen.append(line)
            match = re.search(pattern, line)
            if match:
                return match

    def command(self, text, pattern, timeout=5.0):
        while not self._lines.empty():
            self._lines.get_nowait()
        self.write_raw((text + "\n").encode())
        return self.expect(pattern, timeout)

    def wait(self, timeout=20):
        self.proc.wait(timeout=timeout)
        self._join()
        return self.proc.returncode

    def stop(self, sig=signal.SIGTERM, timeout=20):
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
        return self.wait(timeout)


def child_processes(pid):
    """(pid, state) for every process whose parent is pid, read from /proc."""
    children = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open(f"/proc/{entry}/stat") as f:
                stat = f.read()
        except OSError:
            continue
        fields = stat[stat.rfind(")") + 2:].split()
        if int(fields[1]) == pid:
            children.append((int(entry), fields[0]))
    return children


def open_fd_count(pid):
    return len(os.listdir(f"/proc/{pid}/fd"))


def process_alive(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            state = f.read().rsplit(")", 1)[1].split()[0]
    except OSError:
        return False
    return state != "Z"


def make_fixture_tree():
    """Creates a temp tree: <tmp>/www (served), <tmp>/secret.txt and <tmp>/outside/ (not served)."""
    tmp = tempfile.mkdtemp(prefix="webshell-test-")
    www = os.path.join(tmp, "www")
    shutil.copytree(os.path.join(PROJECT_ROOT, "www"), www)

    with open(os.path.join(tmp, "secret.txt"), "wb") as f:
        f.write(SECRET + b"\n")
    os.mkdir(os.path.join(tmp, "outside"))
    with open(os.path.join(tmp, "outside", "secret.txt"), "wb") as f:
        f.write(SECRET + b"\n")

    os.mkdir(os.path.join(www, "sub"))
    with open(os.path.join(www, "sub", "page.html"), "wb") as f:
        f.write(b"<p>sub page</p>\n")
    with open(os.path.join(www, "sub", "index.html"), "wb") as f:
        f.write(b"<p>sub index</p>\n")
    open(os.path.join(www, "empty.txt"), "wb").close()

    os.symlink(os.path.join(tmp, "secret.txt"), os.path.join(www, "link_secret.txt"))
    os.symlink(os.path.join(tmp, "outside"), os.path.join(www, "linkdir"))
    os.symlink("sub/page.html", os.path.join(www, "inner_link.html"))
    os.mkfifo(os.path.join(www, "fifo.txt"))
    return tmp, www


def sanitizer_problems(stderr):
    markers = (
        "ERROR: AddressSanitizer",
        "ERROR: LeakSanitizer",
        "WARNING: ThreadSanitizer",
        "runtime error:",
        "SUMMARY: UndefinedBehavior",
    )
    return [line for line in stderr.splitlines() if any(m in line for m in markers)]
