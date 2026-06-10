# LiteKV

A distributed in-memory key-value store built from scratch in C++17, inspired by Redis. Features an event-driven architecture with Linux epoll, RESP protocol, AOF persistence, LRU eviction, and master-replica replication over Docker.

## Architecture

```
Client Request (RESP protocol)
         ↓
  epoll Event Loop (Linux) / Thread-per-connection (macOS)
         ↓
  zero-copy RESP Parser (std::string_view)
         ↓
  LRU Storage Engine (unordered_map + linked list)
         ↓
  AOF Logger → disk persistence
         ↓
  Replication → propagate to replicas
```

## Features

- **epoll event loop** — single-threaded async I/O on Linux, handles thousands of connections
- **RESP protocol** — full Redis Serialization Protocol parser with zero-copy std::string_view
- **LRU eviction** — least-recently-used eviction when max_keys limit is reached
- **TTL support** — per-key expiration with lazy deletion
- **AOF persistence** — append-only file logging, crash recovery on restart
- **Master-replica replication** — write to master, read from replica via Docker Compose
- **Cross-platform** — epoll on Linux, thread-per-connection fallback on macOS

## Commands

```
SET key value [EX seconds]
GET key
DEL key
EXISTS key
TTL key
DBSIZE
INFO
FLUSHALL
PING
```

## Benchmark

```
[PING]  67,069 ops/sec  P99: 0.055ms
[SET]   59,540 ops/sec  P99: 0.021ms
[GET]   72,294 ops/sec  P99: 0.019ms
```

## Quick Start

```bash
# Single instance
mkdir build && cd build
cmake .. && make
./litekv --port 6380

# Master-replica with Docker
docker-compose up --build

# Test replication
redis-cli -p 6380 SET city Rome
redis-cli -p 6381 GET city  # "Rome"
```

## Run Benchmarks

```bash
python3 tests/benchmark.py
```

## Tech Stack

C++17, CMake, epoll, POSIX sockets, Docker
