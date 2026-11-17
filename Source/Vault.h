#pragma once

#include <vector>
#include "ParameterMap.h"
#include "Mint.h"

// The Vault holds patches you chose to keep ("Bank It"). Unlike the Ledger it
// never forgets on its own: when it's full you shred a note to make room.
// Message thread only.
namespace cash
{
struct BankedNote
{
    Patch patch {};
    juce::String serial;   // CashCode::serialNumber of the patch
    juce::int64 timeMs = 0;
    MintRef mint;          // M13: the Mint model it plays (if it switches the Mint on)
};

class Vault
{
public:
    static constexpr size_t defaultCapacity = 8;

    explicit Vault (size_t capacity = defaultCapacity);

    // Banks the patch. Returns its index (the existing one if this exact patch
    // is already banked), or -1 if the vault is full.
    int deposit (const Patch&, juce::int64 timeMs = juce::Time::currentTimeMillis(), MintRef mint = {});
    bool shred (size_t index);
    int find (const Patch&, const MintModel* mint = nullptr) const; // -1 if not banked

    const BankedNote& at (size_t index) const { return notes[index]; }
    size_t size() const { return notes.size(); }
    size_t getCapacity() const { return capacity; }
    bool isFull() const { return notes.size() >= capacity; }
    void clear() { notes.clear(); }

private:
    std::vector<BankedNote> notes;
    size_t capacity;
};
}
