// Adapt the native Live registry's stable paths; display names are never IDs.
const collections = {wave: 'Waves', swing: 'Swings', effect: 'Effects', post_effect: 'Post Effect Stack'};
export const PAGE_SIZE = 24;
export function pageOf(items, page, size = PAGE_SIZE) {
  const pages = Math.max(1, Math.ceil(items.length / size));
  const current = Math.max(0, Math.min(page, pages - 1));
  return {items: items.slice(current * size, (current + 1) * size), page: current, pages, total: items.length};
}
export function buildNavigation(targets) {
  const groups = new Map();
  const names = new Map();
  const sectionMaps = new Map();
  const itemMaps = new Map();
  for (const target of targets) {
    const parts = target.path.split('/');
    if (parts[0] === 'layer' && parts.length >= 3 && !collections[parts[2]]) {
      const id = parts.slice(0, 2).join('/');
      if (!names.has(id)) names.set(id, target.section.slice(0, target.section.lastIndexOf(' — ')) || target.section);
    }
  }
  for (const target of targets) {
    const parts = target.path.split('/');
    const type = parts.length >= 3 && ['layer', 'group'].includes(parts[0]) ? parts[0] : 'project';
    const id = type === 'project' ? 'project' : parts.slice(0, 2).join('/');
    if (!groups.has(id)) {
      // A normal section ends in its category, while item sections add type/name.
      const label = type === 'project' ? 'Project' : type === 'group' ? target.section.replace(/^[^—]+ — /, '')
        : names.has(id) ? names.get(id)
        : target.section.split(' — ')[0];
      groups.set(id, {id, type, label, sections: [], targets: []});
      sectionMaps.set(id, new Map());
    }
    const group = groups.get(id);
    const collection = type === 'layer' && parts.length >= 5 && collections[parts[2]];
    const label = type === 'layer' && target.section.startsWith(group.label + ' — ')
      ? target.section.slice(group.label.length + 3) : target.section;
    const sectionId = collection ? parts[2] : label;
    let section = sectionMaps.get(id).get(sectionId);
    if (!section) {
      section = {id: sectionId, label: collection || label, targets: [], items: []};
      group.sections.push(section); sectionMaps.get(id).set(sectionId, section); itemMaps.set(section, new Map());
    }
    group.targets.push(target); section.targets.push(target);
    if (collection) {
      const itemId = parts.slice(0, 4).join('/');
      let item = itemMaps.get(section).get(itemId);
      if (!item) { item = {id: itemId, label: label.replace(/^[^—]+ — /, ''), targets: []}; section.items.push(item); itemMaps.get(section).set(itemId, item); }
      item.targets.push(target);
    }
  }
  return [...groups.values()];
}
export function searchTargets(targets, query) {
  const words = query.trim().toLocaleLowerCase().split(/\s+/).filter(Boolean);
  return targets.filter(target => words.every(word => `${target.label} ${target.section} ${target.path}`.toLocaleLowerCase().includes(word)));
}
