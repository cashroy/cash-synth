# Progress

## Status
Milestone 10 (tempo sync) **done**. Next: milestone 11 (delay + reverb).

## Toolchain (checked 2026-11-17)
- Apple clang 17 (Command Line Tools only, no full Xcode), macOS SDK 26.1, CMake 4.5.0, 8 cores.
- No Ninja: use the "Unix Makefiles" generator (`make -j8`).
- JUCE tags: latest 8.x is `8.0.17-security` (last regular release `8.0.15`). JUCE 9 exists (9.0.5) but spec says JUCE 8. Catch2 latest `v3.16.1`.

## Milestones
1. [x] CMake + JUCE skeleton (loaded fine in Logic, confirmed 2026-11-17)
2. [x] Synth engine + advanced knob panel
3. [x] CashCode + ParameterMap + Mutator + Ledger + tests
4. [x] Coin/Investment UI, Bank It, Risk, Vault, Jackpot
5. [x] Stash presets + state saving
6. [x] Cash Match (+ drive, LFO 2 routing, pitch env)
7. [x] Lead mode: mono/legato, glide, priority, unison, controllers
8. [x] Section on/off toggles + automation-ready (smoothing, safe mode switches)
9. [x] Cash Match v2: whole sample, per-part scores, section switches, parallel search
10. [x] Tempo sync: LFO 1/2 rates as Hz or note values, following the host BPM automatically
11. [ ] Delay + reverb (user, 2026-11-17), delay time syncable
12. [ ] Polish, pluginval, README

## Key decisions
- Build and test on this Mac (Intel, macOS 26.1); keep the Windows/VS2022 build supported.
- Repo root is this folder (no nested `CashSynth/`).
- JUCE `8.0.17-security` and Catch2 `v3.16.1` via FetchContent (shallow).
- Tests link the `CashSynth` shared-code static lib and borrow its include dirs/defines via generator expressions (no second JUCE compile).
- Warnings-as-errors applied per source file (`CASH_SOURCES`, test files), so JUCE modules are unaffected. Add new sources to `CASH_SOURCES`.
- `COPY_PLUGIN_AFTER_BUILD` installs VST3/AU to `~/Library/Audio/Plug-Ins`.
- Brand name **Cashroy** (COMPANY_NAME, shown in DAWs), bundle ID `com.cashroy.cashsynth`. Hidden codes stay `Chru`/`Csyn` (never displayed). LICENSE copyright still says Charlie Ruffell: user's call.
- If auval/Logic shows a stale name: `killall -9 AudioComponentRegistrar`.
- Formats: VST3, Standalone, AU (Apple only). Native arch only in dev, ad-hoc signing.
- Warnings as errors on our code only (`/W4 /WX`, `-Wall -Wextra -Werror`).

## Engine (M2)
- 31 APVTS params (39 since M6), IDs in `Source/Parameters.h` (stable, versioned `ParameterID{id,1}`; never rename).
- `SynthEngine` wraps `juce::Synthesiser`, 16 voices. Reads params once per block into `SynthParams`; global LFO buffer, rendered in chunks so nothing allocates on the audio thread.
- Voice: osc1 + osc2 (PolyBLEP saw/square, sine, tri), sine sub (-1 oct), noise → TPT SVF (LP/BP/HP, coeffs every 8 samples) → amp ADSR. Filter env ±60 st, LFO ±36 st cutoff / 0-12 st pitch, key track, pitch bend ±2 st, velocity sens.
- UI: `AdvancedPanel` (own component so M4 can show/hide it), `CashLookAndFeel` gold-on-green, 1100x640. Double-click knob = default.
- Hidden test `CASH_SNAPSHOT=$PWD/Docs/editor.png ./build/CashSynthTests "[snapshot]"` renders the front, plus `editor-back.png` (flipped).

