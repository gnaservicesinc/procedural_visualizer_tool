export function clientInfo() {
  const runtime = globalThis.browser?.runtime || globalThis.chrome?.runtime;
  const ua = navigator.userAgent;
  const match = ua.match(/(Edg)\/([\d.]+)/) || ua.match(/(Firefox|Chrome|Version)\/([\d.]+)/);
  const name = {Edg: 'Edge', Firefox: 'Firefox', Chrome: 'Chrome', Version: 'Safari'};
  return {browser: match ? `${name[match[1]]} ${match[2]}` : ua || 'Browser not reported',
    platform: navigator.userAgentData?.platform || (/Mac/.test(navigator.platform) ? 'macOS' : /Win/.test(navigator.platform) ? 'Windows' : navigator.platform) || 'System not reported',
    version: runtime?.getManifest?.().version || 'unknown',
    features: ['remote-media-v1', 'remote-disconnect-v1', 'remote-reconnect-v1']};
}

import {Cipher, unb64} from './protocol.mjs';
export class Connection {
  constructor(identity, host, {onStatus, onStream, onControl, iceServers = []} = {}) {
    this.identity = identity; this.host = host; this.cipher = new Cipher(identity);
    this.onStatus = onStatus || (() => {}); this.onStream = onStream || (() => {});
    this.onControl = onControl || (() => {});
    this.iceServers = iceServers; this.pending = new Map(); this.chunks = new Map(); this.closed = false;
    this.active = true; this.ready = false; this.relay = false;
    this.responseTimeouts = 0;
    this.session = crypto.randomUUID(); this.stream = new MediaStream();
  }
  async connect(active = this.active) {
    this.active = active;
    const first = 49152 + parseInt(this.host.id.replaceAll('-', '').slice(0, 8), 16) % 16000;
    const automatic = [0, 4093, 8191, 12289].map(offset => 49152 + (first - 49152 + offset) % 16000).flatMap(port =>
      [`ws://127.0.0.1:${port}`, `ws://pvt-${this.host.id}.local:${port}`]);
    const choices = [...new Set([...this.host.endpoints, ...automatic])].map(url => ({url, relay: false}));
    if (this.host.signaling_url) choices.push({url: this.host.signaling_url, relay: true});
    while (!this.closed) {
      for (const choice of choices) {
        if (this.closed) return;
        try { await this.attempt(choice); this.retryDelay = 1000; return; }
        catch { this.cleanup(); }
      }
      if (this.closed) return;
      this.onStatus(this.active ? 'Waiting for PVT · reconnecting automatically' : 'Disconnected');
      await new Promise(resolve => {
        const finish = () => { clearTimeout(this.retryTimer); globalThis.removeEventListener?.('online', finish); this.cancelRetry = null; resolve(); };
        this.cancelRetry = finish;
        globalThis.addEventListener?.('online', finish, {once: true});
        this.retryTimer = setTimeout(finish, this.retryDelay || 1000);
      });
      this.retryDelay = Math.min((this.retryDelay || 1000) * 2, 10000);
    }
  }
  reconnect() {
    if (this.closed) return;
    if (this.reconnecting) { this.retryRequested = true; return; }
    this.reconnecting = true;
    this.cleanup();
    this.onStatus(this.active ? 'Waiting for PVT · reconnecting automatically' : 'Disconnected');
    // Let the previous event finish before opening a replacement connection.
    Promise.resolve().then(() => this.connect()).finally(() => { this.reconnecting = false; if (this.retryRequested) { this.retryRequested = false; this.reconnect(); } });
  }
  async attempt({url, relay}) {
    this.onStatus(this.active ? 'Connecting to PVT…' : 'Disconnected');
    this.session = crypto.randomUUID();
    const ws = new WebSocket(url); this.ws = ws;
    this.relay = relay;
    this.local = !relay && ['127.0.0.1', '[::1]', 'localhost'].includes(new URL(url).hostname);
    this.authenticated = false;
    const pc = new RTCPeerConnection({iceServers: this.iceServers}); this.pc = pc;
    this.channel = pc.createDataChannel('pvt-control', {ordered: true});
    this.channel.onmessage = event => { try {
      const message = JSON.parse(event.data);
      if (message.op === 'remote_control') this.onControl(message);
      else this.result(message);
    } catch { this.reconnect(); } };
    if (this.identity.public.role === 'display') {
      pc.addTransceiver('video', {direction: 'recvonly'});
      pc.addTransceiver('audio', {direction: 'recvonly'});
      pc.ontrack = event => { this.stream.addTrack(event.track); this.onStream(this.stream); };
    }
    await new Promise((resolve, reject) => {
      let connected = false; let settled = false;
      let timer = setTimeout(() => reject(Error('Waiting for PVT')), 2500);
      this.cancelAttempt = () => { clearTimeout(timer); reject(Error('Connection cancelled')); };
      const fail = reason => { if (settled) return; clearTimeout(timer); reject(reason); };
      const success = (media = true) => {
        if (settled) return;
        settled = true; connected = media; this.ready = true;
        clearTimeout(timer); this.cancelAttempt = null;
        this.onStatus(media ? 'Connected' : 'Disconnected');
        if (!media) this.startPresence();
        resolve();
      };
      pc.onconnectionstatechange = () => {
        if (pc.connectionState === 'connected' && this.channel.readyState === 'open') success();
        clearTimeout(this.disconnectedTimer);
        if (pc.connectionState === 'disconnected') {
          // ICE can recover short route changes without tearing down media.
          this.disconnectedTimer = setTimeout(() => {
            if (this.pc === pc && pc.connectionState === 'disconnected') {
              if (connected && this.active) this.reconnect(); else fail(Error('WebRTC connection lost'));
            }
          }, 8000);
        }
        if (['failed', 'closed'].includes(pc.connectionState)) {
          if (!connected) fail(Error('WebRTC connection failed'));
          else if (this.active) { this.reconnect(); }
        }
      };
      this.channel.onopen = success;
      this.channel.onclose = () => { if (connected && this.active) this.reconnect(); else fail(Error('Waiting for PVT')); };
      ws.onerror = () => { if (!connected) fail(Error('Waiting for PVT')); };
      ws.onclose = () => {
        this.authenticated = false; this.registered = false; this.ready = false;
        if (this.closed) return;
        if (settled) this.reconnect();
        else fail(Error('Host rejected the connection; check mutual pairing'));
      };
      // Serialize signaling to protect ordering and avoid duplicate offer work.
      let messages = Promise.resolve();
      ws.onmessage = event => {
        messages = messages.then(async () => {
          if (this.closed || this.ws !== ws) return;
          const message = JSON.parse(event.data);
          if (message.challenge) {
            clearTimeout(timer);
            timer = setTimeout(() => reject(Error('Waiting for PVT')), 35000);
            if (relay) {
              ws.send(JSON.stringify({op: 'register', id: this.identity.public.id, key: this.identity.public.ed25519,
                signature: await this.cipher.sign(['pvt-relay-v1', this.identity.public.id, message.challenge])}));
              this.registered = true;
              if (this.active) await this.offer(ws, pc);
              else { await this.sendPresence(false); success(false); pc.close(); this.pc = null; }
            } else {
              this.challenge = message.challenge;
              ws.send(JSON.stringify(await this.cipher.seal(this.host, {op: 'hello', challenge: message.challenge, client: clientInfo()})));
            }
          } else if (message.op === 'result' && this.local && this.authenticated) {
            this.result(message);
          } else {
            const payload = await this.cipher.open(this.host, message);
            if (this.closed || this.ws !== ws) return;
            if (payload.op === 'hello' && payload.challenge === this.challenge && !this.authenticated) {
              this.authenticated = true;
              if (this.active) await this.offer(ws, pc);
              else { await this.sendPresence(false); success(false); pc.close(); this.pc = null; }
            } else if (payload.op === 'answer' && payload.session === this.session) {
              await pc.setRemoteDescription({type: 'answer', sdp: payload.sdp});
            } else if (payload.op === 'remote_control') {
              this.onControl(payload);
            } else throw Error('Unexpected signaling reply');
          }
        }).catch(reason => { if (settled) { this.reconnect(); } else fail(reason); });
      };
    });
  }
  async sendPresence(connected = this.active) {
    if (this.ws?.readyState !== WebSocket.OPEN || (!this.authenticated && !this.registered)) return false;
    this.ws.send(JSON.stringify(await this.cipher.seal(this.host,
      {op: 'presence', connected, client: clientInfo()})));
    return true;
  }
  startPresence() {
    clearInterval(this.presenceTimer);
    this.presenceTimer = setInterval(() => this.sendPresence(false).catch(() => this.reconnect()), 15000);
  }
  async standby() {
    this.active = false;
    await this.sendPresence(false).catch(() => false);
    this.cleanupMedia();
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) this.reconnect();
    else this.startPresence();
    this.onStatus('Disconnected');
  }
  resume() {
    if (this.active && this.channel?.readyState === 'open') return;
    this.active = true; clearInterval(this.presenceTimer); this.presenceTimer = null;
    this.reconnect();
  }
  async offer(ws, pc) {
    await pc.setLocalDescription(await pc.createOffer());
    if (pc.iceGatheringState !== 'complete') await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(Error('ICE gathering timed out')), 15000);
      pc.onicegatheringstatechange = () => { if (pc.iceGatheringState === 'complete') { clearTimeout(timer); resolve(); } };
    });
    if (this.closed || this.ws !== ws) return;
    ws.send(JSON.stringify(await this.cipher.seal(this.host, {op: 'offer', client: clientInfo(), session: this.session, sdp: pc.localDescription.sdp})));
  }
  command(action, fields = {}) {
    const message = {op: 'command', id: crypto.randomUUID(), action, ...fields};
    if (this.pending.size >= 16) return Promise.reject(Error('Too many pending controls'));
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(message.id); this.chunks.delete(message.id); reject(Error('PVT did not respond')); if (++this.responseTimeouts >= 2) this.reconnect(); }, 6500);
      this.pending.set(message.id, {resolve, reject, timer});
      try {
        const raw = JSON.stringify(message);
        if (this.local && this.authenticated && this.ws?.readyState === WebSocket.OPEN) this.ws.send(raw);
        else if (this.channel?.readyState === 'open' && this.channel.bufferedAmount < 65536) this.channel.send(raw);
        else throw Error('Not connected or connection is busy');
      } catch (error) { clearTimeout(timer); this.pending.delete(message.id); reject(error); }
    });
  }
  result(message) {
    if (message.op === 'chunk') {
      if (!this.pending.has(message.id)) return;
      if (!Number.isInteger(message.count) || message.count < 1 || message.count > 256 || !Number.isInteger(message.index)) throw Error('Invalid reply fragments');
      let transfer = this.chunks.get(message.id);
      if (!transfer) { transfer = {count:message.count,parts:[],length:0}; this.chunks.set(message.id, transfer); }
      if (transfer.count !== message.count || message.index !== transfer.parts.length) throw Error('Out-of-order reply fragments');
      const part = unb64(message.data);
      if (part.length > 16384 || transfer.length + part.length > 4194304) throw Error('Reply exceeds transfer limit');
      transfer.parts.push(part); transfer.length += part.length;
      if (transfer.parts.length === transfer.count) {
        this.chunks.delete(message.id);
        const bytes = new Uint8Array(transfer.length); let offset=0;
        for (const part of transfer.parts) {bytes.set(part,offset);offset+=part.length;}
        const result = JSON.parse(new TextDecoder().decode(bytes));
        if (result.op !== 'result' || result.id !== message.id) throw Error('Fragment identifier mismatch');
        this.result(result);
      }
      return;
    }
    const request = this.pending.get(message.id);
    if (!request) return;
    this.responseTimeouts = 0;
    this.pending.delete(message.id); clearTimeout(request.timer);
    if (message.ok) request.resolve(message); else request.reject(Error(message.error || 'Command rejected'));
  }
  cleanupMedia() {
    clearTimeout(this.disconnectedTimer);
    this.responseTimeouts = 0;
    this.cancelAttempt?.(); this.cancelAttempt = null;
    if (this.channel) this.channel.onclose = this.channel.onmessage = this.channel.onopen = null;
    if (this.pc) { this.pc.onconnectionstatechange = null; this.pc.close(); }
    this.channel = this.pc = null;
    for (const request of this.pending.values()) { clearTimeout(request.timer); request.reject(Error('Disconnected')); }
    this.pending.clear(); this.chunks.clear();
    for (const track of this.stream.getTracks()) track.stop();
    this.stream = new MediaStream(); this.onStream(null);
  }
  cleanup() {
    clearInterval(this.presenceTimer); this.presenceTimer = null;
    this.cleanupMedia();
    if (this.ws) { this.ws.onclose = this.ws.onerror = this.ws.onmessage = null; this.ws.close(); }
    this.ws = null; this.authenticated = false; this.registered = false; this.ready = false;
  }
  disconnect() { this.closed = true; this.cancelRetry?.(); this.cleanup(); this.onStatus('Disconnected'); }
}
