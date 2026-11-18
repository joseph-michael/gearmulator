# 88emuPlayer VST3 for macOS

## Scope and assumptions
- Start from upstream `dsp56300/gearmulator`; the supplied workspace was empty.
- Reuse the existing JUCE processor, 88emu engine, and interface. Prefer existing plugin infrastructure where it meets the request.
- Build and verify the original standalone before changing implementation.
- Target native arm64 on this Mac first. ROMs are external user assets and must not be committed.
- No ROM folder has been supplied and Ableton Live was not found in `/Applications`; distinguish automated verification from required user checks.

## Verifiable plan
- [x] Inspect checkout, local instructions, 88emuPlayer, plugin targets, ROM loading, state, MIDI scheduling, and instance ownership. Record findings here.
- [x] Configure and successfully build unmodified `88emuPlayer`; record exact command and artifact.
- [x] Choose the smallest integration using existing JUCE processor/engine/UI, and check the implementation plan before editing source.
- [x] Build a VST3 instrument with host MIDI input and stereo audio output while retaining the standalone target.
- [x] Implement and test versioned project state and independent per-instance settings, including ROM references and restoration before UI creation.
- [x] Verify MIDI sample offsets/block boundaries, variable block sizes, non-realtime rendering, state round trips, malformed state, and two-instance isolation using meaningful automated tests.
- [x] Rebuild standalone and VST3, inspect bundle architecture/metadata, and run available host/validator checks.
- [x] Write build/install/ROM instructions and a concrete Ableton checklist for scanning, MIDI timing, two instances, saved-project reopening, and offline export.
- [x] Review the final diff and record results, limitations, and remaining user checks.

## Progress
- Workspace inspection: empty directory, no Git checkout or ROMs. Upstream README already advertises an 88emu plugin; inspect its relationship to 88emuPlayer before adding another wrapper.
- Build tools: CMake 4.4.4 and Ninja 1.13.2 installed in `/private/tmp/88-midi-build-tools`; Apple command-line SDK is present, full Xcode is not selected.

## Review
- Implemented and installed a native arm64 VST3 instrument using the existing JUCE processor, engine and interface; retained and rebuilt standalone and CLI.
- Product settings are private to each VST3 instance. Version-1 chunks restore model/power/settings and finite GS/LA setup; standalone wrapper/audio configuration is excluded and retained separately.
- Built original standalone first; baseline preserved. Final standalone `--help` succeeds outside the coding sandbox, and CLI renders audible stereo using supplied ROMs (99,271 frames, 44.1 kHz/24-bit, RMS 0.012493).
- Final tests: 8/8 focused unit/regression tests plus real VST3 host audio checks passed. GS setup tests: 84,296 checks, zero failures. Real VST3 tests passed at 44.1/48 kHz with SC-55mkII ROMs: state reopening, independent instances, MIDI offset/block timing, identical realtime/offline output.
- Two native editor peers opened and closed successfully, with RmlUi resources/fonts loaded. Component snapshots omit the embedded native view and are black; visual UI acceptance remains manual.
- Both built bundles and installed VST3 pass strict signature verification. Installed at `~/Library/Audio/Plug-Ins/VST3/88emuPlayer.vst3`.
- Live is available through CUA despite not appearing in `/Applications`; read-only inspection found an existing Set with its audio engine off. No Set or Live settings were changed. Native Live acceptance is pending below.
- No commits, upstream pushes, or ROM modifications. Implementation is limited to the 88emu product and a focused macOS preset/documentation.

## User acceptance still required
- [x] In Live: rescan VST3, open the editor, and confirm MIDI/stereo playback. User confirmed the installed plugin loads and plays as an instrument on 2026-11-08; screenshot shows the reused SC-55mkII interface.
- [ ] In Live: save two differently configured instances, quit/reopen the Set, and verify sound before opening editors.
- [ ] In Live: compare 64/128/512-buffer timing, loop/stop/seek, Export Audio, and Freeze/Flatten at 44.1/48 kHz.
- [ ] In standalone: confirm normal audio/MIDI device selection and reopening with retained settings.
- State limits: Receive All before saving physical panel changes; active voices/effect history and unsupported non-GS/non-LA board setup are not snapshots. See `doc/88emu_vst3_macos.md`.


### Baseline and implementation check-in
- Upstream revision: `83480f3cf64390524cbbfad40bd7575c2bb017b4`, submodules pinned to the checkout.
- Baseline built successfully before source changes: `cmake --build build/88emu-macos --target 88emuplayer_Standalone -j 6`; log `/private/tmp/88emu-build-baseline.log`. Native arm64 executable runs `--help` successfully. Original bundle retained at `build/88emu-baseline/88emuPlayer.app`.
- Configure uses Ninja, Release, arm64, deployment target 14.0, only 88emu enabled, Standalone ON, other formats OFF, installer/bridge OFF, CMake policy minimum 3.5.
- Baseline bundle needs ad-hoc signing for strict bundle verification; address packaging after final build.
- ROMs confirmed at `~/Documents/The Usual Suspects/88emuPlayer/roms` (read-only test inputs). Live is absent from `/Applications`.
- Implementation decision: extend existing target, reuse finite GS/LA restore messages; isolate plugin settings with a non-saving PropertiesFile; host owns MIDI/audio, so no PortMidi endpoints in VST3. Keep the common default ROM search root.
- State contract: model, power, output/resampling/boot/card/UI settings and tracked GS/LA configuration; transient sounding notes excluded. Physical panel edits require Receive All before saving; unsupported non-GS/non-LA boards need a documented limitation.
- Restore must finish via sample-driven emulation before host MIDI, without editor timers. Report actual engine latency and pass host transport.