## Money logic (M3)
- `ParameterMap`: append-only table of all 31 IDs (steps, ordered, mutation weight). Order = CashCode bit order. `Patch` = `std::array<float,31>` normalised; `capturePatch/applyPatch/defaultPatch`, `samePatch` (1e-4 tolerance: real-unit round trip drifts).
- Weight 0 = never mutated: `masterGain`, `velocitySens`. Pitch params low weight.
- `CashCode`: `$XXXX-XXXX-...` Crockford base32 (47 chars in v1, 56 in v2). 4-bit version, stepped params minimal bits, continuous 8 bits, CRC-16. Decode ignores case/spaces/dashes, O→0, I/L→1. Default-patch code pinned in a test: changing the layout means bumping `CashCode::version`.
- `CashCode::serialNumber` → `"KD 04718823 Q"` fingerprint for the bill (not reversible).
- `Mutator`: own splitmix64 `Rng` + Irwin-Hall gaussian (same result on every platform). `invest({amount, risk, seed}, locks)`: amount = how many/how far, risk = wild jumps + wave/filter-type swaps (risk 0 never swaps). `jackpot(seed, locks)` re-rolls all. Same randoms drawn per param whether locked or not. `makePlayable` rails: tonal level ≥ 0.35, cutoff sane per filter type, reso ≤ 0.85, attack ≤ ~1.5 s, release ≤ ~4 s.
- `Ledger`: undo/redo history (cap 100, drops no-op entries). Processor: `invest`, `hitJackpot`, `setPatch`, `undo`, `redo`; hand tweaks since the last move are recorded as their own "Hand tweak" entry first.
- Ledger is NOT saved with the project (M5 decision, see State).

