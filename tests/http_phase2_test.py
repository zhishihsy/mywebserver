#!/usr/bin/env python3

import hashlib
import os
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HOST = "127.0.0.1"


def receive_response(connection, buffered=b""):
    while b"\r\n\r\n" not in buffered:
        chunk = connection.recv(65536)
        if not chunk:
            raise AssertionError("connection closed before response headers")
        buffered += chunk
    raw_headers, buffered = buffered.split(b"\r\n\r\n", 1)
    lines = raw_headers.split(b"\r\n")
    status = int(lines[0].split()[1])
    headers = {}
    for line in lines[1:]:
        name, value = line.split(b":", 1)
        headers[name.strip().lower()] = value.strip()
    length = int(headers[b"content-length"])
    while len(buffered) < length:
        chunk = connection.recv(65536)
        if not chunk:
            raise AssertionError("connection closed before response body")
        buffered += chunk
    return status, headers, buffered[:length], buffered[length:]


def request(port, payload):
    with socket.create_connection((HOST, port), timeout=5) as connection:
        connection.sendall(payload)
        return receive_response(connection)[:3]


def wait_until_ready(process, port):
    deadline = time.time() + 5
    while time.time() < deadline:
        if process.poll() is not None:
            raise AssertionError("server exited during startup")
        try:
            status, _, body = request(
                port, b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"
            )
            if status == 200 and body == b"OK\n":
                return
        except OSError:
            time.sleep(0.05)
    raise AssertionError("server did not start")


def assert_status(port, raw_request, expected):
    status, _, _ = request(port, raw_request)
    assert status == expected, (status, expected, raw_request)


def main():
    resources = ROOT / "resources"
    fixtures = {
        "phase2-image.png": b"\x89PNG\r\n\x1a\n" + os.urandom(32 * 1024),
        "phase2-video.mp4": b"\x00\x00\x00\x18ftypmp42" + os.urandom(512 * 1024),
        "phase2-large.bin": os.urandom(8 * 1024 * 1024),
    }
    forbidden = resources / "phase2-forbidden.txt"
    internal_error = resources / "phase2-internal-error.sock"
    internal_error_socket = None
    process = None
    log = None
    try:
        if not hasattr(socket, "AF_UNIX"):
            raise RuntimeError("this test requires Linux/Unix domain sockets")

        for name, data in fixtures.items():
            (resources / name).write_bytes(data)
        forbidden.write_text("forbidden", encoding="ascii")
        forbidden.chmod(0)
        internal_error_socket = socket.socket(socket.AF_UNIX)
        internal_error_socket.bind(str(internal_error))

        with socket.socket() as port_socket:
            port_socket.bind((HOST, 0))
            port = port_socket.getsockname()[1]

        log = tempfile.TemporaryFile()
        process = subprocess.Popen(
            [str(ROOT / "webserver"), "-p", str(port), "-m", "3"],
            cwd=ROOT,
            stdout=log,
            stderr=log,
        )
        wait_until_ready(process, port)

        post_body = b'{"phase":2,"method":"POST"}'
        status, headers, body = request(
            port,
            b"POST /echo HTTP/1.1\r\nHost: localhost\r\n"
            b"Content-Type: application/json\r\nContent-Length: "
            + str(len(post_body)).encode()
            + b"\r\n\r\n"
            + post_body,
        )
        assert status == 200 and body == post_body
        assert headers[b"content-type"] == b"application/json"

        assert_status(
            port, b"GET / HTTP/1.1\r\n\r\n", 400
        )
        assert_status(
            port,
            b"GET /bad%2 HTTP/1.1\r\nHost: localhost\r\n\r\n",
            400,
        )
        assert_status(
            port,
            b"GET /../secret HTTP/1.1\r\nHost: localhost\r\n\r\n",
            403,
        )
        assert_status(
            port,
            b"GET /phase2-forbidden.txt HTTP/1.1\r\nHost: localhost\r\n\r\n",
            403,
        )
        assert_status(
            port,
            b"GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n",
            404,
        )
        status, headers, _ = request(
            port, b"PUT / HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        assert status == 405 and headers[b"allow"] == b"GET, POST"
        assert_status(
            port,
            b"POST /echo HTTP/1.1\r\nHost: localhost\r\n"
            b"Content-Length: 1048577\r\n\r\n",
            413,
        )
        assert_status(
            port,
            b"GET /phase2-internal-error.sock HTTP/1.1\r\n"
            b"Host: localhost\r\n\r\n",
            500,
        )

        for name, expected in fixtures.items():
            status, _, body = request(
                port,
                f"GET /{name} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode(),
            )
            assert status == 200
            assert hashlib.sha256(body).digest() == hashlib.sha256(expected).digest()

        pipeline = (
            b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"
            b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n"
            b"pipe"
            b"GET /phase2-image.png HTTP/1.1\r\nHost: localhost\r\n"
            b"Connection: close\r\n\r\n"
        )
        with socket.create_connection((HOST, port), timeout=5) as connection:
            connection.sendall(pipeline)
            buffered = b""
            first = receive_response(connection, buffered)
            second = receive_response(connection, first[3])
            third = receive_response(connection, second[3])
            assert first[0] == 200 and first[2] == b"OK\n"
            assert second[0] == 200 and second[2] == b"pipe"
            assert third[0] == 200 and third[2] == fixtures["phase2-image.png"]
            assert third[1][b"connection"] == b"close"

        print("phase 2 HTTP tests passed")
    finally:
        if process is not None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if log is not None:
            log.close()
        if forbidden.exists():
            forbidden.chmod(0o644)
        if internal_error_socket is not None:
            internal_error_socket.close()
        internal_error.unlink(missing_ok=True)
        for name in fixtures:
            (resources / name).unlink(missing_ok=True)
        forbidden.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
