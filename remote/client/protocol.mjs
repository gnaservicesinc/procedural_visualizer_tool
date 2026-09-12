// Shared protocol source. Vendored into each standalone extension by
// scripts/sync-remote-clients.py; never maintain separate protocol variants.
const encoder = new TextEncoder();
const decoder = new TextDecoder();
export const compact = value => encoder.encode(JSON.stringify(value));
export const b64 = bytes => { let text = ''; for (const b of new Uint8Array(bytes)) text += String.fromCharCode(b); return btoa(text); };
export const unb64 = (text, length) => {
  if (typeof text !== 'string' || text.length > 1048576 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(text)) throw Error('Invalid encoded field');
  const bytes = Uint8Array.from(atob(text), c => c.charCodeAt(0));
  if (length !== undefined && bytes.length !== length) throw Error('Invalid key length');
  return bytes;
};
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
export function endpoint(text, relay = false) {
  if (typeof text !== 'string' || text.length > 2048) throw Error('Invalid endpoint');
  const url = new URL(text);
  if (!['ws:', 'wss:'].includes(url.protocol) || url.username || url.password || url.hash) throw Error('Expected a WebSocket URL without credentials');
  if (relay && url.protocol !== 'wss:' && !['localhost', '127.0.0.1', '[::1]'].includes(url.hostname)) throw Error('Public signaling requires wss://');
  return text;
}
export function profile(value, expected) {
  if (!value || value.version !== 1 || !['pvthost', 'pvtremote'].includes(value.type) || (expected && value.type !== expected)) throw Error('Unsupported pairing file');
  if (!uuid.test(value.id) || typeof value.label !== 'string' || !value.label.trim() || value.label.length > 120) throw Error('Invalid identity or name');
  unb64(value.ed25519, 32); unb64(value.x25519, 32);
  const clean = Object.fromEntries(['version', 'type', 'id', 'label', 'ed25519', 'x25519'].map(key => [key, value[key]]));
  if (value.type === 'pvtremote') {
    if (!['control', 'display'].includes(value.role)) throw Error('Invalid remote role');
    clean.role = value.role;
  } else {
    if (!Array.isArray(value.endpoints) || value.endpoints.length > 16) throw Error('Invalid endpoints');
    clean.endpoints = value.endpoints.map(url => endpoint(url));
    clean.signaling_url = value.signaling_url ? endpoint(value.signaling_url, true) : '';
  }
  return clean;
}
export async function newIdentity(role) {
  const ed = await crypto.subtle.generateKey('Ed25519', true, ['sign', 'verify']);
  const x = await crypto.subtle.generateKey('X25519', true, ['deriveBits']);
  return {
    public: {version: 1, type: 'pvtremote', id: crypto.randomUUID(), label: role === 'control' ? 'PVT Remote Control' : 'PVT Remote Display', role,
      ed25519: b64(await crypto.subtle.exportKey('raw', ed.publicKey)), x25519: b64(await crypto.subtle.exportKey('raw', x.publicKey))},
    ed_private: b64(await crypto.subtle.exportKey('pkcs8', ed.privateKey)), x_private: b64(await crypto.subtle.exportKey('pkcs8', x.privateKey)),
  };
}
export class Cipher {
  constructor(identity) { this.identity = identity; this.public = profile(identity.public); this.seen = new Map(); }
  async signingKey() { return crypto.subtle.importKey('pkcs8', unb64(this.identity.ed_private), 'Ed25519', false, ['sign']); }
  async sign(value) { return b64(await crypto.subtle.sign('Ed25519', await this.signingKey(), compact(value))); }
  async key(peer, from, to) {
    const own = await crypto.subtle.importKey('pkcs8', unb64(this.identity.x_private), 'X25519', false, ['deriveBits']);
    const other = await crypto.subtle.importKey('raw', unb64(peer.x25519, 32), 'X25519', false, []);
    const bits = await crypto.subtle.deriveBits({name: 'X25519', public: other}, own, 256);
    const base = await crypto.subtle.importKey('raw', bits, 'HKDF', false, ['deriveKey']);
    return crypto.subtle.deriveKey({name: 'HKDF', hash: 'SHA-256', salt: new Uint8Array(32), info: encoder.encode(`pvt-remotes/v1/${from}/${to}`)}, base, {name: 'AES-GCM', length: 256}, false, ['encrypt', 'decrypt']);
  }
  header(e) { return [1, e.from, e.to, e.id, e.ts, e.nonce]; }
  async seal(peer, payload) {
    const e = {version: 1, from: this.public.id, to: peer.id, id: crypto.randomUUID(), ts: Math.floor(Date.now() / 1000), nonce: b64(crypto.getRandomValues(new Uint8Array(12)))};
    e.ct = b64(await crypto.subtle.encrypt({name: 'AES-GCM', iv: unb64(e.nonce, 12), additionalData: compact(this.header(e))}, await this.key(peer, e.from, e.to), compact(payload)));
    e.sig = await this.sign([...this.header(e), e.ct]);
    return e;
  }
  async open(peer, e) {
    if (!e || JSON.stringify(e).length > 1048576 || e.version !== 1 || e.from !== peer.id || e.to !== this.public.id) throw Error('Wrong envelope identity');
    const now = Date.now() / 1000;
    if (!Number.isInteger(e.ts) || Math.abs(now - e.ts) > 120 || !uuid.test(e.id)) throw Error('Expired message; check both device clocks');
    for (const [id, time] of this.seen) if (time < now - 240) this.seen.delete(id);
    const token = `${e.from}/${e.id}`;
    if (this.seen.has(token) || this.seen.size >= 8192) throw Error('Replayed message or signaling rate limit');
    // Reserve before asynchronous verification so concurrent duplicates cannot race.
    this.seen.set(token, now);
    try {
      const verify = await crypto.subtle.importKey('raw', unb64(peer.ed25519, 32), 'Ed25519', false, ['verify']);
      if (!await crypto.subtle.verify('Ed25519', verify, unb64(e.sig, 64), compact([...this.header(e), e.ct]))) throw Error('Identity signature did not match');
      const clear = await crypto.subtle.decrypt({name: 'AES-GCM', iv: unb64(e.nonce, 12), additionalData: compact(this.header(e))}, await this.key(peer, e.from, e.to), unb64(e.ct));
      const payload = JSON.parse(decoder.decode(clear));
      if (!payload || typeof payload !== 'object' || Array.isArray(payload)) throw Error('Invalid payload');
      return payload;
    } catch (error) { this.seen.delete(token); throw error; }
  }
}
