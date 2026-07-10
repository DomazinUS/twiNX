#include "activity/twitch_channel.hpp"

#include "api/twitch.hpp"
#include "api/twitch_helix.hpp"
#include "tab/remote_view.hpp"
#include "utils/image.hpp"
#include "view/h_recycling.hpp"
#include "view/recycling_grid.hpp"

#include <borealis/views/dialog.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace {

std::string compactNumber(int value) {
    if (value >= 1000000)
        return fmt::format(
            "{:.1f}M",
            value / 1000000.0);
    if (value >= 1000)
        return fmt::format(
            "{:.1f}K",
            value / 1000.0);
    return std::to_string(value);
}

std::string joinTags(
    const std::vector<std::string>& tags) {
    std::string result;
    const size_t count =
        std::min<size_t>(tags.size(), 6);
    for (size_t index = 0;
         index < count;
         ++index) {
        if (!result.empty())
            result += "  ·  ";
        result += tags[index];
    }
    return result;
}

class CircularAvatar : public brls::View {
public:
    CircularAvatar() {
        loader = new brls::Image();
        loader->setFreeTexture(false);
    }

    ~CircularAvatar() override {
        Image::cancel(loader);
        delete loader;
    }

    void setUrl(const std::string& url) {
        Image::cancel(loader);
        currentUrl = url;
        if (!currentUrl.empty())
            Image::with(loader, currentUrl);
        this->invalidate();
    }

    void draw(
        NVGcontext* vg,
        float x,
        float y,
        float width,
        float height,
        brls::Style,
        brls::FrameContext*) override {
        const float size =
            std::min(width, height);
        const float cx =
            x + width * 0.5f;
        const float cy =
            y + height * 0.5f;
        const float radius =
            size * 0.5f - 2.0f;

        nvgSave(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, radius);
        nvgFillColor(
            vg,
            nvgRGB(91, 46, 145));
        nvgFill(vg);

        if (loader->getTexture() > 0) {
            NVGpaint paint =
                nvgImagePattern(
                    vg,
                    cx - radius,
                    cy - radius,
                    radius * 2.0f,
                    radius * 2.0f,
                    0,
                    loader->getTexture(),
                    1.0f);
            nvgBeginPath(vg);
            nvgCircle(vg, cx, cy, radius);
            nvgFillPaint(vg, paint);
            nvgFill(vg);
        }

        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, radius);
        nvgStrokeColor(
            vg,
            nvgRGBA(255, 255, 255, 100));
        nvgStrokeWidth(vg, 2.0f);
        nvgStroke(vg);
        nvgRestore(vg);
    }

private:
    brls::Image* loader = nullptr;
    std::string currentUrl;
};

enum class MediaKind { Details, Vod, Clip };

struct MediaCard {
    MediaKind kind = MediaKind::Details;
    std::string id;
    std::string title;
    std::string subtitle;
    std::string thumbnail;
    std::string details;
};

class MediaCell : public RecyclingGridItem {
public:
    MediaCell() {
        this->setAxis(brls::Axis::COLUMN);
        this->setFocusable(true);
        this->setCornerRadius(8);
        this->setBackgroundColor(
            nvgRGB(31, 35, 46));
        this->setPadding(0, 0, 10, 0);

        image = new brls::Image();
        image->setHeight(122);
        image->setWidthPercentage(100);
        image->setImageFromRes(
            "img/video-card-bg.png");
        this->addView(image);

        title = new brls::Label();
        title->setFontSize(16);
        title->setSingleLine(true);
        title->setMarginTop(7);
        title->setMarginLeft(9);
        title->setMarginRight(9);
        title->setTextColor(
            nvgRGB(245, 246, 250));
        this->addView(title);

        subtitle = new brls::Label();
        subtitle->setFontSize(13);
        subtitle->setSingleLine(true);
        subtitle->setMarginTop(3);
        subtitle->setMarginLeft(9);
        subtitle->setMarginRight(9);
        subtitle->setTextColor(
            nvgRGB(164, 173, 192));
        this->addView(subtitle);
    }

