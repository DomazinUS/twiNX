#include "activity/twinx_details.hpp"

#include "api/twitch.hpp"
#include "tab/remote_view.hpp"
#include "utils/image.hpp"
#include "view/mpv_core.hpp"

#include <algorithm>
#include <fmt/format.h>
#include <utility>
#include <vector>

namespace {

brls::Box* makeDetailsButton(const std::string& text, brls::Label** outLabel = nullptr) {
    auto* button = new brls::Box();
    button->setFocusable(true);
    button->setAxis(brls::Axis::ROW);
    button->setPadding(14, 24, 14, 24);
    button->setCornerRadius(8);
    button->setBackgroundColor(nvgRGB(91, 46, 145));
    button->setMarginBottom(14);
    button->addGestureRecognizer(new brls::TapGestureRecognizer(button));

    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(20);
    label->setTextColor(nvgRGB(250, 250, 252));
    button->addView(label);
    if (outLabel) *outLabel = label;
    return button;
}

std::string viewersText(int viewers) {
    if (viewers >= 1000000) return fmt::format("{:.1f}M viewers", viewers / 1000000.0);
    if (viewers >= 1000) return fmt::format("{:.1f}K viewers", viewers / 1000.0);
    return std::to_string(viewers) + " viewers";
}

const std::vector<std::string>& qualityLabels() {
    static const std::vector<std::string> labels = {
        "Auto", "Source", "1080p60", "720p60", "720p",
        "480p", "360p", "Audio only",
    };
    return labels;
}

const std::vector<std::string>& qualityValues() {
    static const std::vector<std::string> values = {
        "auto", "source", "1080p60", "720p60", "720p",
        "480p", "360p", "audio_only",
    };
    return values;
}

int qualityIndex(const std::string& value) {
    const auto& values = qualityValues();
    const auto found = std::find(values.begin(), values.end(), value);
    return found == values.end() ? 1 : static_cast<int>(std::distance(values.begin(), found));
}

}  // namespace

TwiNXDetails::TwiNXDetails(twitch::Stream stream)
    : stream(std::move(stream)), preferredQuality(twitch::loadPreferredQuality()) {
    if (preferredQuality.empty()) preferredQuality = twitch::loadConfig().preferredQuality;
    if (preferredQuality.empty()) preferredQuality = "source";

    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(nvgRGB(16, 18, 24));
    this->setPadding(28, 48, 28, 48);

    auto* heading = new brls::Label();
    heading->setText(stream.userName.empty() ? stream.userLogin : stream.userName);
    heading->setFontSize(34);
    heading->setTextColor(nvgRGB(245, 246, 250));
    heading->setMarginBottom(8);
    this->addView(heading);

    statusLabel = new brls::Label();
    statusLabel->setFontSize(17);
    statusLabel->setTextColor(stream.isLive ? nvgRGB(210, 120, 140) : nvgRGB(170, 177, 193));
    statusLabel->setText(stream.isLive ? "LIVE · " + viewersText(stream.viewerCount) : "OFFLINE");
    statusLabel->setMarginBottom(18);
    this->addView(statusLabel);

    auto* body = new brls::Box();
    body->setAxis(brls::Axis::ROW);
    body->setGrow(1.0f);
    this->addView(body);

    preview = new brls::Image();
    preview->setWidth(690);
    preview->setHeight(388);
    preview->setImageFromRes("img/video-card-bg.png");
    preview->setMarginRight(34);
    body->addView(preview);
    if (!stream.thumbnailUrl.empty())
        Image::with(preview, twitch::streamThumbnail(stream.thumbnailUrl, 690, 388));

    auto* info = new brls::Box();
    info->setAxis(brls::Axis::COLUMN);
    info->setGrow(1.0f);
    body->addView(info);

    auto addField = [info](const std::string& caption, const std::string& value) {
        auto* captionLabel = new brls::Label();
        captionLabel->setText(caption);
        captionLabel->setFontSize(15);
        captionLabel->setTextColor(nvgRGB(150, 158, 176));
        captionLabel->setMarginBottom(3);
        info->addView(captionLabel);

        auto* valueLabel = new brls::Label();
        valueLabel->setText(value.empty() ? "—" : value);
        valueLabel->setFontSize(19);
        valueLabel->setTextColor(nvgRGB(238, 240, 246));
        valueLabel->setMarginBottom(16);
        info->addView(valueLabel);
    };

    addField("Stream title", stream.title);
    addField("Category", stream.gameName);
    addField("Language", stream.language.empty() ? "Unknown" : stream.language);
    addField("Audience", stream.isLive ? viewersText(stream.viewerCount) : "Offline");

    watchButton = makeDetailsButton(stream.isLive ? "Watch live" : "Channel offline");
    watchButton->registerClickAction([this](brls::View*) {
        watchLive();
        return true;
    });
    info->addView(watchButton);

    auto* qualityButton = makeDetailsButton("Quality", &qualityLabel);
    qualityButton->registerClickAction([this](brls::View*) {
        chooseQuality();
        return true;
    });
    info->addView(qualityButton);
    updateQualityLabel();

    this->registerAction("Back", brls::BUTTON_B, [](brls::View*) {
        return brls::Application::popActivity();
    });

    brls::sync([this]() { brls::Application::giveFocus(watchButton); });
}

TwiNXDetails::~TwiNXDetails() {
    Image::cancel(preview);
}

void TwiNXDetails::setBusy(bool value, const std::string& message) {
    busy = value;
    if (!message.empty()) statusLabel->setText(message);
}

void TwiNXDetails::updateQualityLabel() {
    const int index = qualityIndex(preferredQuality);
    qualityLabel->setText("Quality · " + qualityLabels().at(index));
}

void TwiNXDetails::chooseQuality() {
    if (busy) return;
    auto* dropdown = new brls::Dropdown(
        "Preferred Twitch quality",
        qualityLabels(),
        [this](int selected) {
            if (selected < 0 || static_cast<size_t>(selected) >= qualityValues().size())
                return true;
            preferredQuality = qualityValues().at(selected);
            updateQualityLabel();
            if (!twitch::savePreferredQuality(preferredQuality))
                brls::Application::notify("twiNX: could not save quality preference");
            return true;
        },
        qualityIndex(preferredQuality));
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

void TwiNXDetails::watchLive() {
    if (busy) return;
    if (!stream.isLive) {
        statusLabel->setText("This channel is currently offline.");
        return;
    }

    auto config = twitch::loadConfig();
    config.channel = stream.userLogin;
    config.preferredQuality = preferredQuality;
    if (config.channel.empty()) {
        statusLabel->setText("This result did not contain a Twitch channel login.");
        return;
    }

    setBusy(true, "Resolving " + config.channel + "…");
    this->ptrLock();
    twitch::resolveLiveAsync(
        config,
        [this](twitch::Resolution result) {
            setBusy(false, "Playing " + result.channel + " · " + result.selected.name);
            const std::string channel = result.channel;
            const std::string url = result.selected.url;
            this->ptrUnlock();
            MPVCore::BOTTOM_BAR = false;
            RemoteView::play(url, "Twitch - " + channel, twitch::mpvExtra());
        },
        [this](const std::string& error) {
            setBusy(false, "Playback error: " + error);
            brls::Application::notify("twiNX: " + error);
            this->ptrUnlock();
        });
}
