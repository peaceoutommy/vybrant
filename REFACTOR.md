# Refactor audit and plan

Audit date: 2026-10-10. Baseline: commit `f398677`, **30,733 lines** in `src/` + `tests/` (`src/harmony/` is an empty folder).
This file is the hand-off between sessions: the audit, the plan, and a progress log. Update the log after every step.

## Goal and honest size estimate

Goal: less duplicated logic, one implementation per behaviour (so the same action behaves the same everywhere),
smaller codebase, **no feature removed or changed** (except the bugs listed below, which get fixed on the way).

Honest estimate: removing duplication and dead code saves roughly **1,100 to 1,300 lines (about 4%)**. Most of the
30k lines are real feature code (piano roll tools, mixer, plugin hosting, DSP), not copies. A much bigger cut would mean
removing features, which is out of scope. The biggest single win is item 4 (piano roll + playlist share one base).

## How to verify every step (do this after each item, never batch several items untested)

```bash
make -j16 2>&1 | grep -E "error|warning: unused"          # must print nothing
QT_QPA_PLATFORM=offscreen bin/musicthingy --selftest      # must end "All checks passed"
python3 tests/ui_test.py                                  # only the 2 dock checks below may fail
make install && cmp bin/musicthingy ~/.local/bin/musicthingy   # the user runs the installed copy
```

Known failures that existed before this plan (not caused by it, unrelated to the refactor):
`bottom dock is restored after a restart` and `top dock is restored after a restart` in `tests/ui_test.py`.

Rules while refactoring:
- After adding a header or changing which headers include which, re-run `qmake6 MusicThingy.pro`; after changing a base
  class (`TimelineView`, `Project.h` structs) do `make clean && make`. qmake's dependency list is fixed at qmake time, and a
  stale object with an old class layout crashed the app at startup (2026-10-10).
- Behaviour stays identical unless the item says it fixes a bug. When unsure, keep the old behaviour.
- Each item that fixes a bug first gets a test that fails on the old code (self-test or UI test), then the fix.
- Do not `git checkout` files to undo scratch edits in this repo; back them up and restore instead.
- Count lines before and after each item (`find src tests -name '*.cpp' -o -name '*.h' -o -name '*.py' | xargs wc -l | tail -1`).

## Progress log

