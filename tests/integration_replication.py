#!/usr/bin/env python3
"""Integration test: starts a real master + replica litekv process pair,
issues commands over RESP sockets, and verifies replication end-to-end.

Usage: python3 tests/integration_replication.py [path-to-litekv-binary]
Exits 0 on success, 1 on failure. Requires the litekv binary to be built
(see build/litekv after `cmake .. && make`).
"""
import os
import socket
import subprocess
import sys
import time

MASTER_PORT = 16390
REPLICA_PORT = 16391
MASTER_AOF = "/tmp/litekv_it_master.aof"
REPLICA_AOF = "/tmp/litekv_it_replica.aof"


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

    for path in (MASTER_AOF, REPLICA_AOF):
        if os.path.exists(path):
            os.remove(path)

    master = subprocess.Popen(
        [binary, "--port", str(MASTER_PORT), "--aof", MASTER_AOF],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    replica = None
    try:
        if not wait_for_port(MASTER_PORT):
            print("FAIL: master did not start listening")
            return 1

        replica = subprocess.Popen(
            [binary, "--port", str(REPLICA_PORT), "--aof", REPLICA_AOF,
             "--replicaof", "127.0.0.1", str(MASTER_PORT)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        if not wait_for_port(REPLICA_PORT):
            print("FAIL: replica did not start listening")
            return 1

        # Give the replica time to complete its handshake with the master.
        time.sleep(0.5)

        master_sock = socket.create_connection(("127.0.0.1", MASTER_PORT))
        replica_sock = socket.create_connection(("127.0.0.1", REPLICA_PORT))

        resp = send_command(master_sock, "SET", "foo", "bar")
        if resp != "+OK\r\n":
            print(f"FAIL: master SET returned unexpected response: {resp!r}")
            return 1

        # Poll the replica until the propagated SET arrives.
        deadline = time.time() + 3.0
        replicated = None
        while time.time() < deadline:
            replicated = send_command(replica_sock, "GET", "foo")
            if replicated == "$3\r\nbar\r\n":
                break
            time.sleep(0.05)

        if replicated != "$3\r\nbar\r\n":
            print(f"FAIL: replica GET did not see replicated value: {replicated!r}")
            return 1

        readonly_resp = send_command(replica_sock, "SET", "foo", "baz")
        if "READONLY" not in readonly_resp:
            print(f"FAIL: replica accepted a write it should have rejected: {readonly_resp!r}")
            return 1

        print("PASS: master SET propagated to replica GET, replica rejects writes")
        return 0
    finally:
        for proc in (replica, master):
            if proc is not None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        for path in (MASTER_AOF, REPLICA_AOF):
            if os.path.exists(path):
                os.remove(path)


if __name__ == "__main__":
    sys.exit(main())
