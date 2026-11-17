#include "Vault.h"
#include "CashCode.h"

namespace cash
{
Vault::Vault (size_t cap) : capacity (juce::jmax<size_t> (1, cap)) {}

int Vault::deposit (const Patch& patch, juce::int64 timeMs)
{
    if (const int existing = find (patch); existing >= 0)
        return existing;
    if (isFull())
        return -1;

    notes.push_back ({ patch, CashCode::serialNumber (patch), timeMs });
    return static_cast<int> (notes.size()) - 1;
}

bool Vault::shred (size_t index)
{
    if (index >= notes.size())
        return false;
    notes.erase (notes.begin() + static_cast<std::ptrdiff_t> (index));
    return true;
}

int Vault::find (const Patch& patch) const
{
    for (size_t i = 0; i < notes.size(); ++i)
        if (samePatch (notes[i].patch, patch))
            return static_cast<int> (i);
    return -1;
}
}
