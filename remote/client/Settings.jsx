import React, {useEffect, useRef, useState} from 'react';
import {profile} from './protocol.mjs';

export function Settings({data, display, onSave, onClose, onExport, store}) {
  const dialog = useRef(null);
  const form = useRef(null);
  const file = useRef(null);
  const [hosts, setHosts] = useState(data.hosts);
  const [syncEnabled, setSync] = useState(data.syncEnabled);
  const [baseline, setBaseline] = useState(JSON.stringify([data.hosts, data.syncEnabled]));
  const [busy, setBusy] = useState(false);
  const [confirmClose, setConfirmClose] = useState(false);
  const [error, setError] = useState('');
  const [saved, setSaved] = useState(false);
  const [syncView, setSyncView] = useState(null);
  const [confirmClear, setConfirmClear] = useState(false);
  const dirty = JSON.stringify([hosts, syncEnabled]) !== baseline;
  useEffect(() => { dialog.current.showModal(); }, []);
  useEffect(() => {
    if (!dirty && !busy) return;
    const guard = event => { event.preventDefault(); event.returnValue = ''; };
    window.addEventListener('beforeunload', guard);
    return () => window.removeEventListener('beforeunload', guard);
  }, [dirty, busy]);
  const close = () => { if (!busy) { if (dirty) setConfirmClose(true); else onClose(); } };
  const save = async (exit = false) => {
    if (!form.current.reportValidity()) { setConfirmClose(false); return; }
    setBusy(true); setError('');
    try {
      const clean = await onSave(hosts.map(host => profile(host, 'pvthost')), syncEnabled);
      setHosts(clean); setBaseline(JSON.stringify([clean, syncEnabled])); setSaved(true); setConfirmClose(false);
      if (exit) onClose();
    } catch (reason) { setError(reason.message || String(reason)); }
    finally { setBusy(false); }
  };
  const importHost = async input => {
    if (!input) return;
    setBusy(true); setError('');
    try {
      if (input.size > 16384) throw Error('Pairing files must be smaller than 16 KiB.');
      const host = profile(JSON.parse(await input.text()), 'pvthost');
      const old = data.hosts.find(h => h.id === host.id) || hosts.find(h => h.id === host.id);
      if (old && (old.ed25519 !== host.ed25519 || old.x25519 !== host.x25519)) throw Error('This host identity has different keys. Remove the old host and save before pairing new keys.');
      if (!old && hosts.length >= 64) throw Error('At most 64 hosts can be paired.');
      setHosts(previous => [...previous.filter(h => h.id !== host.id), host]); setSaved(false);
    } catch (reason) { setError(reason.message || String(reason)); }
    finally { setBusy(false); }
  };
  return <dialog ref={dialog} className="settings-dialog" aria-labelledby="settings-title" onCancel={e => { e.preventDefault(); close(); }} onClick={e => { if (e.target === dialog.current) close(); }}>
    <div className="settings-heading"><div><div className="eyebrow">PVT-{display ? 'RD' : 'RC'}</div><h2 id="settings-title">Hosts & settings</h2></div><button disabled={busy} onClick={close} autoFocus>Done</button></div>
    <form ref={form} onSubmit={e => { e.preventDefault(); save(); }}><fieldset disabled={busy}>
      <div className="settings-body">
        <section className="pairing-guide"><h3>Pair with your PVT desktop</h3><ol><li><strong>Save this remote’s pairing file.</strong><p>Open it in PVT’s Settings → Networking & Remotes.</p></li><li><strong>Open PVT’s host pairing file here.</strong><p>Save changes below to connect automatically.</p></li></ol><div className="button-row"><button type="button" onClick={onExport}>Export .pvtremote</button><button type="button" onClick={() => file.current.click()}>Import .pvthost</button><input ref={file} type="file" accept=".pvthost,application/json" hidden onChange={e => { const input = e.target.files[0]; e.target.value = ''; importHost(input); }}/></div></section>
        <section className="paired-hosts"><h3>Paired hosts <span className="muted">{hosts.length}</span></h3><p>Give each desktop a name you recognize.</p>
          {!hosts.length && <p className="notice">No paired hosts yet. Import a host pairing file to get started.</p>}
          <ul className="hosts">{hosts.map(host => <li key={host.id}><div className="host-profile"><label>Host name<input required maxLength={120} value={host.label} onChange={e => { setHosts(hosts.map(h => h.id === host.id ? {...h, label: e.target.value} : h)); setSaved(false); }}/></label><details><summary>Pairing identity</summary><small>{host.id}</small><code>{host.ed25519}</code></details></div><button type="button" aria-label={`Remove ${host.label}`} onClick={() => { setHosts(hosts.filter(h => h.id !== host.id)); setSaved(false); }}>Remove</button></li>)}</ul>
        </section>
        <section className="sync"><h3>Browser sync</h3><p>Back up public host profiles with your browser account. Your private remote identity stays on this device.</p><label className="switch"><input type="checkbox" checked={syncEnabled} onChange={e => { setSync(e.target.checked); setSaved(false); }}/> Sync public host profiles</label><p className="muted">Turning sync off keeps your local profiles. After reinstalling, pair this remote with PVT again.</p>
          <details><summary>Manage account backup</summary><p>Backup actions take effect immediately. Save or discard any profile changes first.</p><div className="button-row"><button type="button" disabled={dirty} onClick={async () => { setError(''); try { const all = await store.api.storage.sync.get(null); setSyncView(Object.fromEntries(Object.entries(all).filter(([key]) => key.startsWith('pvt.host.')))); } catch (reason) { setError(reason.message); } }}>View synced data</button><button type="button" disabled={dirty} onClick={() => setConfirmClear(true)}>Clear synced data</button></div>{syncView && <pre tabIndex={0}>{JSON.stringify(syncView, null, 2)}</pre>}</details>
        </section>
      </div>
      <div className="settings-actions"><span role="status">{busy ? 'Saving…' : dirty ? 'Unsaved changes' : saved ? 'Changes saved' : 'All changes saved'}</span><button type="submit" className="primary" disabled={!dirty || busy}>Save changes</button></div>
    </fieldset></form>
    {error && <p className="error" role="alert">{error}</p>}
    {confirmClose && <Confirm title="Save your changes?" onCancel={() => setConfirmClose(false)}><p>Your host profiles and sync preference have unsaved changes.</p>{error && <p className="field-error" role="alert">{error}</p>}<div className="button-row"><button disabled={busy} className="primary" onClick={() => save(true)}>Save and close</button><button disabled={busy} onClick={onClose}>Discard changes</button><button disabled={busy} onClick={() => setConfirmClose(false)}>Keep editing</button></div></Confirm>}
    {confirmClear && <Confirm title="Clear account backup?" onCancel={() => { if (!busy) setConfirmClear(false); }}><p>This removes synced host profiles and turns off sync. Local pairings are kept.</p>{error && <p className="field-error" role="alert">{error}</p>}<div className="button-row"><button disabled={busy} onClick={async () => { setBusy(true); setError(''); try { await store.clearSync(); await onSave(hosts, false); setSync(false); setBaseline(JSON.stringify([hosts, false])); setSyncView({}); setConfirmClear(false); } catch (reason) { setError(reason.message); } finally { setBusy(false); } }}>Clear backup</button><button disabled={busy} onClick={() => setConfirmClear(false)}>Cancel</button></div></Confirm>}
  </dialog>;
}
function Confirm({title, children, onCancel}) {
  const ref = useRef(null);
  useEffect(() => { ref.current.showModal(); }, []);
  return <dialog className="confirm-dialog" ref={ref} aria-label={title} onCancel={e => { e.preventDefault(); e.stopPropagation(); onCancel(); }}><h2>{title}</h2>{children}</dialog>;
}
