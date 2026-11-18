# runbook: 88emuPlayer VST3 on Apple Silicon macOS
last-verified: 2026-11-08

## trigger
Build, install, or verify 88emuPlayer as an Ableton Live VST3 instrument, keeping the standalone app available.

## prereqs
- Apple Silicon Mac, macOS 14 or later, Apple command-line tools, CMake 3.25+ and Ninja.
- A recursive checkout: `git submodule update --init --recursive`.
- Your own supported ROMs in `~/Documents/The Usual Suspects/88emuPlayer/roms/`. Subfolders are scanned. No ROMs are included in this repository or the plugin.
- Native Apple Silicon Ableton Live for the Live checks below. This arm64 build does not load into an Intel/Rosetta host.

## steps
From the repository root:

```sh
cmake --preset 88emu-macos-arm64
cmake --build --preset 88emu-macos-arm64 -j 6
codesign --force --deep --sign - bin/plugins/Release/VST3/88emuPlayer.vst3
codesign --force --deep --sign - bin/plugins/Release/Standalone/88emuPlayer.app
codesign --verify --deep --strict bin/plugins/Release/VST3/88emuPlayer.vst3
codesign --verify --deep --strict bin/plugins/Release/Standalone/88emuPlayer.app
```

If using the temporary tools installed for this task, first run `export PATH=/private/tmp/88-midi-build-tools/bin:$PATH`. That environment is temporary; install CMake/Ninja normally for future builds.

The bundles are `bin/plugins/Release/VST3/88emuPlayer.vst3` and `bin/plugins/Release/Standalone/88emuPlayer.app`; the existing CLI remains `bin/plugins/Release/88EmuCli`. Ad-hoc signing is suitable for this local build; it is not Developer ID notarization.

With Live closed, install the VST3 into the per-user system plugin folder:

```sh
mkdir -p "$HOME/Library/Audio/Plug-Ins/VST3"
ditto bin/plugins/Release/VST3/88emuPlayer.vst3 "$HOME/Library/Audio/Plug-Ins/VST3/88emuPlayer.vst3"
```

In Live, enable VST3 system folders in the plug-in preferences and rescan. Load **88emuPlayer** on a MIDI track. Live supplies audio routing, sample rate and incoming MIDI; the standalone's audio/MIDI device settings are hidden in the plugin. Host MIDI reaches group A, channels 1–16. Extra hardware part groups are retained for compatibility with the existing interface, not exposed as separate Ableton MIDI ports in this minimal VST3.

A new instance chooses an available ROM-backed board automatically. Keep ROMs and any PCM card files in place when reopening projects. Missing ROMs produce silence with the existing missing-ROM interface; saved setup is retained so adding ROMs and restarting the selected board can recover it.

## verify

```sh
ctest --test-dir build/88emu-macos --output-on-failure \
  -R '^(88emuPanelState|88emuProjectState|88emuVst3Host|hardwareDevice|partState|setupRestore|mt32Memory|resamplerLatency|dynamicSamplerate|88emuLaunchOptions)$'
```

The VST3 host test uses `EMU88_TEST_ROM_DIR` when set. It returns CTest skip code 77 if that folder is absent. To save stereo verification WAVs:

```sh
build/88emu-macos/source/ronaldo/88emu/88emuplayer/88emuVst3Host_test_artefacts/Release/88emuVst3Host_test \
  "$PWD/bin/plugins/Release/VST3/88emuPlayer.vst3" \
  "$HOME/Documents/The Usual Suspects/88emuPlayer" "$PWD/build/verification"
```

JUCE's macOS temporary folders are under `~/Library/Caches`; a restricted coding sandbox may need permission to run these tests. The host test isolates writable data in a temporary folder and links the ROM folder for reading. It does not open an editor or pump UI timers during audio checks.

Automated checks cover real VST3 discovery, stereo output, ROM-based sound, MIDI sample offsets, different block partitions, project-state reopening before an editor, independent instances and realtime/offline render comparison at 44.1/48 kHz. Unit tests also cover missing ROMs, invalid state, zero/variable-sized callbacks, dynamic resampling, and GS/LA state builders. The GS builder tests exercise all 17 supported GS model enums; actual firmware audio checks use the ROMs available on this Mac.

The two-editor smoke test also opened and closed both hosted native editor peers. Its component PNGs are black because the embedded native view is not captured, so visual UI correctness still needs the check below. Live was inspected read-only with an existing Set open and its audio engine off; no Set or Live settings were changed.

### Required checks inside Ableton Live
These are manual acceptance checks; a JUCE test host cannot verify Live's own project and export behavior.

1. **Scan/interface:** rescan system VST3 folders, insert 88emuPlayer, open its existing front panel and Part Editor. Verify no missing-ROM warning and audible stereo output.
2. **MIDI timing:** make a clip with short notes on beats and notes near clip/loop boundaries. Render at buffer sizes 64, 128 and 512 with Delay Compensation enabled and no track delay. Compare alignment after allowing the same patch's hardware attack. Stop, seek and loop repeatedly; check for stuck notes, missing notes or duplicate notes.
3. **Two instances:** use two tracks with different programs, part levels, player output gains and window scales. Open both editors. Changing one must leave the other unchanged.
4. **Saved project:** save the Set, quit Live completely, reopen it, and play/export before opening either editor. Confirm model, program, level, pan, effects, output gain and each instance's UI settings. Also duplicate a configured track and verify its settings are copied independently.
5. **Offline export:** export the same region at 44.1 and 48 kHz, then Freeze/Flatten a copy. Compare with playback or a realtime recording; listen for silence, missing starts, stuck tails, timing shifts or changes of sound. Include a note at the region start and allow a suitable reverb tail.
6. **Standalone regression:** launch the new `.app`, select the existing output/MIDI devices, play a MIDI file and a live input, change a part, quit and reopen. Confirm standalone audio-device settings are retained.

### State boundaries
- Project state preserves player output/resampler/analog/boot/card/UI settings and tracked GS or MT-32/CM instrument configuration.
- On the recognized SC-55mkII firmware, front-panel instrument bank/program and MIDI receive-channel assignments are captured automatically for all 16 parts. Part Editor and panel edits also tell Live that plugin state needs saving.
- Other physical front-panel settings and boards still require Part Editor **Receive All** before saving; MIDI/Part Editor edits are tracked automatically. Do not save while Receive All is still running. Unknown firmware layouts are not read using guessed memory addresses.
- Active notes, evolving reverb, CPU execution state and transient controller gestures are not captured. Put required performance controllers at the start of the MIDI clip.
- MV-30, PRO-E, NU-10 and MIIG5 full instrument setup recall is not implemented; their selected model and player settings are retained.
- ROMs and PCM cards are external references. Moving or deleting them prevents equivalent sound on reopen.

## rollback
Close Live and remove only `~/Library/Audio/Plug-Ins/VST3/88emuPlayer.vst3`, then rescan plugins. The standalone bundle and ROMs remain available. The unmodified standalone build from this task is retained at `build/88emu-baseline/88emuPlayer.app`.

## links
- [State and wrapper decision](adr/0001-88emu-vst3.md)
- [Original 88emu documentation](88emu_readme.md)
- [Work plan and verification results](../tasks/todo.md)
