// The site's pages, for the browser's routing (App, main.tsx) and the build's prerender (scripts/prerender.mjs)
export const pages = {
  '/': {
    title: 'Modern DAW | Vybrant',
    description: 'Vybrant is a complete digital audio workstation for Linux and Windows. Free, one license per account.',
  },
  '/download': {
    title: 'Download for Windows and Linux | Vybrant',
    description:
      'Download Vybrant, a free digital audio workstation: an installer for 64-bit Windows and an AppImage for Linux, with install steps and system requirements.',
  },
  '/changelog': { title: 'Changelog | Vybrant', description: 'Every Vybrant release and what changed in it, newest first.' },
  '/account': { title: 'Your license | Vybrant', description: 'Sign in to get your free Vybrant license.' },
  '/404': { title: 'Page not found | Vybrant', description: 'There is no page at this address.' },
};
export type Page = keyof typeof pages;
export const pageFor = (path: string): Page =>
  path === '/'
    ? '/'
    : path.startsWith('/download')
      ? '/download'
      : path.startsWith('/changelog')
        ? '/changelog'
        : path.startsWith('/account')
          ? '/account'
          : '/404';