    ~MediaCell() override {
        Image::cancel(image);
    }

    void prepareForReuse() override {
        Image::cancel(image);
        image->setImageFromRes(
            "img/video-card-bg.png");
        title->setText("");
        subtitle->setText("");
    }

    void cacheForReuse() override {
        Image::cancel(image);
    }

    brls::Image* image = nullptr;
    brls::Label* title = nullptr;
    brls::Label* subtitle = nullptr;
};

class MediaDataSource :
    public RecyclingGridDataSource {
public:
    using Selection =
        std::function<void(const MediaCard&)>;

    MediaDataSource(
        std::vector<MediaCard> items,
        Selection selected,
        brls::View* focusAnchor)
        : items(std::move(items)),
          selected(std::move(selected)),
          focusAnchor(focusAnchor) {}

    size_t getItemCount() override {
        return items.size();
    }

    RecyclingGridItem* cellForRow(
        RecyclingView* recycler,
        size_t index) override {
        auto* cell =
            dynamic_cast<MediaCell*>(
                recycler->dequeueReusableCell(
                    "ChannelMedia"));
        const auto& item = items.at(index);
        cell->title->setText(item.title);
        cell->subtitle->setText(
            item.subtitle);
        if (!item.thumbnail.empty())
            Image::with(
                cell->image,
                item.thumbnail);

        // Horizontal media rows recycle cells as they move off-screen.
        // Always give UP a stable, permanent destination outside the
        // recycler so focus can never remain attached to a recycled card.
        if (focusAnchor)
            cell->setCustomNavigationRoute(
                brls::FocusDirection::UP,
                focusAnchor);
        return cell;
    }

    void onItemSelected(
        brls::Box*,
        size_t index) override {
        if (selected)
            selected(items.at(index));
    }

    void clearData() override {
        items.clear();
    }

private:
    std::vector<MediaCard> items;
    Selection selected;
    brls::View* focusAnchor = nullptr;
};

brls::Box* makeButton(
    const std::string& text,
    brls::Label** output = nullptr) {
    auto* button = new brls::Box();
    button->setAxis(brls::Axis::ROW);
    button->setFocusable(true);
    button->setPadding(12, 22, 12, 22);
    button->setCornerRadius(8);
    button->setBackgroundColor(
        nvgRGB(91, 46, 145));
    button->setMarginRight(12);
    button->addGestureRecognizer(
        new brls::TapGestureRecognizer(button));

    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(18);
    label->setTextColor(
        nvgRGB(250, 250, 252));
    button->addView(label);

    if (output)
        *output = label;
    return button;
}

brls::Box* createMediaSection(
    const std::string& titleText,
    const std::vector<MediaCard>& cards,
    const std::string& emptyText,
    std::function<void(const MediaCard&)> selected,
    brls::View* focusAnchor) {
    auto* section = new brls::Box();
    section->setAxis(brls::Axis::COLUMN);
    section->setMarginTop(20);

    auto* header = new brls::Header();
    header->setTitle(titleText);
    header->setHeight(38);
    section->addView(header);

    if (cards.empty()) {
        auto* empty = new brls::Label();
        empty->setText(emptyText);
        empty->setFontSize(16);
        empty->setTextColor(
            nvgRGB(165, 174, 193));
        empty->setMarginTop(14);
        empty->setMarginBottom(14);
        empty->setMarginLeft(4);
        empty->setMarginRight(4);
        section->addView(empty);
        return section;
    }

    auto* row = new HRecyclerFrame();
    row->setHeight(190);
    row->estimatedRowWidth = 285;
    row->estimatedRowSpace = 16;
    row->setPadding(0, 4, 0, 4);
    row->registerCell(
        "ChannelMedia",
        []() {
            return new MediaCell();
        });
    row->setDataSource(
        new MediaDataSource(
            cards,
            std::move(selected),
            focusAnchor));
    section->addView(row);
    return section;
}

}  // namespace

