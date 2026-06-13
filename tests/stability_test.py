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
        connection.sendall(
            b"GET /health HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Connection: close\r\n\r\n"
        )
        response = b""
        while True:
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


def fd_count(pid):
    fd_directory = Path(f"/proc/{pid}/fd")
    if not fd_directory.exists():
        return None
    return len(list(fd_directory.iterdir()))


def main():
    port = reserve_port()
    environment = os.environ.copy()
    environment["LOG_ENABLED"] = "0"

    with tempfile.TemporaryFile() as output:
        process = subprocess.Popen(
            [
                str(ROOT / "webserver"),
                "-p",
                str(port),
                "-t",
                "4",
                "-i",
                "1",
            ],
            cwd=ROOT,
            env=environment,
            stdout=output,
            stderr=output,
        )

        slow_client = None
        try:
            wait_until_ready(process, port)
            baseline_fds = fd_count(process.pid)

            for _ in range(500):
                request(port)

            time.sleep(0.2)
            final_fds = fd_count(process.pid)
            if baseline_fds is not None and final_fds is not None:
                assert final_fds <= baseline_fds + 3, (
                    baseline_fds,
                    final_fds,
                )

            slow_client = socket.create_connection((HOST, port), timeout=3)
            slow_client.sendall(
                b"GET /health HTTP/1.1\r\nHost: localhost\r\n"
            )
            time.sleep(1.5)
            slow_client.settimeout(1)
            assert slow_client.recv(1) == b""

            request(port)
        finally:
            if slow_client is not None:
                slow_client.close()
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

        assert process.returncode == 0

    print("stability tests passed (fd lifecycle and idle timeout)")


if __name__ == "__main__":
    main()
