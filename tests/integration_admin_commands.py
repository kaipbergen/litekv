#!/usr/bin/env python3
"""Integration test: starts a real litekv process and exercises admin
commands over raw RESP sockets. These are dispatched directly in
src/server.cpp with no equivalent Storage-level unit test path, so this
smoke test is their real verification.

Usage: python3 tests/integration_admin_commands.py [path-to-litekv-binary]
Exits 0 on success, 1 on failure. Requires the litekv binary to be built
(see build/litekv after `cmake .. && make`).
"""
import os
import socket
import subprocess
import sys
import time

PORT = 16392
AOF_PATH = "/tmp/litekv_it_admin.aof"


def send_command(sock, *args):
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(str(arg))}\r\n{arg}\r\n"
    sock.sendall(cmd.encode())
    return sock.recv(4096).decode()


def wait_for_port(port, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "build/litekv"
    if not os.path.isfile(binary):
        print(f"FAIL: litekv binary not found at {binary}")
        return 1

    if os.path.exists(AOF_PATH):
        os.remove(AOF_PATH)

    proc = subprocess.Popen(
        [binary, "--port", str(PORT), "--aof", AOF_PATH],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        if not wait_for_port(PORT):
            print("FAIL: server did not start listening")
            return 1

        conn = socket.create_connection(("127.0.0.1", PORT))
        conn_b = socket.create_connection(("127.0.0.1", PORT))

        # DBSIZE reflects keys actually present.
        resp = send_command(conn, "DBSIZE")
        assert resp == ":0\r\n", f"expected empty DBSIZE, got {resp!r}"

        send_command(conn, "SET", "a", "1")
        send_command(conn, "SET", "b", "2")
        send_command(conn, "SET", "c", "3")
        resp = send_command(conn, "DBSIZE")
        assert resp == ":3\r\n", f"expected DBSIZE 3, got {resp!r}"

        # FLUSHALL clears everything, reflected by DBSIZE and GET.
        resp = send_command(conn, "FLUSHALL")
        assert resp == "+OK\r\n", f"expected FLUSHALL OK, got {resp!r}"
        resp = send_command(conn, "DBSIZE")
        assert resp == ":0\r\n", f"expected DBSIZE 0 after FLUSHALL, got {resp!r}"
        resp = send_command(conn, "GET", "a")
        assert resp == "$-1\r\n", f"expected nil after FLUSHALL, got {resp!r}"

        # UNWATCH actually clears the watch, not just returns OK: without it,
        # a concurrent modification to a watched key aborts EXEC; with it,
        # the same modification no longer matters.
        send_command(conn, "SET", "watched", "orig")
        resp = send_command(conn, "WATCH", "watched")
        assert resp == "+OK\r\n", f"expected WATCH OK, got {resp!r}"

        send_command(conn_b, "SET", "watched", "changed-by-other-client")

        resp = send_command(conn, "UNWATCH")
        assert resp == "+OK\r\n", f"expected UNWATCH OK, got {resp!r}"

        resp = send_command(conn, "MULTI")
        assert resp == "+OK\r\n", f"expected MULTI OK, got {resp!r}"
        send_command(conn, "SET", "watched", "final")
        resp = send_command(conn, "EXEC")
        assert resp != "*-1\r\n", (
            "EXEC was aborted even though UNWATCH should have cleared the watch"
        )

        conn.close()
        conn_b.close()
        print("PASS: DBSIZE, FLUSHALL, and UNWATCH behave correctly")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        if os.path.exists(AOF_PATH):
            os.remove(AOF_PATH)


if __name__ == "__main__":
    sys.exit(main())