TwitchChannelPage::TwitchChannelPage(
    std::string channelLogin)
    : channelLogin(std::move(channelLogin)),
      requestCancel(
          std::make_shared<std::atomic_bool>(
              false)) {
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(
        brls::Application::contentWidth,
        brls::Application::contentHeight);
    this->setBackgroundColor(
        nvgRGB(15, 17, 23));

    scroll = new brls::ScrollingFrame();
    scroll->setDimensions(
        brls::Application::contentWidth,
        brls::Application::contentHeight);
    scroll->setScrollingBehavior(
        brls::ScrollingBehavior::NATURAL);
    this->addView(scroll);

    content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPadding(24, 42, 30, 42);
    scroll->setContentView(content);

    auto* top = new brls::Box();
    top->setAxis(brls::Axis::ROW);
    top->setAlignItems(
        brls::AlignItems::CENTER);
    content->addView(top);

    auto* title = new brls::Label();
    title->setText("Twitch channel");
    title->setFontSize(30);
    title->setTextColor(
        nvgRGB(245, 246, 250));
    title->setGrow(1.0f);
    top->addView(title);

    status = new brls::Label();
    status->setText("Loading channel…");
    status->setFontSize(15);
    status->setTextColor(
        nvgRGB(164, 173, 192));
    top->addView(status);

    offlineImage = new brls::Image();
    offlineImage->setHeight(215);
    offlineImage->setWidthPercentage(100);
    offlineImage->setMarginTop(14);
    offlineImage->setImageFromRes(
        "img/video-card-bg.png");
    offlineImage->setVisibility(
        brls::Visibility::GONE);
    content->addView(offlineImage);

    auto* identity = new brls::Box();
    identity->setAxis(brls::Axis::ROW);
    identity->setAlignItems(
        brls::AlignItems::CENTER);
    identity->setMarginTop(16);
    content->addView(identity);

    avatar = new CircularAvatar();
    avatar->setWidth(108);
    avatar->setHeight(108);
    avatar->setMarginRight(20);
    identity->addView(avatar);

    auto* identityText = new brls::Box();
    identityText->setAxis(
        brls::Axis::COLUMN);
    identityText->setGrow(1.0f);
    identity->addView(identityText);

    name = new brls::Label();
    name->setText(channelLogin);
    name->setFontSize(30);
    name->setTextColor(
        nvgRGB(245, 246, 250));
    identityText->addView(name);

    liveState = new brls::Label();
    liveState->setText("Checking status…");
    liveState->setFontSize(17);
    liveState->setTextColor(
        nvgRGB(172, 180, 199));
    liveState->setMarginTop(5);
    identityText->addView(liveState);

    headline = new brls::Label();
    headline->setFontSize(21);
    headline->setTextColor(
        nvgRGB(238, 239, 244));
    headline->setMarginTop(16);
    content->addView(headline);

    metadata = new brls::Label();
    metadata->setFontSize(16);
    metadata->setTextColor(
        nvgRGB(166, 176, 195));
    metadata->setMarginTop(7);
    content->addView(metadata);

    description = new brls::Label();
    description->setFontSize(16);
    description->setTextColor(
        nvgRGB(199, 204, 216));
    description->setMarginTop(14);
    content->addView(description);

    auto* actions = new brls::Box();
    actions->setAxis(brls::Axis::ROW);
    actions->setMarginTop(16);
    content->addView(actions);

    playButton =
        makeButton(
            "Watch live",
            &playButtonLabel);
    playButton->setId("channel/watch-live");
    playButton->setVisibility(
        brls::Visibility::GONE);
    playButton->registerClickAction(
        [this](brls::View*) {
            playLive();
            return true;
        });
    actions->addView(playButton);

    refreshButton =
        makeButton("Refresh");
    refreshButton->setId("channel/refresh");
    refreshButton->registerClickAction(
        [this](brls::View*) {
            load();
            return true;
        });
    actions->addView(refreshButton);

    scheduleBox = new brls::Box();
    scheduleBox->setAxis(brls::Axis::COLUMN);
    scheduleBox->setMarginTop(18);
    content->addView(scheduleBox);

    videosHolder = new brls::Box();
    videosHolder->setAxis(brls::Axis::COLUMN);
    content->addView(videosHolder);

    clipsHolder = new brls::Box();
    clipsHolder->setAxis(brls::Axis::COLUMN);
    content->addView(clipsHolder);

    categoriesHolder = new brls::Box();
    categoriesHolder->setAxis(
        brls::Axis::COLUMN);
    content->addView(categoriesHolder);

    this->registerAction(
        "Back",
        brls::BUTTON_B,
        [](brls::View*) {
            return brls::Application::popActivity();
        });

    load();
}

