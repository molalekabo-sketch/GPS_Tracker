const test = require('node:test');
const assert = require('node:assert/strict');
const { parseCsvLine, parseGpsFrame, formatSerialError } = require('../serialParser');

test('parseCsvLine splits a CSV row into values', () => {
  const row = '-29.117100,26.223600,1420.50,12.30,02';
  assert.deepStrictEqual(parseCsvLine(row), ['-29.117100', '26.223600', '1420.50', '12.30', '02']);
});

test('parseGpsFrame converts a serial CSV frame into a GPS packet', () => {
  const frame = '-29.117100,26.223600,1420.50,12.30,02';
  assert.deepStrictEqual(parseGpsFrame(frame), {
    latitude: -29.1171,
    longitude: 26.2236,
    altitude: 1420.5,
    speed: 12.3,
    isBacklog: true,
    timestamp: null
  });
});

test('formatSerialError returns a friendly message for access denied errors', () => {
  const result = formatSerialError({ message: 'Opening COM10: Access denied' }, 'COM10');
  assert.equal(result.status, 'error');
  assert.match(result.accessibleMessage, /busy or locked/i);
  assert.match(result.hint, /unplug/i);
});
