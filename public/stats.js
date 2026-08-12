(function (root, factory) {
  const api = factory();
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  }
  root.computeTrackStats = api.computeTrackStats;
}(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  function computeTrackStats(segment) {
    if (!segment || segment.length === 0) {
      return {
        totalDistanceKm: 0,
        elapsedSec: 0,
        maxSpeedKmh: null,
        pointCount: 0
      };
    }

    const normalized = segment.map((point) => ({
      lat: Number(point.lat ?? point.latitude),
      lon: Number(point.lon ?? point.longitude),
      speed: Number(point.speed),
      timestamp: point.timestamp
    }));

    let totalDistanceKm = 0;
    for (let i = 1; i < normalized.length; i += 1) {
      const prev = normalized[i - 1];
      const curr = normalized[i];
      if (!Number.isFinite(prev.lat) || !Number.isFinite(prev.lon) || !Number.isFinite(curr.lat) || !Number.isFinite(curr.lon)) {
        continue;
      }

      const latDiff = (curr.lat - prev.lat) * 111;
      const lonDiff = (curr.lon - prev.lon) * 111 * Math.cos(curr.lat * Math.PI / 180);
      totalDistanceKm += Math.sqrt(latDiff * latDiff + lonDiff * lonDiff);
    }

    const firstTs = normalized[0].timestamp;
    const lastTs = normalized[normalized.length - 1].timestamp;
    const firstMs = firstTs ? Date.parse(firstTs) : NaN;
    const lastMs = lastTs ? Date.parse(lastTs) : NaN;

    let elapsedSec = 0;
    if (Number.isFinite(firstMs) && Number.isFinite(lastMs) && lastMs >= firstMs) {
      elapsedSec = (lastMs - firstMs) / 1000;
    }

    let maxSpeedKmh = null;
    for (let i = 0; i < normalized.length; i += 1) {
      const speedMps = normalized[i].speed;
      if (Number.isFinite(speedMps) && speedMps >= 0) {
        const speedKmh = speedMps * 3.6;
        if (maxSpeedKmh === null || speedKmh > maxSpeedKmh) {
          maxSpeedKmh = speedKmh;
        }
      }
    }

    return {
      totalDistanceKm,
      elapsedSec,
      maxSpeedKmh,
      pointCount: normalized.length
    };
  }

  return { computeTrackStats };
}));
