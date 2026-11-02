import { test } from 'node:test';
import assert from 'node:assert/strict';
import { countLicenses } from '../dist/stats.js';

test('licenses are counted across every page of accounts, and only accounts that hold one', async () => {
  const users = Array.from({ length: 1234 }, (_, i) => ({ privateMetadata: i % 3 === 0 ? { license: `lic-${i}` } : {} }));
  const asked = [];
  const page = async (offset) => (asked.push(offset), { data: users.slice(offset, offset + 500), totalCount: users.length });
  assert.equal(await countLicenses(page), 412);   // 0, 3, ... 1233
  assert.deepEqual(asked, [0, 500, 1000]);
  assert.equal(await countLicenses(async () => ({ data: [], totalCount: 0 })), 0);
});
