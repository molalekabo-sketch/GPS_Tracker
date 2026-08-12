const test = require('node:test');
const assert = require('node:assert/strict');
const { parseCsvLine, parseGpsFrame, formatSerialError, parseBacklogCsvRow, splitSerialLines, sanitizeSerialDebugText } = require('../serialParser');

test('parseCsvLine splits a CSV row into values', () => {
  const row = '2026-07-31 09:25:00,-29.117100,26.223600,1420.50,12.30,true';
  assert.deepStrictEqual(parseCsvLine(row), ['2026-07-31 09:25:00', '-29.117100', '26.223600', '1420.50', '12.30', 'true']);
});

test('parseGpsFrame converts a serial CSV frame into a GPS packet', () => {
  const frame = '2026-07-31 09:25:00,-29.117100,26.223600,1420.50,12.30,true';
  assert.deepStrictEqual(parseGpsFrame(frame), {
    latitude: -29.1171,
    longitude: 26.2236,
    altitude: 1420.5,
    speed: 12.3,
    isBacklog: true,
    timestamp: '2026-07-31 09:25:00'
  });
});

test('parseGpsFrame preserves optional voltage and current fields', () => {
  const frame = '2026-07-31 09:25:00,-29.117100,26.223600,1420.50,12.30,true,13.2,2.4';
  assert.deepStrictEqual(parseGpsFrame(frame), {
    latitude: -29.1171,
    longitude: 26.2236,
    altitude: 1420.5,
    speed: 12.3,
    isBacklog: true,
    voltage: 13.2,
    current: 2.4,
    timestamp: '2026-07-31 09:25:00'
  });
});

test('parseBacklogCsvRow parses the standardized backlog format', () => {
  const row = '2026-07-31 09:25:00,-29.121841,26.213877,1423.40,0.00,true';
  assert.deepStrictEqual(parseBacklogCsvRow(row), {
    timestamp: '2026-07-31 09:25:00',
    latitude: -29.121841,
    longitude: 26.213877,
    altitude: 1423.4,
    speed: 0,
    isBacklog: true
  });
});

test('splitSerialLines handles CR, LF, and CRLF delimiters', () => {
  const firstChunk = splitSerialLines('2026-07-31 09:25:00,-29.117100,26.223600,1420.50,12.30,true\r');
  assert.deepStrictEqual(firstChunk.lines, ['2026-07-31 09:25:00,-29.117100,26.223600,1420.50,12.30,true']);
  assert.equal(firstChunk.remainder, '');

  const secondChunk = splitSerialLines('2026-07-31 09:26:00,-29.117200,26.223700,1421.50,12.40,true\n2026-07-31 09:27:00,-29.117300,26.223800,1422.50,12.50,true');
  assert.deepStrictEqual(secondChunk.lines, ['2026-07-31 09:26:00,-29.117200,26.223700,1421.50,12.40,true']);
  assert.equal(secondChunk.remainder, '2026-07-31 09:27:00,-29.117300,26.223800,1422.50,12.50,true');

  const thirdChunk = splitSerialLines(secondChunk.remainder + '\n');
  assert.deepStrictEqual(thirdChunk.lines, ['2026-07-31 09:27:00,-29.117300,26.223800,1422.50,12.50,true']);
  assert.equal(thirdChunk.remainder, '');
});

test('sanitizeSerialDebugText removes timestamps from non-telemetry serial messages', () => {
  assert.equal(sanitizeSerialDebugText('2026-07-31 09:25:00,-29.117100,26.223600,1420.50,12.30,true'), '');
  assert.equal(sanitizeSerialDebugText('2026-07-31 09:25:00,Waiting for GPS fix...'), 'Waiting for GPS fix...');
  assert.equal(sanitizeSerialDebugText('GPS fix acquired'), 'GPS fix acquired');
});

test('formatSerialError returns a friendly message for access denied errors', () => {
  const result = formatSerialError({ message: 'Opening COM10: Access denied' }, 'COM10');
  assert.equal(result.status, 'error');
  assert.match(result.accessibleMessage, /busy or locked/i);
  assert.match(result.hint, /unplug/i);
});
