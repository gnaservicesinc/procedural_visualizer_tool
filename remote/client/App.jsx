import React, {useEffect, useRef, useState} from 'react';
import browser from 'webextension-polyfill';
import {ProfileStore} from './storage.mjs';
import {profile} from './protocol.mjs';
import {Connection} from './transport.mjs';
import './style.css';

export function App({role}) {
  const display = role === 'display';
  const store = useRef(new ProfileStore(browser, role));
  const connection = useRef(null);
  const video = useRef(null);
  const file = useRef(null);
  const [data, setData] = useState(null);
  const [selected, setSelected] = useState('');
  const [status, setStatus] = useState('Disconnected');
  const [error, setError] = useState('');
  const [connected, setConnected] = useState(false);
  const [busy, setBusy] = useState(false);
  const [hostState, setHostState] = useState(null);
  const [activeController, setActiveController] = useState('');
  const [search, setSearch] = useState('');
  const [section, setSection] = useState('');
  const [settings, setSettings] = useState(false);
  const [edit, setEdit] = useState(null);
  const [syncView, setSyncView] = useState(null);
  const [muted, setMuted] = useState(true);
  const selectedHost = data?.hosts.find(host => host.id === selected);
  const controlling = connected && activeController === data?.identity.public.id;
  const run = async action => { setError(''); try { return await action(); } catch (reason) { setError(reason.message || String(reason)); } };
  const refresh = async (current = connection.current) => {
    if (!current) return;
    const response = await current.command('state');
    if (connection.current !== current) return;
    setHostState(response.state); setActiveController(response.active_control);
  };
  useEffect(() => {
    let cancelled = false;
    store.current.load().then(saved => {
      if (cancelled) return;
      setData(saved); setSelected(saved.selected || saved.hosts[0]?.id || '');
    }).catch(reason => setError(`Cannot initialize secure identity: ${reason.message}. Use a browser with Ed25519 and X25519 support.`));
    return () => { cancelled = true; connection.current?.disconnect(); };
  }, []);
  useEffect(() => {
    if (!connected) return;
    let pending = false;
    const timer = setInterval(async () => {
      if (pending) return;
      pending = true;
      try { await refresh(); } catch (reason) { setError(reason.message); } finally { pending = false; }
    }, 1000);
    return () => clearInterval(timer);
  }, [connected]);
  const disconnect = () => {
    connection.current?.disconnect(); connection.current = null;
    setConnected(false); setHostState(null); setActiveController(''); setStatus('Disconnected'); setBusy(false);
  };
  const selectHost = id => {
    disconnect(); setSelected(id); setSection('');
    run(() => browser.storage.local.set({selected: id}));
  };
  const connect = async () => {
    if (!selectedHost || !data) return;
    disconnect(); setBusy(true);
    const current = new Connection(data.identity, selectedHost, {
      onStatus: text => { if (connection.current === current) { setStatus(text); if (text === 'Disconnected') setConnected(false); } },
      onStream: stream => { if (video.current) video.current.srcObject = stream; },
    });
    connection.current = current;
    try {
      await current.connect();
      if (connection.current !== current) return;
      setConnected(true); await refresh(current);
    } catch (reason) { if (connection.current === current) { disconnect(); throw reason; } }
    finally { setBusy(false); }
  };
  const saveHosts = async hosts => {
    await store.current.saveHosts(hosts, data.syncEnabled);
    setData({...data, hosts});
  };
  const importHost = async input => {
    if (!input) return;
    if (input.size > 16384) throw Error('Pairing files must be smaller than 16 KiB');
    const host = profile(JSON.parse(await input.text()), 'pvthost');
    const previous = data.hosts.find(p => p.id === host.id);
    if (previous && (previous.ed25519 !== host.ed25519 || previous.x25519 !== host.x25519)) throw Error('This host identity has different keys. Remove the old host before pairing new keys.');
    const hosts = [...data.hosts.filter(p => p.id !== host.id), host];
    await saveHosts(hosts); selectHost(host.id);
  };
  const download = (value, name) => {
    const url = URL.createObjectURL(new Blob([JSON.stringify(value, null, 2)], {type: 'application/json'}));
    const anchor = document.createElement('a'); anchor.href = url; anchor.download = name; anchor.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
  const command = async (action, fields = {}) => {
    await connection.current.command(action, fields); await refresh();
  };
  const changeTarget = async (target, value) => {
    if (!Number.isFinite(value)) throw Error('Enter a finite number');
    const reply = await connection.current.command('set', {path: target.path, value, revision: hostState.revision});
    setHostState(previous => ({...previous, revision: reply.revision}));
    await refresh();
  };
  const targets = hostState?.targets || [];
  const sections = [...new Set(targets.map(t => t.section))];
  const filtered = targets.filter(t => (!section || t.section === section) && `${t.label} ${t.section} ${t.path}`.toLowerCase().includes(search.toLowerCase()));
  return <div className="app">
    <header>
      <div className="brand" aria-hidden="true"><span/><span/><span/><span/></div>
      <div className="title"><h1>{display ? 'Remote Display' : 'Remote Control'}</h1><p>Procedural Visualizer Tool</p></div>
      <span className={`connection-status ${connected ? 'online' : ''}`} role="status"><i/>{status}</span>
      <button onClick={() => setSettings(!settings)} aria-expanded={settings}>Hosts & settings</button>
    </header>
    <div className="connection-bar">
      <label>Host <select value={selected} onChange={event => selectHost(event.target.value)} aria-label="Active host">
        <option value="">Select a paired host</option>{data?.hosts.map(host => <option key={host.id} value={host.id}>{host.label}</option>)}
      </select></label>
      <button className="primary" disabled={!data || !selectedHost} onClick={() => run(connected || busy ? async () => disconnect() : connect)}>{busy ? 'Cancel connection' : connected ? 'Disconnect' : 'Connect'}</button>
      <label className="switch"><input type="checkbox" disabled={!connected} checked={hostState?.background || false} onChange={event => run(() => command('background', {value: event.target.checked}))}/> Host in background</label>
    </div>
    {error && <div className="error" role="alert">{error}<button onClick={() => setError('')} aria-label="Dismiss error">×</button></div>}
    {settings && <section className="settings" aria-label="Hosts and settings">
      <div className="settings-heading"><h2>Hosts & pairing</h2><button onClick={() => setSettings(false)}>Done</button></div>
      <p>Export this remote’s public identity and import it in PVT’s Networking & Remotes. Then import the host’s .pvthost file here.</p>
      <div className="button-row">
        <button disabled={!data} onClick={() => download(data.identity.public, `PVT-${display ? 'RD' : 'RC'}.pvtremote`)}>Export .pvtremote</button>
        <button disabled={!data} onClick={() => file.current.click()}>Import .pvthost</button>
        <input ref={file} type="file" accept=".pvthost,application/json" hidden onChange={event => { const selected = event.target.files[0]; event.target.value = ''; run(() => importHost(selected)); }}/>
      </div>
      <ul className="hosts">{data?.hosts.map(host => <li key={host.id}><div><strong>{host.label}</strong><small>{host.id}</small><code title="Pinned Ed25519 public key">{host.ed25519}</code></div><div className="button-row"><button onClick={() => setEdit({...host, endpointText: host.endpoints.join('\n')})}>Edit</button><button onClick={() => run(async () => { if (selected === host.id) selectHost(''); await saveHosts(data.hosts.filter(p => p.id !== host.id)); })}>Remove</button></div></li>)}</ul>
      {edit && <form className="edit-host" onSubmit={event => { event.preventDefault(); run(async () => { const updated = profile({...edit, endpoints: edit.endpointText.split('\n').map(s => s.trim()).filter(Boolean)}, 'pvthost'); if (selected === updated.id) disconnect(); await saveHosts(data.hosts.map(p => p.id === updated.id ? updated : p)); setEdit(null); }); }}>
        <h3>Edit host profile</h3><label>Name<input required maxLength={120} value={edit.label} onChange={event => setEdit({...edit, label: event.target.value})}/></label>
        <label>Local endpoints, one per line<textarea rows={3} value={edit.endpointText} onChange={event => setEdit({...edit, endpointText: event.target.value})}/></label>
        <label>Signaling URL<input value={edit.signaling_url} onChange={event => setEdit({...edit, signaling_url: event.target.value})}/></label>
        <div className="button-row"><button className="primary">Save profile</button><button type="button" onClick={() => setEdit(null)}>Cancel</button></div>
      </form>}
      <div className="sync"><h2>Browser sync</h2><p>Only public host profiles are synced. Your private remote identity stays on this device. Reinstalling restores synced hosts and your sync preference, but requires pairing the new remote identity in PVT.</p>
        <label className="switch"><input type="checkbox" disabled={!data} checked={data?.syncEnabled || false} onChange={event => { const enabled = event.target.checked; run(async () => { if (enabled) { await browser.storage.local.set({syncEnabled: true}); const restored = await store.current.load(); await store.current.saveHosts(restored.hosts, true); setData(restored); } else { await store.current.disableSync(); setData({...data, syncEnabled: false}); } }); }}/> Sync public host profiles with my browser account</label>
        <p className="muted">Turning sync off keeps local profiles. Use Clear synced data to remove the account backup.</p>
        <div className="button-row"><button onClick={() => run(async () => { const all = await browser.storage.sync.get(null); setSyncView(Object.fromEntries(Object.entries(all).filter(([key]) => key.startsWith('pvt.host.')))); })}>View synced data</button><button onClick={() => run(async () => { await store.current.clearSync(); setData({...data, syncEnabled: false}); setSyncView({}); })}>Clear synced data</button></div>
        {syncView && <pre tabIndex={0}>{JSON.stringify(syncView, null, 2)}</pre>}
      </div>
    </section>}
    <main>
      {display ? <section className="display"><div className="screen"><video ref={video} autoPlay playsInline muted={muted} controls={false}/>{!connected && <div className="empty"><div className="display-symbol" aria-hidden="true"/><h2>Your stage, wherever you are.</h2><p>Pair a PVT host, then connect to its live output.</p><button onClick={() => setSettings(true)}>Set up a host</button></div>}</div><div className="display-tools"><span>{connected ? 'Live audio & video' : 'Waiting for a host'}</span><button disabled={!connected} onClick={() => { setMuted(!muted); video.current?.play().catch(reason => setError(reason.message)); }}>{muted ? 'Enable audio' : 'Mute audio'}</button><button disabled={!connected} onClick={() => run(() => video.current.requestFullscreen())}>Full screen</button></div></section>
      : <section className="control"><aside><h2>Parameters</h2><label className="search"><span>Search all controls</span><input type="search" value={search} placeholder="Layer, effect, or parameter…" onChange={event => { setSearch(event.target.value); setSection(''); }}/></label><nav aria-label="Parameter sections"><button className={!section ? 'selected' : ''} onClick={() => setSection('')}>All parameters <span>{targets.length}</span></button>{sections.map(name => <button key={name} className={section === name ? 'selected' : ''} onClick={() => setSection(name)}>{name}</button>)}</nav></aside><div className="parameters">
        <div className="performance"><h2>{selectedHost?.label || 'No host selected'}</h2><div className="button-row"><button disabled={!controlling || hostState?.busy} onClick={() => run(() => command('live', {value: !hostState.live}))}>{hostState?.live ? 'Stop live output' : 'Go live'}</button><button disabled={!controlling || hostState?.busy} onClick={() => run(() => command('playback', {value: !hostState.playing}))}>{hostState?.playing ? 'Pause' : 'Play'}</button><button disabled={!controlling || hostState?.busy} onClick={() => run(() => command('undo'))}>Undo</button><button disabled={!controlling || hostState?.busy} onClick={() => run(() => command('redo'))}>Redo</button></div></div>
        {!connected ? <div className="empty"><h2>Bring your controls closer.</h2><p>Connect a paired host to browse its layers, effects, and project controls.</p><button onClick={() => setSettings(true)}>Set up a host</button></div> : <>
          {!controlling && <p className="notice">Viewing parameters. Select this extension as Active Control Remote in PVT to edit.</p>}
          {hostState?.busy && <p className="notice">PVT is loading, saving, or exporting. Editing will resume when it finishes.</p>}
          <div className="table-heading"><span>{section || 'All parameters'} · {filtered.length}</span><span>Value</span></div>
          {filtered.slice(0, 300).map(target => <Parameter key={target.path} target={target} disabled={!controlling || hostState?.busy} onChange={value => run(() => changeTarget(target, value))}/>)}
          {filtered.length > 300 && <p className="notice">Showing 300 of {filtered.length} controls. Choose a section or search to narrow the list.</p>}
          {!filtered.length && <p className="notice">{targets.length ? 'No parameters match your search.' : 'Waiting for the host’s project parameters…'}</p>}
        </>}
      </div></section>}
    </main>
    <footer><span>PVT-{display ? 'RD' : 'RC'}</span><span>{data?.syncEnabled ? 'Public profiles synced' : 'Profiles stored locally'} · Keep this tab open while connected</span></footer>
  </div>;
}
function Parameter({target, disabled, onChange}) {
  const [value, setValue] = useState(String(target.value));
  const [editing, setEditing] = useState(false);
  useEffect(() => { if (!editing) setValue(String(target.value)); }, [target.value, editing]);
  return <div className="parameter"><div><label htmlFor={target.path}>{target.label}</label><small>{target.section}</small></div>{target.kind === 0 ? <input id={target.path} aria-label={target.label} type="checkbox" disabled={disabled} checked={target.value !== 0} onChange={event => onChange(event.target.checked ? 1 : 0)}/> : <form onSubmit={event => { event.preventDefault(); setEditing(false); onChange(Number(value)); }}><input id={target.path} type="number" disabled={disabled} value={value} min={target.minimum} max={target.maximum} step={target.kind === 2 ? 'any' : 1} onFocus={() => setEditing(true)} onChange={event => setValue(event.target.value)} aria-label={target.label}/><button disabled={disabled || value === String(target.value)} type="submit">Set</button></form>}</div>;
}
