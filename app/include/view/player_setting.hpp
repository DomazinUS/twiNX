//
// Created by fang on 2023/3/4.
//

#pragma once

#include <borealis.hpp>
#include <api/jellyfin/media.hpp>
#include <string>

class ButtonClose;

class PlayerSetting : public brls::Box {
public:
    PlayerSetting(const jellyfin::Source* src = nullptr, std::string twitchChannel = "");
    ~PlayerSetting() override;

    bool isTranslucent() override { return true; }

    View* getDefaultFocus() override { return this->settings->getDefaultFocus(); }

    inline static int selectedSubtitle = 0;
    inline static int selectedAudio = 0;

    enum class Equalizer {
        BRIGHTNESS,
        CONTRAST,
        SATURATION,
        HUE,
        GAMMA,
    };

private:
    BRLS_BIND(brls::ScrollingFrame, settings, "player/settings");
    BRLS_BIND(brls::Box, cancel, "player/cancel");

    BRLS_BIND(brls::SelectorCell, subtitleTrack, "setting/track/subtitle");
    BRLS_BIND(brls::SelectorCell, audioTrack, "setting/track/audio");
    BRLS_BIND(brls::SelectorCell, btnTwitchQuality, "setting/twitch/quality");
    BRLS_BIND(brls::SelectorCell, btnTwitchDecoder, "setting/twitch/decoder");
    BRLS_BIND(brls::SelectorCell, btnTwitchChatMode, "setting/twitch/chat_mode");
    BRLS_BIND(brls::SelectorCell, btnTwitchChatParticipation, "setting/twitch/chat_participation");
    BRLS_BIND(brls::SelectorCell, btnTwitchChatComposer, "setting/twitch/chat_composer");
    BRLS_BIND(brls::SelectorCell, btnTwitchChatEmotes, "setting/twitch/chat_emotes");
    BRLS_BIND(brls::SelectorCell, btnTwitchChatOverlaySize, "setting/twitch/chat_overlay_size");
    BRLS_BIND(brls::SelectorCell, btnTwitchChatOverlayPosition, "setting/twitch/chat_overlay_position");
    BRLS_BIND(brls::SelectorCell, btnTwitchChatDockedWidth, "setting/twitch/chat_docked_width");
    BRLS_BIND(brls::SelectorCell, btnTwitchChatOverlayWidth, "setting/twitch/chat_overlay_width");
    BRLS_BIND(brls::SelectorCell, btnTwitchChatFont, "setting/twitch/chat_font");
    BRLS_BIND(brls::SelectorCell, btnTwitchChatOpacity, "setting/twitch/chat_opacity");
    BRLS_BIND(brls::BooleanCell, btnTwitchChatTimestamps, "setting/twitch/chat_timestamps");
    BRLS_BIND(brls::BooleanCell, btnBottomBar, "setting/player/bottom_bar");
    BRLS_BIND(brls::BooleanCell, btnOSDOnToggle, "setting/player/osd_on_toggle");
    BRLS_BIND(brls::BooleanCell, btnFullscreen, "setting/fullscreen");
    BRLS_BIND(brls::BooleanCell, btnAlwaysOnTop, "setting/always_on_top");
    BRLS_BIND(brls::SelectorCell, btnVideoMirror, "setting/video/mirror");
    BRLS_BIND(brls::SelectorCell, btnVideoRotation, "setting/video/rotation");
    BRLS_BIND(brls::SelectorCell, btnVideoAspect, "setting/video/aspect");
    BRLS_BIND(brls::SliderCell, btnSubsync, "setting/video/subsync");
    // equalizer setting
    BRLS_BIND(brls::RadioCell, btnEqualizerReset, "setting/equalizer/reset");
    BRLS_BIND(brls::SliderCell, btnEqualizerBrightness, "setting/equalizer/brightness");
    BRLS_BIND(brls::SliderCell, btnEqualizerContrast, "setting/equalizer/contrast");
    BRLS_BIND(brls::SliderCell, btnEqualizerSaturation, "setting/equalizer/saturation");
    BRLS_BIND(brls::SliderCell, btnEqualizerGamma, "setting/equalizer/gamma");
    BRLS_BIND(brls::SliderCell, btnEqualizerHue, "setting/equalizer/hue");

    void setupEqualizer(brls::SliderCell* cell, const std::string& title, Equalizer item, double initValue);

    void registerHideBackground(brls::View* view);
};