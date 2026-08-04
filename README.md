# caching-proxy

A mini CDN — an HTTP caching reverse proxy written in C, running on a Raspberry
Pi 5. Requests hit an in-memory cache (hash table + LRU eviction); misses are
forwarded to an origin server, cached, and relayed.

The point of the project was never just to build one — it was to build it **four
different ways**, benchmark them rigorously against each other, and understand
*why* they differ. Along the way it became a study in something more useful than
"which is fastest": how to measure honestly, and how much the answer depends on
what you actually optimize for.

---

## Architecture

- **Hash table** with separate chaining (djb2) for O(1) key lookup.
- **LRU eviction** via a doubly-linked recency list + fixed capacity, so lookup
  *and* eviction are both O(1).
- **HTTP/1.1 keep-alive** on the client side; persistent origin connections
  (connection reuse) on the origin side for the threaded and pool servers.
- **Origin forwarding**: on a miss the proxy fetches from a fixed origin
  (nginx), caches the response keyed by request path, and relays it.

Four server architectures share this core and differ only in how they handle
concurrency:

| Server               | Concurrency model                                        |
|----------------------|----------------------------------------------------------|
| `server.c`           | Single-threaded (one accept loop)                        |
| `server_threaded.c`  | Thread-per-connection                                    |
| `server_pool.c`      | Fixed thread pool (16 workers + job queue)               |
| `server_epoll.c`     | Single-threaded epoll event loop (nginx-style)           |

Build any of them:

    gcc -Wall -Wextra -O2 -o server server.c
    gcc -Wall -Wextra -O2 -pthread -o server_threaded server_threaded.c
    gcc -Wall -Wextra -O2 -pthread -o server_pool server_pool.c
    gcc -Wall -Wextra -O2 -o server_epoll server_epoll.c
    ./server            # (or whichever)

---

## Part 1: The cache

A hash table for lookup, an LRU list for eviction, a socket server, and origin
forwarding. The first question was simple: how much does caching actually buy
you? Measured on a cache hit (in-memory) versus a miss (full origin fetch), the
hit path was roughly **29x** the throughput of the miss path — which is exactly
what you'd expect, since a hit is a hash lookup and a memcpy while a miss pays
for a network round-trip.

## Part 2: Eviction policy — LRU vs LFU

I implemented a second policy, LFU (least-frequently-used), and compared hit
rates across three synthetic workloads (`cache_compare.c`) — 200,000 requests
over 1,000 keys, cache capacity 10% of the keyspace.

| Workload                   | LRU hit rate | LFU hit rate | Winner        |
|----------------------------|-------------:|-------------:|---------------|
| Static skew (Zipf s=1.2)   |        75.6% |        81.3% | LFU  +5.7     |
| Stronger skew (Zipf s=1.5) |        91.9% |        93.7% | LFU  +1.8     |
| Drifting popularity        |        75.4% |     **9.5%** | **LRU +65.9** |

**Takeaway.** Under stable popularity, LFU wins — it protects genuinely hot
items. But under *drifting* popularity, LFU **collapses to 9.5%**: old frequency
counts become permanent baggage. Items that were popular in the past accumulate
high counts and can't be evicted, while newly-hot items (starting at freq=1) get
evicted immediately and never build up. LRU has no such memory and adapts,
holding ~75%. This is why production caches favor adaptive policies (ARC, LRU-K)
that blend recency and frequency rather than committing to either extreme.

## Part 3 & 4: Four concurrency architectures

Single-threaded degrades under load, so I built thread-per-connection, then a
thread pool, then an epoll event loop — each a response to what the previous
one's benchmarks revealed. The full head-to-head comparison is below, but two
findings from the build are worth stating on their own:

- **Thread-per-connection is a workload-dependent tradeoff, not a free win.**
  Naively it "should" beat single-threaded, but for CPU-cheap cache *hits* the
  per-connection thread cost matters; where it earns its keep is the I/O-bound
  *miss* path, where waiting on the origin overlaps across threads.
- **The thread pool** removes the per-request thread-creation cost by reusing a
  fixed set of workers, and (with persistent per-worker origin connections)
  reuses upstream connections across the whole process lifetime.
- **The epoll event loop** stops mapping connections to threads at all: one
  thread, non-blocking sockets, a state machine that on a miss drives *both* the
  client and origin sockets without ever blocking.

---

## Methodology (read this before the numbers)

The first version of this benchmark had real flaws, flagged by an experienced
reviewer, and the numbers below are the **corrected** measurements. What changed,
and why it matters:

- **Off-box load generation.** Originally the load generator (`wrk`), the proxy,
  and the origin all ran on the same 4-core Pi, competing for CPU — which
  contaminates every cross-architecture comparison. Now `wrk` runs on a separate
  laptop, wired to the Pi over Ethernet (verified: 1000-ping flood, 0% loss,
  ~0.3ms).
- **A real origin.** The original origin was `python3 -m http.server`, which is
  single-threaded and tops out around 1,500–2,400 req/s — so every miss number
  was really measuring *Python's* ceiling, not the proxy. Replaced with **nginx**
  serving static files.
- **HTTP keep-alive.** The original server closed the connection after every
  response (HTTP/1.0). Over a real network that means a full TCP handshake *per
  request*, which dominates the measurement. Adding HTTP/1.1 keep-alive (client
  and origin side) removed that, and dropped single-connection latency from
  ~1.8ms to ~400us.
