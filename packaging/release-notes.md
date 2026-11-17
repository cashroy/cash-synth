The first release of **Cash Synth**, a money-themed synthesizer by Cashroy: a $100 bill on the front, every knob on the back, and **Cash Match** to rebuild any sample as a playable synth sound.

## Downloads

| | File | Installs |
| --- | --- | --- |
| **macOS** 11+ (Apple silicon and Intel) | `CashSynth-mac.pkg` | VST3, Audio Unit, Standalone app |
| **Windows** 10/11 (64-bit) | `CashSynth-Setup.exe` | VST3, Standalone app |

The installers aren't signed by Apple or Microsoft yet, so your computer will warn you the first time:
- **macOS**: if it says the pkg can't be opened, right-click it and choose **Open** (or go to System Settings > Privacy & Security and click **Open Anyway**).
- **Windows**: if SmartScreen appears, click **More info** then **Run anyway**.

Then rescan plug-ins in your DAW (Logic does this on its next launch).

## What's in it

- 2 oscillators + sub + noise, a state-variable filter, drive, 2 LFOs with tempo sync, poly / mono / legato with glide and unison, stereo delay and reverb.
- Money moves: Invest, Risk, Jackpot, Bank It, the Vault, a Ledger with undo, knob padlocks, and a CashCode for every sound.
- **Cash Match**: drop in a sample and get two versions: **EXACT** (the Mint, resynthesised from sine partials + noise, 99 % average on real instruments) and **LAYERS** (an editable patch of up to 8 wavetable layers + 2 noise layers, 95 % average), each playable on any key.
- The LAYERS page: every layer's tuning, envelope (with a curve for kicks and plucks), filter, LFO depths and noise band (with 6-48 dB edges) is a normal, automatable knob.

Source code: this repository. Cash Synth is built with JUCE 8.
