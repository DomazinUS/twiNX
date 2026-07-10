#pragma once

#include "api/twitch_helix.hpp"

#include <borealis.hpp>

#include <string>

class TwiNXDetails : public brls::Box {
public:
    explicit TwiNXDetails(twitch::Stream stream);
    ~TwiNXDetails() override;

private:
    void chooseQuality();
    void updateQualityLabel();
    void watchLive();
    void setBusy(bool value, const std::string& message = "");

    twitch::Stream stream;
    std::string preferredQuality;
    bool busy = false;

    brls::Image* preview = nullptr;
    brls::Label* statusLabel = nullptr;
    brls::Label* qualityLabel = nullptr;
    brls::Box* watchButton = nullptr;
};
