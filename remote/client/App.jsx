import React, {useEffect, useRef, useState} from 'react';
import browser from 'webextension-polyfill';
import {ProfileStore} from './storage.mjs';
import {Settings} from './Settings.jsx';
import {ControlBrowser} from './ControlBrowser.jsx';
import {Connection} from './transport.mjs';
import './style.css';

export function App({role, icon}) {
  const display = role === 'display';
  const store = useRef(new ProfileStore(browser, role));
  const connection = useRef(null);
  const video = useRef(null);
  const [data, setData] = useState(null);
  const [selected, setSelected] = useState('');
  const [paused, setPaused] = useState(false);
  const [status, setStatus] = useState('Disconnected');
  const [error, setError] = useState('');
  const [connected, setConnected] = useState(false);
  const [hostState, setHostState] = useState(null);
  const [activeController, setActiveController] = useState('');
  const [settings, setSettings] = useState(false);
  const [muted, setMuted] = useState(true);
  const selectedHost = data?.hosts.find(host => host.id === selected);
  const hostConnectionKey = selectedHost && JSON.stringify({...selectedHost, label: ''});
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
      setPaused(saved.paused === true);
      if (!saved.hosts.length) setSettings(true);
    }).catch(() => setError('This browser could not prepare pairing.'));
    return () => { cancelled = true; connection.current?.disconnect(); };
  }, []);
  useEffect(() => {
    if (!connected) return;
    let pending = false;
    const timer = setInterval(async () => {
      if (pending) return;
      pending = true;
      try { await refresh(); } catch { /* Transport owns reconnection; routine polling stays quiet. */ } finally { pending = false; }
    }, 1000);
    return () => clearInterval(timer);
  }, [connected]);
  const disconnect = () => {
    connection.current?.disconnect(); connection.current = null;
    setConnected(false); setHostState(null); setActiveController(''); setStatus('Disconnected');
  };
  const selectHost = id => {
    disconnect(); setSelected(id); setPaused(false);
    run(() => browser.storage.local.set({selected: id, paused: false}));
  };
  useEffect(() => {
    if (!selectedHost || !data || paused) return;
    const current = new Connection(data.identity, selectedHost, {
      onStatus: text => {
        if (connection.current !== current) return;
        setStatus(text); setConnected(text === 'Connected');
        if (text !== 'Connected') { setHostState(null); setActiveController(''); }
      },
      onStream: stream => { if (connection.current === current && video.current) video.current.srcObject = stream; },
    });
    connection.current = current;
    current.connect().catch(() => { if (connection.current === current) setError('PVT could not connect.'); });
    return () => { current.disconnect(); if (connection.current === current) connection.current = null; };
  }, [hostConnectionKey, data?.identity, paused]);
  const toggleConnection = async () => {
    const next = !paused;
    if (next) disconnect();
    setPaused(next);
    await browser.storage.local.set({paused: next});
  };
  const saveSettings = async (hosts, syncEnabled) => {
    // Use the existing profile store for validation, pinned identities and sync.
    if (syncEnabled && !data.syncEnabled) hosts = await store.current.mergeSyncedHosts(hosts, true);
    await store.current.saveHosts(hosts, syncEnabled);
    if (!syncEnabled && data.syncEnabled) await store.current.disableSync();
    setData(previous => ({...previous, hosts, syncEnabled}));
    if (!hosts.some(host => host.id === selected)) {
      const next = hosts[0]?.id || '';
      selectHost(next);
      await browser.storage.local.set({selected: next, paused: false});
    }
    return hosts;
  };
  const download = (value, name) => {
    const url = URL.createObjectURL(new Blob([JSON.stringify(value, null, 2)], {type: 'application/json'}));
    const anchor = document.createElement('a'); anchor.href = url; anchor.download = name; anchor.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
  const command = async (action, fields = {}) => {
    const current = connection.current;
    if (!current) throw Error('Connect to PVT to use this control.');
    await current.command(action, fields); await refresh(current);
  };
  const changeTarget = async (target, value) => {
    if (!Number.isFinite(value)) throw Error('Enter a finite number');
    const current = connection.current;
    if (!current || !hostState) throw Error('Connect to PVT to edit this control.');
    const reply = await current.command('set', {path: target.path, value, revision: hostState.revision});
    if (connection.current !== current) return;
    setHostState(previous => previous && ({...previous, revision: reply.revision}));
    await refresh(current);
  };
  const targets = hostState?.targets || [];
  return <div className={`app ${display ? 'display-app' : 'control-app'}`}>
    <header>
      <img className="brand" src={icon} alt={`PVT-${display ? 'RD' : 'RC'}`} width="52" height="52"/>
      <div className="title"><h1>{display ? 'Remote Display' : 'Remote Control'}</h1><p>Procedural Visualizer Tool</p></div>
      <span className={`connection-status ${connected ? 'online' : ''}`} role="status"><i/>{status}</span>
      <button onClick={() => setSettings(true)} aria-haspopup="dialog" aria-expanded={settings}>Hosts & settings</button>
    </header>
    <div className="connection-bar">
      <label>Host <select value={selected} onChange={event => selectHost(event.target.value)} aria-label="Active host">
        <option value="">Select a paired host</option>{data?.hosts.map(host => <option key={host.id} value={host.id}>{host.label}</option>)}
      </select></label>
      <button className="primary" disabled={!data || !selectedHost} onClick={() => run(toggleConnection)}>{paused ? 'Connect' : connected ? 'Disconnect' : 'Pause connection'}</button>
      <label className="switch"><input type="checkbox" disabled={!connected} checked={hostState?.background || false} onChange={event => run(() => command('background', {value: event.target.checked}))}/> Host in background</label>
    </div>
    {error && <div className="error" role="alert">{error}<button onClick={() => setError('')} aria-label="Dismiss error">×</button></div>}
    {settings && data && <Settings data={data} display={display} store={store.current} onSave={saveSettings} onClose={() => setSettings(false)} onExport={() => download(data.identity.public, `PVT-${display ? 'RD' : 'RC'}.pvtremote`)}/>}
    <main>
      {display ? <section className="display"><div className="stage-heading"><div><div className="eyebrow">Live stage</div><h2>{selectedHost?.label || 'Remote Display'}</h2></div><span className="muted">{connected ? 'Output from PVT' : 'Connect your desktop to begin'}</span></div><div className="screen"><video ref={video} autoPlay playsInline muted={muted} controls={false}/>{!connected && <div className="empty"><div className="display-symbol" aria-hidden="true"/><h2>{paused ? 'Display paused' : 'Your PVT stage'}</h2><p>{paused ? 'Choose Connect above to resume the live output.' : selectedHost ? status : 'Pair with PVT once. Its live output connects automatically.'}</p><button onClick={() => setSettings(true)}>Set up a host</button></div>}</div><div className="display-tools"><span>{connected ? muted ? 'Audio muted' : 'Audio enabled' : paused ? 'Disconnected' : 'Waiting for a host'}</span><button disabled={!connected} onClick={() => { setMuted(!muted); video.current?.play().catch(reason => setError(reason.message)); }}>{muted ? 'Enable audio' : 'Mute audio'}</button><button disabled={!connected} onClick={() => run(() => video.current.requestFullscreen())}>Full screen</button></div></section>
      : <section className="control">
        <div className="performance"><h2>{selectedHost?.label || 'No host selected'}</h2><div className="button-row"><button disabled={!controlling || hostState?.busy} onClick={() => run(() => command('live', {value: !hostState.live}))}>{hostState?.live ? 'Stop live output' : 'Go live'}</button><button disabled={!controlling || hostState?.busy} onClick={() => run(() => command('playback', {value: !hostState.playing}))}>{hostState?.playing ? 'Pause' : 'Play'}</button><button disabled={!controlling || hostState?.busy} onClick={() => run(() => command('undo'))}>Undo</button><button disabled={!controlling || hostState?.busy} onClick={() => run(() => command('redo'))}>Redo</button></div></div>
        {!connected ? <div className="empty"><h2>Bring your controls closer.</h2><p>Connect a paired host to browse its layers, effects, and project controls.</p><button onClick={() => setSettings(true)}>Set up a host</button></div> : <>
          {!controlling && <p className="notice">Viewing parameters. Select this extension as Active Control Remote in PVT to edit.</p>}
          {hostState?.busy && <p className="notice">PVT is loading, saving, or exporting. Editing will resume when it finishes.</p>}
          <ControlBrowser key={selected} targets={targets} disabled={!controlling || hostState?.busy} onChange={changeTarget}/>
        </>}
      </section>}
    </main>
    <footer><span>PVT-{display ? 'RD' : 'RC'} · {browser.runtime.getManifest().version}</span><span>{data?.syncEnabled ? 'Public profiles synced' : 'Profiles stored locally'} · Keep this tab open while connected</span></footer>
  </div>;
}
