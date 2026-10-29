import { useEffect, useState } from 'react';
import type { BuildOs } from './platform';

// The release workflow copies every release's AppImage and Windows installer to this bucket (Cloudflare R2) and points
// latest.json at them (.github/workflows/downloads.yml): the repository is private, so its own release files aren't public
export const DOWNLOADS = 'https://downloads.vybrant.studio';

export interface Build {
  size: number;
  url: string;   // the file itself: a link to it starts the download
}
export interface Release {
  version: string;
  date: string;
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
      .then((r: { version: string; date: string } & Record<BuildOs, { path: string; size: number } | undefined>) => {
        const build = (b?: { path: string; size: number }): Build | undefined => b && { size: b.size, url: `${DOWNLOADS}/${b.path}` };
        setRelease({ version: r.version, date: r.date, linux: build(r.linux), windows: build(r.windows) });
        setState('ready');
      })
      .catch(() => setState('none'));
  }, []);
  return { release, state };
}
