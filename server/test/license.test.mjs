import { test } from 'node:test';
import assert from 'node:assert/strict';
import { generateKeyPairSync, verify } from 'node:crypto';
import { signLicense } from '../dist/license.js';

test('a license is the payload and its Ed25519 signature, both base64url', () => {
  const { privateKey, publicKey } = generateKeyPairSync('ed25519');
  const payload = { v: 1, sub: 'user_1', name: 'Ada Lovelace', email: 'ada@example.com' };
  const [body, sig] = signLicense(payload, privateKey).split('.');
  assert.deepEqual(JSON.parse(Buffer.from(body, 'base64url').toString()), payload);
  assert.ok(verify(null, Buffer.from(body, 'base64url'), publicKey, Buffer.from(sig, 'base64url')));
  const forged = Buffer.from(JSON.stringify({ ...payload, email: 'eve@example.com' }));
  assert.ok(!verify(null, forged, publicKey, Buffer.from(sig, 'base64url')));
});