TwitchChannelPage::~TwitchChannelPage() {
    if (requestCancel)
        requestCancel->store(true);
    if (playbackCancel)
        playbackCancel->store(true);
    Image::cancel(offlineImage);
}

brls::View* TwitchChannelPage::getDefaultFocus() {
    // Never delegate the page default to a recycled media card. The action
    // buttons are permanent for the entire lifetime of the channel page.
    if (data.live &&
        playButton->getVisibility() ==
            brls::Visibility::VISIBLE)
        return playButton;
    return refreshButton;
}

void TwitchChannelPage::load() {
    if (requestCancel)
        requestCancel->store(true);
    requestCancel =
        std::make_shared<std::atomic_bool>(
            false);

    status->setText("Loading channel…");
    this->ptrLock();

    twitch::loadChannelPageAsync(
        twitch::loadConfig(),
        channelLogin,
        [this](twitch::ChannelPageData loaded) {
            data = std::move(loaded);
            showData(data);
            this->ptrUnlock();
        },
        [this](const std::string& error) {
            status->setText(
                "Channel error: " + error);
            brls::Application::notify(
                "twiNX: " + error);
            this->ptrUnlock();
        },
        requestCancel);
}

void TwitchChannelPage::showData(
    twitch::ChannelPageData loaded) {
    // Dynamic sections own recyclable cells. Move focus to a permanent
    // control before clearing them so Borealis never retains a detached view.
    if (refreshButton)
        brls::Application::giveFocus(refreshButton);

    const std::string displayName =
        loaded.profile.displayName.empty()
            ? loaded.profile.login
            : loaded.profile.displayName;

    name->setText(displayName);
    static_cast<CircularAvatar*>(avatar)->setUrl(
        loaded.profile.profileImageUrl);

    if (!loaded.profile.offlineImageUrl.empty()) {
        offlineImage->setVisibility(
            brls::Visibility::VISIBLE);
        Image::with(
            offlineImage,
            loaded.profile.offlineImageUrl);
    } else {
        Image::cancel(offlineImage);
        offlineImage->setVisibility(
            brls::Visibility::GONE);
    }

    if (loaded.live) {
        liveState->setText(
            "LIVE · " +
            compactNumber(
                loaded.liveStream.viewerCount) +
            " viewers");
        liveState->setTextColor(
            nvgRGB(255, 88, 130));
        headline->setText(
            loaded.liveStream.title.empty()
                ? loaded.information.title
                : loaded.liveStream.title);
        playButton->setVisibility(
            brls::Visibility::VISIBLE);
    } else {
        liveState->setText("OFFLINE");
        liveState->setTextColor(
            nvgRGB(177, 183, 198));
        headline->setText(
            loaded.information.title);
        playButton->setVisibility(
            brls::Visibility::GONE);
    }

    std::string meta =
        loaded.information.gameName;
    if (!loaded.information
             .broadcasterLanguage.empty()) {
        if (!meta.empty())
            meta += "  ·  ";
        meta += loaded.information
                    .broadcasterLanguage;
    }

    const std::string tags =
        joinTags(loaded.information.tags);
    if (!tags.empty()) {
        if (!meta.empty())
            meta += "\n";
        meta += tags;
    }
    metadata->setText(meta);

    description->setText(
        loaded.profile.description.empty()
            ? "This channel has no description."
            : loaded.profile.description);

    status->setText(
        loaded.live
            ? "Channel is live"
            : "Channel is offline");

    scheduleBox->clearViews();
    auto* scheduleHeader = new brls::Header();
    scheduleHeader->setTitle("Schedule");
    scheduleHeader->setHeight(38);
    scheduleBox->addView(scheduleHeader);

    if (loaded.schedule.empty()) {
        auto* empty = new brls::Label();
        empty->setText(
            loaded.scheduleError.empty()
                ? "No published schedule."
                : "Schedule is unavailable.");
        empty->setFontSize(16);
        empty->setTextColor(
            nvgRGB(165, 174, 193));
        scheduleBox->addView(empty);
    } else {
        const size_t count =
            std::min<size_t>(
                loaded.schedule.size(),
                4);
        for (size_t index = 0;
             index < count;
             ++index) {
            const auto& segment =
                loaded.schedule[index];
            auto* label = new brls::Label();
            std::string text =
                segment.startTime;
            if (!segment.title.empty())
                text += "  ·  " +
                    segment.title;
            if (!segment.categoryName.empty())
                text += "  ·  " +
                    segment.categoryName;
            label->setText(text);
            label->setFontSize(15);
            label->setTextColor(
                nvgRGB(195, 201, 215));
            label->setMarginTop(6);
            scheduleBox->addView(label);
        }
    }

    videosHolder->clearViews();
    clipsHolder->clearViews();
    categoriesHolder->clearViews();

    std::vector<MediaCard> videos;
    for (const auto& video :
         loaded.videos) {
        MediaCard card;
        card.kind = MediaKind::Vod;
        card.id = video.id;
        card.title = video.title;
        card.subtitle =
            video.duration +
            "  ·  " +
            compactNumber(video.viewCount) +
            " views";
        card.thumbnail =
            twitch::mediaThumbnail(
                video.thumbnailUrl,
                320,
                180);
        card.details =
            video.title +
            "\n\nDuration: " +
            video.duration +
            "\nViews: " +
            std::to_string(video.viewCount) +
            "\nPublished: " +
            video.publishedAt +
            "\n\nPress A to play this broadcast.";
        videos.push_back(std::move(card));
    }

    videosHolder->addView(
        createMediaSection(
            "Recent broadcasts",
            videos,
            loaded.videosError.empty()
                ? "No recent broadcasts were returned."
                : "Recent broadcasts are unavailable.",
            [this](const MediaCard& card) {
                if (card.kind == MediaKind::Vod)
                    playVod(card.id, card.title, card.details);
                else
                    showMediaDetails(card.title, card.details);
            },
            refreshButton));

    std::vector<MediaCard> clips;
    for (const auto& clip :
         loaded.clips) {
        MediaCard card;
        card.kind = MediaKind::Clip;
        card.id = clip.id;
        card.title = clip.title;
        card.subtitle =
            compactNumber(clip.viewCount) +
            " views  ·  " +
            clip.creatorName;
        card.thumbnail =
            clip.thumbnailUrl;
        card.details =
            clip.title +
            "\n\nCreator: " +
            clip.creatorName +
            "\nViews: " +
            std::to_string(clip.viewCount) +
            "\nDuration: " +
            fmt::format(
                "{:.1f}s",
                clip.duration) +
            "\nCreated: " +
            clip.createdAt +
            "\n\nPress A to play this clip.";
        clips.push_back(std::move(card));
    }

    clipsHolder->addView(
        createMediaSection(
            "Clips",
            clips,
            loaded.clipsError.empty()
                ? "No recent clips were returned."
                : "Clips are unavailable.",
            [this](const MediaCard& card) {
                if (card.kind == MediaKind::Clip)
                    playClip(card.id, card.title, card.details);
                else
                    showMediaDetails(card.title, card.details);
            },
            refreshButton));

    std::vector<MediaCard> categories;
    for (const auto& category :
         loaded.categories) {
        MediaCard card;
        card.title = category.name;
        card.subtitle =
            "Current channel category";
        card.thumbnail =
            twitch::categoryThumbnail(
                category.boxArtUrl,
                188,
                250);
        card.details =
            category.name;
        categories.push_back(
            std::move(card));
    }

    categoriesHolder->addView(
        createMediaSection(
            "Channel category",
            categories,
            "No current category information.",
            [this](const MediaCard& card) {
                showMediaDetails(
                    card.title,
                    card.details);
            },
            refreshButton));

    content->invalidate();
    scroll->setContentOffsetY(0, false);

    brls::Application::giveFocus(
        loaded.live &&
                playButton->getVisibility() == brls::Visibility::VISIBLE
            ? static_cast<brls::View*>(playButton)
            : static_cast<brls::View*>(refreshButton));
}

