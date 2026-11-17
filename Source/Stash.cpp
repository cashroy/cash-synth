#include "Stash.h"
#include "CashCode.h"

namespace cash
{
namespace
{
    const juce::Identifier rootType ("CashStash"), patchType ("Patch");
}

Stash::Stash (juce::File f) : folder (std::move (f)) {}

juce::File Stash::defaultFolder()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return base.getChildFile ("Cashroy").getChildFile ("Cash Synth").getChildFile ("Stash");
}

juce::File Stash::fileFor (const juce::String& name) const
{
    auto legal = juce::File::createLegalFileName (name.trim());
    if (legal.isEmpty())
        legal = "Untitled";
    return folder.getChildFile (legal + extension);
}

juce::File Stash::save (const juce::String& name, const Patch& patch, const MintModel* mint) const
{
    if (! folder.createDirectory())
        return {};

    const auto file = fileFor (name);
    juce::ValueTree root (rootType);
    root.setProperty ("version", formatVersion, nullptr);
    root.setProperty ("name", nameOf (file), nullptr);
    root.setProperty ("code", CashCode::encode (patch), nullptr);
    root.appendChild (patchToTree (patch, patchType), nullptr);
    if (mint != nullptr && patch[static_cast<size_t> (ParameterMap::indexOf ("mintOn"))] >= 0.5f)
    {
        juce::ValueTree m ("Mint");
        m.setProperty ("data", mint->toMemory().toBase64Encoding(), nullptr);
        root.appendChild (m, nullptr);
    }

    const auto xml = root.createXml();
    if (xml == nullptr || ! xml->writeTo (file))
        return {};
    return file;
}

std::optional<Patch> Stash::load (const juce::File& file, const Patch& fallback, MintRef* mint)
{
    const auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName (rootType.toString()))
        return std::nullopt;

    const auto root = juce::ValueTree::fromXml (*xml);
    if (mint != nullptr)
    {
        mint->reset();
        juce::MemoryBlock block;
        if (const auto m = root.getChildWithName ("Mint"); m.isValid() && block.fromBase64Encoding (m.getProperty ("data").toString()))
            *mint = MintModel::fromMemory (block.getData(), block.getSize());
    }
    const auto patch = root.getChildWithName (patchType);
    if (patch.isValid())
        return patchFromTree (patch, fallback);

    // Hand-written stash with only a code: still accept it.
    if (auto decoded = CashCode::decode (root.getProperty ("code").toString(), fallback))
        return decoded;
    return std::nullopt;
}

juce::Array<juce::File> Stash::list() const
{
    auto files = folder.findChildFiles (juce::File::findFiles, false, juce::String ("*") + extension);
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
               { return nameOf (a).compareNatural (nameOf (b)) < 0; });
    return files;
}

bool Stash::remove (const juce::File& file)
{
    if (! file.existsAsFile())
        return false;
    return file.moveToTrash() || file.deleteFile();
}
}
