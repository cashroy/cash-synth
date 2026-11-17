#include "CashCode.h"

namespace cash::CashCode
{
namespace
{
    constexpr const char* alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; // Crockford base32
    constexpr int groupSize = 4;
    constexpr int crcBits = 16;

    int bitsFor (const ParamSpec& s)
    {
        if (s.steps <= 1)
            return continuousBits;
        int bits = 0;
        while ((1 << bits) < s.steps)
            ++bits;
        return bits;
    }

    int paramsIn (int codeVersion)
    {
        switch (codeVersion)
        {
            case 1:  return ParameterMap::numParamsV1;
            case 2:  return ParameterMap::numParamsV2;
            case 3:  return ParameterMap::numParamsV3;
            case 4:  return ParameterMap::numParamsV4;
            case 5:  return ParameterMap::numParamsV5;
            case 6:  return ParameterMap::numParamsV6;
            case 7:  return ParameterMap::numParamsV7;
            default: return ParameterMap::numParams;
        }
    }

    int payloadBits (int codeVersion)
    {
        int n = 4;
        for (int i = 0; i < paramsIn (codeVersion); ++i)
            n += bitsFor (ParameterMap::spec (i));
        return n;
    }

    int payloadBytes (int v) { return (payloadBits (v) + 7) / 8; }
    int totalBits (int v)    { return payloadBytes (v) * 8 + crcBits; }
    int numChars (int v)     { return (totalBits (v) + 4) / 5; }

    struct BitWriter
    {
        std::vector<uint8_t> bits;
        void put (uint32_t value, int count)
        {
            for (int b = count - 1; b >= 0; --b)
                bits.push_back (static_cast<uint8_t> ((value >> b) & 1u));
        }
    };

    struct BitReader
    {
        const std::vector<uint8_t>& bits;
        size_t pos = 0;
        uint32_t get (int count)
        {
            uint32_t v = 0;
            for (int b = 0; b < count; ++b)
                v = (v << 1) | bits[pos++];
            return v;
        }
    };

    uint16_t crc16 (const std::vector<uint8_t>& bits, size_t numBits)
    {
        // CRC-16/CCITT-FALSE over whole bytes, MSB first.
        uint16_t crc = 0xFFFF;
        for (size_t byte = 0; byte < numBits / 8; ++byte)
        {
            uint8_t value = 0;
            for (size_t b = 0; b < 8; ++b)
                value = static_cast<uint8_t> ((value << 1) | bits[byte * 8 + b]);

            crc = static_cast<uint16_t> (crc ^ (value << 8));
            for (int i = 0; i < 8; ++i)
                crc = static_cast<uint16_t> ((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
        }
        return crc;
    }

    int charValue (juce::juce_wchar c)
    {
        c = juce::CharacterFunctions::toUpperCase (c);
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        for (int i = 0; i < 32; ++i)
            if (alphabet[i] == c)
                return i;
        return -1;
    }

    uint64_t fnv1a (const juce::String& s)
    {
        uint64_t h = 1469598103934665603ull;
        for (auto p = s.getCharPointer(); ! p.isEmpty(); ++p)
        {
            h ^= static_cast<uint64_t> (*p);
            h *= 1099511628211ull;
        }
        return h;
    }
}

juce::String encode (const Patch& patch)
{
    BitWriter w;
    w.put (static_cast<uint32_t> (version), 4);

    for (int i = 0; i < ParameterMap::numParams; ++i)
    {
        const auto& s = ParameterMap::spec (i);
        const float v = ParameterMap::snap (s, patch[static_cast<size_t> (i)]);
        const auto q = s.steps > 1 ? ParameterMap::toStep (s, v)
                                   : juce::roundToInt (v * static_cast<float> ((1 << continuousBits) - 1));
        w.put (static_cast<uint32_t> (q), bitsFor (s));
    }

    while (w.bits.size() % 8 != 0)
        w.put (0, 1);
    w.put (crc16 (w.bits, w.bits.size()), crcBits);
    while (w.bits.size() % 5 != 0)
        w.put (0, 1);

    juce::String out;
    out << prefix;
    for (size_t c = 0; c < w.bits.size() / 5; ++c)
    {
        if (c > 0 && c % groupSize == 0)
            out << '-';
        int v = 0;
        for (size_t b = 0; b < 5; ++b)
            v = (v << 1) | w.bits[c * 5 + b];
        out << alphabet[v];
    }
    return out;
}

std::optional<Patch> decode (juce::StringRef code, const Patch& fallback)
{
    std::vector<uint8_t> bits;
    bool seenPrefix = false;
    for (auto p = code.text; ! p.isEmpty(); ++p)
    {
        const auto c = *p;
        if (c == '-' || juce::CharacterFunctions::isWhitespace (c))
            continue;
        if (c == prefix && bits.empty() && ! seenPrefix)
        {
            seenPrefix = true;
            continue;
        }
        const int v = charValue (c);
        if (v < 0)
            return std::nullopt;
        for (int b = 4; b >= 0; --b)
            bits.push_back (static_cast<uint8_t> ((v >> b) & 1));
    }

    if (bits.size() < 4)
        return std::nullopt;

    BitReader r { bits };
    const auto codeVersion = static_cast<int> (r.get (4));
    if (codeVersion < 1 || codeVersion > version)
        return std::nullopt;
    if (static_cast<int> (bits.size()) != numChars (codeVersion) * 5)
        return std::nullopt;

    Patch patch = fallback;
    for (int i = 0; i < paramsIn (codeVersion); ++i)
    {
        const auto& s = ParameterMap::spec (i);
        const auto q = static_cast<int> (r.get (bitsFor (s)));
        if (s.steps > 1)
        {
            if (q >= s.steps)
                return std::nullopt;
            patch[static_cast<size_t> (i)] = ParameterMap::fromStep (s, q);
        }
        else
        {
            patch[static_cast<size_t> (i)] = static_cast<float> (q) / static_cast<float> ((1 << continuousBits) - 1);
        }
    }

    const auto payloadEnd = static_cast<size_t> (payloadBytes (codeVersion) * 8);
    while (r.pos < payloadEnd)
        if (r.get (1) != 0)
            return std::nullopt;

    const auto expected = crc16 (bits, payloadEnd);
    if (r.get (crcBits) != expected)
        return std::nullopt;

    while (r.pos < bits.size())
        if (r.get (1) != 0)
            return std::nullopt;

    return patch;
}

juce::String serialNumber (const Patch& patch)
{
    auto h = fnv1a (encode (patch));
    auto letter = [&h]
    {
        const char c = "ABCDEFGHJKLMNPQRSTUVWXYZ"[h % 24];
        h /= 24;
        return c;
    };

    juce::String s;
    s << letter() << letter() << ' ';
    s << juce::String (static_cast<int64_t> (h % 100000000ull)).paddedLeft ('0', 8);
    h /= 100000000ull;
    s << ' ' << letter();
    return s;
}
}