void TwitchChannelPage::playLive() {
    if (!data.live) {
        brls::Application::notify(
            "This channel is currently offline");
        return;
    }

    auto config = twitch::loadConfig();
    config.channel =
        data.profile.login.empty()
            ? channelLogin
            : data.profile.login;
    config.preferredQuality =
        twitch::loadPreferredQuality();
    if (config.preferredQuality.empty())
        config.preferredQuality = "source";

    status->setText("Resolving live stream…");
    this->ptrLock();

    twitch::resolveLiveAsync(
        config,
        [this](twitch::Resolution result) {
            status->setText(
                "Opening live stream…");
            const std::string channel =
                result.channel;
            const std::string url =
                result.selected.url;
            this->ptrUnlock();

            RemoteView::play(
                url,
                "Twitch - " + channel,
                twitch::mpvExtra(),
                channel);
        },
        [this](const std::string& error) {
            status->setText(
                "Playback error: " + error);
            brls::Application::notify(
                "twiNX: " + error);
            this->ptrUnlock();
        });
}


void TwitchChannelPage::playVod(
    const std::string& id,
    const std::string& title,
    const std::string& details) {
    if (playbackCancel)
        playbackCancel->store(true);
    playbackCancel =
        std::make_shared<std::atomic_bool>(false);

    status->setText("Resolving VOD playback…");
    this->ptrLock();

    twitch::resolveVodAsync(
        id,
        [this, title](twitch::MediaPlayback playback) {
            status->setText(
                playback.quality.empty()
                    ? "Opening VOD…"
                    : "Opening VOD · " + playback.quality);
            const std::string url = playback.url;
            this->ptrUnlock();
            RemoteView::play(
                url,
                "Twitch VOD · " + title,
                twitch::mpvExtra(),
                "");
        },
        [this, title, details](const std::string& error) {
            status->setText("VOD playback unavailable");
            this->ptrUnlock();
            showMediaDetails(
                title,
                details +
                    "\n\nPlayback error: " + error);
        },
        playbackCancel);
}

void TwitchChannelPage::playClip(
    const std::string& slug,
    const std::string& title,
    const std::string& details) {
    if (playbackCancel)
        playbackCancel->store(true);
    playbackCancel =
        std::make_shared<std::atomic_bool>(false);

    status->setText("Resolving clip playback…");
    this->ptrLock();

    twitch::resolveClipAsync(
        slug,
        [this, title](twitch::MediaPlayback playback) {
            status->setText(
                playback.quality.empty()
                    ? "Opening clip…"
                    : "Opening clip · " + playback.quality);
            const std::string url = playback.url;
            this->ptrUnlock();
            RemoteView::play(
                url,
                "Twitch clip · " + title,
                twitch::mpvExtra(),
                "");
        },
        [this, title, details](const std::string& error) {
            status->setText("Clip playback unavailable");
            this->ptrUnlock();
            showMediaDetails(
                title,
                details +
                    "\n\nPlayback error: " + error);
        },
        playbackCancel);
}

void TwitchChannelPage::showMediaDetails(
    const std::string& title,
    const std::string& details) {
    auto* dialog = new brls::Dialog(
        details.empty()
            ? title
            : details);
    dialog->addButton("OK", []() {});
    dialog->open();
}
