#include "api/twitch.hpp"
#include "api/twitch_chat.hpp"
#include "utils/config.hpp"
#include "utils/event.hpp"
#include "utils/haptics.hpp"
#include "utils/orientation.hpp"
#include "view/button_close.hpp"
#include "view/mpv_core.hpp"
#include "view/player_setting.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

using namespace brls::literals;

namespace {

const std::vector<std::string>& twitchQualityLabels() {
    static const std::vector<std::string> labels = {
        "Auto", "Source", "1080p60", "720p60", "720p",
        "480p", "360p", "Audio only",
    };
    return labels;
}

const std::vector<std::string>& twitchQualityValues() {
    static const std::vector<std::string> values = {
        "auto", "source", "1080p60", "720p60", "720p",
        "480p", "360p", "audio_only",
    };
    return values;
}

int twitchQualityIndex(const std::string& quality) {
    const auto& values = twitchQualityValues();
    const auto found = std::find(values.begin(), values.end(), quality);
    return found == values.end() ? 1 : static_cast<int>(std::distance(values.begin(), found));
}

const std::vector<std::string>& twitchDecoderLabels() {
    static const std::vector<std::string> labels = {
        "Software (stable)",
        "Hardware (experimental)",
        "Hybrid (experimental)",
    };
    return labels;
}

const std::vector<std::string>& portraitOrientationLabels() {
    static const std::vector<std::string> labels = {
        "Auto (Joy-Con sensor)",
        "Landscape",
        "Portrait clockwise",
        "Portrait counter-clockwise",
    };
    return labels;
}

const std::vector<std::string>& keyboardHapticLabels() {
    static const std::vector<std::string> labels = {
        "Off", "Light", "Medium", "Strong", "Maximum",
    };
    return labels;
}

const std::vector<std::string>& chatModeLabels() {
    static const std::vector<std::string> labels = {
        "Off", "Right panel", "Overlay",
    };
    return labels;
}

const std::vector<std::string>& chatParticipationLabels() {
    static const std::vector<std::string> labels = {
        "Read only",
        "Interactive",
    };
    return labels;
}

const std::vector<std::string>& chatComposerLabels() {
    static const std::vector<std::string> labels = {
        "Nintendo keyboard",
        "twiNX emote composer",
    };
    return labels;
}

const std::vector<std::string>& chatEmoteModeLabels() {
    static const std::vector<std::string> labels = {
        "Static",
        "Animated",
    };
    return labels;
}

const std::vector<std::string>& chatOverlaySizeLabels() {
    static const std::vector<std::string> labels = {
        "Full height",
        "Compact",
    };
    return labels;
}

const std::vector<std::string>& chatOverlayPositionLabels() {
    static const std::vector<std::string> labels = {
        "Top right",
        "Top left",
        "Bottom right",
        "Bottom left",
    };
    return labels;
}

const std::vector<std::string>& chatDockedWidthLabels() {
    static const std::vector<std::string> labels = {
        "Compact", "TV style", "Wide",
    };
    return labels;
}

const std::vector<int>& chatDockedWidthValues() {
    static const std::vector<int> values = {300, 330, 380};
    return values;
}

const std::vector<std::string>& chatOverlayWidthLabels() {
    static const std::vector<std::string> labels = {
        "Compact", "Standard", "Wide",
    };
    return labels;
}

const std::vector<int>& chatOverlayWidthValues() {
    static const std::vector<int> values = {300, 360, 430};
    return values;
}

const std::vector<std::string>& chatFontLabels() {
    static const std::vector<std::string> labels = {
        "Small", "Medium", "Large",
    };
    return labels;
}

const std::vector<int>& chatFontValues() {
    static const std::vector<int> values = {13, 16, 19};
    return values;
}

const std::vector<std::string>& chatOpacityLabels() {
    static const std::vector<std::string> labels = {
        "40%", "60%", "80%", "95%",
    };
    return labels;
}

const std::vector<int>& chatOpacityValues() {
    static const std::vector<int> values = {40, 60, 80, 95};
    return values;
}

int closestIndex(const std::vector<int>& values, int wanted) {
    int best = 0;
    int distance = std::abs(values.front() - wanted);
    for (size_t index = 1; index < values.size(); ++index) {
        const int candidate = std::abs(values[index] - wanted);
        if (candidate < distance) {
            distance = candidate;
            best = static_cast<int>(index);
        }
    }
    return best;
}

}  // namespace

