import test from 'node:test';
import assert from 'node:assert/strict';
import {buildNavigation, pageOf, searchTargets} from '../client/navigation.mjs';
const target = (path, section, label = 'Amount') => ({path, section, label, value: 0});
test('native stable IDs separate duplicate layer and effect names', () => {
  const targets = [target('project.fps', 'Project'), target('layer/a/opacity', 'Same — name — Mix'), target('layer/b/opacity', 'Same — name — Mix'), target('layer/a/effect/1/intensity', 'Same — name — Effect — Echo — bright'), target('layer/a/effect/2/intensity', 'Same — name — Effect — Echo — bright'), target('group/g/enabled', 'Group — Same — name')];
  const groups = buildNavigation(targets);
  assert.equal(groups.length, 4);
  assert.equal(groups[1].label, 'Same — name');
  assert.equal(groups[2].label, 'Same — name');
  assert.equal(groups[3].label, 'Same — name');
  const effects = groups[1].sections.find(s => s.id === 'effect');
  assert.equal(effects.items.length, 2);
  assert.equal(effects.items[0].label, 'Echo — bright');
  assert.notEqual(effects.items[0].id, effects.items[1].id);
  assert.deepEqual(groups.flatMap(g => g.targets), [targets[0], targets[1], targets[3], targets[4], targets[2], targets[5]]);
});
test('thousands of controls remain reachable through bounded pages and global search', () => {
  const targets = Array.from({length:3588}, (_, i) => target(`layer/a/wave/${i}/amplitude`, `Layer — Wave — Wave ${i}`, 'Wave amplitude'));
  const waves = buildNavigation(targets)[0].sections[0];
  assert.equal(waves.items.length, 3588);
  const found = searchTargets(targets, 'Wave 3587 amplitude');
  assert.equal(found.length, 1);
  assert.equal(found[0], targets.at(-1));
  const visited = [];
  for (let page = 0; page < pageOf(targets, 0).pages; page++) {
    const result = pageOf(targets, page);
    assert.ok(result.items.length <= 24); visited.push(...result.items);
  }
  assert.deepEqual(visited, targets);
});
test('filtering, project replacement and deleted selections clamp pagination', () => {
  assert.deepEqual(pageOf([], 200), {items:[], page:0, pages:1, total:0});
  assert.equal(pageOf([1,2], 200).page, 0);
  assert.equal(pageOf([1,2], -5).page, 0);
  assert.deepEqual(searchTargets([target('project.fps', 'Project', 'Playback FPS')], '  PROJECT   playback ' ).map(t=>t.path), ['project.fps']);
  assert.deepEqual(buildNavigation([]), []);
  assert.equal(buildNavigation([target('unknown.target', 'New section')])[0].sections[0].label, 'New section');
});
