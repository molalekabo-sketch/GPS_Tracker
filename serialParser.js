function parseCsvLine(line) {
  const out = [];
  let current = '';
  let inQuotes = false;

  for (let i = 0; i < line.length; i += 1) {
    const ch = line[i];
    if (ch === '"') {
      inQuotes = !inQuotes;
      continue;
    }
    if (ch === ',' && !inQuotes) {
      out.push(current);
      current = '';
      continue;
    }
    current += ch;
  }

  out.push(current);
  return out;
}

function parseNumber(value) {
  const parsed = parseFloat(value);
  return Number.isFinite(parsed) ? parsed : null;
}

function parseGpsFrame(rawFrame) {
  const line = String(rawFrame || '').trim();
  if (!line) return null;

  const parts = parseCsvLine(line);
  if (parts.length < 4) return null;

  const [latitudeRaw, longitudeRaw, altitudeRaw, speedRaw, backlogRaw] = parts;
  const latitude = parseNumber(latitudeRaw);
  const longitude = parseNumber(longitudeRaw);
  const altitude = parseNumber(altitudeRaw);
  const speed = parseNumber(speedRaw);

  if ([latitude, longitude, altitude, speed].some((value) => value === null)) {
    return null;
  }

  const backlogValue = backlogRaw === undefined ? '' : backlogRaw.trim();
  let isBacklog = false;
  if (backlogValue) {
    const normalized = backlogValue.toLowerCase();
    if (normalized === 'true' || normalized === '1' || normalized === 'yes' || normalized === 'y') {
      isBacklog = true;
    } else {
      const parsedInt = parseInt(backlogValue, 10);
      isBacklog = !Number.isNaN(parsedInt) ? parsedInt !== 0 : false;
    }
  }

  return {
    latitude,
    longitude,
    altitude,
    speed,
    isBacklog,
    timestamp: null
  };
}

function formatSerialError(error, portPath) {
  const detail = error && typeof error.message === 'string' ? error.message : String(error || 'Unknown serial error');
  const normalized = detail.toLowerCase();

  if (normalized.includes('access denied') || normalized.includes('permission denied')) {
    return {
      status: 'error',
      detail,
      accessibleMessage: `The port${portPath ? ` ${portPath}` : ''} is busy or locked by another app. Close any serial monitor, Arduino IDE, or other program using it, then try again.`,
      hint: 'If the device is still not usable, unplug and reconnect the USB cable and try again.'
    };
  }

  if (normalized.includes('already open') || normalized.includes('in use')) {
    return {
      status: 'error',
      detail,
      accessibleMessage: `The port${portPath ? ` ${portPath}` : ''} is already in use. Close the other connection and retry.`,
      hint: 'You can also select a different COM/TTY port if the board is attached elsewhere.'
    };
  }

  return {
    status: 'error',
    detail,
    accessibleMessage: detail,
    hint: 'Ensure the ESP32 is connected and the selected port is correct.'
  };
}

module.exports = {
  parseCsvLine,
  parseGpsFrame,
  formatSerialError
};