PlayerSetting::PlayerSetting(const jellyfin::Source* src, std::string twitchChannel) {
    this->inflateFromXMLRes("xml/view/player_setting.xml");
    brls::Logger::debug("PlayerSetting: create");
    this->audioTrack->detail->setVisibility(brls::Visibility::GONE);
    this->subtitleTrack->detail->setVisibility(brls::Visibility::GONE);

    if (twitchChannel.empty()) {
        btnTwitchQuality->setVisibility(brls::Visibility::GONE);
        btnTwitchDecoder->setVisibility(brls::Visibility::GONE);
        btnPortraitOrientation->setVisibility(brls::Visibility::GONE);
        btnKeyboardHaptic->setVisibility(brls::Visibility::GONE);
        btnTwitchChatMode->setVisibility(brls::Visibility::GONE);
        btnTwitchChatParticipation->setVisibility(brls::Visibility::GONE);
        btnTwitchChatComposer->setVisibility(brls::Visibility::GONE);
        btnTwitchChatEmotes->setVisibility(brls::Visibility::GONE);
        btnTwitchChatDockedWidth->setVisibility(brls::Visibility::GONE);
        btnTwitchChatOverlayWidth->setVisibility(brls::Visibility::GONE);
        btnTwitchChatFont->setVisibility(brls::Visibility::GONE);
        btnTwitchChatOpacity->setVisibility(brls::Visibility::GONE);
        btnTwitchChatTimestamps->setVisibility(brls::Visibility::GONE);
    } else {
        const std::string savedQuality = twitch::loadPreferredQuality().empty()
            ? "source"
            : twitch::loadPreferredQuality();
        btnTwitchQuality->init(
            "Twitch stream quality",
            twitchQualityLabels(),
            twitchQualityIndex(savedQuality),
            [twitchChannel](int selected) {
                if (selected < 0 || static_cast<size_t>(selected) >= twitchQualityValues().size())
                    return;

                const std::string quality = twitchQualityValues().at(selected);
                if (!twitch::savePreferredQuality(quality))
                    brls::Application::notify("twiNX: could not save quality preference");

                auto config = twitch::loadConfig();
                config.channel = twitchChannel;
                config.preferredQuality = quality;
                brls::Application::notify("twiNX: switching Twitch quality…");

                twitch::resolveLiveAsync(
                    config,
                    [](twitch::Resolution result) {
                        MPVCore::BOTTOM_BAR = false;
                        MPVCore::instance().setUrl(result.selected.url, twitch::mpvExtra());
                        brls::Application::notify(
                            "twiNX: switched to " + result.selected.name);
                    },
                    [](const std::string& error) {
                        brls::Application::notify("twiNX quality error: " + error);
                    });
            });

        auto& orientationController =
            twinx::portrait::OrientationController::instance();
        btnPortraitOrientation->init(
            "Display orientation (experimental)",
            portraitOrientationLabels(),
            static_cast<int>(orientationController.mode()),
            [](int selected) {
                const int bounded = std::clamp(selected, 0, 3);
                auto& controller =
                    twinx::portrait::OrientationController::instance();
                if (!controller.setMode(
                        static_cast<twinx::portrait::OrientationMode>(bounded))) {
                    brls::Application::notify(
                        "Portrait Lab: could not save orientation preference");
                    return;
                }
                if (bounded == 0) {
                    brls::Application::notify(
                        "Portrait Lab: automatic Joy-Con orientation enabled");
                }
            });

        btnKeyboardHaptic->init(
            "Touch keyboard vibration intensity",
            keyboardHapticLabels(),
            std::clamp(
                static_cast<int>(std::lround(
                    twinx::haptics::keyboardIntensity() * 4.0f)),
                0,
                4),
            [](int selected) {
                const float intensity =
                    static_cast<float>(std::clamp(selected, 0, 4)) / 4.0f;
                if (!twinx::haptics::setKeyboardIntensity(intensity))
                    brls::Application::notify(
                        "Portrait Lab: could not save vibration preference");
                twinx::haptics::keyboardPulse();
            });

        const twitch::DecoderMode decoderMode =
            twitch::loadDecoderMode();

        btnTwitchDecoder->init(
            "Twitch decoder",
            twitchDecoderLabels(),
            static_cast<int>(decoderMode),
            [twitchChannel](int selected) {
                if (selected < static_cast<int>(twitch::DecoderMode::Software) ||
                    selected > static_cast<int>(twitch::DecoderMode::Hybrid))
                    return;

                const auto mode = static_cast<twitch::DecoderMode>(selected);
                if (!twitch::saveDecoderMode(mode)) {
                    brls::Application::notify(
                        "twiNX: could not save decoder preference");
                    return;
                }

                auto& mpv = MPVCore::instance();
                if (mode == twitch::DecoderMode::Software) {
                    brls::Application::notify(
                        "twiNX: using stable software decoding");
                    mpv.command("set", "hwdec", "no");
                } else {
                    mpv.command("set", "vd-lavc-dr", "no");
                    mpv.command(
                        "set",
                        "hwdec",
                        MPVCore::PLAYER_HWDEC_METHOD.c_str());
                    brls::Application::notify(
                        mode == twitch::DecoderMode::Hybrid
                            ? "twiNX: Hybrid starts in hardware and falls back "
                              "to software during Twitch transitions"
                            : "Hardware decoding is experimental and may crash "
                              "after Twitch commercials");
                }

                auto config = twitch::loadConfig();
                config.channel = twitchChannel;
                brls::Application::notify(
                    "twiNX: reloading stream with selected decoder…");

                twitch::resolveLiveAsync(
                    config,
                    [](twitch::Resolution result) {
                        MPVCore::BOTTOM_BAR = false;
                        MPVCore::instance().setUrl(
                            result.selected.url,
                            twitch::mpvExtra());
                    },
                    [](const std::string& error) {
                        brls::Application::notify(
                            "twiNX decoder reload error: " + error);
                    });
            });

        const twitch::ChatPreferences chat = twitch::loadChatPreferences();

        btnTwitchChatMode->init(
            "Live chat",
            chatModeLabels(),
            static_cast<int>(chat.mode),
            [](int selected) {
                auto preferences = twitch::loadChatPreferences();
                preferences.mode = static_cast<twitch::ChatMode>(
                    std::clamp(selected, 0, 2));
                if (!twitch::saveChatPreferences(preferences))
                    brls::Application::notify("twiNX: could not save chat mode");
            });

        btnTwitchChatParticipation->init(
            "Chat participation",
            chatParticipationLabels(),
            static_cast<int>(chat.participation),
            [](int selected) {
                auto preferences = twitch::loadChatPreferences();
                preferences.participation =
                    selected == 1
                        ? twitch::ChatParticipation::Interactive
                        : twitch::ChatParticipation::ReadOnly;
                if (!twitch::saveChatPreferences(preferences)) {
                    brls::Application::notify(
                        "twiNX: could not save chat participation");
                    return;
                }

                if (preferences.participation ==
                        twitch::ChatParticipation::Interactive &&
                    !twitch::hasChatWriteScope()) {
                    brls::Application::notify(
                        "Interactive chat requires signing out and "
                        "signing in again once");
                }
            });

        btnTwitchChatComposer->init(
            "Message composer",
            chatComposerLabels(),
            static_cast<int>(chat.composerMode),
            [](int selected) {
                auto preferences =
                    twitch::loadChatPreferences();
                preferences.composerMode =
                    selected == 0
                        ? twitch::ChatComposerMode::NintendoKeyboard
                        : twitch::ChatComposerMode::TwiNXComposer;

                if (!twitch::saveChatPreferences(preferences)) {
                    brls::Application::notify(
                        "twiNX: could not save composer mode");
                }
            });

        btnTwitchChatEmotes->init(
            "Chat emotes",
            chatEmoteModeLabels(),
            static_cast<int>(chat.emoteMode),
            [](int selected) {
                auto preferences =
                    twitch::loadChatPreferences();
                preferences.emoteMode =
                    selected == 1
                        ? twitch::ChatEmoteMode::Animated
                        : twitch::ChatEmoteMode::Static;

                if (!twitch::saveChatPreferences(preferences)) {
                    brls::Application::notify(
                        "twiNX: could not save chat emote mode");
                    return;
                }

                brls::Application::notify(
                    preferences.emoteMode ==
                            twitch::ChatEmoteMode::Animated
                        ? "Animated chat emotes enabled"
                        : "Static chat emotes enabled");
            });

        btnTwitchChatOverlaySize->init(
            "Overlay chat size",
            chatOverlaySizeLabels(),
            static_cast<int>(chat.overlaySize),
            [](int selected) {
                auto preferences = twitch::loadChatPreferences();
                preferences.overlaySize =
                    selected == 1
                        ? twitch::ChatOverlaySize::Compact
                        : twitch::ChatOverlaySize::FullHeight;
                if (!twitch::saveChatPreferences(preferences))
                    brls::Application::notify(
                        "twiNX: could not save overlay chat size");
            });

        btnTwitchChatOverlayPosition->init(
            "Overlay chat position",
            chatOverlayPositionLabels(),
            static_cast<int>(chat.overlayPosition),
            [](int selected) {
                auto preferences = twitch::loadChatPreferences();
                preferences.overlayPosition =
                    static_cast<twitch::ChatOverlayPosition>(
                        std::clamp(selected, 0, 3));
                if (!twitch::saveChatPreferences(preferences))
                    brls::Application::notify(
                        "twiNX: could not save overlay chat position");
            });

        btnTwitchChatDockedWidth->init(
            "Right chat panel width",
            chatDockedWidthLabels(),
            closestIndex(
                chatDockedWidthValues(),
                chat.dockedWidth),
            [](int selected) {
                if (selected < 0 ||
                    static_cast<size_t>(selected) >=
                        chatDockedWidthValues().size())
                    return;
                auto preferences = twitch::loadChatPreferences();
                preferences.dockedWidth =
                    chatDockedWidthValues().at(selected);
                if (!twitch::saveChatPreferences(preferences))
                    brls::Application::notify(
                        "twiNX: could not save docked chat width");
            });

        btnTwitchChatOverlayWidth->init(
            "Overlay chat width",
            chatOverlayWidthLabels(),
            closestIndex(
                chatOverlayWidthValues(),
                chat.overlayWidth),
            [](int selected) {
                if (selected < 0 ||
                    static_cast<size_t>(selected) >=
                        chatOverlayWidthValues().size())
                    return;
                auto preferences = twitch::loadChatPreferences();
                preferences.overlayWidth =
                    chatOverlayWidthValues().at(selected);
                if (!twitch::saveChatPreferences(preferences))
                    brls::Application::notify(
                        "twiNX: could not save overlay chat width");
            });

        btnTwitchChatFont->init(
            "Chat text size",
            chatFontLabels(),
            closestIndex(chatFontValues(), chat.fontSize),
            [](int selected) {
                if (selected < 0 ||
                    static_cast<size_t>(selected) >= chatFontValues().size())
                    return;
                auto preferences = twitch::loadChatPreferences();
                preferences.fontSize = chatFontValues().at(selected);
                if (!twitch::saveChatPreferences(preferences))
                    brls::Application::notify("twiNX: could not save chat text size");
            });

        btnTwitchChatOpacity->init(
            "Chat background",
            chatOpacityLabels(),
            closestIndex(chatOpacityValues(), chat.opacity),
            [](int selected) {
                if (selected < 0 ||
                    static_cast<size_t>(selected) >= chatOpacityValues().size())
                    return;
                auto preferences = twitch::loadChatPreferences();
                preferences.opacity = chatOpacityValues().at(selected);
                if (!twitch::saveChatPreferences(preferences))
                    brls::Application::notify("twiNX: could not save chat opacity");
            });

        btnTwitchChatTimestamps->init(
            "Chat timestamps",
            chat.timestamps,
            [](bool enabled) {
                auto preferences = twitch::loadChatPreferences();
                preferences.timestamps = enabled;
                if (!twitch::saveChatPreferences(preferences))
                    brls::Application::notify("twiNX: could not save timestamp preference");
            });
    }

    this->registerAction("hints/cancel"_i18n, brls::BUTTON_B, [](brls::View* view) {
        brls::Application::popActivity();
        return true;
    });

    this->cancel->registerClickAction([](...) {
        brls::Application::popActivity();
        return true;
    });
    this->cancel->addGestureRecognizer(new brls::TapGestureRecognizer(this->cancel));

    auto& mpv = MPVCore::instance();

    std::vector<std::string> audioTrack, audioSource;
    std::vector<int> audioStream;
    std::vector<std::string> subTrack = {"main/player/none"_i18n};
    std::vector<std::string> subSource = {"main/player/none"_i18n};
    std::vector<int> subStream = {0};

    int64_t count = mpv.getInt("track-list/count");
    for (int64_t n = 0; n < count; n++) {
        std::string type = mpv.getString(fmt::format("track-list/{}/type", n));
        std::string title = mpv.getString(fmt::format("track-list/{}/title", n));
        if (title.empty()) title = mpv.getString(fmt::format("track-list/{}/lang", n));
        if (title.empty()) title = fmt::format("{} track {}", type, n);
        if (type == "sub")
            subTrack.push_back(title);
        else if (type == "audio")
            audioTrack.push_back(title);
    }

    if (src != nullptr) {
        for (auto& s : src->MediaStreams) {
            if (s.Type == jellyfin::streamTypeAudio) {
                audioSource.push_back(s.DisplayTitle);
                audioStream.push_back(s.Index);
            } else if (s.Type == jellyfin::streamTypeSubtitle) {
                subSource.push_back(s.DisplayTitle);
                subStream.push_back(s.Index);
            }
        }
    }
    // 字幕选择
    if (subTrack.size() > 1) {
        int64_t value = mpv.getInt("sid");
        this->subtitleTrack->init("main/player/subtitle"_i18n, subTrack, value, [&mpv](int selected) {
            selectedSubtitle = selected;
            mpv.setInt("sid", selected);
        });
    } else if (subSource.size() > 1) {
        int value = 0;
        for (size_t i = 0; i < subStream.size(); i++)
            if (subStream[i] == selectedSubtitle) value = i;
        this->subtitleTrack->init("main/player/subtitle"_i18n, subSource, value, [subStream](int selected) {
            selectedSubtitle = subStream[selected];
            MPVCore::instance().getCustomEvent()->fire(QUALITY_CHANGE, nullptr);
        });
    } else {
        this->subtitleTrack->setVisibility(brls::Visibility::GONE);
    }
    // 音轨选择
    if (audioTrack.size() > 1) {
        int64_t value = mpv.getInt("aid", 1) - 1;
        this->audioTrack->init("main/player/audio"_i18n, audioTrack, value, [&mpv](int selected) {
            selectedAudio = selected + 1;
            mpv.setInt("aid", selectedAudio);
        });
        this->audioTrack->detail->setVisibility(brls::Visibility::GONE);
    } else if (audioSource.size() > 1) {
        int value = 0;
        for (size_t i = 0; i < audioStream.size(); i++)
            if (audioStream[i] == selectedAudio) value = i;
        this->audioTrack->init("main/player/audio"_i18n, audioSource, value, [audioStream](int selected) {
            selectedAudio = audioStream[selected];
            MPVCore::instance().getCustomEvent()->fire(QUALITY_CHANGE, nullptr);
        });
    } else {
        this->audioTrack->setVisibility(brls::Visibility::GONE);
    }

    auto& conf = AppConfig::instance();

/// Fullscreen
#if (defined(__APPLE__) || defined(__linux__) || defined(_WIN32)) && !defined(ANDROID)
    btnFullscreen->init(
        "main/setting/others/fullscreen"_i18n, conf.getItem(AppConfig::FULLSCREEN, false), [](bool value) {
            VideoContext::FULLSCREEN = value;
            AppConfig::instance().setItem(AppConfig::FULLSCREEN, value);
            brls::Application::getPlatform()->getVideoContext()->fullScreen(value);
        });

    btnAlwaysOnTop->init(
        "main/setting/others/always_on_top"_i18n, conf.getItem(AppConfig::ALWAYS_ON_TOP, false), [](bool value) {
            AppConfig::instance().setItem(AppConfig::ALWAYS_ON_TOP, value);
            brls::Application::getPlatform()->setWindowAlwaysOnTop(value);
        });
#else
    btnFullscreen->setVisibility(brls::Visibility::GONE);
    btnAlwaysOnTop->setVisibility(brls::Visibility::GONE);
#endif

    btnBottomBar->init(
        "main/setting/playback/bottom_bar"_i18n, conf.getItem(AppConfig::PLAYER_BOTTOM_BAR, true), [&conf](bool value) {
            MPVCore::BOTTOM_BAR = value;
            conf.setItem(AppConfig::PLAYER_BOTTOM_BAR, value);
        });

    btnOSDOnToggle->init(
        "main/setting/playback/osd_on_toggle"_i18n, conf.getItem(AppConfig::OSD_ON_TOGGLE, true), [&conf](bool value) {
            MPVCore::OSD_ON_TOGGLE = value;
            conf.setItem(AppConfig::OSD_ON_TOGGLE, value);
        });

    /// Player mirror
    btnVideoMirror->init("main/setting/filter/mirror"_i18n,
        {
            "hints/off"_i18n,
            "main/setting/filter/hflip"_i18n,
            "main/setting/filter/vflip"_i18n,
        },
        MPVCore::VIDEO_FILTER, [&mpv](int value) {
            MPVCore::VIDEO_FILTER = value;
            switch (value) {
            case 1:
                mpv.command("set", "vf", "hflip");
                break;
            case 2:
                mpv.command("set", "vf", "vflip");
                break;
            default:
                mpv.command("set", "vf", "");
            }
            // 如果正在使用硬解，那么将硬解更新为 auto-copy，避免直接硬解因为不经过 cpu 处理导致镜像翻转无效
            if (MPVCore::HARDWARE_DEC) {
                const char* hwdec = value > 0 ? "auto-copy" : MPVCore::PLAYER_HWDEC_METHOD.c_str();
                mpv.command("set", "hwdec", hwdec);
                brls::Logger::info("MPV hardware decode: {}", hwdec);
            }
        });

    btnVideoRotation->init("main/setting/filter/rotation"_i18n,
        {
            "hints/off"_i18n,
            "90",
            "180",
            "270",
        },
        MPVCore::VIDEO_ROTATION, [&mpv](int value) {
            MPVCore::VIDEO_ROTATION = value;
            switch (value) {
            case 1:
                mpv.command("set", "video-rotate", "90");
                return;
            case 2:
                mpv.command("set", "video-rotate", "180");
                return;
            case 3:
                mpv.command("set", "video-rotate", "270");
                return;
            default:
                mpv.command("set", "video-rotate", "0");
            }
        });

    /// Player aspect
    btnVideoAspect->init("main/setting/aspect/header"_i18n,
        {
            "main/setting/aspect/auto"_i18n,
            "main/setting/aspect/stretch"_i18n,
            "main/setting/aspect/crop"_i18n,
            "4:3",
            "16:9",
        },
        conf.getOptionIndex(AppConfig::PLAYER_ASPECT), [&mpv, &conf](int value) {
            auto& opt = conf.getOptions(AppConfig::PLAYER_ASPECT);
            MPVCore::VIDEO_ASPECT = opt.options.at(value);
            mpv.setAspect(MPVCore::VIDEO_ASPECT);
            conf.setItem(AppConfig::PLAYER_ASPECT, MPVCore::VIDEO_ASPECT);
        });

    /// Subsync
    double subDelay = mpv.getDouble("sub-delay");
    btnSubsync->title->setMarginRight(0);
    btnSubsync->slider->setMarginRight(0);
    btnSubsync->slider->setPointerSize(20);
    btnSubsync->setDetailText(fmt::format("{:.1f}", subDelay));
    btnSubsync->init("main/setting/playback/subsync"_i18n, (subDelay + 10) * 0.05f, [this](float value) {
        float data = value * 20 - 10.f;
        MPVCore::instance().setDouble("sub-delay", data);
        btnSubsync->setDetailText(fmt::format("{:.1f}", data));
    });

    btnEqualizerReset->registerClickAction([this](View* view) {
        btnEqualizerBrightness->slider->setProgress(0.5f);
        btnEqualizerContrast->slider->setProgress(0.5f);
        btnEqualizerSaturation->slider->setProgress(0.5f);
        btnEqualizerGamma->slider->setProgress(0.5f);
        btnEqualizerHue->slider->setProgress(0.5f);
        return true;
    });
    registerHideBackground(btnEqualizerReset);
    setupEqualizer(btnEqualizerBrightness, "main/setting/equalizer/brightness"_i18n, Equalizer::BRIGHTNESS,
        mpv.getDouble("brightness"));
    setupEqualizer(
        btnEqualizerContrast, "main/setting/equalizer/contrast"_i18n, Equalizer::CONTRAST, mpv.getDouble("contrast"));
    setupEqualizer(btnEqualizerSaturation, "main/setting/equalizer/saturation"_i18n, Equalizer::SATURATION,
        mpv.getDouble("saturation"));
    setupEqualizer(btnEqualizerGamma, "main/setting/equalizer/gamma"_i18n, Equalizer::GAMMA, mpv.getDouble("hue"));
    setupEqualizer(btnEqualizerHue, "main/setting/equalizer/hue"_i18n, Equalizer::HUE, mpv.getDouble("gamma"));
}

