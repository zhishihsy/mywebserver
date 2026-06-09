#!/usr/bin/env python3

import argparse
import concurrent.futures
import http.client
import statistics
import time


def send_request(host, port, timeout):
    started_at = time.perf_counter()
    connection = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        connection.request("GET", "/health")
        response = connection.getresponse()
        body = response.read()
        success = response.status == 200 and body == b"OK\n"
        return success, time.perf_counter() - started_at, ""
    except Exception as error:
        return False, time.perf_counter() - started_at, str(error)
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser(
        description="Run concurrent requests against the web server."
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--requests", type=int, default=1000)
    parser.add_argument("--concurrency", type=int, default=50)
    parser.add_argument("--timeout", type=float, default=5.0)
    args = parser.parse_args()

    started_at = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=args.concurrency
    ) as executor:
        futures = [
            executor.submit(
                send_request, args.host, args.port, args.timeout
            )
            for _ in range(args.requests)
        ]
        results = [future.result() for future in futures]

    elapsed = time.perf_counter() - started_at
    successful = [latency for ok, latency, _ in results if ok]
    errors = [error for ok, _, error in results if not ok and error]

    print(f"Requests: {args.requests}")
    print(f"Successful: {len(successful)}")
    print(f"Failed: {args.requests - len(successful)}")
    print(f"Elapsed: {elapsed:.3f}s")
    print(f"Throughput: {args.requests / elapsed:.2f} req/s")

    if successful:
        ordered = sorted(successful)
        p95_index = min(
            len(ordered) - 1, int(len(ordered) * 0.95)
        )
        print(f"Mean latency: {statistics.mean(successful) * 1000:.2f}ms")
        print(f"P95 latency: {ordered[p95_index] * 1000:.2f}ms")

    if errors:
        print(f"First error: {errors[0]}")

    if len(successful) != args.requests:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