| # | Item | Status | Lines after | Notes |
|---|------|--------|-------------|-------|
| 1 | Note `operator==` (Release lane edits lost) | **done** 2026-10-10 | 30,796 total (all 3 items, incl. ~100 test lines) | `Note`/`Step` `operator== = default`; also the step-shape check now keeps notes with a non-default release velocity (steps can't store it), else step-only channels still lost the edit. Tests: 2 UI checks (Release lane, notes + step-only channel), both failed before. |
| 2 | Clip `operator==` (gain / fade-curve drags skip undo) | **done** 2026-10-10 | (see 1) | `Clip` `operator== = default`, `PlaylistView::commitEdit` compares whole clips. Test: UI `plgain` drag + one undo restores gain (failed before: undo removed the clip instead). |
| 3 | One live-control setter (mixer pan automation) | **done** 2026-10-10 | (see 1) | `App::liveControl<T>()` used by channel vol/pan, mixer vol/pan, send level; records `targetValue(t)`, so mixer pan **and send levels** now record automation too. `setEffectMix` lost its unused `commit` parameter. Self-test records vol/pan/send (pan, send failed before). Production code -37 lines net. |
| 4 | `TimelineView` base for piano roll + playlist | **done** 2026-10-10 | 30,454 | New `src/ui/TimelineView.{h,cpp}` (357 lines): tools + one `Tool` enum, snap, x/tick mapping, scrollbars, cached background, zoom / pinch / Ctrl-wheel / sideways scroll, ruler ticks, playhead, rubber band (draw, drag, release), shared key map (`handleSharedKey`), `cutSelected`. `deleteTimeSpace/deleteLoopSpace` -> `deleteSpace` (same for insert). Net -138 lines incl. ~55 new test lines (key-shortcut UI tests for both views; `key`, `plinfo` commands). Fixed on the way: Escape never cleared a selection (the global Escape shortcut took it; views now claim it when something is selected); piano roll menu said Duplicate = Ctrl+D (it is Ctrl+B). Not shared (kept per view on purpose): grid line density, vertical scrolling, nudge/move keys, clipboard data. Possible follow-up: share the horizontal half of `updateScrollbars`. |
| 5 | `channelTypeForKey(key)` + one add-instrument menu | **done** 2026-10-10 | 30,592 | `channelTypeForKey()` in `Project.h` used by App, browser, rack (3x). Rack right-click on empty space now opens the '+' menu (plugin instruments included). -27 lines. |
| 6 | Delete dead code | **done** 2026-10-10 | 30,619 | Removed every function / accessor / signal listed in finding 6, plus `m_channelPdc`, the RT `Xrun` message and its enum value (nothing read it). `LedButton` right-click still does nothing. -177 lines. |
| 7 | Instrument MIDI dispatch + voice stealing helpers | not started | | |
| 8 | Project panel menu duplication | **done** 2026-10-10 | 30,409 (code) | One samples menu for any count (single-sample extras on top), `selection()`, `removeSamples()`, `removeUnusedSamples()`; Delete key + usage count reuse them / `clipsOf()`. Separator for a single sample now sits before 'Remove from project'. -49 lines; UI tests for remove-unused and remove-with-clips + one undo (`samples remove/unused` commands). |
| 9 | Shared rename dialog + pattern actions | **done** 2026-10-10 | 30,406 (8, 9, 13 incl. ~48 test lines) | `askName()` (Controls.h): trims, refuses empty unless allowed - used by all 14 name prompts (patterns, channels, playlist tracks, mixer tracks, clips, markers, templates, presets, envelope presets, themes). `addPatternActions()` (App.h): Rename / Split / Color / Clone for the rack, project panel and browser pattern menus. Fixed: the browser could rename a pattern to an empty name; whitespace-only names were accepted. Browser pattern menu gained Split / Color. Self-tests for both helpers. ~-26 lines. |
| 10 | Drop MIME helpers | **done** 2026-10-10 | 30,449 | `kItemMime`, `hasAppDrop`, `dropItem`, `setDropItem` in `InAppDrag.h`; the format string now appears once (was 12x), payload parsed in one place (was 4x). +12 lines net (helpers > saved lines) plus a new UI test: a sample dropped on the rack adds a channel. |
| 11 | One knob-cell builder | **done** 2026-10-10 | 30,400 | `paramCell` / `knobCell` / `choiceCell` / `toggleCell` (static, PluginWindow.cpp) build the cells of both the generic panel and the audio clip editor; callers set the width. Only -6 lines (builders ~ the copies). Clip editor's Mode label now uses the panel's 1 px spacing (matches the sampler). Screenshots checked. |
| 12 | One mixer-track picker menu | **done** 2026-10-10 | 30,458 | `fillMixerTrackMenu()` (App.h/.cpp) used by the rack (2x) and playlist (2x). Production about -4 lines; +12-line self-test (lists tracks, checks current, reports the pick). |
| 13 | ControlServer mouse-event helper | **done** 2026-10-10 | (see 9) | `sendDrag()` (press, N moves, release; moves sent even for a zero-length drag) and `sendPinch()` replace 8 hand-built gestures. Test-only code, ~-25 lines. |
| 14 | `AudioEngine::releaseAllNotes()` | **done** 2026-10-10 | 30,436 (14-16) | Stop, Pause, Seek, Panic use one RT-safe function (Panic also resets instruments). New self-tests: Stop and Panic each release a held note. |
| 15 | "Place at playhead" helper | **done** 2026-10-10 | (see 14) | `App::firstPlaylistTrack()` replaces 6 copies (browser, project panel x3 + its lambda, control server, self-test). |
| 16 | App `edit<T>()` instead of 5 pass-through wrappers | **done** 2026-10-10 | (see 14) | 6 wrappers (incl. `editLane`) -> `App::edit<T>()`; ~70 call sites renamed (`editClip(` -> `edit<Clip>(` ...). Items 14-16: -18 lines net incl. +16 test lines. |
| 17 | `onPrepare()` calls `reset()` by default | **done** 2026-10-10 | 30,434 | `NativeProcessor::onPrepare()` calls `reset()`; `MT_EFFECT` no longer declares `onPrepare`, only the 12 effects with real prepare work do. Removed 16 effect + 4 instrument trivial definitions. Only -2 lines net (the 12 declarations ate the saving); gain is one rule. |
| 18 | `theme::dimLabel()` | not started | | |
| 19 | One mute cursor | **done** 2026-10-10 | (see 20) | Piano roll and playlist mute modes use `toolCursorFor(MuteTool)`; `theme::muteCursor()` deleted. |
| 20 | Small leftovers | **done** 2026-10-10 | 30,376 (19+20) | `|| true` went in item 4; Phaser `+ L[i] * 0.0f` removed; both `showAreaMenu` use their arguments instead of a fake `QContextMenuEvent`. 19+20: -24 lines. |

Test hooks added for items 1-3: control-server commands `prlane`, `prrel`, `plgain`; `PlaylistView::debugGainHandle`.

Lesson from 10/12/17: helpers for 2-4 short copies cost about what they save. For size, prefer the items with real
volume left: 8 (~-60), 13 (~-60), 9 (~-50), 11 (~-40), 7 (~-80, real-time code).

Suggested order: 1, 2, 3 (bugs, small) -> 6 (dead code, makes the rest easier to read) -> 5, 14, 15, 16, 17 (small,
low risk) -> 10, 12, 9, 8, 11, 13 (UI helpers) -> 7 (DSP, real-time code: careful) -> 4 (large, sub-steps) -> 18-20.

---

## Findings

Load assumed: one person using a desktop app. The real-time audio thread must never allocate or lock, so DSP refactors
(item 7, 14) must keep that property.

### Must fix

**1. Edits in the piano roll's "Release" lane are thrown away** (`src/ui/PianoRollPanel.cpp:664-705`)
- What this is: `commitEdit` compares the notes before and after a drag and only saves an undo step if something changed.
- Problem: the comparison lists fields by hand and leaves out `releaseVelocity` and `channel`. Dragging in the Release
  lane changes only `releaseVelocity`, so the edit counts as "no change" and the notes are put back on mouse release.
  The same hand-written comparison appears twice in the function.
- Fix: `bool operator==(const Note&) const = default;` (and on `Step`) in `Project.h`, then compare
  `after.notes == m_before.notes && after.steps == m_before.steps`. Deletes ~20 lines. Add a UI test: switch the lane to
  Release, drag, check the value stuck.
- If we skip it: the Release lane does nothing for the user.

**2. Dragging a clip's gain handle or a fade's curve skips undo, playback and "unsaved changes"** (`src/ui/PlaylistPanel.cpp:703-735`, drags at `:1111-1133`)
- What this is: the playlist edits clips in place during a drag, then `commitEdit` turns the change into an undo step.
- Problem: its comparison checks start, length, track, offset, fade lengths and mute only, not `gain`,
  `fadeInShape` or `fadeOutShape`. A gain-handle drag (or an up/down fade-curve drag) is seen as "no change": no undo
  step, no change notice, so the engine keeps playing the old gain and the project is not marked unsaved. Closing the
  app then loses the change without asking.
- Fix: `bool operator==(const Clip&) const = default;` in `Project.h`; compare `after == it->second`. Add a UI test that
  drags the gain handle and checks the undo text / the clip gain after a reload.
- If we skip it: silent lost edits, and what you hear differs from what you see.

**3. Mixer pan is not recorded as automation** (`src/ui/App.cpp:1049-1053`)
- What this is: with the record mode set to Automation, moving a control records automation points.
- Problem: `setChannelVolume`, `setChannelPan` and `setMixerVolume` record points; `setMixerPan` does not, although
  `ParamTarget::MixerPan` exists. The five live-control setters (channel vol/pan, mixer vol/pan, send level) are
  copy-pasted, which is how one was missed. `setEffectMix`'s `commit=false` path is never used (only called with true).
- Fix: one private helper `liveControl(atomic, target, normalizedForAutomation, commitFn, liveFn)` used by all five;
  mixer pan then records like the rest. Remove the unused `commit` branch of `setEffectMix`.
- If we skip it: recorded mixes miss pan moves, and the next copy-paste setter will repeat the mistake.

### Should fix

**4. The piano roll and the playlist are two copies of one timeline widget** (`src/ui/PianoRollPanel.cpp`, `src/ui/PlaylistPanel.cpp`, 2,067 + 2,027 lines)
- What this is: both views are a scrolling, zoomable time grid with a ruler, a playhead, tools, a rubber-band
  selection, snapping, a cached background and the same keyboard shortcuts.
- Problem: all of this is written twice, nearly line for line: `snapTicks/snap/snapNear` (a 3rd copy in `App::snapTicks`,
  a 4th in `PlaylistView::snapRound`), `setTool/toolCursorFor`, scrollbar creation and `valueChanged` wiring, the cached
  background pixmap + `invalidate()`, `zoomTimeAt` + pinch `event()` + Ctrl-wheel zoom, ruler painting (only the tick
  heights differ: 6/11/15 vs 8/13/17 px), playhead line, rubber-band drawing and hit logic, the key map
  (Delete / Ctrl+Delete / Ctrl+Insert / Ctrl+C/X/V/B/D/A, tool letters), copy/cut skeleton. Fixes have to be made twice
  (the trackpad zoom fix this week was).
- Fix: a `TimelineView : QWidget` base in `src/ui/TimelineView.{h,cpp}` that owns scroll/zoom/snap/cache/ruler/playhead/
  rubber band/tool cursor/key map, with virtual hooks (`headerWidth()`, `maxExtent()`, `deleteSelected()`, `copySelected()`,
  `paste()`, `duplicateSelected()`, `selectAll()`, `deleteSpace()`, `insertSpace()`, `paintContent()`). `App::snapTick`
  stays the one snap rule. Do it in sub-steps, building and running all tests after each:
  4a snap family + tool cursor; 4b scrollbars + cache + invalidate; 4c zoom/pinch/wheel; 4d ruler + playhead painting;
  4e rubber band; 4f key map; 4g copy/cut/paste skeleton; 4h time-space insert/delete (template over notes/clips).
  Expected: -400 to -500 lines.
- If we skip it: every timeline fix costs double and the two editors drift apart.

**5. "Which channel type is this instrument?" is decided in five places** (`App.cpp:695`, `BrowserPanel.cpp:754`, `ChannelRackPanel.cpp:700,988,1064`)
- What this is: adding a channel from an instrument key (`native.drumsampler`, `native.tonegen`, ...) needs a `ChannelType`.
- Problem: the key-to-type chain is copied five times, and the "Add channel" instrument menu twice in the rack. The copies
  already differ: right-clicking empty rack space shows "Add channel" **without** the "Plugin instruments" submenu that the
  rack's "+" menu has.
- Fix: `ChannelType channelTypeForKey(const QString&)` next to `ChannelType` in `Project.h`; the rack's right-click
  opens the same `m_addMenu` (emit `addChannelRequested`). ~-50 lines.
- If we skip it: a new instrument type must be added in five places, and menus keep differing.

**6. Dead code** (verified by grepping the whole tree incl. `tests/`)
- Never called: `App::checkPluginAvailability` (`App.cpp:382`), `App::movePattern`, `App::setSnap`,
  `PianoRollView::cancelEdit`, `PlaylistView::gainY`, `RackView::stepsPerBar`, `TransportBar::indexFromSnapDivision`,
  `MidiEngine::sendNow`, `AudioDeviceManager::defaultInput/defaultOutput`, `SampleCache::isLoading`,
  `AudioEngine::applyAutomation` (empty body), `theme::deleteCursor` (`Theme.cpp:402`), `ticksToSamples` /
  `samplesToTicks` (`Types.h`).
- Unused inline accessors: `activeGeneration, atomicFor, channelPdc, forget (Anim.h), hbar, history, isOffline, isRunning,
  isSmoothing, learning, notifyClean, patternLoopLength, pendingCount, pendingPaths, pluginHost, rebuildPending, recorder,
  redoText, resetXruns, samplesProcessed, setPrefix, setPreviewGain, setWidth, takeCounter`.
- Signals emitted but never connected: `AudioEngine::xrunOccurred`, `SampleCache::loadFailed`,
  `MidiEngine::noteReceived`, `MidiEngine::portsChanged`, `ProjectModel::projectReplaced`, `App::recordingFinished`,
  `LedButton::rightClicked`. Check each with `grep -rn` once more right before deleting (a connect may use the
  `SIGNAL()` string form or a lambda name).
- Fix: delete them. ~-170 lines. If we skip it: readers keep tracing code that never runs.

**7. Every instrument repeats the MIDI loop and the voice-stealing rule** (`src/dsp/instruments/Instruments.cpp`)
- What this is: each instrument walks the sample-accurate MIDI events and picks a voice for each note-on.
- Problem: the `while (mi < midi.count && midi.events[mi].sampleOffset <= i)` dispatch is copied in all 6 instruments;
  "take a free voice, otherwise steal the oldest" is written 3 times inline (Wavetable, FM) plus `Synth::allocVoice`,
  with Sampler (quietest) and DrumSampler (furthest along) using their own steal rule. Wavetable/FM note-offs ignore
  `noteId` while Sampler honours it.
- Fix: in `InstrumentBase`: `template <class F> void dispatchMidi(const MidiEventList&, int& mi, int i, F&& onEvent)` and
  `template <class V, class Worse> V* pickVoice(std::array<V,N>&, Worse)`. Real-time safe (no allocation). ~-80 lines.
  Render a short note through each instrument before and after (the self-test already does energy checks) and compare.
- If we skip it: voice handling fixes must be made five times.

**8. The project panel's menus repeat themselves** (`src/ui/ProjectSamplesPanel.cpp:328-415`)
- Problem: "Remove all unused samples" is written as a lambda **and** again inline; "Remove from project" exists for one
  sample and for several, nearly identical; the one-sample menu repeats the several-samples menu entries. Usage counting
  ("is this sample on the playlist") is done in `rebuild()`, `clipsOf()` and both remove-unused copies.
- Fix: one menu for a list of paths (the single case is a list of one); one `removeSamples(paths)`; one `removeUnused()`;
  reuse `clipsOf()` for counting. ~-60 lines.

**9. Rename dialogs and pattern actions are copied, with different rules** (`ChannelRackPanel.cpp:884,1038`, `MainWindow.cpp:559`, `ProjectSamplesPanel.cpp:293`, `PlaylistPanel.cpp:1290,1663`, `MixerPanel.cpp:495,579`)
- Problem: rename-pattern exists 3 times, rename-playlist-track 2 times, rename-mixer-track 2 times; empty names are
  refused for patterns but meaningful for mixer tracks, decided per copy. The pattern menu (Rename / Color / Clone /
  Split by channel) is built separately in the rack, the project panel and the main menu.
- Fix: `std::optional<QString> askName(QWidget*, title, current)` in `Controls.h`; `addPatternActions(QMenu&, ids)` used
  by the three menus. ~-50 lines.

**10. Drag-and-drop format handled by hand everywhere** (12 uses of `"application/x-musicthingy-item"`)
- Problem: the MIME string is repeated 12 times and the `"kind|value"` payload is parsed in 4 places (rack, playlist,
  mixer slots, project panel); each target also repeats the accept boilerplate.
- Fix: in `InAppDrag.h`: `kItemMime`, `bool hasAppItem(const QMimeData*)`, `std::pair<QString,QString> appItem(const
  QMimeData*)`, `QStringList droppedAudioFiles(const QMimeData*)`. ~-40 lines.

**11. The knob cell is built twice** (`src/ui/PluginWindow.cpp:60-145` and `:440-500`)
- Problem: the generic parameter panel and the audio clip editor each build "knob + value label + fixed cell + typed
  value" separately; they must look identical and line up.
- Fix: one `KnobCell` builder in `Controls.h/.cpp` used by both. ~-40 lines.

**12. The "pick a mixer track" menu is built four times** (`ChannelRackPanel.cpp:465,913`, `PlaylistPanel.cpp:807,1678`)
- Fix: `fillMixerTrackMenu(QMenu*, const Project&, Id current, std::function<void(Id)>)`. ~-25 lines.

**13. The control server builds mouse events by hand 15 times** (`src/ui/ControlServer.cpp`)
- Problem: press / move steps / release blocks are copied into `prclick, prdrag, prmouse, pldrag, rackclick, browserplus...`.
- Fix: one `sendDrag(QWidget*, QPoint from, QPoint to, Qt::MouseButton, Qt::KeyboardModifiers, int steps)`. Test-only
  code, so the UI test is the check. ~-60 lines.

**14. "Release every note" is written four times in the audio thread** (`src/audio/AudioEngine.cpp:334-389`)
- Problem: Stop, Pause, Seek and Panic each loop over the channels clearing note-offs and queueing AllNotesOff.
- Fix: `void releaseAllNotes(bool resetInstruments)`. ~-20 lines. Real-time safe (no allocation).

**15. "Put it on the first playlist track at the playhead" is written five times** (`BrowserPanel.cpp:808`, `ProjectSamplesPanel.cpp:112,280,388`, `ControlServer.cpp:374`)
- Fix: `Id App::firstPlaylistTrack()` (creates one if none). ~-20 lines.

### Nice to have

**16. Five pass-through wrappers in App** (`App.h:124-157`: `editPattern, editTrack, editClip, editAudioSource, editMixerTrack`)
- Fix: one `template <class T> void edit(Id, desc, fn, mergeId = 0)`; mechanical rename at call sites. ~-15 lines.

**17. `onPrepare() { reset(); }` written in 17 processors**
- Fix: `NativeProcessor::onPrepare()` calls `reset()` by default (prepare runs on the UI thread, so this is safe). ~-17.

**18. Dim label boilerplate** (~20 sites of `new QLabel` + `theme::styled(.., "color:@textDim@...")`)
- Fix: `theme::dimLabel(text, parent, extraCss = {})`. ~-20.

**19. Two different mute cursors** (`Theme.cpp:418` `muteCursor()` used once in `PianoRollPanel.cpp:1224`; everywhere
else the mute tool uses `toolCursor(Glyph::MuteSpeaker)`)
- Fix: use `toolCursor(Glyph::MuteSpeaker)` and delete `muteCursor()`. ~-17, and the mute mode looks the same everywhere.

**20. Small leftovers**: `if (proj.transport.loopEnabled || true)` (`PlaylistPanel.cpp:538`), `+ L[i] * 0.0f`
(`Effects.cpp:827`), and the fake `QContextMenuEvent` built only to read a position in both `showAreaMenu` functions.

Not worth extracting (checked): effects DSP (already shares `DspUtils.h`, `syncDivisionSeconds`, macro class decls),
dialog button boxes (standard Qt idiom), serializer load/save pairs (symmetric by nature, covered by the round-trip
test), VST2/VST3 MIDI conversion (different APIs), ALSA MIDI code.

Not checked in depth: `Vst3Abi.h` / `Vst2Abi.h` (binary interface declarations), `Dialogs.cpp` beyond its structure,
`Theme.cpp` glyph drawing, the real-time math in `AudioEngine.cpp` / `SnapshotBuilder.cpp`, `PluginHost.cpp` scanner,
`MidiEngine.cpp`.

---

## What each file does

| File | Lines | Job |
|---|---|---|
| `src/main.cpp` | 167 | Entry point: CLI flags (`--selftest`, `--render`, `--scan-plugin`...), forces xcb, starts App + MainWindow. |
| `src/SelfTest.cpp` | 540 | Built-in test suite run by `--selftest` (model, serializer, undo, DSP, engine, UI helpers). |
| **core/** | | |
| `core/Types.h` | 39 | Ticks/PPQ, ids, time conversions, `clamp`. |
| `core/Parameter.h` | 167 | `ParamDesc` + `ParameterSet` (atomic values, normalized mapping) for every processor. |
| `core/RingBuffer.h` | 87 | Lock-free SPSC queues (UI <-> audio thread). |
| `core/UndoStack.h` | 77 | `Command` + undo/redo stack with merging. |
| `core/Log.h` | 23 | Log sink. |
| **project/** | | |
| `project/Project.h` | 390 | The data model: patterns, notes, steps, channels, clips, mixer, automation, audio sources, transport. |
| `project/Project.cpp` | 179 | Pattern length, default/demo project, routing cycle check. |
| `project/ProjectModel.h/.cpp` | 305+109 | Owns the Project; generic edit/insert/remove/reorder commands; change notifications. |
| `project/ProjectSerializer.h/.cpp` | 19+519 | JSON save/load with format version + migration. |
| **dsp/** | | |
| `dsp/Processor.h/.cpp` | 123+53 | `IProcessor` interface, `MidiEvent(List)`, `NativeProcessor` base, value formatting. |
| `dsp/DspUtils.h` | 395 | ADSR, filters (SVF/biquad), LFO, delay lines, PDC delay, fades, RNG. |
| `dsp/NativeRegistry.h/.cpp` | 31+77 | Key -> factory for native instruments/effects. |
| `dsp/instruments/Instruments.h/.cpp` | 432+1112 | Sampler (stretch modes), DrumSampler, Synth, Wavetable, FM, ToneGenerator, MIDI out. |
| `dsp/effects/Effects.h/.cpp` | 387+1280 | 30 native effects (dynamics, EQ, delays, reverb, modulation, distortion, utility, analyzer, meter). |
| **audio/** | | |
| `audio/AudioEngine.h/.cpp` | 276+1065 | RT callback: commands, transport, sequencer, live MIDI, mixer graph, PDC, clips, metronome, preview. |
| `audio/SnapshotBuilder.cpp` | 520 | Project -> immutable RT Snapshot; runtime registry of processors and controls. |
| `audio/Runtime.h` | 243 | Snapshot / Rt* structs, note-off scheduler, engine commands/messages. |
| `audio/AudioFile.h/.cpp` | 89+358 | Load/resample/stretch (Rubber Band modes), BPM from names, sample cache, waveform peaks. |
| `audio/AudioDeviceManager.h/.cpp` | 71+193 | PortAudio device list and stream. |
| `audio/PreviewPlayer.h/.cpp` | 44+71 | Streaming browser preview loader. |
| `audio/RecordingEngine.h/.cpp` | 72+143 | Recording rings + disk writer thread, takes. |
| `audio/RenderEngine.h/.cpp` | 34+117 | Offline export through the same engine. |
| **midi/** | | |
| `midi/MidiEngine.h/.cpp` | 67+253 | ALSA sequencer in/out thread, clock, CC learn. |
| **plugins/** | | |
| `plugins/Vst3Abi.h`, `Vst2Abi.h` | 516+201 | Self-written plugin binary interfaces. |
| `plugins/Vst3Plugin.h/.cpp` | 108+1103 | VST3 host adapter (buses, params, events, state, editor). |
| `plugins/Vst2Plugin.h/.cpp` | 74+370 | VST2 host adapter. |
| `plugins/PluginHost.h/.cpp` | 104+411 | Plugin database, out-of-process scanner, yabridge helpers, instance creation. |
| `plugins/PluginEmbedWidget.h/.cpp` | 41+96 | X11 window for plugin editors; keyboard-focus reclaim for typing mode. |
| **ui/** | | |
| `ui/App.h/.cpp` | 253+1604 | Owns all subsystems; every user action as an undoable command; transport, autosave, presets, automation, MIDI learn, recording glue. |
| `ui/MainWindow.h/.cpp` | 124+1303 | Docks/workspace, menus + shortcuts, typing keyboard, editor windows, layout save, rack docking. |
| `ui/TransportBar.h/.cpp` | 72+332 | Top bar: transport, tempo, time sig, pattern picker, snap, hint box, stats. |
| `ui/BrowserPanel.h/.cpp` | 169+871 | File/instrument/plugin browser, search index, preview bar, '+' add button, drag source. |
| `ui/ProjectSamplesPanel.h/.cpp` | 100+500 | Project strip: samples / patterns / automation tabs, menus, drops, details. |
| `ui/ChannelRackPanel.h/.cpp` | 143+1197 | Channel rack: step grid, graph editor, channel menus, drops. |
| `ui/PianoRollPanel.h/.cpp` | 232+2067 | Piano roll view (notes, lanes, tools, scales, chords) + its toolbar panel. |
| `ui/PlaylistPanel.h/.cpp` | 198+2027 | Playlist view (clips, fades, gain, automation points, brush, markers) + toolbar panel. |
| `ui/MixerPanel.h/.cpp` | 211+826 | Mixer strips, effect slots, routing wires, sends, effect picker. |
| `ui/PluginWindow.h/.cpp` | 91+602 | Editor windows: generic knob panel, plugin window, audio clip editor. |
| `ui/ProcessorEditors.cpp` | 750 | Custom native editors: Sampler, DrumSampler pads, EQ curve, spectrum, meter. |
| `ui/ControlServer.h/.cpp` | 23+494 | Local socket commands for scripting and the UI test. |
| `ui/CommandRegistry.h/.cpp` | 47+66 | Named commands with user-editable shortcuts. |
| `ui/InAppDrag.h/.cpp` | 12+149 | In-app drag loop (badge, targets get normal drag events). |
| `ui/Theme.h/.cpp` | 162+710 | Palettes, stylesheet tokens, glyph icons and cursors, rounded frames. |
| `ui/Anim.h` | 184 | Shared animation ticker / easing. |
| `ui/widgets/Controls.h/.cpp` | 204+456 | Knob (snap, typed values), Fader, LevelMeter, DragValue, LedButton, toast. |
| `ui/widgets/WaveView.h` | 142 | Waveform view shared by the sampler and clip editors. |
| `ui/dialogs/Dialogs.h/.cpp` | 140+863 | Audio/MIDI settings, plugin manager, export, missing files, shortcuts, autosave, project info, themes. |
| `tests/ui_test.py` | 258 | UI behaviour tests through the control socket in an offscreen instance. |
