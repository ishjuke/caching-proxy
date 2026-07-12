# caching-proxy

A mini CDN — an HTTP caching reverse proxy written in C, running on a Raspberry Pi 5. Requests hit an in-memory cache (hash table + LRU eviction); misses are forwarded to an origin server, cached, and relayed.

## Architecture

- **Hash table** with separate chaining for O(1) key lookup.
- **LRU eviction** via a doubly-linked recency list + fixed capacity, so lookup *and* eviction are both O(1).
- **Single-threaded socket server** (`socket`/`bind`/`listen`/`accept`) serving HTTP on port 8080.
- **Origin forwarding**: on a cache miss the proxy opens an outbound connection to a fixed origin, fetches the response, caches it keyed by request path, and relays it to the client.

Build and run:

    gcc -Wall -Wextra -o server server.c
    ./server

## Benchmarks

Measured on a **Raspberry Pi 5 (8GB)** — Broadcom BCM2712 (quad-core Arm Cortex-A76 @ 2.4GHz), Raspberry Pi OS Lite (64-bit), booting from microSD. Proxy compiled with `gcc -Wall -Wextra`.

### Setup

- **Proxy** (this project): single-threaded, port 8080.
- **Origin server:** Python's built-in `http.server` on port 9000, serving small static files:

      cd origin-test && python3 -m http.server 9000

- **Load generator:** wrk (installed via apt), 2 threads / 50 connections / 10s.

### Cache HIT (served from the in-memory LRU cache)

    wrk -t2 -c50 -d10s http://localhost:8080/index.html

### Cache MISS (forced origin fetch on every request)

A Lua script appends a random query string per request so no two requests share a cache key, forcing a full origin round-trip each time:

    -- miss.lua
    request = function()
       local path = "/index.html?nocache=" .. math.random(1, 100000000)
       return wrk.format("GET", path)
    end

    wrk -t2 -c50 -d10s -s miss.lua http://localhost:8080/index.html

### Results

| Scenario   | Requests/sec | Avg latency |
|------------|-------------:|------------:|
| Cache HIT  |      ~46,240 |     15.0 ms |
| Cache MISS |       ~1,590 |     16.1 ms |

**~29x higher throughput on cache hits** vs. origin fetches under 50 concurrent connections. Hits are a pure in-memory hash lookup; misses pay the full cost of a socket connection, request, and read from the origin. The latency tail under load (max ~1.66s on both runs) reflects the single-threaded accept loop — the natural next optimization.
## Concurrency experiment: thread-per-connection

The single-threaded server degrades under rising concurrency (throughput
falls as connections pile up behind one accept loop). I added a
thread-per-connection model (one worker thread per client, a mutex
guarding the shared cache) and re-ran the sweep.

### Throughput vs. concurrency (cache hits)

| Connections | Single-threaded (req/s) | Threaded (req/s) |
|-------------|------------------------:|-----------------:|
| 10          |                  47,769 |           21,240 |
| 50          |                  38,390 |           16,254 |
| 100         |                  24,460 |           14,809 |
| 200         |                  23,739 |           14,203 |

### Cache-miss throughput (50 connections)

| Server          | Requests/sec |
|-----------------|-------------:|
| Single-threaded |        1,591 |
| Threaded        |    **2,149** |

### Takeaway

Threading is a **workload-dependent tradeoff, not a free win**:

- **Cache hits are CPU-cheap** (a hash lookup + memcpy). Per-request
  thread-creation overhead exceeds the actual work, so the
  single-threaded server has higher hit throughput.
- **Cache misses are I/O-bound** (a blocking origin round-trip). Threading
  lets one thread wait on the origin while others keep serving, giving
  **~35% higher miss throughput**.
- Threading also tightened latency under moderate load (e.g. at 50
  connections the max dropped from ~1.66s to ~18ms).

The right next step is a **thread pool** — reusing a fixed set of workers
instead of spawning one per connection would remove the per-request
creation cost that hurts the hit path, while keeping the miss-path
parallelism.