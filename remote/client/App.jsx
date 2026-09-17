import React, {useEffect, useRef, useState} from 'react';
import browser from 'webextension-polyfill';
import {ProfileStore} from './storage.mjs';
import {Settings} from './Settings.jsx';
import {ControlBrowser} from './ControlBrowser.jsx';
import {Connection, clientInfo} from './transport.mjs';
import './style.css';

export function App({role, icon}) {
  const display = role === 'display';
  const store = useRef(new ProfileStore(browser, role));
  const connection = useRef(null);
  const video = useRef(null);
  const [data, setData] = useState(null);
  const [selected, setSelected] = useState('');
  const [status, setStatus] = useState('Disconnected');
  const [error, setError] = useState('');
  const [connected, setConnected] = useState(false);
  const [connectionRequested, setConnectionRequested] = useState(null);
  const [hostState, setHostState] = useState(null);
  const [settings, setSettings] = useState(false);
  const [audioEnabled, setAudioEnabled] = useState(false);
  const [remoteMedia, setRemoteMedia] = useState(null);
  const selectedHost = data?.hosts.find(host => host.id === selected);
  const hostConnectionKey = selectedHost && JSON.stringify({...selectedHost, label: ''});
  const controlling = connected && !display && hostState !== null;
  const run = async action => { setError(''); try { return await action(); } catch (reason) { setError(reason.message || String(reason)); } };
  const refresh = async (current = connection.current) => {
    if (!current) return;
    const response = await current.command('state');
    if (connection.current !== current) return;
    setHostState(response.state);
    if (response.remote_media) setRemoteMedia(response.remote_media);
  };
  useEffect(() => {
    let cancelled = false;
    store.current.load().then(saved => {
      if (cancelled) return;
      const initial = saved.hosts.some(host => host.id === saved.selected) ? saved.selected : saved.hosts[0]?.id || '';
      setData(saved); setSelected(initial);
      setConnectionRequested(initial ? !saved.disabledHosts.includes(initial) : true);
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
    if (video.current) video.current.srcObject = null;
    setConnected(false); setHostState(null); setRemoteMedia(null); setStatus('Disconnected');
  };
  const setConnectionPreference = async (hostId, enabled) => {
    const saved = await browser.storage.local.get(['disabledHosts']);
    const disabled = new Set(Array.isArray(saved.disabledHosts) ? saved.disabledHosts : []);
    if (enabled) disabled.delete(hostId); else disabled.add(hostId);
    const disabledHosts = [...disabled];
    await browser.storage.local.set({disabledHosts});
    setData(previous => previous && ({...previous, disabledHosts}));
    if (selected === hostId) setConnectionRequested(enabled);
  };
  const selectHost = id => {
    disconnect(); setSelected(id); setConnectionRequested(!data?.disabledHosts.includes(id));
    run(() => browser.storage.local.set({selected: id}));
  };
  useEffect(() => {
    if (!selectedHost || !data || connectionRequested === null) return;
    const current = new Connection(data.identity, selectedHost, {
      onStatus: text => {
        if (connection.current !== current) return;
        setStatus(text); setConnected(text === 'Connected');
        if (text !== 'Connected') { setHostState(null); }
      },
      onStream: stream => { if (connection.current === current && video.current) video.current.srcObject = stream; },
      onControl: message => {
        if (connection.current !== current) return;
        if (message.action === 'media' && message.media) setRemoteMedia(message.media);
        if (message.action === 'disconnect') {
          current.standby();
          setConnectionPreference(selectedHost.id, false).catch(reason => setError(reason.message));
        }
        if (message.action === 'reconnect') {
          setConnectionPreference(selectedHost.id, true).then(() => current.resume())
            .catch(reason => setError(reason.message));
        }
      },
    });
    connection.current = current;
    current.connect(connectionRequested).catch(() => { if (connection.current === current) setError('PVT could not connect.'); });
    return () => { current.disconnect(); if (connection.current === current) connection.current = null; };
  }, [hostConnectionKey, data?.identity]);
  useEffect(() => {
    if (!display || !connected || !connection.current) return;
    run(async () => {
      const response = await connection.current.command('remote_media', {audio_enabled: audioEnabled});
      if (response.remote_media) setRemoteMedia(response.remote_media);
    });
  }, [connected]);
  const saveSettings = async (hosts, syncEnabled) => {
    // Use the existing profile store for validation, pinned identities and sync.
    if (syncEnabled && !data.syncEnabled) hosts = await store.current.mergeSyncedHosts(hosts, true);
    await store.current.saveHosts(hosts, syncEnabled);
    if (!syncEnabled && data.syncEnabled) await store.current.disableSync();
    setData(previous => ({...previous, hosts, syncEnabled}));
    if (!hosts.some(host => host.id === selected)) {
      const next = hosts[0]?.id || '';
      selectHost(next);
      await browser.storage.local.set({selected: next});
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
        {!data?.hosts.length && <option value="">Import a PVT host file</option>}{data?.hosts.map(host => <option key={host.id} value={host.id}>{host.label}</option>)}
      </select></label>
      <button disabled={!selectedHost || connectionRequested === null} onClick={() => run(async () => {
        if (connectionRequested) {
          if (connected && display) await connection.current.command('remote_connection', {enabled: false});
          await setConnectionPreference(selectedHost.id, false);
          await connection.current?.standby();
        } else {
          await setConnectionPreference(selectedHost.id, true);
          connection.current?.resume();
        }
      })}>{connectionRequested ? 'Disconnect' : 'Connect'}</button>
      <label className="switch"><input type="checkbox" disabled={!connected} checked={hostState?.background || false} onChange={event => run(() => command('background', {value: event.target.checked}))}/> Host in background</label>
    </div>
    {error && <div className="error" role="alert">{error}<button onClick={() => setError('')} aria-label="Dismiss error">×</button></div>}
    {settings && data && <Settings data={data} display={display} store={store.current} onSave={saveSettings} onClose={() => setSettings(false)} onExport={() => download({...data.identity.public, client: clientInfo()}, `PVT-${display ? 'RD' : 'RC'}.pvtremote`)}/>}
    <main>
      {display ? <section className="display"><div className="stage-heading"><div><div className="eyebrow">Live stage</div><h2>{selectedHost?.label || 'Remote Display'}</h2></div><span className="muted">{connected ? remoteMedia?.paused ? 'Output paused' : 'Output from PVT' : 'Connect your desktop to begin'}</span></div><div className="screen"><video ref={video} autoPlay playsInline muted={!audioEnabled} controls={false}/>{!connected && <div className="empty"><div className="display-symbol" aria-hidden="true"/><h2>Your PVT stage</h2><p>{selectedHost ? status : 'Pair with PVT once. Its live output connects automatically.'}</p><button onClick={() => setSettings(true)}>Set up a host</button></div>}</div><div className="display-tools"><span>{connected ? !audioEnabled ? 'Audio disabled here' : remoteMedia?.muted ? 'Audio muted by PVT' : 'Audio enabled' : 'Waiting for a host'}</span><button disabled={!connected} onClick={() => run(async () => {
        const paused = !remoteMedia?.paused;
        const response = await connection.current.command('remote_media', {paused, ...(paused ? {pause_mode: remoteMedia?.pause_mode || 'blackout'} : {})});
        setRemoteMedia(response.remote_media);
      })}>{remoteMedia?.paused ? 'Resume output' : 'Pause output'}</button><button disabled={!connected} onClick={() => run(async () => {
        const enabling = !audioEnabled;
        if (enabling) { setAudioEnabled(true); if (video.current) video.current.muted = false; }
        const response = await connection.current.command('remote_media', enabling
          ? {audio_enabled: true, muted: false} : {muted: !remoteMedia?.muted});
        setRemoteMedia(response.remote_media);
        video.current?.play().catch(reason => setError(reason.message));
      })}>{!audioEnabled ? 'Enable audio' : remoteMedia?.muted ? 'Unmute' : 'Mute'}</button><button disabled={!connected} onClick={() => run(() => video.current.requestFullscreen())}>Full screen</button></div></section>
      : <section className="control">
        <div className="performance"><h2>{selectedHost?.label || 'No host selected'}</h2><div className="button-row"><button disabled={!controlling || hostState?.busy} onClick={() => run(() => command('live', {value: !hostState.live}))}>{hostState?.live ? 'Stop live output' : 'Go live'}</button><button disabled={!controlling || hostState?.busy} onClick={() => run(() => command('playback', {value: !hostState.playing}))}>{hostState?.playing ? 'Pause' : 'Play'}</button><button disabled={!controlling || hostState?.busy} onClick={() => run(() => command('undo'))}>Undo</button><button disabled={!controlling || hostState?.busy} onClick={() => run(() => command('redo'))}>Redo</button></div></div>
        {!connected ? <div className="empty"><h2>Bring your controls closer.</h2><p>Connect a paired host to browse its layers, effects, and project controls.</p><button onClick={() => setSettings(true)}>Set up a host</button></div> : <>
          {hostState?.busy && <p className="notice">PVT is loading, saving, or exporting. Editing will resume when it finishes.</p>}
          <ControlBrowser key={selected} targets={targets} disabled={!controlling || hostState?.busy} onChange={changeTarget}/>
        </>}
      </section>}
    </main>
    <footer><span>PVT-{display ? 'RD' : 'RC'} · {browser.runtime.getManifest().version}</span><span>{data?.syncEnabled ? 'Public profiles synced' : 'Profiles stored locally'} · Keep this tab open while connected</span></footer>
  </div>;
}
