// A new Ed25519 key pair for signing licenses: the private key goes in the server's .env, the public key into Vybrant
// (kProductKey in src/License.cpp). Licenses made with an old private key stop working once Vybrant ships the new public key.
import { generateKeyPairSync } from 'node:crypto';

const { privateKey, publicKey } = generateKeyPairSync('ed25519');
const raw = publicKey.export({ format: 'der', type: 'spki' }).subarray(-32);   // SPKI ends with the 32 raw key bytes
console.log(`LICENSE_PRIVATE_KEY=${privateKey.export({ format: 'der', type: 'pkcs8' }).toString('base64')}`);
console.log(`public key for src/License.cpp: ${raw.toString('hex')}`);
