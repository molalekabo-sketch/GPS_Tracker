const test = require('node:test');
const assert = require('node:assert/strict');
const { computeTrackStats } = require('../public/stats');

test('computeTrackStats reports the maximum speed reached', () => {
  const points = [
    { lat: 0, lon: 0, speed: 5, timestamp: '2026-08-05T00:00:00.000Z' },
    { lat: 0.01, lon: 0, speed: 20, timestamp: '2026-08-05T00:00:10.000Z' },
    { lat: 0.02, lon: 0, speed: 12, timestamp: '2026-08-05T00:00:20.000Z' }
  ];

  const result = computeTrackStats(points);
  assert.equal(result.maxSpeedKmh, 72);
});

test('computeTrackStats accumulates total distance across the full route', () => {
  const points = [
    { lat: 0, lon: 0, speed: 0, timestamp: '2026-08-05T00:00:00.000Z' },
    { lat: 0.01, lon: 0, speed: 5, timestamp: '2026-08-05T00:00:10.000Z' },
    { lat: 0.02, lon: 0, speed: 10, timestamp: '2026-08-05T00:00:20.000Z' }
  ];

  const result = computeTrackStats(points);
  assert.ok(result.totalDistanceKm > 0, 'distance should accumulate for each segment');
  assert.ok(Math.abs(result.totalDistanceKm - 2.22) < 0.2, 'route distance should be estimated in km');
  assert.equal(result.pointCount, 3);
});
