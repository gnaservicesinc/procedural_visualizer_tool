import {newIdentity, profile} from './protocol.mjs';
const prefix = 'pvt.host.';
export class ProfileStore {
  constructor(api, role) { this.api = api; this.role = role; }
  async load() {
    const saved = await this.api.storage.local.get(['identity', 'hosts', 'syncEnabled', 'selected', 'paused']);
    if (!saved.identity) {
      saved.identity = await newIdentity(this.role);
      await this.api.storage.local.set({identity: saved.identity});
    }
    if (saved.identity.public.role !== this.role) throw Error('Extension identity role mismatch');
    let hosts = (saved.hosts || []).map(item => profile(item, 'pvthost'));
    // Reinstallation restores a previous sync opt-in; explicit local-only
    // devices never read host data from sync.
    const restoredPreference = saved.syncEnabled === undefined
      ? (await this.api.storage.sync.get(['pvt.settings']))['pvt.settings']?.enabled === true : false;
    const syncEnabled = saved.syncEnabled === true || restoredPreference;
    if (syncEnabled) {
      const synced = await this.api.storage.sync.get(null);
      const byId = new Map(hosts.map(p => [p.id, p]));
      for (const [key, value] of Object.entries(synced)) if (key.startsWith(prefix)) {
        const host = profile(value, 'pvthost');
        const existing = byId.get(host.id);
        if (existing && (existing.ed25519 !== host.ed25519 || existing.x25519 !== host.x25519)) throw Error('Synced host key changed. Remove and pair it again explicitly.');
        byId.set(host.id, host);
      }
      hosts = [...byId.values()];
      if (hosts.length > 64) throw Error('Host limit reached');
      await this.api.storage.local.set({hosts, syncEnabled});
    }
    return {...saved, hosts, syncEnabled};
  }
  async saveHosts(hosts, syncEnabled) {
    if (hosts.length > 64) throw Error('At most 64 hosts can be paired');
    const clean = hosts.map(host => profile(host, 'pvthost'));
    if (new Set(clean.map(p => p.id)).size !== clean.length) throw Error('Duplicate host identity');
    if (syncEnabled) {
      const current = await this.api.storage.sync.get(null);
      const records = Object.fromEntries(clean.map(p => [prefix + p.id, p]));
      if (Object.entries(records).some(([key, value]) => new TextEncoder().encode(key + JSON.stringify(value)).length > 7500)) throw Error('A host exceeds the sync item limit; use local-only storage');
      if (new TextEncoder().encode(JSON.stringify(records)).length > 95000) throw Error('Profiles exceed browser sync quota; use local-only storage');
      await this.api.storage.sync.set({...records, 'pvt.settings': {enabled:true}});
      await this.api.storage.sync.remove(Object.keys(current).filter(key => key.startsWith(prefix) && !(key in records)));
    }
    await this.api.storage.local.set({hosts: clean, syncEnabled});
  }
  async disableSync() {
    await this.api.storage.local.set({syncEnabled: false});
    await this.api.storage.sync.set({'pvt.settings': {enabled:false}});
  }
  async clearSync() {
    // Disable first: failed quota/network operations must not resume syncing.
    await this.api.storage.local.set({syncEnabled: false});
    const current = await this.api.storage.sync.get(null);
    await this.api.storage.sync.remove(Object.keys(current).filter(key => key.startsWith(prefix) || key === 'pvt.settings'));
  }
}
