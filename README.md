# MusicThingy

A Linux-native digital audio workstation with a pattern-based workflow: Channel Rack + step sequencer, Piano Roll,
Playlist arrangement, a routable Mixer with sends/sidechains, native instruments and effects, VST2/VST3/LV2 hosting,
automation, audio/MIDI recording and offline rendering. Original UI, original DSP.

## Building

Dependencies (all available from common distro repositories):

| Purpose | Library |
|---|---|
| UI | Qt 6 (Core, Gui, Widgets) |
| Audio I/O | PortAudio (ALSA / JACK / PipeWire host APIs) |
| MIDI | ALSA sequencer (`libasound`) |
| Audio files | libsndfile, libsamplerate |
| Time stretch / pitch | Rubber Band |
| MP3 export | LAME |
| Spectrum analyzer | FFTW3 (single precision) |
| Plugin GUIs | libX11 |
| LV2 hosting | lilv (+ LV2 headers) |

```bash
qmake6 MusicThingy.pro && make -j$(nproc)      # primary build
# or
cmake -B build-cmake && cmake --build build-cmake -j$(nproc)
./bin/musicthingy
```

Command line: `musicthingy [project.mtp] [--render out.wav] [--selftest] [--screenshot file.png] [--quit]`.
`--selftest` runs the built-in test suite (serializer round-trip, undo/redo, every native processor, delay
compensation, routing-cycle detection, automation, offline render). `--scan-plugin <path>` is used internally by the
out-of-process plugin scanner.

Plugin GUIs need an X11 window, so on Wayland the app starts under XWayland (`QT_QPA_PLATFORM=xcb`) unless you set
the platform yourself.

## Tests and code style

```bash
./bin/musicthingy --selftest          # engine / model checks (serializer, undo, processors, render)
python3 tests/ui_test.py              # UI behaviour through the control socket, in an offscreen instance
```

Both run in CI (`.github/workflows/ci.yml`). The UI test uses a private config directory and socket, so it is
safe to run while the app is open. Sources are formatted with `clang-format` (`.clang-format` in the root):

```bash
git ls-files 'src/*.cpp' 'src/*.h' | xargs clang-format -i
```

## Architecture

```
src/
  core/      Types (ticks, ids), lock-free SPSC queues, parameter model with atomics, command-based UndoStack
  project/   Project model (source of truth), JSON serializer with format version + migration, ProjectModel + commands
  dsp/       IProcessor interface shared by native and hosted processors, DSP utilities,
             instruments/ (Sampler, DrumSampler, Synth, Wavetable, FM, ToneGenerator, MIDI out),
             effects/ (dynamics, EQ/filters, delays/reverb, modulation, distortion, utility, analyzer, meter)
  audio/     AudioDeviceManager (PortAudio), AudioEngine (RT callback, transport clock, sequencer, mixer graph,
             PDC, metronome, preview), SnapshotBuilder (Project -> immutable RT graph), RecordingEngine
             (lock-free rings -> disk thread), RenderEngine (offline export), AudioFile (loading, stretching,
             multiresolution waveform cache)
  midi/      MidiEngine (ALSA sequencer I/O thread, clock, controller learn)
  plugins/   Vst3Abi.h / Vst2Abi.h (self-written binary interface declarations), Vst3Plugin, Vst2Plugin, Lv2Plugin adapters,
             PluginHost / PluginDatabase / PluginScanner (out-of-process), PluginEmbedWidget (X11 editor host)
  ui/        App (wires everything; every UI action is a command), MainWindow (dockable workspace), TransportBar,
             BrowserPanel, ChannelRackPanel, PianoRollPanel, PlaylistPanel, MixerPanel, PluginWindow (generic
             parameter editor, custom native editors, plugin GUI embedding), dialogs/
```

### Threading model

* The **Project** is only ever touched on the UI thread through undoable `Command`s pushed to `ProjectModel`.
* Every change requests a coalesced rebuild of an immutable **Snapshot** (RT graph: compiled patterns/notes in ticks,
  clips, mixer tracks in topological order, resolved automation targets, PDC delays). The snapshot is handed to the
  audio thread through a lock-free command queue; the audio thread returns the old one for deletion on the UI thread.
* Continuous controls (volume, pan, sends, processor parameters) are atomics that live in a runtime registry which
  survives rebuilds, so knob moves are immediate and automation writes straight into them from the RT thread.
* The audio callback never allocates, locks, or touches the filesystem: buffers are preallocated per snapshot, MIDI
  event lists and note-off schedulers are fixed-capacity arrays, meters are atomics polled by the UI at ~30 Hz.
