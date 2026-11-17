# Progress

## Status
Milestone 6 (Cash Match + drive / LFO 2 / pitch env) **done**. Next: milestone 7 (polish, pluginval, README).

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
7. [ ] Polish, pluginval, README

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

## Open questions
- Licence: JUCE 8 is AGPLv3 or commercial (free Starter tier), not permissive. Project code is MIT; a release route is undecided. Fine for personal use.

## Build
`cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j8 && ctest --test-dir build`
M5 verified 2026-11-17: clean build (-Werror), 41 Catch2 tests pass (adds patch tree, Stash files, project state round trip + fresh opening balance, bad/bare state, processor stash, stash bar, bill scan), `auval` PASS, snapshot checked by eye.
M6 verified 2026-11-17: clean build (-Werror), 51 tests pass (~13 s), no JUCE assertions printed, `auval` PASS, snapshots checked by eye.
- Fixed: the `juce_Slider.cpp:1485` assertion was `RiskDial` passing a negative start angle to `setRotaryParameters` (JUCE wants both angles in [0, 4pi)); now 1.5pi..2.5pi, same dial.

## Next
- Try in Logic: drop a real one-shot (bass, pluck, pad) into Cash Match, HEAR MATCH vs HEAR SAMPLE, DEPOSIT, play the shown note. Check old projects/stashes still load (new knobs should be off).
- Milestone 7: polish (flip animation, lock toggles per knob, maybe multithreaded matching), pluginval, README.
