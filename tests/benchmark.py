import random
import socket
import time
import statistics

HOST = "127.0.0.1"
PORT = 6380

def encode_command(*args):
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(str(arg))}\r\n{arg}\r\n"
    return cmd.encode()

def send_command(sock, *args):
    sock.sendall(encode_command(*args))
    return sock.recv(1024).decode()

def recv_exact(sock, nbytes):
    chunks = []
    remaining = nbytes
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)

def benchmark(name, func, iterations=10000):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((HOST, PORT))

    # Warmup
    for _ in range(100):
        func(sock)

    # Benchmark
    latencies = []
    start = time.time()
    for i in range(iterations):
        t0 = time.perf_counter()
        func(sock)
        latencies.append((time.perf_counter() - t0) * 1000)
    elapsed = time.time() - start

    sock.close()

    ops_sec = iterations / elapsed
    p50 = statistics.median(latencies)
    p99 = sorted(latencies)[int(0.99 * len(latencies))]

    print(f"[{name}]")
    print(f"  Throughput:  {ops_sec:,.0f} ops/sec")
    print(f"  P50 latency: {p50:.3f}ms")
    print(f"  P99 latency: {p99:.3f}ms")
    print()

def bench_set(sock):
    send_command(sock, "SET", "bench_key", "bench_value")

def bench_get(sock):
    send_command(sock, "GET", "bench_key")

def bench_ping(sock):
    send_command(sock, "PING")

def benchmark_pipeline(name, batch_size=100, batches=100):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((HOST, PORT))

    reply = b"+OK\r\n"
    batch_payload = b"".join(
        encode_command("SET", f"pipe_key_{i}", "pipe_value") for i in range(batch_size)
    )
    expected_bytes = len(reply) * batch_size

    # Warmup
    sock.sendall(batch_payload)
    recv_exact(sock, expected_bytes)

    total_ops = batch_size * batches
    start = time.time()
    for _ in range(batches):
        sock.sendall(batch_payload)
        recv_exact(sock, expected_bytes)
    elapsed = time.time() - start

    sock.close()

    ops_sec = total_ops / elapsed
    print(f"[{name}]")
    print(f"  Batch size:  {batch_size}")
    print(f"  Throughput:  {ops_sec:,.0f} ops/sec")
    print()

def bench_mixed(sock, keys, rng):
    roll = rng.random()
    key = rng.choice(keys)
    if roll < 0.70:
        send_command(sock, "GET", key)
    elif roll < 0.90:
        send_command(sock, "SET", key, "mixed_value")
    else:
        send_command(sock, "INCR", key)

if __name__ == "__main__":
    print("=" * 40)
    print("LiteKV Benchmark")
    print("=" * 40)
    print()
    benchmark("PING", bench_ping)
    benchmark("SET",  bench_set)
    benchmark("GET",  bench_get)

    benchmark_pipeline("PIPELINE SET (batch=100)", batch_size=100, batches=100)
    benchmark_pipeline("PIPELINE SET (batch=1000)", batch_size=1000, batches=20)

    rng = random.Random(42)
    mixed_keys = [f"mixed_key_{i}" for i in range(50)]
    benchmark("MIXED (70% GET / 20% SET / 10% INCR)",
              lambda sock: bench_mixed(sock, mixed_keys, rng))