## Look (M4+): super cool money design
- Main face is a **$100 bill** parody: "THE UNITED STATES OF CASHROY", "ONE HUNDRED CASH", "IN SYNTH WE TRUST".
- Changed 2026-11-17 (user's request): the background should look like a real US $100, using a public-domain scan (e.g. Wikimedia Commons; check the file page says public domain, record source + licence in a CREDITS file). Rules for showing US currency (18 U.S.C. 504): colour images one-sided and under 75% or over 150% of real size. So tint/fade it and keep our Cashroy text, seal, serials and scope on top, so it never reads as spendable money. Keep `paintFront` as the fallback if the image is missing.
- Portrait oval holds a live waveform/scope instead of a president. Treasury-style seal = **Jackpot** button. Live `serialNumber` printed in the corners; click it to copy the full CashCode.
- Green/black guilloche linework, engraved-style fonts, gold coins for Investment amounts, Risk as a stock-ticker style dial, Bank It/Vault as a stack of bills. Flip the bill over to reveal the Advanced panel.

## Bill UI (M4)
- Editor 1100x724 (M5: +stash bar on top): bill 1076x470 on a dark desk, money tray underneath. Snapshots in `Docs/editor.png` / `Docs/editor-back.png`.
- `BillArt`: original guilloche (rosettes, rope frame), arc text, serif/mono fonts, `drawCoin`, `FrontLayout` (shared by painter and components). Static art cached per pixel scale (`ArtCache`).
- `BillComponents`: `BillFront` (scope portrait, Jackpot seal, 2 serial tags that copy the CashCode, flip tab), `BillBack` (AdvancedPanel). Flip = instant show/hide (animation left for M7).
- **Every rotary knob is a gold coin** (`CashLookAndFeel::drawRotarySlider` uses `art::drawCoin`); value arc kept round the rim.
- `MoneyTray`: Invest coins 10c/25c/50c/$1 (= amount 0.1/0.25/0.5/1.0), `RiskDial` (SAFE..YOLO gauge + ticker: T-BILLS..CRYPTO), Ledger UNDO/REDO + memo, Bank It (strapped stack) + 8 vault slots (click = withdraw; right-click = withdraw/copy CashCode/shred).
- Processor: opening balance recorded in the constructor; `canUndo/canRedo` (count unrecorded hand tweaks); `bankIt/withdraw/shred`; `risk` lives in the processor; `ScopeBuffer` (lock-free 4096 atomic floats, mono tap after render).
- `Vault`: cap 8, dedupes identical patches, refuses when full (never drops silently). Withdraw is an undoable ledger entry.
- Editor polls at 15 Hz (`syncFromProcessor`) for serials, ledger, vault state; scope at 30 Hz only while showing.
- No lock UI yet (Mutator supports a LockMask; candidate for M7 or a lock toggle per knob).

## Bill background (scan, 2026-11-17)
- `Resources/hundred_obverse_gray.jpg`: greyscale (sips) copy of the Commons public-domain 2009 $100 obverse (BEP). Source, licence, SHA-256 and the currency-rule notes are in `CREDITS.md`.
- Embedded with `juce_add_binary_data` (`CashAssets`, `BinaryData.h`). `art::makeTintedScan()` makes a faded green duotone, blanks the real serials, signatures and series year (patch with the surrounding tone + Gaussian feather).
- `paintFront` = scan + paper halo behind the scope + feathered paper plates under our text; `paintFrontPainted` (old all-original art) is the fallback if the scan won't load. Corner medallions/big copper 100 only on the fallback (the scan has its own).
- Test checks: duotone (no strong colour, nothing darker than faded ink), serial blanked, both painters draw.

## State + Stash (M5)
- Project state = XML `CashSynthState` v1: `risk`, `stash` name + `stashModified`, the APVTS `PARAMS` tree, `Vault` notes as exact normalised values per param ID (not CashCode: that's 8-bit lossy) + bank time.
- **Ledger not saved**: it's session history (like the DAW's undo). After `setStateInformation` the ledger is cleared and a fresh "Opening balance" of the loaded sound is recorded (`openBooks()`).
- Loading is forgiving: garbage/foreign state ignored (nothing changes); a bare APVTS `PARAMS` tree still loads; `patchFromTree` keeps defaults for missing IDs and ignores unknown ones.
- Vault guarded by `vaultLock` because hosts may call `getStateInformation` off the message thread. `setStateInformation` assumed on the message thread.
- `Stash`: one `.cashstash` XML per sound (`name`, `code` for humans, `Patch` exact values) in `~/Library/Application Support/Cashroy/Cash Synth/Stash` (`%APPDATA%\Cashroy\...` on Windows). Code-only files load too. Same name overwrites; natural sort.
- Processor: `stashIt(name)`, `loadStash(file)` (undoable ledger entry "Opened stash: X"), `getStashName`, `isStashModified`.
- `StashBar` above the bill: brand left; STASH combo, < > (wrap), SAVE (name dialog, confirms before replacing a different stash), ... (open folder, shred to bin, rescan). Shows CHANGED / "not in stash". Rescans the folder every 1.5 s.
- Host programs still 1 (stash not exposed as programs).

## Cash Match (M6)
- What it is (user, 2026-11-17): drop in a short sample; a background search finds the closest patch and shows a match %. Plus extra controls to help matches.
- New params, appended (ParameterMap 31 -> 39, `CashCode::version` 2): `driveType` (Soft/Hard/Fold), `driveAmount`, `lfoToDrive` (LFO 1 -> drive), `lfo2Wave/Rate/Amount/Target` (Cutoff, Pitch, Amp, Drive, Osc 2), `pitchEnvAmt` (filter env -> pitch, +/-24 st). All default to off. Drive is per voice, after the filter, before the amp env.
- v1 CashCodes still decode: `decode(code, fallback)` fills the missing params from the fallback (callers pass the default patch). Old golden code checked in MatchTests.
- `SoundPrint` (analysis at 44.1 kHz): RMS envelope (48 frames), 28 log bands x 6 time segments, 12 harmonics, flatness, centroid, YIN pitch, key-down time. All relative to the sound's own peak. `distance` weights loud bands/frames most; `matchPercent = 100 * exp(-40 d^2)`.
- `PatchRenderer`: private `SynthEngine` bound to its own atomics (new `SynthEngine(RawLookup)` ctor), so it runs off the message thread without touching the APVTS (APVTS from a worker thread races its tree flush).
- `SynthEngine::reset()` now zeroes oscillator phases so offline renders repeat exactly (live playing still free-runs between notes). Without it, detuned saws scored the same patch at ~86 %.
- `CashMatcher` (juce::Thread, owned by the processor): load file (WAV/AIFF/FLAC/MP3/OGG; mono, resampled, leading silence trimmed, max 2.5 s) -> guess (amp env, noise, cutoff, wave from analysis) -> 48 jackpots -> 14 x 12 invests round the best 4 -> 2 knob-by-knob polish passes (~400 renders). Matched at the detected note.
- Results: synth-made pluck 37 % -> 100 %; Karplus-Strong string 32 % -> 82 % (tests).
- Processor: `startMatch`, `depositMatch` (ledger entry "Cash Match: name 87%", undoable), `previewTarget/previewMatch` via `PreviewPlayer` (audio thread only try-locks a SpinLock).
- UI: CASH MATCH button in the stash bar (or drop an audio file anywhere on the editor) shows `MatchPanel`, a cheque over the bill: waveform drop zone (sample + best-match outline), match % box with CLEARED/PARTIAL/BOUNCED/PENDING stamp, progress, memo, LOAD / HEAR SAMPLE / MATCH IT / HEAR MATCH / DEPOSIT. Snapshot `Docs/editor-match.png`.
- Advanced panel: MASTER moved to row 1; new DRIVE section; pitch knob in FILTER / PITCH ENV; LFO 2 section.
- Note names follow Logic (middle C = C3).

## Lead mode (M7)
- 11 params appended (39 -> 50, `CashCode::version` 3, 68 chars; v1/v2 still decode): `voiceMode` (Poly/Mono/Legato), `glideTime` (0-2 s), `glideMode` (Always/Legato), `glideType` (Time/Rate = time per octave), `notePriority` (Last/Low/High), `unisonVoices` 1-8, `unisonDetune` (100 % = 1 st between outer voices), `unisonSpread`, `bendRange` 1-24 st (default 2), `modVibrato` (CC1 -> LFO 1 pitch, 1 st at 100 %, default 50 %), `velToCutoff` (soft notes up to 48 st darker). Defaults = the M6 sound.
- Weights: mode, glide mode/type, priority, bend, mod wheel = 0 (never mutated). Glide 0.3, unison voices 0.3, detune 0.4, spread 0.3, vel->cutoff 0.3. Jackpot keeps lead tame: glide on ~25 % (<= ~90 ms), unison 2-4 voices ~30 %, else off.
- **`juce::Synthesiser` dropped**: `SynthEngine` owns 16 `CashVoice`s (plain class now) and does MIDI itself (omni; note on/off, pitch wheel, CC1, sustain CC64, all notes/sound off), split at each event's sample.
- Poly: each note takes `unison` voices; free voice first, else steal the oldest (key-up first), never one started for the same note. Poly glide too (from the last note played).
- Mono/Legato: held-key stack + priority; releasing the sounding key falls back to the priority pick of the keys still held. Mono retriggers envs on every new note (also fallback); Legato only when no key was down. Mono voices = voices 0..unison-1. Changing mode (or unison while mono) releases everything.
- Glide is linear in semitones, per sample, from where the voice actually is (mid-glide ok). "Legato" glide mode = only when a key was already held.
- Voices are stereo: pan gains L = 1 - pan, R = 1 + pan (centre unchanged, mono sum unchanged); unison voices spread -1..+1 in detune and pan, level 1/sqrt(n). `reset()` staggers voice phases (voice 0 still 0) so unison doesn't start in phase and renders stay repeatable.
- Old projects: APVTS `replaceState` keeps *current* values for params missing from the tree, so `setStateInformation` now adds missing params at their defaults first (an M6 project loads as M6, even into an instance set to Mono).
- Cash Match polish skips glide and vel->cutoff (a single full-velocity note can't hear them).
- UI: back has a 4th row VOICE (Mode, Priority, Unison, Detune, Spread) / GLIDE (Time, When, Type) / PLAY (Bend, Mod Vib, Vel > Cut); spacing tightened so the knobs keep their size. Front: `VoiceModeSwitch` (POLY/MONO/LEGATO, `ParameterAttachment`) on a paper plate under the legal-tender text (`FrontLayout::voiceSwitch`).
- Fixed: a slider whose value box never changes width in `resized()` kept the default LAF outline (box was built before the editor set `CashLookAndFeel`). Colours now set on each slider.
- Tests (`Tests/LeadTests.cpp`, 9 cases): mono/legato voice counts incl. unison and poly steals, retrigger vs legato envelope, glide timing (constant time, constant rate, legato-only, poly), priority Last/Low/High fallback + sustain pedal, unison width (spread 0 = mono, level steady, pans), bend range / mod wheel / vel->cutoff, v2 code + M6 project/vault load with lead off, jackpot/invest leave mode alone, front switch <-> param.

## Section switches + automation (M8)
- 10 bool params appended (50 -> 60, `CashCode::version` 4, 69 chars; v1-v3 decode with switches ON): `osc1On osc2On subNoiseOn filterOn filterEnvOn` (incl. pitch env) `driveOn lfo1On lfo2On unisonOn glideOn`. Default on = M7 sound. `param::sectionSwitchIds()`, `isSectionSwitch`, `allSectionsOn`.
- **Engine lanes**: `SynthEngine` fills one value per sample per chunk for every continuous knob, gate and crossfade (`Lane` enum in `SynthVoice.h`, linear `SmoothedValue`s: 15 ms knobs, 10 ms switches/choices; preallocated in `prepare`, snapped there so offline renders are unchanged). Cutoff stays per-voice multiplicative (20 ms), master gain as before.
- Choices crossfade: osc waves, filter type (one SVF gives LP/BP/HP), drive type, LFO waves, LFO 2 target (a weight lane per target). Sustain levels are lanes too (ADSR gets them per sample while ramping; JUCE's ADSR jumps otherwise).
- Bypass: off sections skip work for the whole chunk when fully off (filter, drive, LFO generation, oscillators). Filter state is cleared before it fades back. Filter env keeps running while off (cheap; keeps retrigger semantics). LFO 2 tremolo needs its own gate lane (`laneLfo2On`): its "1 - x" form isn't zero when the LFO is.
- Unison off = 1 voice; glide off = 0 s. Unison place (-1..1) + level are smoothed per voice; detune/spread are applied live from lanes. A sounding (or stolen) voice glides to its new place.
- **Voicing hand-over** (`changeVoicing`): poly<->mono/legato and unison count changes keep held keys sounding. Poly->mono moves the priority key's voices into the mono slots (no retrigger), others release. Mono->poly: sounding note keeps its voices (pedal-held stays pedal-held), other held keys start. Poly unison change `regroup`s every held key.
- Money moves: weights 0.15 for subNoise/drive/lfo1/lfo2/unison/glide switches, 0 for osc1/osc2/filter/filterEnv. Jackpot: each effect switch off with 15 % (own Rng stream). `makePlayable`: osc 1 or 2 must be on (osc 1 first; sub only if both locked off); tonal level counts switched-on sources only. Cash Match forces all switches on and doesn't polish them (M9).
- Host view: all params automatable; names now "LFO 1 ..." for LFO 1 (IDs unchanged); cutoff text parses "kHz"; octave has an "oct" label. Test round-trips every param's text.
- UI: engraved ON/OFF slide switch in each section title (`AdvancedPanel::SectionSwitch`, `ParameterAttachment`); off = knobs/labels at 35 % alpha, still editable. VOICE split into VOICE (Mode, Priority) + UNISON (Voices, Detune, Spread); knob sizes unchanged. `getNumControls` counts switches.
- Tests (`Tests/AutomationTests.cpp`, 9 cases): **golden renders** of 4 scenes from the M7 engine (`Tests/GoldenRenders.inc`, `GoldenScenes.h`) match within 1e-5; old code/project/vault/stash load all-on; each switch bypasses exactly its section; bypassed filter = dry; toggling mid-note and automating all 60 params (sweeps + hard jumps, 64-sample blocks) stay under 2x the steady max sample step, no NaNs; hand-over; jackpot rails; UI switches; host text.
- Known: slamming cutoff 20 Hz <-> 20 kHz with resonance makes a resonant "zap" (~5x steady step; tested <= 8x). With reso 0 it's as clean as the rest. Per-sample coefficients didn't change it.
- Tests that set a param after `prepare` now hear a 15 ms ramp: reset before rendering if a test needs the new value instantly.

## Cash Match v2 (M9)
- Whole sample up to 10 s (`CashMatcher::maxSeconds`), kept **stereo** (target audio, renders and previews are 2-ch; `PreviewPlayer` plays stereo).
- `SoundPrint` frame track: hop 10 ms (grows past 4 s, max 400 frames). Per frame: level, pitch (YIN CMNDF from an FFT autocorrelation, window 2048, octave-up guard, outliers snapped to neighbour median, short runs a harmonic off moved back), brightness (log2 centroid). Steady region = first frame within 1 dB of peak +50 ms .. last loud frame before key-up.
- `findWobble`: quadratic detrend, residual clipped at 3 MAD-sigmas, shortest ACF peak >= 0.8 x strongest (0.7-15 Hz). Gives vibrato (st), tremolo (dB), wah (oct; dropped if it follows the vibrato rate). Plus `width` (side/mid RMS), `pitchStart`/settle, brightness start/settle.
- f0 = median settled pitch of the frame track (pitch env/vibrato don't pull it).
- Fixed M6 bug: key-down detection walked back through a flat sustain (held notes read as 0.1 s); now skips the silent tail and needs a clearly falling run.
- 6 parts (`analysis::Part`): ENVELOPE (level, ~50 ms smoothed), TONE (old spectrum/harmonics/flatness), PITCH (st per loud frame), BRIGHTNESS (~110 ms smoothed), MOVEMENT (wobble depths + rates), WIDTH. Weights 22/30/13/15/10/10 (unpitched: pitch weight to tone/env). Same `matchPercent` per part. Candidates analysed `like` the target (same frames/region).
- Guess: envelope from smoothed 10 ms level; filter env from brightness drop; pitch env from `pitchStart`; LFO 1 vibrato/wah; LFO 2 tremolo on amp; unison from width; slow wobbles in a wide sample = beating (not LFOs). Sets section switches to only what was heard; returns a `features` mask, `overlay()` lays measured features on any patch (start, defaults, best-so-far).
- Search: batches scored on a `juce::ThreadPool` (cores-1, max 8, low priority; worker 0 = search thread), deterministic (same seed = same result). Staged polish per part group (env, pitch, brightness, movement, width, tone, switches, rest): all nearby values in parallel, then the winners combined. Tidy pass switches off sections costing <= 0.003 distance. Result has `parts` + `sectionsOff`.
- Engine: `resetPhases` reseeds voice noise, so a render is the same on any worker's engine.
- Cheque: amount box shorter, ITEMISED grid (6 parts, % + bar; n/a for pitch on unpitched), memo says how many sections were switched off; info line lists vibrato/tremolo/wah/pitch drop/stereo heard.
- Results (tests): vibrato+3-voice unison+12 st drop lead 95 % (all parts >= 80); pluck 99 %; Karplus string 87 % (M6: 82 %); plain lead 100 % with 8 sections off.

## Tempo sync (M10)
- `Tempo.h/.cpp`: `tempo::Info` (bpm, sig, playing, ppq) from `AudioPlayHead` each block; no host / no BPM / nonsense = 120, 4/4, stopped. 24 note values, **slowest first** (knob only speeds up): 4 bars D ... 1/32 T. Bars follow the time signature, 1/2..1/32 don't. `noteHz`, `nearestNote` (in octaves).
- 4 params appended (60 -> 64, `CashCode::version` 5, 71 chars): `lfo1Sync`/`lfo2Sync` (Choice Time/Sync, default Time), `lfo1Note`/`lfo2Note` (default 1/4). Weights 0.1 (sync) / 0.5 (note, ordered). `numParamsV4 = 60`; `isSectionSwitch` is now V3..V4 only.
- Engine: `render (buffer, midi, tempo)`. SYNC rate = BPM/60/length through the existing 15 ms rate ramp. Playing + ppq = **locked**: phase = frac(ppq / length) (from song start, so time-signature changes mid-song aren't followed for bar values). Transport start or seek/loop (ppq off by > 0.001 qn from expected) snaps; note-value/mode changes glide onto the grid (~20 ms pull, exact once within 1e-4). Stopped = free-run at the synced rate. TIME path is byte-identical (golden renders pass).
- Jackpot: each LFO synced with 15 % (own stream). Cash Match forces TIME on every candidate and doesn't polish sync/note; `depositMatch` keeps an LFO the user had in SYNC synced, on the note nearest the matched Hz at the current host tempo.
- UI: `AdvancedPanel::RateModeSwitch` (TIME|SYNC gold plate) replaces the "Rate" label; SYNC swaps the Hz knob for a note knob in the same spot. Dims with its section.
- Tests (`Tests/TempoTests.cpp`, 9 cases, fake play head): note Hz at 14 BPM/sig combos (4/4, 3/4, 7/8, 6/8, 5/4, 2/2), host defaults, free-run rate, lock from first sample at 6 BPM/sig/note combos + seek + stop, TIME ignores the song, note/mode/BPM change mid-note <= 2x steady step, v4 code/M9 project/stash load unsynced, jackpot rate, UI toggle.

## Open questions
- Licence: JUCE 8 is AGPLv3 or commercial (free Starter tier), not permissive. Project code is MIT; a release route is undecided. Fine for personal use.

## Build
`cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j8 && ctest --test-dir build`
M5 verified 2026-11-17: clean build (-Werror), 41 Catch2 tests pass (adds patch tree, Stash files, project state round trip + fresh opening balance, bad/bare state, processor stash, stash bar, bill scan), `auval` PASS, snapshot checked by eye.
M6 verified 2026-11-17: clean build (-Werror), 51 tests pass (~13 s), no JUCE assertions printed, `auval` PASS, snapshots checked by eye.
M7 verified 2026-11-17: clean build (-Werror), 60 tests pass, no JUCE assertions, `auval` PASS, front/back snapshots checked by eye.
M8 verified 2026-11-17: clean build (-Werror), 70 tests pass (~26 s), no JUCE assertions, `auval` PASS, front/back snapshots checked by eye (back shows Drive + LFO 2 off).
- Fixed: the `juce_Slider.cpp:1485` assertion was `RiskDial` passing a negative start angle to `setRotaryParameters` (JUCE wants both angles in [0, 4pi)); now 1.5pi..2.5pi, same dial.

M9 verified 2026-11-17: clean build (-Werror), 78 tests pass (~41 s Debug), no JUCE assertions, `auval` PASS, cheque snapshot checked by eye.
M10 verified 2026-11-17: clean build (-Werror), 87 tests pass (~24 s), no JUCE assertions, `auval` PASS, back snapshot checked by eye.

## Plan: tempo sync + effects (asked 2026-11-17)
- LFO part done in M10 (see above). M11: delay time and reverb pre-delay get the same TIME/SYNC switch and note list (`tempo::noteQuarters`).
- Note values, the usual set: 4 bars, 2 bars, 1 bar, 1/2, 1/4, 1/8, 1/16, 1/32, each straight, dotted (x1.5) and triplet (x2/3). Shown as "1/4", "1/4 D", "1/4 T", "1 bar".
- **BPM comes from the host automatically** (`AudioPlayHead::getPosition()` each block: BPM, time signature, PPQ, playing). Never typed in by the user. Standalone / no BPM: 120. Bar length follows the time signature.
- While the host plays, synced LFOs lock their phase to the song position (PPQ), so they line up with the beat and stay in time after seeking; when stopped they free-run at the synced rate. A per-LFO retrigger (free / restart on note) is a candidate.
- New params appended (stable IDs, `CashCode` version bump, older codes/projects decode with sync off = today's sound): `lfo1Sync`, `lfo1Note`, `lfo2Sync`, `lfo2Note` (M10); `delayOn`, `delayTimeMs`, `delaySync`, `delayNote`, `delayFeedback`, `delayMix`, `delayPingPong`, `delayTone`, `reverbOn`, `reverbSize`, `reverbDecay`, `reverbDamp`, `reverbPreDelay`, `reverbPreSync`, `reverbPreNote`, `reverbWidth`, `reverbMix` (M11, final list decided then).
- Engine: a tempo context (BPM, sig, PPQ, playing) passed into `SynthEngine::render`; `PatchRenderer` (Cash Match) renders at 120 BPM with transport stopped. Rate smoothing stays click-free when the BPM or note value changes.
- UI: a small TIME/SYNC toggle next to each rate knob; in SYNC the knob steps through the note values and its label reads the note. Effects get their own section(s) on the back of the bill (and on/off switches like M8).
- Money moves: Jackpot/Invest may pick synced rates (low weight); effects mix kept modest by `makePlayable`. Cash Match: matches synced LFOs in Hz (converts back when the user wants sync), and leaves delay/reverb off unless the sample clearly has a tail/echo (decide in M11).
- Tests: rate = BPM-derived Hz for each note value at several BPMs incl. odd time signatures; phase locked to PPQ; BPM change mid-note is smooth; old codes/projects load unsynced; delay echo lands at the synced time; reverb tail decays, mix 0 = dry, effects off = bit-identical to M9 renders (golden renders).

## Next
- Try in Logic: Cash Match on real samples (long pads, stereo chorus, vibrato leads); check CPU while matching during playback.
- Known limits: very fast pitch drops (< ~50 ms) aren't tracked (46 ms window); unison beating is a real level/brightness wobble so it costs some ENVELOPE/BRIGHTNESS.
- Try in Logic: synced LFOs at a few tempos, start/stop/loop, odd time signatures.
- Milestone 11: delay + reverb (see Plan above). Then 12: polish (flip animation, lock toggles per knob), pluginval, README.
