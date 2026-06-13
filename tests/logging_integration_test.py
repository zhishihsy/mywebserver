#!/usr/bin/env python3

import os
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HOST = "127.0.0.1"


def reserve_port():
    with socket.socket() as port_socket:
        port_socket.bind((HOST, 0))
        return port_socket.getsockname()[1]


def request(port):
    with socket.create_connection((HOST, port), timeout=3) as connection:
        connection.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
        response = b""
        while b"OK\n" not in response:
            chunk = connection.recv(4096)
            if not chunk:
                break
            response += chunk
        assert b"200 OK" in response and response.endswith(b"OK\n")


def wait_until_ready(process, port):
    deadline = time.time() + 5
    while time.time() < deadline:
        if process.poll() is not None:
            raise AssertionError("server exited during startup")
        try:
            request(port)
            return
        except OSError:
            time.sleep(0.05)
    raise AssertionError("server did not start")


def stop_server(process):
    process.send_signal(signal.SIGTERM)
    process.wait(timeout=5)
    assert process.returncode == 0


def run_async_rotation_test(directory):
    port = reserve_port()
    base_file = directory / "async.log"
    env = os.environ.copy()
    env.update(
        {
            "LOG_ENABLED": "1",
            "LOG_ASYNC": "1",
            "LOG_LEVEL": "DEBUG",
            "LOG_FILE": str(base_file),
            "LOG_QUEUE_SIZE": "2",
            "LOG_MAX_LINES": "3",
        }
    )
    with tempfile.TemporaryFile() as output:
        process = subprocess.Popen(
            [str(ROOT / "webserver"), "-p", str(port)],
            cwd=ROOT,
            env=env,
            stdout=output,
            stderr=output,
        )
        wait_until_ready(process, port)
        for _ in range(8):
            request(port)
        stop_server(process)

    files = sorted(directory.glob("async_*.log"))
    assert len(files) >= 2, "line rotation did not create multiple files"
    lines = []
    for path in files:
        file_lines = path.read_text(encoding="utf-8").splitlines()
        assert 1 <= len(file_lines) <= 3
        lines.extend(file_lines)
    assert any("[INFO]" in line for line in lines)
    assert any("[DEBUG]" in line for line in lines)
    assert all("[tid " in line for line in lines)


def run_level_filter_test(directory):
    port_socket = socket.socket()
    port_socket.bind((HOST, 0))
    port_socket.listen()
    port = port_socket.getsockname()[1]
    env = os.environ.copy()
    env.update(
        {
            "LOG_ENABLED": "1",
            "LOG_ASYNC": "0",
            "LOG_LEVEL": "ERROR",
            "LOG_FILE": str(directory / "filtered.log"),
        }
    )
    try:
        process = subprocess.run(
            [str(ROOT / "webserver"), "-p", str(port)],
            cwd=ROOT,
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=5,
            check=False,
        )
    finally:
        port_socket.close()
    assert process.returncode == 1

    files = list(directory.glob("filtered_*.log"))
    assert len(files) == 1
    lines = files[0].read_text(encoding="utf-8").splitlines()
    assert lines and all("[ERROR]" in line for line in lines)


def run_disabled_test(directory):
    port = reserve_port()
    disabled_file = directory / "disabled.log"
    env = os.environ.copy()
    env.update(
        {
            "LOG_ENABLED": "0",
            "LOG_FILE": str(disabled_file),
        }
    )
    with tempfile.TemporaryFile() as output:
        process = subprocess.Popen(
            [str(ROOT / "webserver"), "-p", str(port)],
            cwd=ROOT,
            env=env,
            stdout=output,
            stderr=output,
        )
        wait_until_ready(process, port)
        stop_server(process)
    assert not list(directory.glob("disabled_*.log"))


def main():
    with tempfile.TemporaryDirectory() as temporary_directory:
        directory = Path(temporary_directory)
        run_async_rotation_test(directory)
        run_level_filter_test(directory)
        run_disabled_test(directory)
    print("logging integration tests passed")


if __name__ == "__main__":
    main()
