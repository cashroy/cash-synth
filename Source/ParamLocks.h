#pragma once

#include "Mutator.h"

namespace cash
{
// M12: knobs the user locked. Invest and Jackpot never touch a locked parameter.
// Changed on the message thread; toString() may run on any thread (hosts save
// state off it), so changes and reads share a lock. Saved with the project (not with stashes or vault notes:
// a lock is how you work, not part of the sound).
class ParamLocks
{
public:
    bool isLocked (juce::StringRef id) const
    {
        const int i = ParameterMap::indexOf (id);
        return i >= 0 && mask.test (static_cast<size_t> (i));
    }

    void setLocked (juce::StringRef id, bool locked)
    {
        const int i = ParameterMap::indexOf (id);
        if (i < 0 || mask.test (static_cast<size_t> (i)) == locked)
            return;
        {
            const juce::ScopedLock sl (lock);
            mask.set (static_cast<size_t> (i), locked);
        }
        if (onChange)
            onChange();
    }

    const LockMask& getMask() const { return mask; }
    size_t count() const { return mask.count(); }

    void clear()
    {
        if (mask.none())
            return;
        {
            const juce::ScopedLock sl (lock);
            mask.reset();
        }
        if (onChange)
            onChange();
    }

    // "filterCutoff,ampAttack": param IDs, so the list survives new params.
    juce::String toString() const
    {
        juce::StringArray ids;
        const juce::ScopedLock sl (lock);
        for (int i = 0; i < ParameterMap::numParams; ++i)
            if (mask.test (static_cast<size_t> (i)))
                ids.add (ParameterMap::spec (i).id);
        return ids.joinIntoString (",");
    }

    void fromString (const juce::String& s)
    {
        LockMask m;
        for (const auto& id : juce::StringArray::fromTokens (s, ",", {}))
            if (const int i = ParameterMap::indexOf (id.trim()); i >= 0)
                m.set (static_cast<size_t> (i));
        if (m == mask)
            return;
        {
            const juce::ScopedLock sl (lock);
            mask = m;
        }
        if (onChange)
            onChange();
    }

    std::function<void()> onChange; // the UI repaints its padlocks

private:
    LockMask mask;
    juce::CriticalSection lock;
};
}
