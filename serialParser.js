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

function parseBooleanValue(value) {
  if (value === undefined || value === null) return false;
  const normalized = String(value).trim().toLowerCase();
  if (!normalized) return false;
  if (['true', '1', 'yes', 'y', 'on'].includes(normalized)) return true;
  if (['false', '0', 'no', 'n', 'off'].includes(normalized)) return false;
  const parsedInt = Number.parseInt(normalized, 10);
  return !Number.isNaN(parsedInt) ? parsedInt !== 0 : false;
}

function parseStandardGpsRow(line) {
  const parts = parseCsvLine(String(line || '').trim());
  if (parts.length < 6) return null;

  const [timestampRaw, latitudeRaw, longitudeRaw, altitudeRaw, speedRaw, backlogRaw, voltageRaw, currentRaw] = parts;
  const latitude = parseNumber(latitudeRaw);
  const longitude = parseNumber(longitudeRaw);
  const altitude = parseNumber(altitudeRaw);
  const speed = parseNumber(speedRaw);
  const voltage = parseNumber(voltageRaw);
  const current = parseNumber(currentRaw);

  if ([latitude, longitude, altitude, speed].some((value) => value === null)) {
    return null;
  }

  const payload = {
    timestamp: timestampRaw || null,
    latitude,
    longitude,
    altitude,
    speed,
    isBacklog: parseBooleanValue(backlogRaw)
  };

  if (voltage !== null) {
    payload.voltage = voltage;
  }

  if (current !== null) {
    payload.current = current;
  }

  return payload;
}

function parseBacklogCsvRow(line) {
  const trimmed = String(line || '').trim();
  if (!trimmed) return null;

  const parts = parseCsvLine(trimmed);
  if (parts.length < 6) return null;

  const standardRow = parseStandardGpsRow(trimmed);
  if (standardRow) {
    return standardRow;
  }

  const [timestampRaw, sequenceRaw, latitudeRaw, longitudeRaw, altitudeRaw, speedRaw, voltageRaw, currentRaw] = parts;
  const sequence = Number.parseInt(sequenceRaw, 10);
  const latitude = parseNumber(latitudeRaw);
  const longitude = parseNumber(longitudeRaw);
  const altitude = parseNumber(altitudeRaw);
  const speed = parseNumber(speedRaw);
  const voltage = parseNumber(voltageRaw);
  const current = parseNumber(currentRaw);

  if ([latitude, longitude, altitude, speed].some((value) => value === null) || Number.isNaN(sequence)) {
    return null;
  }

  const payload = {
    timestamp: timestampRaw || '',
    latitude,
    longitude,
    altitude,
    speed,
    isBacklog: true
  };

  if (voltage !== null) {
    payload.voltage = voltage;
  }

  if (current !== null) {
    payload.current = current;
  }

  return payload;
}

function splitSerialLines(rawBuffer) {
  const text = String(rawBuffer ?? '');
  if (!text) {
    return { lines: [], remainder: '' };
  }

  const parts = text.split(/\r\n|\n|\r/);
  const remainder = parts.pop() || '';
  const lines = parts.filter((line) => String(line).trim() !== '');

  return { lines, remainder };
}

function sanitizeSerialDebugText(rawFrame) {
  const text = String(rawFrame ?? '').trim();
  if (!text) return '';

  if (parseGpsFrame(text)) {
    return '';
  }

  const normalizedText = text.replace(/^MSG\s*,\s*/i, '');
  const parts = normalizedText.split(',');
  if (parts.length > 1) {
    const firstCandidate = parts[0].trim();
    const looksLikeTimestamp = !Number.isNaN(Date.parse(firstCandidate));
    if (looksLikeTimestamp) {
      const payload = parts.slice(1).join(',').trim();
      return payload;
    }
  }

  return normalizedText;
}

function parseGpsFrame(rawFrame) {
  const line = String(rawFrame || '').trim();
  if (!line) return null;

  const standardRow = parseStandardGpsRow(line);
  if (standardRow) {
    return {
      ...standardRow,
      timestamp: standardRow.timestamp
    };
  }

  const parts = parseCsvLine(line);
  if (parts.length < 4) return null;

  const [latitudeRaw, longitudeRaw, altitudeRaw, speedRaw, backlogRaw, voltageRaw, currentRaw] = parts;
  const latitude = parseNumber(latitudeRaw);
  const longitude = parseNumber(longitudeRaw);
  const altitude = parseNumber(altitudeRaw);
  const speed = parseNumber(speedRaw);
  const voltage = parseNumber(voltageRaw);
  const current = parseNumber(currentRaw);

  if ([latitude, longitude, altitude, speed].some((value) => value === null)) {
    return null;
  }

  const payload = {
    latitude,
    longitude,
    altitude,
    speed,
    isBacklog: parseBooleanValue(backlogRaw),
    timestamp: null
  };

  if (voltage !== null) {
    payload.voltage = voltage;
  }

  if (current !== null) {
    payload.current = current;
  }

  return payload;
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
  parseBacklogCsvRow,
  parseGpsFrame,
  splitSerialLines,
  sanitizeSerialDebugText,
  formatSerialError
};
