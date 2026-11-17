# Progress

## Status
Milestone 2 (synth engine + advanced knob panel): **done**. Next: milestone 3.

## Toolchain (checked 2026-11-17)
- Apple clang 17 (Command Line Tools only, no full Xcode), macOS SDK 26.1, CMake 4.5.0, 8 cores.
- No Ninja: use the "Unix Makefiles" generator (`make -j8`).
- JUCE tags: latest 8.x is `8.0.17-security` (last regular release `8.0.15`). JUCE 9 exists (9.0.5) but spec says JUCE 8. Catch2 latest `v3.16.1`.

## Milestones
1. [x] CMake + JUCE skeleton (loaded fine in Logic, confirmed 2026-11-17)
2. [x] Synth engine + advanced knob panel
3. [ ] CashCode + ParameterMap + Mutator + Ledger + tests
4. [ ] Coin/Investment UI, Bank It, Risk, Vault, Jackpot
5. [ ] Stash presets + state saving
6. [ ] Cash Match
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
- 31 APVTS params, IDs in `Source/Parameters.h` (stable, versioned `ParameterID{id,1}`; never rename).
- `SynthEngine` wraps `juce::Synthesiser`, 16 voices. Reads params once per block into `SynthParams`; global LFO buffer, rendered in chunks so nothing allocates on the audio thread.
- Voice: osc1 + osc2 (PolyBLEP saw/square, sine, tri), sine sub (-1 oct), noise → TPT SVF (LP/BP/HP, coeffs every 8 samples) → amp ADSR. Filter env ±60 st, LFO ±36 st cutoff / 0-12 st pitch, key track, pitch bend ±2 st, velocity sens.
- UI: `AdvancedPanel` (own component so M4 can show/hide it), `CashLookAndFeel` gold-on-green, 1100x640. Double-click knob = default.
- State save/restore NOT done yet (M5): Logic projects won't recall knobs until then.
- Hidden test `CASH_SNAPSHOT=out.png ./build/CashSynthTests "[snapshot]"` renders the editor to PNG.

## Open questions
- Licence: JUCE 8 is AGPLv3 or commercial (free Starter tier), not permissive. Project code is MIT; a release route is undecided. Fine for personal use.

## Build
`cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j8 && ctest --test-dir build`
M2 verified: clean build (-Werror), 11 Catch2 tests pass (pitch, filter, gain, release, polyphony, 40-round param fuzz, editor), `auval -v aumu Csyn Chru` PASS.

## Next
- Optional: play it in Logic to judge the sound by ear.
- Milestone 3: CashCode + ParameterMap + Mutator + Ledger + tests (ParameterMap keys on the IDs in `Parameters.h`).
