# Native C++ Rate Limiter for Node.js

A fast rate limiter using shared memory and token buckets. It supports multiple Node.js worker processes on the same machine.

## Install

```bash
npm install
npx node-gyp configure build
```

## Quick Start

```js
const rateLimiter = require('./build/Release/rate_limiter.node');

rateLimiter.initSharedMemory({ maxTokens: 100, windowMs: 1000 });

app.use((req, res, next) => {
  const ip = req.headers['x-forwarded-for'] || req.socket.remoteAddress;
  if (!rateLimiter.consumeTokenFast(ip)) {
    return res.status(429).json({ error: 'Too Many Requests' });
  }
  next();
});
```

Set `RATE_LIMITER_SHM_NAME` before loading the addon when you need an isolated
limiter namespace (for example, separate applications on the same machine).
The default is `/node_rate_limiter_shm`.

## Architecture

        Node.js / Express
              │
              ▼
        Node-API binding
              │
              ▼
        C++ Rate Limiter
              │
        ┌─────┴─────┐
        ▼           ▼
        Shared       Atomic
        Memory       Operations
        │
        ▼
        Hash Table → LRU

- **Shared memory** (`/node_rate_limiter_shm`) — used by multiple worker processes on one machine. Each process has its own V8 heap; only the SHM region is shared.
- **Token bucket** — tokens refill every `windowMs` milliseconds at rate configured by `maxTokens`.
- **LRU eviction** — uses a process-shared spinlock to safely update the table and evict old entries when it is full.
- **Fixed table** — 65,536 buckets, FNV-1a hash with a per-shared-memory-region seed.
- **Configuration consistency** — workers must use the same `maxTokens` and `windowMs`; conflicting configuration is rejected.

## How It Works

1. **Hash** — FNV-1a hash of the IP string produces a 64-bit value
2. **Bucket lookup** — `ip_hash % 65536` selects the bucket index
3. **Process-shared synchronization** — a shared spinlock serializes table mutations across workers, while counters remain atomic.
4. **Token refill** — if `(now - last_ts) > windowMs`, tokens are restored up to `max_tokens`
5. **LRU eviction** — when the table is full, oldest entries are evicted using a hardware spinlock to safely remove from the linked list
6. **Per-IP isolation** — each unique IP gets its own bucket; different IPs do not interfere

## Configuration

| Option      |     Description                  |Default|
|-------------|----------------------------------|-------|
| `maxTokens` | Max tokens per IP window         | `10`  |
| `windowMs`  | Refill interval in ms            | `1000`|
| `policy`    | Eviction policy: `LRU` or `FIFO` | `LRU` |



## Build

```bash
npm install
npx node-gyp configure build
```

## Test

```bash
npm test
```

## Benchmark

Run the included benchmark:

```bash
node benchmark/simple.js
```

Output: requests/second for the configured workload.

## Limitations (by design)

- Single machine only — no Redis, no cluster sync
- Fixed window — burst possible at window boundaries
- No persistence across reboots (shared memory is lost if all processes exit)
- One config for all IPs
- Table mutations use a spinlock for cross-process consistency; this is not lock-free
- The shared-memory object remains available until the host reboots or it is explicitly removed by the system administrator

## License

ISC