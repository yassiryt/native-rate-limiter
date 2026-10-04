# Native C++ Rate Limiter for Node.js

A fast rate limiter that runs in shared memory. No GC pauses. Works across PM2 workers.

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

## How It Works

- **Shared memory** (`/node_rate_limiter_shm`) — survives process restarts, shared across workers
- **Lock-free** — CAS atomic operations, no mutexes
- **Fixed table** — 65,536 buckets, 64-byte aligned to avoid false sharing
- **FNV-1a hash** — random seed per run prevents collision attacks
- **Token bucket** — tokens refill every `windowMs` milliseconds
- **LRU Eviction** - Uses a hardware spinlock to safely delete old IPs when the table is full.

## Limitations (by design)

- Single machine only (no Redis, no cluster sync)
- Fixed window (burst at window boundaries)
- No persistence across reboots
- One config for all IPs

## Production Tips

- Call `cleanup()` in `process.on('exit')` and `process.on('SIGTERM')`
- Monitor `/dev/shm/node_rate_limiter_shm` size (about 4 MB)
- Run with `--max-old-space-size` lower since this bypasses V8 heap

## License

ISC