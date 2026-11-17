#pragma once

#include <optional>
#include "ParameterMap.h"

// A CashCode is a whole patch as a short, shareable text code, e.g.
//   $C1A4-0ZQ8-...
// Layout (bits, MSB first): 4-bit version | each param in ParameterMap order
// (stepped: just enough bits for its step index; continuous: 8 bits) | zero pad
// to a byte | CRC-16/CCITT of the bytes so far. Written in Crockford base32,
// grouped in fours. Decoding ignores case, spaces and dashes, and reads O as 0
// and I/L as 1, so codes survive being typed by hand.
namespace cash::CashCode
{
inline constexpr int version = 2; // v2 (M6) appends 8 params; v1 codes still decode
inline constexpr int continuousBits = 8;
inline constexpr char prefix = '$';

juce::String encode (const Patch&);
// nullopt if malformed, unknown version or bad checksum. Older codes carry
// fewer params: the missing ones come from `fallback` (pass the default patch).
std::optional<Patch> decode (juce::StringRef code, const Patch& fallback = {});

// Banknote-style serial for the UI, e.g. "KD 04718823 Q". A fingerprint of the
// encoded patch: same sound, same serial. Not reversible (show the full code to share).
juce::String serialNumber (const Patch&);
}
