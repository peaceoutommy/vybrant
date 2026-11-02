// After `vite build` and the SSR build of src/entry-server.tsx: each page's HTML, rendered by the app itself, goes into its
// own file with its own title, description, canonical URL, link-preview tags and structured data, so crawlers that don't
// run JavaScript (and Google's first pass) read the page. Cloudflare Pages serves /download from download.html and
// 404.html, with a 404 status, for any address without a file.
import { readFileSync, rmSync, writeFileSync } from 'node:fs';

const SITE = 'https://vybrant.studio';
const { render } = await import(new URL('../dist-ssr/entry-server.js', import.meta.url));
const template = readFileSync('dist/index.html', 'utf8');

const home = 'Vybrant is a complete digital audio workstation for Linux and Windows. Free, one license per account.';
const app = {
  '@context': 'https://schema.org',
  '@type': 'SoftwareApplication',
  name: 'Vybrant',
  alternateName: 'Vybrant DAW',
  applicationCategory: 'MultimediaApplication',
  operatingSystem: 'Windows 10, Windows 11, Linux',
  description: home,
  url: `${SITE}/`,
  downloadUrl: `${SITE}/download`,
  image: `${SITE}/og.jpg`,
  screenshot: ['pattern', 'playlist', 'mixer', 'plugins', 'instruments'].map((s) => `${SITE}/showcase-${s}.webp`),
  offers: { '@type': 'Offer', price: '0', priceCurrency: 'EUR' },
  publisher: { '@type': 'Organization', name: 'Vybrant', url: `${SITE}/`, logo: `${SITE}/logo.png` },
};
const pages = [
  { path: '/', file: 'index.html', title: 'Modern DAW | Vybrant', description: home, data: app },
  {
    path: '/download',
    file: 'download.html',
    title: 'Download for Windows and Linux | Vybrant',
    description: 'Download Vybrant, a free digital audio workstation: an installer for 64-bit Windows and an AppImage for Linux, with install steps and system requirements.',
  },
  { path: '/account', file: 'account.html', title: 'Your license | Vybrant', description: 'Sign in to get your free Vybrant license.', noindex: true },
  { path: '/404', file: '404.html', title: 'Page not found | Vybrant', description: 'There is no page at this address.', noindex: true },
];

const attr = (s) => s.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');
for (const p of pages) {
  const head = [
    `<title>${attr(p.title)}</title>`,
    `<meta name="description" content="${attr(p.description)}" />`,
    p.noindex ? '<meta name="robots" content="noindex" />' : `<link rel="canonical" href="${SITE}${p.path}" />`,
    '<meta property="og:type" content="website" />',
    '<meta property="og:site_name" content="Vybrant" />',
    `<meta property="og:title" content="${attr(p.title)}" />`,
    `<meta property="og:description" content="${attr(p.description)}" />`,
    p.noindex ? '' : `<meta property="og:url" content="${SITE}${p.path}" />`,
    `<meta property="og:image" content="${SITE}/og.jpg" />`,
    '<meta property="og:image:width" content="1200" />',
    '<meta property="og:image:height" content="630" />',
    '<meta name="twitter:card" content="summary_large_image" />',
    p.data ? `<script type="application/ld+json">${JSON.stringify(p.data).replace(/</g, '\\u003c')}</script>` : '',
  ].filter(Boolean);
  const html = template
    .replace(/<title>[\s\S]*?<\/title>\s*<meta name="description"[^>]*>/, head.join('\n    '))
    .replace('<div id="root"></div>', `<div id="root">${render(p.path)}</div>`);
  // index.html changed shape: say so instead of shipping pages without their tags or content
  if (!html.includes(head[2]) || html.includes('<div id="root"></div>')) throw new Error(`prerender: could not fill ${p.file}`);
  writeFileSync(`dist/${p.file}`, html);
  console.log(`prerendered ${p.path} -> dist/${p.file}`);
}
rmSync('dist-ssr', { recursive: true });
