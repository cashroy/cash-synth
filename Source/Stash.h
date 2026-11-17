#pragma once

#include <optional>
#include "ParameterMap.h"
#include "Mint.h"

// The Stash: named presets, one small XML file per sound, in a folder the user
// can open, copy and share. Message thread only.
//
//   <CashStash version="1" name="Fat Bass" code="$...">
//     <Patch osc1Wave="0.333" cutoff="0.62" ... />
//     <Mint data="(base64)"/>   M13: only when the sound plays a Mint model
//   </CashStash>
//
// `code` is the CashCode, for humans only: loading reads the exact values in Patch.
namespace cash
{
class Stash
{
public:
    static constexpr const char* extension = ".cashstash";
    static constexpr int formatVersion = 1;

    explicit Stash (juce::File folder = defaultFolder());

    // ~/Library/Application Support/Cashroy/Cash Synth/Stash on macOS,
    // %APPDATA%\Cashroy\Cash Synth\Stash on Windows.
    static juce::File defaultFolder();

    juce::File getFolder() const { return folder; }
    void setFolder (const juce::File& f) { folder = f; }

    // Saves (or overwrites a stash with the same name). Returns the file, or {} on failure.
    juce::File save (const juce::String& name, const Patch&, const MintModel* mint = nullptr) const;
    // nullopt if the file is missing or isn't a stash. `fallback` fills any missing parameters.
    // `mint` (optional) gets the stash's Mint model, or null if it has none.
    static std::optional<Patch> load (const juce::File&, const Patch& fallback, MintRef* mint = nullptr);
    juce::Array<juce::File> list() const; // sorted by name, case-insensitive
    juce::File fileFor (const juce::String& name) const;
    static juce::String nameOf (const juce::File& f) { return f.getFileNameWithoutExtension(); }
    static bool remove (const juce::File&); // to the bin/trash where possible

private:
    juce::File folder;
};
}
