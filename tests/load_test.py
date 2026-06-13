#!/usr/bin/env python3

import argparse
import asyncio
import statistics
import time


REQUEST = (
    b"GET /health HTTP/1.1\r\n"
    b"Host: localhost\r\n"
    b"Connection: close\r\n\r\n"
)


async def one_request(host, port, timeout, semaphore):
    started = time.perf_counter()
    async with semaphore:
        writer = None
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(host, port),
                timeout=timeout,
            )
            writer.write(REQUEST)
            await writer.drain()
            response = await asyncio.wait_for(
                reader.read(),
                timeout=timeout,
            )
            success = (
                b"HTTP/1.1 200 OK" in response
                and response.endswith(b"OK\n")
            )
            return success, time.perf_counter() - started, ""
        except Exception as error:
            return False, time.perf_counter() - started, str(error)
        finally:
            if writer is not None:
                writer.close()
                try:
                    await writer.wait_closed()
                except OSError:
                    pass


async def run(args):
    semaphore = asyncio.Semaphore(args.concurrency)
    started = time.perf_counter()
    tasks = [
        one_request(
            args.host,
            args.port,
            args.timeout,
            semaphore,
        )
        for _ in range(args.connections)
    ]
    results = await asyncio.gather(*tasks)
    elapsed = time.perf_counter() - started

    latencies = [latency for ok, latency, _ in results if ok]
    errors = [error for ok, _, error in results if not ok]
    successes = len(latencies)
    print(f"Connections: {args.connections}")
    print(f"Peak concurrency: {args.concurrency}")
    print(f"Successful: {successes}")
    print(f"Failed: {len(errors)}")
    print(f"Elapsed: {elapsed:.3f}s")
    print(f"Throughput: {successes / elapsed:.2f} req/s")

    if latencies:
        ordered = sorted(latencies)
        p95 = ordered[min(len(ordered) - 1, int(len(ordered) * 0.95))]
        print(f"Mean latency: {statistics.mean(latencies) * 1000:.2f}ms")
        print(f"P95 latency: {p95 * 1000:.2f}ms")
    if errors:
        print(f"First error: {errors[0]}")
    if errors:
        raise SystemExit(1)


def main():
    parser = argparse.ArgumentParser(
        description="Validate many concurrent connections against the server."
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--connections", type=int, default=10000)
    parser.add_argument("--concurrency", type=int, default=10000)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()
    if args.connections <= 0 or args.concurrency <= 0:
        parser.error("connections and concurrency must be positive")
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