PlayerSetting::~PlayerSetting() { brls::Logger::debug("PlayerSetting: delete"); }

void PlayerSetting::setupEqualizer(brls::SliderCell* cell, const std::string& title, Equalizer item, double initValue) {
    if (initValue < -100)
        initValue = -100;
    else if (initValue > 100)
        initValue = 100;

    cell->detail->setWidth(50);
    cell->title->setWidth(116);
    cell->title->setMarginRight(0);
    cell->slider->setStep(0.05f);
    cell->slider->setMarginRight(0);
    cell->slider->setPointerSize(20);
    cell->setDetailText(fmt::format("{:.0f}", initValue));
    cell->init(title, (initValue + 100) * 0.005f, [cell, item](float value) {
        auto& mpv = MPVCore::instance();
        int data = (int)(value * 200 - 100);
        cell->setDetailText(std::to_string(data));
        switch (item) {
        case Equalizer::BRIGHTNESS:
            mpv.setInt("brightness", data);
            break;
        case Equalizer::CONTRAST:
            mpv.setInt("contrast", data);
            break;
        case Equalizer::SATURATION:
            mpv.setInt("saturation", data);
            break;
        case Equalizer::GAMMA:
            mpv.setInt("gamma", data);
            break;
        case Equalizer::HUE:
            mpv.setInt("hue", data);
            break;
        default:;
        }
    });
    registerHideBackground(cell->getDefaultFocus());
}

void PlayerSetting::registerHideBackground(brls::View* view) {
    view->getFocusEvent()->subscribe([this](...) { this->setBackgroundColor(nvgRGBAf(0.0f, 0.0f, 0.0f, 0.0f)); });
    view->getFocusLostEvent()->subscribe(
        [this](...) { this->setBackgroundColor(brls::Application::getTheme().getColor("brls/backdrop")); });
}