* Samples are loaded/resampled/stretched on a thread pool; the RT thread only reads immutable `AudioAsset` data.
* Recording writes into per-track lock-free float rings drained by a disk-writer thread.
* Plugin scanning runs in child processes; plugin `process()` calls are wrapped in a crash guard that bypasses a
  crashing plugin and keeps the project alive.

### Signal flow

```
Channel (instrument) --vol/pan--> mixer track generator input --PDC delay--> [insert chain] --pre sends-->
  --fader/pan--> meter --post sends--> output track ... --> Master --> device (+ metronome, browser preview)
Audio clips / hardware inputs enter the same generator input of their mixer track.
```

Plugin delay compensation: each track reports its chain latency; generators feeding a track are delayed by
`maxPathLatency - pathLatency(track)` and sends get an extra delay when the send destination is shorter than the
main path. Delays are recomputed whenever an effect is added, removed, bypassed or reports a new latency.

## Workflow cheat sheet

* **Channel Rack**: click steps (right-click erases, wheel = velocity, Ctrl+wheel = pitch). Graph editor for
  velocity / pan / pitch / probability / shift per step. Swing, step count and resolution per pattern. Drag samples
  or plugins from the Browser onto the rack. Double-click a channel to open its editor.
* **Piano Roll**: tools 1/2/3 (draw/select/erase); drag note edges to resize; Ctrl+Shift+drag duplicates; Ctrl+Q
  quantize, Ctrl+L legato, Tools menu for arpeggiate/strum/randomize/transpose; chord helper, scale highlighting,
  ghost notes, note repeat; Ctrl+wheel zoom, Shift+wheel scroll, middle-drag pan.
* **Playlist**: click places the selected pattern; slice tool (4) splits, mute tool (5); drag the ruler for the
  loop region; double-click ruler adds markers (right-click); audio clips: fades (handles / dialog), crossfade,
  reverse, normalize, gain, time stretch & pitch (Rubber Band), fit to tempo, transient slicing.
* **Mixer**: double-click an empty slot to add an effect, click to open, drag to reorder, first pixels toggle
  bypass. Select a strip, right-click another strip to route / send / sidechain to it. Inputs per strip for
  recording (arm with R). PDC delay shown next to the pan knob.
* **Automation**: right-click any knob/fader > *Create automation clip* (points editable in the Playlist with
  linear / stepped / smooth / bezier curves), or record automation by moving controls with the record mode set to
  *Automation*.
* **MIDI**: Options > MIDI settings to enable inputs/outputs and MIDI clock; right-click a control > *Link to MIDI
  controller* and move a knob. The typing keyboard (Z–M / Q–P) plays the selected channel.
* **Recording**: arm mixer strips with an input, press Record. Count-in, punch in/out, loop recording with takes
  (each pass becomes a new take track).
* **Export**: File > Export audio (WAV/AIFF/FLAC/MP3/OGG, sample rate, bit depth, mono, normalize, dither, tail).
* Autosave every 2 minutes with crash recovery on start, templates, recent files, project sample collection and a
  missing-file relink dialog.

## Plugins

VST3 bundles (`*.vst3/Contents/x86_64-linux/*.so`) and VST2 `.so` files are discovered in the standard Linux paths
(`~/.vst3`, `/usr/lib/vst3`, `~/.vst`, `/usr/lib/vst`, `/usr/lib/lxvst`, `$VST_PATH`, `$VST3_PATH`) and any folder
added in the Plugin Manager. The host talks to plugins through its own minimal declarations of the public binary
interfaces (`src/plugins/Vst3Abi.h`, `Vst2Abi.h`); no SDK code is included, so redistribution is not bound by the
SDK licences. Plugin state, parameters (by stable id), latency, editors (X11 embedded with a host run loop for VST3)
and MIDI mapping are supported. Failed plugins are listed with the reason and can be hidden or rescanned.

LV2 plugins are found through lilv in `$LV2_PATH`, `~/.lv2`, `/usr/local/lib/lv2` and `/usr/lib/lv2` (read in-process: lilv only
parses the bundles' Turtle). Control ports become parameters; MIDI, transport (`time:Position`), latency, the worker, options and
state (`state:interface`, else the control port values) are supported. X11 UIs are embedded directly; plugins without one get
the generic parameter editor. Mono plugins process the sum of the stereo pair and their output goes to both channels.

## Project format

`.mtp` files are JSON (`formatVersion`, `project`, `transport`, `patterns`, `channels`, `playlist`, `mixer`,
`plugins`, `automation`, `audioReferences`, `markers`, `midiMappings`). Object references are stable ids, sample
paths are stored relative to the project when possible, plugin state is base64. `ProjectSerializer::migrate()`
is the hook for future format versions.
