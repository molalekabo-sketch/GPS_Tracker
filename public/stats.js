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

    let totalDistanceKm = 0;
    for (let i = 1; i < segment.length; i += 1) {
      const prev = segment[i - 1];
      const curr = segment[i];
      const latDiff = (curr.lat - prev.lat) * 111;
      const lonDiff = (curr.lon - prev.lon) * 111 * Math.cos(curr.lat * Math.PI / 180);
      totalDistanceKm += Math.sqrt(latDiff * latDiff + lonDiff * lonDiff);
    }

    const firstTs = segment[0].timestamp;
    const lastTs = segment[segment.length - 1].timestamp;
    const firstMs = firstTs ? Date.parse(firstTs) : NaN;
    const lastMs = lastTs ? Date.parse(lastTs) : NaN;

    let elapsedSec = 0;
    if (Number.isFinite(firstMs) && Number.isFinite(lastMs) && lastMs >= firstMs) {
      elapsedSec = (lastMs - firstMs) / 1000;
    }

    let maxSpeedKmh = null;
    for (let i = 0; i < segment.length; i += 1) {
      const speedMps = Number(segment[i].speed);
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
      pointCount: segment.length
    };
  }

  return { computeTrackStats };
}));
