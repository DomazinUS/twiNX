#pragma once

#include "api/http.hpp"
#include "api/twitch_helix.hpp"

#include <borealis.hpp>

#include <string>

class TwitchChannelPage : public brls::Box {
public:
    explicit TwitchChannelPage(std::string channelLogin);
    ~TwitchChannelPage() override;

    brls::View* getDefaultFocus() override;

private:
    void load();
    void showData(twitch::ChannelPageData data);
    void playLive();
    void playVod(
        const std::string& id,
        const std::string& title,
        const std::string& details);
    void playClip(
        const std::string& slug,
        const std::string& title,
        const std::string& details);
    void showMediaDetails(
        const std::string& title,
        const std::string& details);

    std::string channelLogin;
    twitch::ChannelPageData data;
    HTTP::Cancel requestCancel;
    HTTP::Cancel playbackCancel;

    brls::ScrollingFrame* scroll = nullptr;
    brls::Box* content = nullptr;
    brls::Label* status = nullptr;
    brls::Image* offlineImage = nullptr;
    brls::View* avatar = nullptr;
    brls::Label* name = nullptr;
    brls::Label* liveState = nullptr;
    brls::Label* headline = nullptr;
    brls::Label* description = nullptr;
    brls::Label* metadata = nullptr;
    brls::Box* playButton = nullptr;
    brls::Label* playButtonLabel = nullptr;
    brls::Box* refreshButton = nullptr;
    brls::Box* scheduleBox = nullptr;
    brls::Box* videosHolder = nullptr;
    brls::Box* clipsHolder = nullptr;
    brls::Box* categoriesHolder = nullptr;
};
