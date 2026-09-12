import {Cipher, unb64} from './protocol.mjs';
export class Connection {
  constructor(identity, host, {onStatus, onStream, iceServers = []} = {}) {
    this.identity = identity; this.host = host; this.cipher = new Cipher(identity);
    this.onStatus = onStatus || (() => {}); this.onStream = onStream || (() => {});
    this.iceServers = iceServers; this.pending = new Map(); this.chunks = new Map(); this.closed = false;
    this.session = crypto.randomUUID(); this.stream = new MediaStream();
  }
  async connect() {
    const choices = [...this.host.endpoints.map(url => ({url, relay: false})), ...(this.host.signaling_url ? [{url: this.host.signaling_url, relay: true}] : [])];
    let error = Error('This host has no connection endpoints. Export it again from PVT.');
    for (const choice of choices) {
      if (this.closed) throw Error('Connection cancelled');
      try { await this.attempt(choice); return; }
      catch (reason) { error = reason; this.cleanup(); }
    }
    throw error;
  }
  async attempt({url, relay}) {
    this.onStatus(`Connecting to ${new URL(url).hostname}…`);
    const ws = new WebSocket(url); this.ws = ws;
    this.local = !relay && ['127.0.0.1', '[::1]', 'localhost'].includes(new URL(url).hostname);
    this.authenticated = false;
    const pc = new RTCPeerConnection({iceServers: this.iceServers}); this.pc = pc;
    this.channel = pc.createDataChannel('pvt-control', {ordered: true});
    this.channel.onmessage = event => { try { this.result(JSON.parse(event.data)); } catch { this.disconnect(); } };
    if (this.identity.public.role === 'display') {
      pc.addTransceiver('video', {direction: 'recvonly'});
      pc.addTransceiver('audio', {direction: 'recvonly'});
      pc.ontrack = event => { this.stream.addTrack(event.track); this.onStream(this.stream); };
    }
    await new Promise((resolve, reject) => {
      let connected = false;
      const timer = setTimeout(() => reject(Error('Connection timed out. Check pairing, networking and ICE servers.')), relay ? 35000 : 12000);
      this.cancelAttempt = () => { clearTimeout(timer); reject(Error('Connection cancelled')); };
      const fail = reason => { clearTimeout(timer); reject(reason); };
      const success = () => {
        if (connected) return;
        connected = true; clearTimeout(timer); this.cancelAttempt = null;
        this.onStatus(this.local ? 'Connected · local control + WebRTC' : 'Connected · WebRTC'); resolve();
      };
      pc.onconnectionstatechange = () => {
        if (pc.connectionState === 'connected' && this.channel.readyState === 'open') success();
        if (['failed', 'closed', 'disconnected'].includes(pc.connectionState)) {
          this.onStatus('Disconnected');
          if (!connected) fail(Error('WebRTC connection failed'));
          else { this.cleanup(); }
        }
      };
      this.channel.onopen = success;
      ws.onerror = () => fail(Error(`Cannot reach ${new URL(url).hostname}`));
      ws.onclose = () => { this.authenticated = false; if (!connected) fail(Error('Host rejected the connection; check mutual pairing')); };
      // Serialize signaling to protect ordering and avoid duplicate offer work.
      let messages = Promise.resolve();
      ws.onmessage = event => {
        messages = messages.then(async () => {
          if (this.closed || this.ws !== ws) return;
          const message = JSON.parse(event.data);
          if (message.challenge) {
            if (relay) {
              ws.send(JSON.stringify({op: 'register', id: this.identity.public.id, key: this.identity.public.ed25519,
                signature: await this.cipher.sign(['pvt-relay-v1', this.identity.public.id, message.challenge])}));
              await this.offer(ws, pc);
            } else {
              this.challenge = message.challenge;
              ws.send(JSON.stringify(await this.cipher.seal(this.host, {op: 'hello', challenge: message.challenge})));
            }
          } else if (message.op === 'result' && this.local && this.authenticated) {
            this.result(message);
          } else {
            const payload = await this.cipher.open(this.host, message);
            if (payload.op === 'hello' && payload.challenge === this.challenge && !this.authenticated) {
              this.authenticated = true;
              await this.offer(ws, pc);
            } else if (payload.op === 'answer' && payload.session === this.session) {
              await pc.setRemoteDescription({type: 'answer', sdp: payload.sdp});
            } else throw Error('Unexpected signaling reply');
          }
        }).catch(reason => { if (connected) { this.cleanup(); this.onStatus('Disconnected'); } else fail(reason); });
      };
    });
  }
  async offer(ws, pc) {
    await pc.setLocalDescription(await pc.createOffer());
    if (pc.iceGatheringState !== 'complete') await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(Error('ICE gathering timed out')), 15000);
      pc.onicegatheringstatechange = () => { if (pc.iceGatheringState === 'complete') { clearTimeout(timer); resolve(); } };
    });
    if (this.closed || this.ws !== ws) return;
    ws.send(JSON.stringify(await this.cipher.seal(this.host, {op: 'offer', session: this.session, sdp: pc.localDescription.sdp})));
  }
  command(action, fields = {}) {
    const message = {op: 'command', id: crypto.randomUUID(), action, ...fields};
    if (this.pending.size >= 16) return Promise.reject(Error('Too many pending controls'));
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(message.id); this.chunks.delete(message.id); reject(Error('PVT did not respond')); }, 6500);
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
    this.pending.delete(message.id); clearTimeout(request.timer);
    if (message.ok) request.resolve(message); else request.reject(Error(message.error || 'Command rejected'));
  }
  cleanup() {
    this.cancelAttempt?.(); this.cancelAttempt = null;
    if (this.ws) { this.ws.onclose = this.ws.onerror = this.ws.onmessage = null; this.ws.close(); }
    if (this.pc) { this.pc.onconnectionstatechange = null; this.pc.close(); }
    this.channel = this.ws = this.pc = null; this.authenticated = false;
    for (const request of this.pending.values()) { clearTimeout(request.timer); request.reject(Error('Disconnected')); }
    this.pending.clear(); this.chunks.clear();
    for (const track of this.stream.getTracks()) track.stop();
    this.stream = new MediaStream(); this.onStream(null);
  }
  disconnect() { this.closed = true; this.cleanup(); this.onStatus('Disconnected'); }
}