### First integration verification
- Built Standalone, VST3, CLI and focused test targets successfully. Both bundles pass strict ad-hoc signature verification after signing.
- Real VST3 host, supplied SC-55-family ROMs: audible finite stereo at 44.1/48 kHz; reopening before editor/prepare produces sound; two-instance MIDI volume/state isolation passes; variable/fixed block onset matches (4380/4380 at 44.1k, 4406/4406 at 48k); +4096 sample note shift passes; realtime/offline relative RMS error 0. Rendered WAVs in `build/verification`.
- Expanded GS builder tests: 84,296 assertions, zero failures across 17 GS models.
- Focused unit checks passed: project state, missing ROMs/invalid chunks/instance isolation, hardware MIDI boundaries, part state, LA memory, dynamic sample rates and resampler latency. Existing launch-options test initially hit sandbox-only JUCE cache-directory permissions, then passed with cache access.
- Review fixes before final rebuild: allowlist product settings to prevent recursive standalone `filterState` embedding; retain standalone audio/wrapper config; refresh latency after analog-mode changes; cancel pending async updates on destruction; force fresh boot for configuration restore; hide standalone audio/MIDI menu entries in VST3.

### Final artifacts and verification evidence
- Bundles: `bin/plugins/Release/VST3/88emuPlayer.vst3`, `bin/plugins/Release/Standalone/88emuPlayer.app`; native arm64, ad-hoc signed, VST3 Instrument/Synth metadata.
- Build preset: `88emu-macos-arm64`; build/install/manual acceptance in `doc/88emu_vst3_macos.md`; state decision in `doc/adr/0001-88emu-vst3.md`.
- Evidence: `build/verification/unit-tests.txt`, `vst3-host-results.txt`, `standalone-cli.txt`, and generated stereo WAVs. The standalone's restricted-sandbox native startup abort was resolved by running the same `--help` outside the sandbox (exit 0); no source workaround was needed.

## Fix: instrument selections lost across channels/instances

User report: instruments selected in the plugin are not retained when changing/adding channels. The clarification is instrument selection, not a named preset file.

- [x] Reproduce panel instrument changes diverging from the saved part state with supplied SC-55mkII ROMs.
- [x] Identify a deterministic, bounded way to include actual panel-selected instruments in host state without advancing or interrupting the live audio timeline.
- [x] Add a regression that fails before the fix, covering multiple parts and a fresh reopened processor/instance.
- [x] Implement the smallest capture/restore correction; preserve MIDI timing, independent instances and standalone behavior.
- [x] Rebuild, run focused unit and ROM integration tests, sign and update the installed VST3.
- [x] Record results and any required Ableton restart/reopen check.

Implementation check-in: investigate board MIDI parameter replies/RAM access and panel event ownership before choosing the capture path. Existing MIDI-only recall tests did not cover physical panel instrument changes.

### Instrument recall findings and implementation
- Reproduction before fix: `88emuPanelState_test` exits 1, “front-panel instrument change was absent from project state”; log `/private/tmp/88emu-panel-repro.log`.
- Physical panel handlers bypass the MIDI configuration tracker; there were also no explicit JUCE non-parameter-state dirty notifications.
- Actual SC-55mkII 1.01 ROM probing confirmed the primary 16-part GS memory layout using physical button presses, MIDI changes, RQ1 replies and the firmware parameter descriptor table. A known internal-ROM hash and handler signatures guard the read-only API. Unknown layouts are left untouched.
- The processor now includes actual bank/program/receive-channel assignments in project state. Firmware writes schedule message-thread capture/host dirty notification, and Part Editor configuration edits notify the host directly. Save does not advance emulated time.
- Regression also covers two independent instances, per-part receive channels, actual firmware state after restore, and avoiding host dirty notifications on ordinary MIDI playback.

### Instrument recall fix review and verification
- Actual SC-55mkII panel bank/program/receive-channel selections are captured across all 16 parts; supported firmware detection uses the known boot ROM and verified GS handler signatures. Unknown layouts keep the existing behavior rather than reading guessed RAM addresses.
- Part Editor selections awaiting firmware MIDI delivery remain authoritative during immediate saves/track duplication. A later physical instrument/channel button edit supersedes older queued editor selections. Ordinary settled host MIDI does not continuously dirty Sets.
- Regression passes: physical instrument changes on two parts, MIDI-channel assignment, fresh-instance firmware recall, adding another instance, host dirty notifications, immediate editor-save without audio, two queued programs with 8-sample callbacks and intervening saves, and a later physical panel override.
- Final Release build succeeded for VST3, standalone, CLI and test targets. Both bundles were ad-hoc signed and strictly verified. All 10 targeted tests passed (9.32 seconds), including real VST3 ROM playback, state reopening, independent instances and realtime/offline comparison.
- Updated installed VST3 at `~/Library/Audio/Plug-Ins/VST3/88emuPlayer.vst3`; its executable SHA-256 matches the tested build (`35bb1a0825a039bced2babe14c9627332c71cb9ae1a136ca2d88f8e35c27264f`). Previous installed bundle backed up at `build/verification/instrument-recall/88emuPlayer-before-fix.vst3`.
- Remaining user check: completely quit/reopen Live to unload the previous plugin, choose the desired instruments again if the old build already lost them, save a two-instance Set, then reopen and confirm recall. No Live Set or ROM files were edited.
