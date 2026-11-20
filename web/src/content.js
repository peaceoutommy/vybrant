// All site copy lives here, Vybrant's own wording from the previous site.
// Images are Vybrant's screenshots, copied into public/. art = [colour A, colour B, pattern] for tiles without a picture.

const SITE = 'https://vybrant.studio'

export const nav = [
  { id: 'home', label: 'Home' },
  { id: 'features', label: 'Features' },
  { id: 'start', label: 'Start' },
  { id: 'requirements', label: 'Requirements' },
  { id: 'download', label: 'Download' },
  { id: 'account', label: 'Account' },
]

export const hero = {
  label: 'Vybrant',
  tagline: ['A digital audio workstation', 'for Windows and Linux'],
  word: 'free',
  cta: { label: 'Download', href: '#download' },
  shot: { img: '/showcase-playlist.webp', alt: 'The playlist: patterns, an audio loop and automation clips arranged over 24 bars' },
  // the big words, bottom left; they swap every few seconds
  pairs: [['From first', 'loop'], ['to finished', 'track.']],
}

export const license = {
  title: 'No trial, no card, no catch.',
  body: 'Sign in with Google or your email; one free license per account. Paste it in when Vybrant asks, and it works offline from then on.',
  cta: { label: 'Get your free license', href: '#account' },
  // the app's own license dialog, rendered from vybrant app/src/License.cpp with the app's theme
  shot: { img: '/license-dialog.webp', alt: 'Vybrant’s license dialog: paste your license key, then Activate' },
}

// the app as it looks: the bento next to the license card
export const screens = [
  { tag: 'Channel rack', title: 'Program beats on the step grid and parts in the piano roll', img: '/showcase-pattern.webp', href: '#features', wide: true },
  { tag: 'Playlist', title: 'Arrange the song from patterns, audio and automation', img: '/showcase-playlist.webp', href: '#features', wide: true },
  { tag: 'Mixer', title: 'Route each track through inserts, sends and buses', img: '/showcase-mixer.webp', href: '#features' },
  { tag: 'Effects', title: 'EQ, dynamics, delay, reverb and modulation to a track', img: '/showcase-plugins.webp', href: '#features' },
  { tag: 'Instruments', title: 'Built-in samplers, drums and three synths', img: '/showcase-instruments.webp', href: '#features' },
]


export const features = {
  eyebrow: 'What’s inside',
  title: 'Everything a track needs.',
  body: 'Write, arrange, record and mix in one app, with instruments and effects built in and your own plugins alongside them.',
  items: [
    { title: 'Patterns and piano roll', text: 'Sketch beats in the step sequencer, then play parts into the piano roll, with the other instruments faintly behind the one you edit.', img: '/feature-pattern.webp', alt: 'The channel rack over the piano roll: drum steps, bass, chords and a lead melody' },
    { title: 'Mixer', text: 'Inserts, sends and sidechains, buses, freeze and delay compensation.', img: '/feature-mixer.webp', alt: 'Mixer strips with their effects, and the send from the chords to the reverb' },
    { title: 'Playlist', text: 'Arrange patterns, audio and automation on as many tracks as the song needs.', img: '/feature-playlist.webp', alt: 'A song arranged in the playlist: drums, bass, chords, lead, an audio loop and two automation lanes' },
    { title: 'Built-in sound', text: 'Samplers, drums and three synths, plus EQ, dynamics, delay, reverb and modulation.', img: '/feature-eq.webp', alt: 'The parametric EQ’s curve over a live spectrum' },
    { title: 'Your plugins', text: 'Each one can run in its own process, so a crash stays contained and the song keeps playing.', art: ['#4c1d95', '#1e1b4b', 'rings'], mark: 'VST3, VST2, CLAP, LV2' },
    { title: 'Recording', text: 'Loop recording with takes and comping, count-in, punch in and out.', art: ['#581c87', '#0c0814', 'lines'], mark: 'Take 1, 2, 3' },
    { title: 'Export', text: 'Mixdowns, stems and MIDI files, at any sample rate.', art: ['#2e1065', '#6b21a8', 'fog'], mark: 'Song.wav, Song.mid' },
  ],
}

export const start = {
  eyebrow: 'Get started',
  title: 'Three steps to your first track.',
  body: 'No trial, no card, no catch: one free license per account.',
  steps: [
    { title: 'Download Vybrant', text: 'For 64-bit Linux and Windows, the same file for every update.', href: `${SITE}/download` },
    { title: 'Get your license', text: 'Sign in with Google or your email; one free license per account.', href: '#account' },
    { title: 'Make your first track', text: 'Paste your license in when Vybrant asks, then start making music. It works offline from then on.' },
  ],
}

export const requirements = {
  eyebrow: 'Requirements',
  title: 'What it needs.',
  body: 'A recent 64-bit system and an audio device.',
  columns: ['', 'Windows', 'Linux'],
  rows: [
    ['System', 'Windows 10 or 11, 64-bit', 'Any 64-bit (x86_64) distribution from 2024 on: Arch, CachyOS, Manjaro, Ubuntu 24.04+, Fedora 40+, Debian 13+ (glibc 2.39 or newer)'],
    ['Audio', 'Any audio device: WASAPI, DirectSound or MME', 'PipeWire, JACK or ALSA; ALSA for MIDI'],
    ['Plugins', 'VST3, VST2 and CLAP, 64-bit', 'VST3, VST2, CLAP and LV2, 64-bit'],
    ['AppImage', '—', 'FUSE 2 (libfuse2), shipped by most distributions'],
    ['Plugin windows', '—', 'X11 or XWayland'],
  ],
}

export const download = {
  eyebrow: 'Download',
  title: 'Make your next track here.',
  body: 'Free on Linux and Windows: every feature, one license per account.',
  // the buttons link straight to the files in the latest release (useLatestRelease in App.jsx)
  builds: [
    { name: 'Windows', os: 'windows', kind: '64-bit Windows 10 and 11' },
    { name: 'Linux', os: 'linux', kind: '64-bit Linux' },
  ],
  license: { label: 'Get your free license', note: 'One free license per account', href: '#account' },
  unavailable: 'Downloads are unavailable right now, try again in a minute.',
}

// Sign-in and the license itself come from Vybrant's license API (see Account in App.jsx)
export const account = {
  eyebrow: 'Account',
  title: 'Your license',
  body: 'Your account comes with one free license. Generate it once and use it on your computers.',
  signedOut: 'Sign in with Google or your email; one free license per account.',
  signIn: 'Sign in',
  signOut: 'Sign out',
  loading: 'Connecting…',
  unavailable: 'Sign-in isn’t available right now, try again in a minute.',
  looking: 'Looking for your license…',
  generate: 'Generate license',
  generating: 'Generating…',
  copy: 'Copy license',
  copied: 'Copied',
  download: { label: 'Download Vybrant', href: '#download' },
  hint: 'Open Vybrant and paste it when asked, or later under Help → License…',
}

export const cookies = { text: 'We only use the cookies that keep you signed in. No tracking, no ads.', ok: 'Got it' }

export const notFound = {
  code: '404',
  title: 'Page not found | Vybrant',
  heading: 'Nothing here.',
  body: 'This address doesn’t lead to a page on vybrant.studio.',
  cta: 'Go to the home page',
}

export const footer = { brand: 'vybrant' }
