#pragma once

#include <deque>
#include <optional>
#include "ParameterMap.h"
#include "Mint.h"

// The Ledger is the patch history: every investment, jackpot or hand tweak is
// a transaction you can undo and redo. Message thread only.
namespace cash
{
struct Transaction
{
    Patch patch {};
    juce::String memo;  // "Invested 25% @ 50% risk", "Jackpot!", ...
    juce::int64 timeMs = 0;
    MintRef mint;       // M13: the Mint model this sound plays (if it switches the Mint on)
};

class Ledger
{
public:
    static constexpr size_t defaultCapacity = 100;

    explicit Ledger (size_t capacity = defaultCapacity);

    // Appends after the current entry and drops any redo tail. Ignored if the
    // patch equals the current one. Oldest entries go once over capacity.
    // Returns true if something was recorded.
    bool record (const Patch&, const juce::String& memo, juce::int64 timeMs = juce::Time::currentTimeMillis(), MintRef mint = {});

    std::optional<Patch> undo(); // moves back and returns that patch, or nullopt at the start
    std::optional<Patch> redo();
    bool canUndo() const { return ! entries.empty() && position > 0; }
    bool canRedo() const { return ! entries.empty() && position + 1 < entries.size(); }

    const Transaction* current() const { return entries.empty() ? nullptr : &entries[position]; }
    const Transaction& at (size_t index) const { return entries[index]; }
    size_t size() const { return entries.size(); }
    size_t getPosition() const { return position; }
    size_t getCapacity() const { return capacity; }
    void clear();

private:
    std::deque<Transaction> entries;
    size_t position = 0;
    size_t capacity;
};
}
