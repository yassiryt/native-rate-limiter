const rateLimiter = require('./build/Release/rate_limiter.node');
const http = require('http');

let requests = 0;
let startTime = Date.now();

rateLimiter.initSharedMemory({ maxTokens: 50, windowMs: 1000 });

const server = http.createServer((req, res) => {
  const ip = req.connection.remoteAddress || '127.0.0.1';
  if (!rateLimiter.consumeTokenFast(ip)) {
    res.writeHead(429);
    res.end('Too Many Requests');
    return;
  }
  requests++;
  res.writeHead(200, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ status: 'ok' }));
});

server.listen(0, () => {
  const port = server.address().port;
  console.log(`Benchmark server running on port ${port}`);

  setTimeout(() => {
    server.close();
    const elapsed = (Date.now() - startTime) / 1000;
    const reqPerSec = requests / elapsed;
    console.log(`\nRequests: ${requests}`);
    console.log(`Elapsed: ${elapsed.toFixed(2)} s`);
    console.log(`Rate: ${reqPerSec.toFixed(2)} req/s`);
    process.exit(0);
  }, 3000);

  console.log('Running benchmark for 3 seconds...');
});