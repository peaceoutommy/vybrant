import { createPrivateKey, KeyObject, sign } from 'node:crypto';

export interface LicensePayload {
  v: 1;
  sub: string;   // Clerk user id
  name: string;
  email: string;
}

export function loadPrivateKey(base64Der: string): KeyObject {
  return createPrivateKey({ key: Buffer.from(base64Der, 'base64'), format: 'der', type: 'pkcs8' });
}

// "<payload>.<signature>", both base64url: Ed25519 over the payload's JSON bytes. Vybrant checks it offline with the
// matching public key (src/License.cpp), so the format here and there must stay the same.
export function signLicense(payload: LicensePayload, key: KeyObject): string {
  const body = Buffer.from(JSON.stringify(payload));
  return `${body.toString('base64url')}.${sign(null, body, key).toString('base64url')}`;
}
