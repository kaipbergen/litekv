import socket
import time
import statistics

HOST = "127.0.0.1"
PORT = 6380

def send_command(sock, *args):
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(str(arg))}\r\n{arg}\r\n"
    sock.sendall(cmd.encode())
    return sock.recv(1024).decode()

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

if __name__ == "__main__":
    print("=" * 40)
    print("LiteKV Benchmark")
    print("=" * 40)
    print()
    benchmark("PING", bench_ping)
    benchmark("SET",  bench_set)
    benchmark("GET",  bench_get)