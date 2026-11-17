#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Vault.h"

class CashSynthAudioProcessor;

namespace cash
{
// A gold coin you press to invest that amount.
class CoinButton final : public juce::Button
{
public:
    CoinButton (const juce::String& denomination, float amount);
    void paintButton (juce::Graphics&, bool isOver, bool isDown) override;
    bool hitTest (int x, int y) override;
    float getAmount() const { return amount; }

private:
    juce::String denomination;
    float amount;
};

// Stock-ticker style risk gauge: drag up/down or round, double-click for 50%.
class RiskDial final : public juce::Slider
{
public:
    RiskDial();
    void paint (juce::Graphics&) override;
    static juce::String rating (float risk); // "T-BILLS" ... "CRYPTO"
};

// A stack of bills in a currency strap. Banks the current sound.
class BankItButton final : public juce::Button
{
public:
    BankItButton();
    void paintButton (juce::Graphics&, bool isOver, bool isDown) override;
    void setState (bool full, bool banked);

private:
    bool vaultFull = false, alreadyBanked = false;
};

// One slot in the vault: a mini bill, or an empty space.
class VaultSlot final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit VaultSlot (int index);
    void setNote (const BankedNote*, bool isCurrent);
    bool isEmpty() const { return serial.isEmpty(); }
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    std::function<void (int)> onWithdraw, onShred, onCopy;

private:
    int index;
    juce::String serial;
    bool current = false;
};

// Everything under the bill: Invest coins, Risk, Ledger (undo/redo), Bank It + Vault.
class MoneyTray final : public juce::Component
{
public:
    explicit MoneyTray (CashSynthAudioProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

    void sync(); // pull ledger / vault / risk state from the processor

    static constexpr int numCoins = 4;
    CoinButton& getCoin (int i) { return *coins[static_cast<size_t> (i)]; }
    RiskDial& getRiskDial() { return risk; }
    juce::TextButton& getUndoButton() { return undo; }
    juce::TextButton& getRedoButton() { return redo; }
    BankItButton& getBankItButton() { return bankIt; }
    VaultSlot& getVaultSlot (int i) { return *slots[static_cast<size_t> (i)]; }
    int getNumVaultSlots() const { return static_cast<int> (slots.size()); }
    juce::String getMemo() const { return memo; }

    std::function<void()> onMoneyMoved;

private:
    struct Section { juce::String title; juce::Rectangle<int> bounds; };
    void afterMove();

    CashSynthAudioProcessor& processor;
    std::array<std::unique_ptr<CoinButton>, numCoins> coins;
    RiskDial risk;
    juce::TextButton undo { "UNDO" }, redo { "REDO" };
    BankItButton bankIt;
    std::vector<std::unique_ptr<VaultSlot>> slots;
    std::array<Section, 4> sections { { { "INVEST", {} }, { "RISK", {} }, { "LEDGER", {} }, { "VAULT", {} } } };
    juce::String memo, entry, vaultCount;
    juce::Rectangle<int> memoArea, vaultCountArea;
};
}
