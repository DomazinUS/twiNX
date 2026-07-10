#pragma once

#include "api/twitch_chat.hpp"

#include <borealis.hpp>

#include <string>
#include <vector>

class RecyclingGrid;
class TwitchDraftPreview;

class TwitchChatComposer : public brls::Box {
public:
    explicit TwitchChatComposer(std::string channel);
    ~TwitchChatComposer() override;

    brls::View* getDefaultFocus() override {
        return draftButton ? draftButton : this;
    }

private:
    enum class Tab {
        Recent = 0,
        Channel = 1,
        All = 2,
    };

    void openKeyboard();
    void insertEmote(const twitch::UserEmote& emote);
    void sendMessage();
    void setTab(Tab value);
    void cycleTab(int direction);
    void refreshGrid();
    void updateDraft();
    void updateTabs();
    void updateChannelTabVisibility();
    void reconcileRecentEmotes();
    std::vector<twitch::UserEmote> visibleEmotes() const;

    std::string channel;
    std::string draft;
    std::vector<twitch::UserEmote> emotes;
    std::vector<twitch::UserEmote> recentEmotes;
    Tab tab = Tab::Recent;
    bool loading = true;
    bool sending = false;
    bool channelSubscribed = false;
    bool subscriptionPermissionGranted = false;

    brls::Label* statusLabel = nullptr;
    TwitchDraftPreview* draftPreview = nullptr;
    brls::Label* counterLabel = nullptr;
    brls::Label* focusedEmoteLabel = nullptr;
    brls::Box* draftButton = nullptr;
    brls::Box* recentTab = nullptr;
    brls::Box* channelTab = nullptr;
    brls::Box* allTab = nullptr;
    RecyclingGrid* grid = nullptr;
};