- **Little's Law reconciles.** The corrected numbers are self-consistent:
  at 50 connections and 62,304 req/s (epoll), Little's Law predicts
  50/62304 ≈ 802us average latency; measured average was 793us. The original
  numbers were off by 3–14x, which was the tell that something in the setup was
  wrong.
- **Full latency distribution.** Reporting p50/p99 (via `wrk --latency`), not
  just average/max — because the whole conclusion is about tail latency, and
  average/max are the two numbers that hide the tail worst.

All four servers were compiled identically (`-O2`, per-request logging disabled)
and measured in one session against the same nginx origin over the same wired
link.

---

## The four-way comparison

### Hit throughput (requests/sec)

| Connections | Single-thread | Thread-per-conn | Pool (16) |  epoll |
|-------------|--------------:|----------------:|----------:|-------:|
| 10          |         1,828 |          24,855 |    22,348 | 19,138 |
| 50          |         2,547 |     **115,107** |    45,162 | 62,304 |
| 100         |         2,806 |     **116,681** |    46,468 | 81,874 |
| 200         |         2,810 |          79,125 |    47,371 | 77,507 |

### Hit-path tail latency (p99)

| Connections | Single-thread | Thread-per-conn | Pool (16) |    epoll |
|-------------|--------------:|----------------:|----------:|---------:|
| 50          |         1.66s |          85.8ms |     109ms | **1.57ms** |
| 100         |         1.10s |          1.74ms |     168ms |   2.22ms |
| 200         |         830ms |          1.71ms |     371ms |   3.53ms |

### Miss throughput (50 connections)

| Server                | Requests/sec |
|-----------------------|-------------:|
| Single-threaded       |        1,354 |
| Thread-per-connection |       39,899 |
| Pool (16)             |       37,967 |
| epoll                 |       16,403 |

(Threaded and pool reuse origin connections, so their miss throughput reflects
real concurrent I/O — ~18x the old connect-per-miss numbers. epoll still opens a
fresh origin connection per miss; see *Known limitations*.)

---

## What the numbers say

There is no single "fastest" architecture. Keep-alive **reshuffled the entire
standings** versus a naive close-per-request setup, and each server now wins a
different thing:

**1. Single-threaded + keep-alive collapses under concurrency.** At 50
connections it manages ~2,547 req/s — barely above its single-connection number
— with a **1.66-second** p99. Keep-alive makes it *serialize*: the server stays
glued to one persistent connection's read-respond loop while the other 49 wait,
instead of round-robining by closing after each request. This is the one server
keep-alive actively *hurt* under load.

**2. Thread-per-connection has the highest peak (116,681 req/s).** With
keep-alive, a thread is spawned per *connection* and lives for that connection's
entire lifetime of many requests — so the thread-creation cost is amortized
across thousands of requests instead of paid per request. This *inverts* its
pre-keep-alive result, where it was the slowest on hits. But it falls off at 200
connections (79k) as 200 threads contend for 4 cores.

**3. The thread pool scales flattest and most predictably** (22k → 47k, steady
across concurrency) because its resource use is bounded at 16 workers. It doesn't
hit the highest peak, and its hit-path tail latency is the worst at moderate
concurrency (the job queue adds variance), but it never collapses and never
thrashes.

**4. epoll has by far the best latency discipline.** 1.57ms p99 at 50
connections, versus tens to hundreds of milliseconds for everyone else, scaling
smoothly from 19k to 82k — and it was the **only** server with zero socket errors
across every test. It doesn't win peak throughput, but it wins *consistency*,
which is exactly why production servers like nginx use an event loop: at scale,
predictable latency matters more than peak throughput, and the event-driven model
delivers it without a thread per connection.

**The real lesson: "fastest" is the wrong question.** Peak throughput picks
thread-per-connection; predictable scaling picks the pool; tail latency and
stability pick the event loop; and a single-threaded server that looked fine
without keep-alive collapses with it. You only see any of this by measuring the
distribution under load — not the headline number.

---

## Known limitations

Named honestly, because a caching proxy has correctness requirements beyond raw
performance:

- **No Cache-Control / Expires / TTL.** Responses are cached by path and kept
  indefinitely — a `no-store` response would be cached forever. This is a
  correctness gap distinct from the performance work.
- **No request coalescing.** N concurrent misses on the same cold key send N
  origin requests (a cache stampede); nginx has `proxy_cache_lock` for exactly
  this. The `miss.lua` workload uses unique keys, so this case isn't exercised.
- **epoll origin keep-alive not implemented.** The event-loop server opens a
  fresh origin connection per miss. Correct origin keep-alive in a single-threaded
  event loop requires an explicit *pool* of upstream connections (as nginx's
  `upstream ... keepalive` does), tracking which client each origin socket serves
  and matching responses back asynchronously — a single shared connection would
  serialize all misses and regress throughput. This is the correct next step and
  is left as future work.
- **~1% read errors under 100%-miss synthetic load** at 30k+ req/s. Instrumenting
  the servers (per-break-path counters) confirmed the proxy closes **zero** client
  connections under load; the errors are TCP-layer resets on the load-generator
  side, inherent to sustained synthetic miss traffic, not a server defect.

---

## Files

- `cache.c` — standalone hash table + LRU (Part 1).
- `cache_compare.c` — LRU vs LFU hit-rate comparison across workloads (Part 2).
- `server.c` — single-threaded proxy.
- `server_threaded.c` — thread-per-connection.
- `server_pool.c` — fixed thread pool + job queue.
- `server_epoll.c` — epoll event loop.
- `miss.lua` — `wrk` script generating unique keys to force cache misses.