# LiteKV — 300 Days of Code

A 300-day roadmap of real, scoped improvements to LiteKV, executed in batches by an
automated daily task (this project rotates with semantic-cache and projectjava, 5 items
per run on its day). Each run: implement the items, build, commit, push. No filler commits —
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
- [x] SADD / SREM (Day 6, 2026-08-04)
- [x] SISMEMBER / SMEMBERS / SCARD (Day 7, 2026-08-06)
- [x] Static analysis integration (clang-tidy) (Day 7, 2026-08-06)

## Sorted sets & transactions
- [x] ZADD / ZSCORE (Day 7, 2026-08-06)
- [x] ZRANGE (Day 7, 2026-08-06)
- [x] MULTI / EXEC / DISCARD (Day 7, 2026-08-06)
- [x] WATCH (optimistic locking) (Day 8, 2026-08-08)
- [x] AddressSanitizer / UBSan debug build target (Day 8, 2026-08-08)

## Pub/Sub & config
- [x] SUBSCRIBE / PUBLISH (Day 8, 2026-08-08)
- [x] PSUBSCRIBE (pattern pub/sub) (Day 8, 2026-08-08)
- [x] CONFIG GET / SET (Day 8, 2026-08-08)
- [x] AUTH command + requirepass config (Day 9, 2026-08-10)
- [x] Code coverage reporting (gcov/lcov) (Day 9, 2026-08-10)
- [x] SELECT (multiple logical DBs) (Day 10, 2026-08-12)
- [x] COPY key (Day 9, 2026-08-10)
- [x] RANDOMKEY (Day 9, 2026-08-10)
- [x] OBJECT ENCODING introspection (Day 9, 2026-08-10)

## Persistence
- [x] SAVE (point-in-time RDB-like snapshot) (Day 10, 2026-08-12)
- [x] BGSAVE (background save) (Day 10, 2026-08-12)
- [x] BGREWRITEAOF (AOF compaction) (Day 10, 2026-08-12)
- [x] Configurable AOF fsync policy (always / everysec / no) (Day 10, 2026-08-12)
- [x] Halfway checkpoint: update README benchmarks + short retrospective (Day 12, 2026-08-18)
- [x] LFU eviction policy (Day 11, 2026-08-15)
- [x] Random eviction policy (Day 11, 2026-08-15)
- [x] Prefer RDB over AOF on restart when newer (Day 11, 2026-08-15)
- [x] AOF entry checksum / corruption detection (Day 11, 2026-08-15)
- [x] Graceful recovery from a partial/truncated AOF write (Day 11, 2026-08-15)

## Replication resilience
- [x] Replication heartbeat/PING keepalive (Day 12, 2026-08-18)
- [x] Replication backlog buffer for partial resync (Day 12, 2026-08-18)
- [x] Sub-replication (replica-of-replica) support (Day 12, 2026-08-18)
- [x] REPLICAOF NO ONE (failover promotion) (Day 12, 2026-08-18)
- [x] Benchmark suite expansion (pipelining, mixed workloads) (Day 13, 2026-08-24)

## Networking & server
- [ ] Non-blocking partial-write buffering for slow clients
- [x] Idle client timeout (Day 13, 2026-08-24)
- [x] Max clients limit + graceful rejection (Day 13, 2026-08-24)
- [x] Unix domain socket support (Day 13, 2026-08-24)
- [ ] SIGHUP config reload
- [x] TCP_NODELAY / SO_KEEPALIVE tuning (Day 13, 2026-08-24)
- [ ] kqueue event loop for macOS, part 1 (design + skeleton, replacing thread-per-connection)
- [ ] kqueue event loop for macOS, part 2 (integrate + verify)

## Observability
- [x] INFO command expansion (memory, uptime, connected_clients) (Day 14, 2026-08-27)
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

## More data type coverage
- [ ] GETRANGE / SETRANGE
- [x] INCRBYFLOAT (Day 15, 2026-09-01)
- [x] GETDEL (Day 15, 2026-09-01)
- [ ] SET with NX / XX / GET option flags
- [ ] BITCOUNT / SETBIT / GETBIT / BITOP
- [ ] LINSERT / LREM / LSET / LTRIM
- [ ] BLPOP / BRPOP (blocking list pops)
- [ ] RPOPLPUSH
- [ ] HINCRBY / HINCRBYFLOAT
- [ ] HSETNX
- [ ] HRANDFIELD
- [ ] HSCAN
- [ ] SPOP / SRANDMEMBER
- [ ] SDIFF / SINTER / SUNION (+ STORE variants)
- [ ] SSCAN
- [ ] ZINCRBY
- [ ] ZRANK / ZREVRANK
- [ ] ZREM
- [ ] ZRANGEBYSCORE / ZCOUNT
- [ ] ZPOPMIN / ZPOPMAX
- [ ] ZUNIONSTORE / ZINTERSTORE

## Key & admin commands
- [x] EXPIREAT / PEXPIREAT (Day 14, 2026-08-27)
- [x] Multi-key EXISTS and DEL (Day 14, 2026-08-27)
- [x] TOUCH (Day 14, 2026-08-27)
- [ ] MOVE (between logical DBs)
- [x] DBSIZE (Day 15, 2026-09-01)
- [x] FLUSHALL (Day 15, 2026-09-01)
- [ ] LASTSAVE
- [x] TIME command (Day 14, 2026-08-27)
- [x] UNWATCH (Day 15, 2026-09-01)
- [ ] CLIENT GETNAME / CLIENT SETNAME
- [ ] DUMP / RESTORE (key serialization for migration between instances)

## Notes
- Day 15 (2026-09-01): Checked off DBSIZE, FLUSHALL, and UNWATCH. All three
  were already implemented in src/server.cpp (confirmed by reading the code)
  but had no test coverage or accompanying commit, so they'd been left
  unchecked since Day 8. Each now has its own commit adding real integration
  test coverage in tests/integration_admin_commands.py.

## Stretch: newer data types & platform hardening
- [ ] HyperLogLog: PFADD / PFCOUNT (approximate cardinality)
- [ ] Minimal Streams subset: XADD / XRANGE / XREAD
- [ ] Minimal Geo subset: GEOADD / GEODIST
- [ ] TLS support for client connections
- [ ] Config file support (litekv.conf) alongside CLI flags
- [ ] Protected mode: bind to localhost only by default unless explicitly configured
- [ ] Chaos test for replication failover (kill master mid-write, verify replica state)
- [ ] docker-compose for a local master+replica dev cluster
- [ ] Final day: 300-day program retrospective + updated benchmark report
