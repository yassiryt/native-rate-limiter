process.env.RATE_LIMITER_SHM_NAME = `/node_rate_limiter_test_${process.pid}`;
const rateLimiter = require('./build/Release/rate_limiter.node');

let passed = 0;
let failed = 0;

function assert(condition, msg) {
  if (condition) {
    passed++;
  } else {
    failed++;
    console.error('FAIL:', msg);
  }
}

function assertEqual(a, b, msg) {
  if (a === b) {
    passed++;
  } else {
    failed++;
    console.error('FAIL:', msg, `expected ${b}, got ${a}`);
  }
}

// Setup
try {
  rateLimiter.initSharedMemory({ maxTokens: 10, windowMs: 1000 });
  console.log('initSharedMemory: OK');
} catch (e) {
  console.error('initSharedMemory failed:', e.message);
  process.exit(1);
}

// Test 1: Different IPs get separate buckets
const ip1Result = rateLimiter.consumeTokenFast('192.168.1.1');
const ip2Result = rateLimiter.consumeTokenFast('10.0.0.1');
assertEqual(ip1Result, true, 'IP 1 should consume token');
assertEqual(ip2Result, true, 'IP 2 should consume token');

// Invalid input is rejected without affecting the limiter.
assertEqual(rateLimiter.consumeTokenFast(null), false, 'Invalid IP should be rejected');

// Test 2: Exhausted IP returns false
let exhausted = true;
for (let i = 0; i < 9; i++) {
  rateLimiter.consumeTokenFast('192.168.1.1');
}
const resultAfterExhaust = rateLimiter.consumeTokenFast('192.168.1.1');
assertEqual(resultAfterExhaust, false, 'Exhausted IP should be rejected');

// Test 3: Refill after windowMs
setTimeout(() => {
  const resultAfterRefill = rateLimiter.consumeTokenFast('192.168.1.1');
  assertEqual(resultAfterRefill, true, 'IP should refill after window');

  // Test 4: Different IP still has its own tokens
  const otherIPResult = rateLimiter.consumeTokenFast('10.0.0.1');
  assertEqual(otherIPResult, true, 'Different IP should work independently');

  const stats = rateLimiter.getStats();
  assert(stats.totalRequests >= 13, 'Stats should report requests');
  assert(stats.acceptedRequests >= 12, 'Stats should report accepted requests');

  console.log(`\nResults: ${passed} passed, ${failed} failed`);
  rateLimiter.cleanup();
  process.exit(failed > 0 ? 1 : 0);
}, 1500);