#!/usr/bin/env python3

import concurrent.futures
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


def reserve_port():
    with socket.socket() as port_socket:
        port_socket.bind((HOST, 0))
        return port_socket.getsockname()[1]


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


def check_fragmented_request(port):
    body = b"fragmented-phase-3"
    headers = (
        b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
        + str(len(body)).encode()
        + b"\r\n\r\n"
    )
    with socket.create_connection((HOST, port), timeout=5) as connection:
        for fragment in (headers[:17], headers[17:], body[:5], body[5:]):
            connection.sendall(fragment)
            time.sleep(0.01)
        status, _, response_body, _ = receive_response(connection)
        assert status == 200 and response_body == body


def check_pipeline(port):
    payload = (
        b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"
        b"POST /echo HTTP/1.1\r\nHost: localhost\r\n"
        b"Content-Length: 8\r\nConnection: close\r\n\r\npipeline"
    )
    with socket.create_connection((HOST, port), timeout=5) as connection:
        connection.sendall(payload)
        first = receive_response(connection)
        second = receive_response(connection, first[3])
        assert first[0] == 200 and first[2] == b"OK\n"
        assert second[0] == 200 and second[2] == b"pipeline"


def check_concurrent_connections(port):
    payload = b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"

    def one_request(_):
        status, _, body = request(port, payload)
        assert status == 200 and body == b"OK\n"

    with concurrent.futures.ThreadPoolExecutor(max_workers=24) as executor:
        list(executor.map(one_request, range(96)))


def run_combination(actor_model, trigger_mode):
    port = reserve_port()
    log = tempfile.TemporaryFile()
    process = subprocess.Popen(
        [
            str(ROOT / "webserver"),
            "-p",
            str(port),
            "-a",
            str(actor_model),
            "-m",
            str(trigger_mode),
            "-t",
            "4",
        ],
        cwd=ROOT,
        stdout=log,
        stderr=log,
    )

    try:
        wait_until_ready(process, port)
        check_fragmented_request(port)
        check_pipeline(port)
        check_concurrent_connections(port)
    finally:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

    log.seek(0)
    output = log.read().decode(errors="replace")
    log.close()

    actor_name = "Proactor" if actor_model == 0 else "Reactor"
    listen_name = "ET" if trigger_mode >= 2 else "LT"
    connect_name = "ET" if trigger_mode % 2 else "LT"
    assert f"Actor Model: {actor_name}" in output
    assert f"Listen Mode: {listen_name}" in output
    assert f"Connect Mode: {connect_name}" in output
    print(
        f"passed actor={actor_model} ({actor_name}), "
        f"trigger={trigger_mode} ({listen_name}/{connect_name})"
    )


def main():
    for actor_model in (0, 1):
        for trigger_mode in range(4):
            run_combination(actor_model, trigger_mode)
    print("phase 3 concurrency tests passed (2 actor models x 4 trigger modes)")


if __name__ == "__main__":
    main()
