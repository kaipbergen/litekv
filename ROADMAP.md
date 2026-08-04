# LiteKV — 100 Days of Code

A 100-day roadmap of real, scoped improvements to LiteKV, executed one item per day by an
automated daily task. Each day: implement the item, build, commit, push. No filler commits —
if an item can't be completed and verified, it stays unchecked and gets picked up next run.

Progress is tracked by checking items off below (`- [ ]` → `- [x] (Day N, YYYY-MM-DD)`).

## Strings & core commands
- [x] INCR (Day 1, 2026-07-24)
- [x] Fix replica auto-reconnect with retry + backoff (Day 1, 2026-07-24)
- [x] Full resync on reconnect (Day 1, 2026-07-24)
- [x] INCRBY / DECRBY (Day 1, 2026-07-24)
- [x] Unit test framework setup (Catch2) + first tests for storage.cpp (Day 2, 2026-07-26)
- [x] APPEND (Day 2, 2026-07-26)
- [x] STRLEN (Day 2, 2026-07-26)
- [x] GitHub Actions CI (build + run tests on every push) (Day 2, 2026-07-26)
- [x] MSET / MGET (Day 2, 2026-07-26)
- [x] GETSET (Day 3, 2026-07-29)
- [x] SETNX (Day 3, 2026-07-29)
- [x] RENAME (Day 3, 2026-07-29)
- [x] TYPE (Day 3, 2026-07-29)
- [x] Unit tests for parser.cpp (RESP edge cases) (Day 3, 2026-07-29)

## TTL & key introspection
- [x] EXPIRE / PERSIST (decoupled from SET EX) (Day 4, 2026-07-31)
- [x] PEXPIRE / PTTL (millisecond precision) (Day 4, 2026-07-31)
- [x] KEYS (glob pattern matching) (Day 4, 2026-07-31)
- [x] SCAN (cursor-based iteration) (Day 4, 2026-07-31)

## Replication hardening
- [x] Replication offset tracking (master + replica report offset) (Day 4, 2026-07-31)
- [x] WAIT (block until N replicas ack) (Day 5, 2026-08-02)

## Hashes
- [x] HSET / HGET (Day 5, 2026-08-02)
- [x] HDEL / HGETALL (Day 5, 2026-08-02)
- [x] HEXISTS / HLEN (Day 5, 2026-08-02)
- [x] HKEYS / HVALS (Day 5, 2026-08-02)
- [x] Integration test script for replication (bash/python, master SET → replica GET) (Day 6, 2026-08-04)

## Lists
- [x] LPUSH / RPUSH (Day 6, 2026-08-04)
- [x] LPOP / RPOP (Day 6, 2026-08-04)
- [x] LRANGE / LLEN (Day 6, 2026-08-04)

## Sets
- [ ] SADD / SREM
- [ ] SISMEMBER / SMEMBERS / SCARD
- [ ] Static analysis integration (clang-tidy)

## Sorted sets & transactions
- [ ] ZADD / ZSCORE
- [ ] ZRANGE
- [ ] MULTI / EXEC / DISCARD
- [ ] WATCH (optimistic locking)
- [ ] AddressSanitizer / UBSan debug build target

## Pub/Sub & config
- [ ] SUBSCRIBE / PUBLISH
- [ ] PSUBSCRIBE (pattern pub/sub)
- [ ] CONFIG GET / SET
- [ ] AUTH command + requirepass config
- [ ] Code coverage reporting (gcov/lcov)
- [ ] SELECT (multiple logical DBs)
- [ ] COPY key
- [ ] RANDOMKEY
- [ ] OBJECT ENCODING introspection

## Persistence
- [ ] SAVE (point-in-time RDB-like snapshot)
- [ ] BGSAVE (background save)
- [ ] BGREWRITEAOF (AOF compaction)
- [ ] Configurable AOF fsync policy (always / everysec / no)
- [ ] Halfway checkpoint: update README benchmarks + short retrospective
- [ ] LFU eviction policy
- [ ] Random eviction policy
- [ ] Prefer RDB over AOF on restart when newer
- [ ] AOF entry checksum / corruption detection
- [ ] Graceful recovery from a partial/truncated AOF write

## Replication resilience
- [ ] Replication heartbeat/PING keepalive
- [ ] Replication backlog buffer for partial resync
- [ ] Sub-replication (replica-of-replica) support
- [ ] REPLICAOF NO ONE (failover promotion)
- [ ] Benchmark suite expansion (pipelining, mixed workloads)

## Networking & server
- [ ] Non-blocking partial-write buffering for slow clients
- [ ] Idle client timeout
- [ ] Max clients limit + graceful rejection
- [ ] Unix domain socket support
- [ ] SIGHUP config reload
- [ ] TCP_NODELAY / SO_KEEPALIVE tuning
- [ ] kqueue event loop for macOS, part 1 (design + skeleton, replacing thread-per-connection)
- [ ] kqueue event loop for macOS, part 2 (integrate + verify)

## Observability
- [ ] INFO command expansion (memory, uptime, connected_clients)
- [ ] SLOWLOG
- [ ] MONITOR
- [ ] Prometheus /metrics endpoint
- [ ] Structured logging (levels, timestamps)
- [ ] COMMAND introspection
- [ ] CLIENT LIST / CLIENT KILL
- [ ] LATENCY monitoring command
- [ ] MEMORY USAGE key
- [ ] DEBUG SLEEP / DEBUG OBJECT helpers
- [ ] Fuzz testing harness for the RESP parser

## Docs & devex
- [ ] CONTRIBUTING.md + issue templates
- [ ] Architecture decision records (docs/adr/*)
- [ ] --help CLI flag + usage text
- [ ] Minimal Python RESP client example in docs
- [ ] Minimal Node.js RESP client example in docs
- [ ] Docker multi-stage build (shrink image size)
- [ ] Helm chart wrapping k8s/ manifests
- [ ] GitHub Actions: build/push Docker image to GHCR on tag
- [ ] LICENSE file (MIT) if missing
- [ ] CODEOWNERS file
- [ ] Updated README benchmark section with post-optimization numbers

## Stretch: scripting & cluster prep
- [ ] Basic Lua EVAL scripting (simplified subset)
- [ ] Consistent hashing utility module (cluster prep)
- [ ] Cluster mode: hash slot assignment, design + skeleton
- [ ] Client-side MOVED-style redirection errors
- [ ] Gossip protocol skeleton for cluster node discovery
- [ ] litekv-cli backup/restore tool
- [ ] Per-client rate limiting / command throttling
- [ ] Basic ACL (multiple users, permission scopes)
- [ ] Optional AOF encryption at rest
- [ ] Day 100: final performance tuning pass + updated benchmark report + 100-day retrospective
