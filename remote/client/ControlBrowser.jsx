import React, {useMemo, useState} from 'react';
import {buildNavigation, pageOf, searchTargets} from './navigation.mjs';

export function Pager({value, onChange, label = 'Controls'}) {
  if (value.pages <= 1) return null;
  return <div className="pager" aria-label={`${label} pages`}><span>{value.total} {label.toLowerCase()} · Page {value.page + 1} of {value.pages}</span><button disabled={!value.page} onClick={() => onChange(value.page - 1)} aria-label={`Previous ${label.toLowerCase()} page`}>Previous</button><button disabled={value.page + 1 === value.pages} onClick={() => onChange(value.page + 1)} aria-label={`Next ${label.toLowerCase()} page`}>Next</button></div>;
}
export function ControlBrowser({targets, disabled, onChange}) {
  const groups = useMemo(() => buildNavigation(targets), [targets]);
  const [groupId, setGroup] = useState('project');
  const [sectionId, setSection] = useState('');
  const [itemId, setItem] = useState('');
  const [navOpen, setNavOpen] = useState(false);
  const [query, setQuery] = useState('');
  const [layerQuery, setLayerQuery] = useState('');
  const [itemBrowserOpen, setItemBrowserOpen] = useState(true);
  const [itemQuery, setItemQuery] = useState('');
  const [page, setPage] = useState(0);
  const [groupPage, setGroupPage] = useState(0);
  const [itemPage, setItemPage] = useState(0);
  const group = groups.find(g => g.id === groupId) || groups[0];
  const section = group?.sections.find(s => s.id === sectionId) || group?.sections[0];
  const item = section?.items.find(i => i.id === itemId) || section?.items[0];
  const searching = Boolean(query.trim());
  const visible = pageOf(searching ? searchTargets(targets, query) : item?.targets || section?.targets || [], page);
  const visibleGroups = pageOf(groups.filter(g => g.type !== 'project' && g.label.toLocaleLowerCase().includes(layerQuery.toLocaleLowerCase())), groupPage, 12);
  const visibleItems = pageOf((section?.items || []).filter(i => i.label.toLocaleLowerCase().includes(itemQuery.toLocaleLowerCase())), itemPage, 8);
  const chooseGroup = id => { setNavOpen(false); setGroup(id); setSection(''); setItem(''); setQuery(''); setItemQuery(''); setPage(0); setItemPage(0); };
  const chooseSection = id => { setItemBrowserOpen(true); setSection(id); setItem(''); setItemQuery(''); setPage(0); setItemPage(0); };
  const locate = target => {
    const owner = groups.find(g => g.targets.includes(target));
    const category = owner.sections.find(s => s.targets.includes(target));
    const entry = category.items.find(i => i.targets.includes(target));
    setItemBrowserOpen(false); setGroup(owner.id); setSection(category.id); setItem(entry?.id || ''); setQuery(''); setItemQuery(''); setItemPage(entry ? Math.floor(category.items.indexOf(entry) / 8) : 0);
    setPage(Math.floor((entry?.targets || category.targets).indexOf(target) / 24));
  };
  return <div className="control-browser">
    <aside className={navOpen ? 'nav-open' : ''}><button className="mobile-navigation" aria-expanded={navOpen} onClick={() => setNavOpen(!navOpen)}>Project & layers <span>{navOpen ? 'Close navigation' : searching ? 'Search results' : group?.label}</span></button><div className="workspace-navigation"><div className="eyebrow">Workspace</div><h2>Project & layers</h2>
      <label className="search"><span>Find a control</span><input type="search" value={query} placeholder="Search the whole project…" onChange={e => { setQuery(e.target.value); setPage(0); }}/></label>
      <nav aria-label="Project navigation">{groups.filter(g => g.type === 'project').map(g => <button key={g.id} className={!searching && group?.id === g.id ? 'selected' : ''} aria-current={!searching && group?.id === g.id ? 'page' : undefined} onClick={() => chooseGroup(g.id)}><span>{g.label}</span><small>Global</small></button>)}</nav>
      <div className="nav-heading">Layers & groups <span>{groups.filter(g => g.type !== 'project').length}</span></div>
      <label className="search"><span className="sr-only">Filter layers and groups</span><input type="search" placeholder="Find a layer…" value={layerQuery} onChange={e => { setLayerQuery(e.target.value); setGroupPage(0); }}/></label>
      <nav aria-label="Layers and groups">{visibleGroups.items.map(g => <button key={g.id} className={!searching && group?.id === g.id ? 'selected' : ''} aria-current={!searching && group?.id === g.id ? 'page' : undefined} onClick={() => chooseGroup(g.id)}><span>{g.label}</span><small>{g.type === 'group' ? 'Group' : groups.filter(x => x.type === 'layer').indexOf(g) + 1}</small></button>)}</nav>
      {!visibleGroups.total && <p className="muted">{layerQuery ? 'No matching layers.' : 'Layers appear here when added in PVT.'}</p>}
      <Pager value={visibleGroups} onChange={setGroupPage} label="Layers"/>
    </div></aside>
    <section className="parameters" aria-label="Parameter editor">
      <div className="editor-heading"><div><div className="eyebrow">{searching ? 'Whole project' : group?.type === 'layer' ? 'Layer' : 'Workspace'}</div><h2>{searching ? 'Search results' : group?.label || 'Project'}</h2><p>{searching ? `${visible.total} matching controls` : 'Choose a section to adjust its controls.'}</p></div>{searching && <button onClick={() => { setQuery(''); setPage(0); }}>Clear search</button>}</div>
      {!searching && <nav className="section-tabs" aria-label="Control sections">{group?.sections.map(s => <button key={s.id} className={section.id === s.id ? 'selected' : ''} aria-current={section.id === s.id ? 'page' : undefined} onClick={() => chooseSection(s.id)}>{s.label}{s.items.length > 0 && <small>{s.items.length}</small>}</button>)}</nav>}
      {!searching && section?.items.length > 0 && <section className="item-browser" aria-label={`${section.label} browser`}><button className="item-browser-toggle" aria-expanded={itemBrowserOpen} onClick={() => setItemBrowserOpen(!itemBrowserOpen)}><span>{item?.label}</span><small>{section.items.length} {section.label.toLowerCase()} · {itemBrowserOpen ? 'Hide browser' : 'Choose another'}</small></button>{itemBrowserOpen && <div className="item-choices"><label className="search"><span>Find in {section.label.toLowerCase()}</span><input type="search" value={itemQuery} placeholder="Search by name…" onChange={e => { setItemQuery(e.target.value); setItemPage(0); }}/></label><div className="item-grid">{visibleItems.items.map(i => <button key={i.id} aria-pressed={item?.id === i.id} className={item?.id === i.id ? 'selected' : ''} onClick={() => { setItem(i.id); setPage(0); setItemBrowserOpen(false); }}><span>{i.label}</span><small>#{section.items.indexOf(i) + 1} · {i.targets.length} controls</small></button>)}</div>{!visibleItems.total && <p className="notice">No matching items. Clear the filter to browse all {section.label.toLowerCase()}.</p>}<Pager value={visibleItems} onChange={setItemPage} label="Items"/></div>}</section>}
      <div className="parameter-panel"><div className="table-heading"><h3>{searching ? 'Matching controls' : item?.label || section?.label || 'Controls'}</h3><span>{visible.total} controls</span></div>
        <div className="parameter-list">{visible.items.map(target => <Parameter key={target.path} target={target} disabled={disabled} onChange={value => onChange(target, value)} context={searching} onLocate={() => locate(target)}/>)}</div>
        {!visible.total && <div className="empty compact"><h3>{searching ? 'No matching controls' : 'Waiting for project controls'}</h3><p>{searching ? 'Try a layer name, effect name, or parameter.' : 'Open a project in PVT to get started.'}</p></div>}
        <Pager value={visible} onChange={setPage}/>
      </div>
      <p className="editor-note">Changes use PVT’s normal project controls. Undo and Redo are available above.</p>
    </section>
  </div>;
}
function Parameter({target, disabled, onChange, context, onLocate}) {
  const [draft, setDraft] = useState(null);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState('');
  const value = draft ?? String(target.value);
  const submit = async next => {
    setPending(true); setError('');
    try { await onChange(next); setDraft(null); } catch (reason) { setError(reason.message || String(reason)); }
    finally { setPending(false); }
  };
  return <div className="parameter"><div className="parameter-label"><label htmlFor={target.path}>{target.label}</label>{context && <button className="context-link" onClick={onLocate}>{target.section} ↗</button>}{error && <small role="alert" className="field-error">{error}</small>}</div>{target.kind === 0 ? <label className="toggle"><span>{target.value ? 'On' : 'Off'}</span><input id={target.path} aria-label={target.label} type="checkbox" disabled={disabled || pending} checked={target.value !== 0} onChange={e => submit(e.target.checked ? 1 : 0)}/></label> : <form onSubmit={e => { e.preventDefault(); if (value.trim()) submit(Number(value)); }}><input id={target.path} type="number" required disabled={disabled || pending} value={value} min={target.minimum} max={target.maximum} step={target.kind === 2 ? 'any' : 1} onChange={e => setDraft(e.target.value)} onKeyDown={e => { if (e.key === 'Escape') { setDraft(null); setError(''); } }} aria-label={target.label}/><button disabled={disabled || pending || value === String(target.value)} type="submit">{pending ? 'Saving…' : 'Set'}</button></form>}</div>;
}
