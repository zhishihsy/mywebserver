#!/usr/bin/env python3

import concurrent.futures
import os
import secrets
import signal
import socket
import subprocess
import tempfile
import time
import urllib.parse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HOST = "127.0.0.1"


def receive_response(connection):
    buffered = b""
    while b"\r\n\r\n" not in buffered:
        chunk = connection.recv(65536)
        if not chunk:
            raise AssertionError("收到响应头前连接已关闭")
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
        buffered += connection.recv(65536)
    return status, headers, buffered[:length]


def request(port, method, path, fields=None, content_type=None):
    body = b""
    headers = [f"{method} {path} HTTP/1.1", "Host: localhost"]
    if fields is not None:
        body = urllib.parse.urlencode(fields).encode()
        headers.append(
            "Content-Type: "
            + (content_type or "application/x-www-form-urlencoded")
        )
        headers.append(f"Content-Length: {len(body)}")
    payload = ("\r\n".join(headers) + "\r\n\r\n").encode() + body

    with socket.create_connection((HOST, port), timeout=10) as connection:
        connection.sendall(payload)
        return receive_response(connection)


def reserve_port():
    with socket.socket() as port_socket:
        port_socket.bind((HOST, 0))
        return port_socket.getsockname()[1]


def wait_until_ready(process, port):
    deadline = time.time() + 10
    while time.time() < deadline:
        if process.poll() is not None:
            raise AssertionError("数据库初始化期间服务器异常退出")
        try:
            if request(port, "GET", "/health")[0] == 200:
                return
        except OSError:
            time.sleep(0.05)
    raise AssertionError("服务器未能启动")


def main():
    if os.environ.get("MYSQL_ENABLED") != "1":
        raise SystemExit(
            "运行 MySQL 集成测试前，请设置 MYSQL_ENABLED=1 和 MYSQL_USER"
        )

    port = reserve_port()
    log = tempfile.TemporaryFile()
    process = subprocess.Popen(
        [str(ROOT / "webserver"), "-p", str(port), "-t", "8"],
        cwd=ROOT,
        env=os.environ.copy(),
        stdout=log,
        stderr=log,
    )

    username = "mysql_test_" + secrets.token_hex(8)
    password = "test-password-" + secrets.token_hex(8)
    credentials = {"username": username, "password": password}

    try:
        wait_until_ready(process, port)

        bad_type = request(
            port, "POST", "/register", credentials, "application/json"
        )
        assert bad_type[0] == 415, bad_type

        def register(_):
            return request(port, "POST", "/register", credentials)[0]

        with concurrent.futures.ThreadPoolExecutor(
            max_workers=24
        ) as executor:
            statuses = list(executor.map(register, range(48)))

        assert statuses.count(201) == 1, statuses
        assert statuses.count(409) == 47, statuses

        assert request(port, "POST", "/login", credentials)[0] == 200
        assert request(
            port,
            "POST",
            "/login",
            {"username": username, "password": "wrong-password"},
        )[0] == 401
        assert request(
            port,
            "POST",
            "/login",
            {"username": "missing_" + username, "password": password},
        )[0] == 401
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
    assert "MySQL: 已启用" in output, output
    print("MySQL 集成测试通过")


if __name__ == "__main__":
    main()
