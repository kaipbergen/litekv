# ⚡ LiteKV

A distributed in-memory key-value store built from scratch in C++17, inspired by Redis. Features an event-driven epoll architecture, RESP protocol, AOF persistence, LRU eviction, and master-replica replication over Docker.

## 🎯 Project Highlights

This project demonstrates low-level systems engineering across networking, concurrency, and storage:

- **Event-driven I/O** — Linux epoll handles thousands of connections in a single thread
- **Zero-copy parsing** — RESP protocol parsed with `std::string_view`, no heap allocations
- **LRU eviction** — doubly linked list + hash map, O(1) eviction under memory pressure
- **AOF persistence** — crash-recovery via append-only file replay on startup
- **Master-replica replication** — write to master, propagate to replicas in real-time

## 🏗️ Architecture

```
Client (redis-cli / any RESP client)
              ↓
    epoll Event Loop (Linux)
    Thread-per-connection (macOS fallback)
              ↓
    zero-copy RESP Parser (std::string_view)
              ↓
    ┌─────────────────────────┐
    │   LRU Storage Engine    │
    │  unordered_map + list   │
    │  TTL + lazy eviction    │
    └─────────────────────────┘
              ↓
    AOF Logger → disk (litekv.aof)
              ↓
    Replication → propagate to replicas
```

## 🔧 Tech Stack

| Component | Implementation | Details |
|-----------|---------------|---------|
| Language | C++17 | Modern C++ with string_view, optional |
| Networking | POSIX sockets + epoll | Non-blocking async I/O on Linux |
| Protocol | RESP2 | Redis Serialization Protocol |
| Storage | LRU + unordered_map | O(1) get/set with eviction |
| Persistence | AOF | Append-only file, crash recovery |
| Replication | TCP stream | Master propagates writes to replicas |
| Build | CMake | Cross-platform build system |
| Deploy | Docker Compose / Kubernetes | Master + replica containers, StatefulSets in k8s |

## 📊 Benchmark

```
[PING]  67,069 ops/sec  |  P50: 0.013ms  |  P99: 0.055ms
[SET]   59,540 ops/sec  |  P50: 0.014ms  |  P99: 0.021ms
[GET]   72,294 ops/sec  |  P50: 0.014ms  |  P99: 0.019ms
```

## 🎓 Key Implementation Details

### 1️⃣ epoll Event Loop
Single-threaded async I/O using Linux epoll — same approach as original Redis. Non-blocking sockets, per-client buffers, complete RESP message validation before processing.

### 2️⃣ Zero-Copy RESP Parser
```cpp
// std::string_view avoids heap allocation
Command Parser::parse(std::string_view input) {
    // parses *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n
}
```

### 3️⃣ LRU Eviction
```cpp
// O(1) access + O(1) eviction
std::list<std::string> lru_list_;
std::unordered_map<std::string,
    std::pair<Entry, std::list<std::string>::iterator>> data_;
```

### 4️⃣ Master-Replica Replication
```
Master (port 6380)          Replica (port 6381)
       │                            │
       │←── REPLCONF ───────────────│
       │─── +OK ───────────────────→│
       │                            │
SET city Rome ──────────────────────→ applied
```

## 🚀 Quick Start

### Single Instance
```bash
mkdir build && cd build
cmake .. && make
./litekv --port 6380 --aof data.aof
```

### Master-Replica with Docker
```bash
docker-compose up --build

# Test replication
redis-cli -p 6380 SET city Rome   # write to master
redis-cli -p 6381 GET city        # read from replica → "Rome"
```

## ☸️ Kubernetes Deployment

The `k8s/` directory ports the same master-replica setup onto Kubernetes, as an alternative to `docker-compose` (both are kept in the repo). It uses `StatefulSet`s instead of `Deployment`s so each replica gets a stable network identity and its own `PersistentVolumeClaim` for the AOF file — a bare `Deployment` would lose the AOF on every pod reschedule since its volumes aren't stably bound to one pod.

### Manifests

| File | Purpose |
|------|---------|
| `k8s/00-namespace.yaml` | `litekv` namespace |
| `k8s/01-configmap.yaml` | Ports, AOF paths, and the master's service host/port used by `--replicaof` |
| `k8s/02-master.yaml` | Headless `Service` + `StatefulSet` for the master, with a `volumeClaimTemplate` for `/data` |
| `k8s/03-replica.yaml` | Headless `Service` + `StatefulSet` for the replica, plus an `initContainer` that waits for the master's port to be reachable before starting (mirrors `depends_on`/healthcheck in `docker-compose.yml`) |

Both Services are headless (`clusterIP: None`) — required for `StatefulSet` pod DNS, and also what the replica dials for replication (`litekv-master.litekv.svc.cluster.local:6380` via the `MASTER_SVC_HOST`/`MASTER_SVC_PORT` ConfigMap keys). Config is injected as env vars via `envFrom`, and Kubernetes' `$(VAR)` expansion substitutes them straight into each container's `args`, so there's no need to bake config into the image.

### Local setup with minikube (macOS)

```bash
brew install minikube
minikube start --driver=docker --cpus=2 --memory=4096

# Build the image directly into minikube's Docker daemon (no registry needed)
eval $(minikube docker-env)
docker build -t litekv:latest .

kubectl apply -f k8s/
kubectl -n litekv get pods -o wide
```

### Test replication

```bash
kubectl -n litekv port-forward svc/litekv-master 16380:6380 &
kubectl -n litekv port-forward svc/litekv-replica 16381:6380 &

redis-cli -p 16380 SET city Rome   # write to master
redis-cli -p 16381 GET city        # read from replica → "Rome"
redis-cli -p 16381 SET city Milan  # ERR READONLY — replicas reject writes
```

### AOF persistence across pod restarts

```bash
kubectl -n litekv delete pod litekv-master-0        # StatefulSet recreates it
kubectl -n litekv wait --for=condition=Ready pod/litekv-master-0 --timeout=60s
kubectl -n litekv logs litekv-master-0 | head -1     # "AOF: loaded N entries"
```

The same PVC (`data-litekv-master-0`) is remounted into the recreated pod, so the AOF replays and prior keys survive.

> **Known limitation (not k8s-specific):** the replica's TCP connection to the master doesn't auto-reconnect if it drops (e.g. after the master pod restarts) — it only tails commands sent over the connection made at startup, and there's no full-resync-on-reconnect. In Kubernetes, `kubectl -n litekv delete pod litekv-replica-0` forces a clean reconnect since the `StatefulSet` restarts it against the current master. The same gap exists in the `docker-compose` setup if the master container restarts.

## 📋 Commands

| Command | Description |
|---------|-------------|
| `SET key value [EX seconds]` | Set key with optional TTL |
| `GET key` | Get value |
| `DEL key` | Delete key |
| `EXISTS key` | Check existence |
| `TTL key` | Remaining TTL (-1 = no expiry, -2 = not found) |
| `DBSIZE` | Number of keys |
| `INFO` | Server role and key count |
| `FLUSHALL` | Clear all keys |
| `PING` | Health check |

## 📈 Run Benchmarks

```bash
python3 tests/benchmark.py
```

## 🔗 Related Projects

This project complements [LLM Semantic Cache](https://github.com/kaipbergen/semantic-cache) — where Redis was used as the cache backend. LiteKV is the from-scratch C++ implementation of that same Redis layer.
