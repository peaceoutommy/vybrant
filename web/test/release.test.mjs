import { test } from 'node:test';
import assert from 'node:assert/strict';
import { groups } from '../src/release.ts';

test('release notes split into their groups; notes without headings are one plain list', () => {
  assert.deepEqual(groups('- Plugin crashes no longer close the app\n- The app now updates itself'), [
    { title: null, items: ['Plugin crashes no longer close the app', 'The app now updates itself'] },
  ]);
  assert.deepEqual(groups('## New\n- Changelog page\n\nfixed:\n* Window buttons on Windows\n## Empty'), [
    { title: 'New', items: ['Changelog page'] },
    { title: 'Fixed', items: ['Window buttons on Windows'] },
  ]);
  assert.deepEqual(groups(''), []);
});
