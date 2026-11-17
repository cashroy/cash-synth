#include "Ledger.h"

namespace cash
{
Ledger::Ledger (size_t cap) : capacity (juce::jmax<size_t> (2, cap)) {}

bool Ledger::record (const Patch& patch, const juce::String& memo, juce::int64 timeMs, MintRef mint)
{
    if (const auto* now = current(); now != nullptr && mint::sameSound (now->patch, now->mint.get(), patch, mint.get()))
        return false;

    if (! entries.empty())
        entries.erase (entries.begin() + static_cast<std::ptrdiff_t> (position + 1), entries.end());

    entries.push_back ({ patch, memo, timeMs, std::move (mint) });
    while (entries.size() > capacity)
        entries.pop_front();

    position = entries.size() - 1;
    return true;
}

std::optional<Patch> Ledger::undo()
{
    if (! canUndo())
        return std::nullopt;
    return entries[--position].patch;
}

std::optional<Patch> Ledger::redo()
{
    if (! canRedo())
        return std::nullopt;
    return entries[++position].patch;
}

void Ledger::clear()
{
    entries.clear();
    position = 0;
}
}
