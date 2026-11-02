import { useEffect, useState } from 'react';
import type { BuildOs } from './platform';

// The release workflow copies every release's AppImage and Windows installer to this bucket (Cloudflare R2) and points
// latest.json at them (.github/workflows/downloads.yml): the repository is private, so its own release files aren't public
export const DOWNLOADS = 'https://download.vybrant.studio';

export interface Build {
  size: number;
  url: string;   // the file itself: a link to it starts the download
}
export interface Release {
  version: string;
  date: string;
  notes: string[];   // what changed: the release tag's message, a line per change
  linux?: Build;
  windows?: Build;
}

// The latest release: null while loading or when there is none
export function useLatestRelease() {
  const [release, setRelease] = useState<Release | null>(null);
  const [state, setState] = useState<'loading' | 'ready' | 'none'>('loading');
  useEffect(() => {
    fetch(`${DOWNLOADS}/latest.json`)
      .then((r) => (r.ok ? r.json() : Promise.reject()))
      .then((r: { version: string; date: string; notes?: string } & Record<BuildOs, { path: string; size: number } | undefined>) => {
        const build = (b?: { path: string; size: number }): Build | undefined => b && { size: b.size, url: `${DOWNLOADS}/${b.path}` };
        const notes = (r.notes ?? '').split('\n').map((l) => l.replace(/^\s*[-*•]\s*/, '').trim()).filter(Boolean);
        setRelease({ version: r.version, date: r.date, notes, linux: build(r.linux), windows: build(r.windows) });
        setState('ready');
      })
      .catch(() => setState('none'));
  }, []);
  return { release, state };
}

export interface Entry {
  version: string;
  date: string;
  notes: string;   // the release tag's message as written
}
// Every release, newest first (downloads.yml keeps releases.json): null while loading or when it can't be had
export function useReleases() {
  const [entries, setEntries] = useState<Entry[] | null>(null);
  const [failed, setFailed] = useState(false);
  useEffect(() => {
    fetch(`${DOWNLOADS}/releases.json`)
      .then((r) => (r.ok ? r.json() : Promise.reject()))
      .then(setEntries)
      .catch(() => setFailed(true));
  }, []);
  return { entries, failed };
}

// A release's notes in groups: a heading line ("## Fixed", "New:") starts one; "- item" lines are its changes
export function groups(notes: string) {
  const out: { title: string | null; items: string[] }[] = [];
  for (const raw of notes.split('\n')) {
    const line = raw.trim();
    if (!line) continue;
    const heading = line.match(/^#+\s*(.+?)\s*:?$/) ?? line.match(/^(new|added|improved|improvements|changed|fixed|fixes|removed|security)\s*:$/i);
    if (heading) out.push({ title: heading[1].charAt(0).toUpperCase() + heading[1].slice(1), items: [] });
    else {
      if (!out.length) out.push({ title: null, items: [] });
      out[out.length - 1].items.push(line.replace(/^[-*•]\s*/, ''));
    }
  }
  return out.filter((g) => g.items.length);
}
