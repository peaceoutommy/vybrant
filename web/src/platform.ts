// The visitor's OS: the browser's own platform hint where it gives one (Chromium browsers), else the user-agent string
export type Os = 'windows' | 'linux' | 'mac' | 'mobile' | 'other';
export type BuildOs = 'windows' | 'linux';   // the ones Vybrant is built for

export function detectOs(): Os {
  if (typeof window === 'undefined') return 'other';   // the build's prerender: no visitor yet
  const hint = (navigator as Navigator & { userAgentData?: { platform?: string; mobile?: boolean } }).userAgentData;
  const platform = (hint?.platform || navigator.platform || '').toLowerCase();
  const ua = navigator.userAgent.toLowerCase();
  if (hint?.mobile || /android|iphone|ipad|ipod/.test(ua)) return 'mobile';
  if (platform.startsWith('win') || ua.includes('windows')) return 'windows';
  if (platform.startsWith('mac') || ua.includes('macintosh')) return 'mac';
  if (platform.includes('linux') || ua.includes('linux') || ua.includes('x11')) return 'linux';
  return 'other';
}

export const os = detectOs();
// The build to lead with: the visitor's own, or Windows (the most common desktop) when theirs has none
export const buildOs: BuildOs = os === 'linux' ? 'linux' : 'windows';
export const osName: Record<BuildOs, string> = { windows: 'Windows', linux: 'Linux' };
