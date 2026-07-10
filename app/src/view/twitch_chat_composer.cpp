#include "view/twitch_chat_composer.hpp"

#include "utils/image.hpp"
#include "view/recycling_grid.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>
#include <utility>

class ComposerPreviewTexturePool {
public:
    static ComposerPreviewTexturePool& instance() {
        // Application-lifetime loader views avoid asynchronous image
        // completions targeting destroyed composer rows/previews.
        static auto* pool =
            new ComposerPreviewTexturePool();
        return *pool;
    }

    brls::Image* get(const std::string& url) {
        if (url.empty()) return nullptr;

        const auto found = entries.find(url);
        if (found != entries.end())
            return found->second;

        if (entries.size() >= MAX_ENTRIES)
            return nullptr;

        auto* image = new brls::Image();
        image->setFreeTexture(false);
        entries.emplace(url, image);
        Image::with(image, url);
        return image;
    }

private:
    static constexpr size_t MAX_ENTRIES = 512;
    std::unordered_map<
        std::string,
        brls::Image*> entries;
};

class TwitchDraftPreview : public brls::View {
public:
    void configure(
        const std::string& value,
        const std::vector<twitch::UserEmote>& emotes) {
        draft = value;
        segments.clear();

        std::unordered_map<
            std::string,
            const twitch::UserEmote*> byName;
        byName.reserve(emotes.size());
        for (const auto& emote : emotes) {
            if (!emote.name.empty())
                byName.emplace(emote.name, &emote);
        }

        size_t position = 0;
        while (position < draft.size()) {
            if (std::isspace(
                    static_cast<unsigned char>(
                        draft[position]))) {
                size_t end = position + 1;
                while (end < draft.size() &&
                    std::isspace(
                        static_cast<unsigned char>(
                            draft[end])))
                    ++end;

                Segment segment;
                segment.text =
                    draft.substr(
                        position,
                        end - position);
                segments.push_back(
                    std::move(segment));
                position = end;
                continue;
            }

            size_t end = position;
            while (end < draft.size() &&
                !std::isspace(
                    static_cast<unsigned char>(
                        draft[end])))
                ++end;

            const std::string token =
                draft.substr(
                    position,
                    end - position);

            Segment segment;
            segment.text = token;

            const auto found = byName.find(token);
            if (found != byName.end()) {
                segment.emote = true;
                segment.image =
                    ComposerPreviewTexturePool::
                        instance().get(
                            found->second->imageUrl);
            }

            segments.push_back(
                std::move(segment));
            position = end;
        }

        this->invalidate();
    }

    void draw(
        NVGcontext* vg,
        float x,
        float y,
        float width,
        float height,
        brls::Style style,
        brls::FrameContext* ctx) override {
        nvgSave(vg);
        nvgIntersectScissor(
            vg,
            x,
            y,
            width,
            height);

        constexpr float FONT_SIZE = 19.0f;
        constexpr float LINE_HEIGHT = 29.0f;
        constexpr float EMOTE_SIZE = 27.0f;
        constexpr int MAX_LINES = 2;

        nvgFontFaceId(
            vg,
            brls::Application::getDefaultFont());
        nvgFontSize(vg, FONT_SIZE);
        nvgTextAlign(
            vg,
            NVG_ALIGN_LEFT |
                NVG_ALIGN_BASELINE);

        if (draft.empty()) {
            nvgFillColor(
                vg,
                nvgRGB(166, 174, 191));
            nvgText(
                vg,
                x,
                y + FONT_SIZE,
                "Press X or A to type a message",
                nullptr);
            nvgRestore(vg);
            return;
        }

        float cursorX = x;
        float cursorY = y + FONT_SIZE;
        int line = 0;
        const float right = x + width;

        auto nextLine = [&]() {
            cursorX = x;
            cursorY += LINE_HEIGHT;
            ++line;
        };

        auto textWidth = [&](const std::string& text) {
            float bounds[4]{};
            return nvgTextBounds(
                vg,
                0,
                0,
                text.c_str(),
                nullptr,
                bounds);
        };

        for (const auto& segment : segments) {
            if (line >= MAX_LINES) break;

            if (segment.emote) {
                if (cursorX > x &&
                    cursorX + EMOTE_SIZE > right)
                    nextLine();
                if (line >= MAX_LINES) break;

                if (segment.image &&
                    segment.image->getTexture() > 0) {
                    const float imageY =
                        cursorY -
                        FONT_SIZE -
                        5.0f;
                    NVGpaint paint =
                        nvgImagePattern(
                            vg,
                            cursorX,
                            imageY,
                            EMOTE_SIZE,
                            EMOTE_SIZE,
                            0,
                            segment.image->getTexture(),
                            1.0f);
                    nvgBeginPath(vg);
                    nvgRect(
                        vg,
                        cursorX,
                        imageY,
                        EMOTE_SIZE,
                        EMOTE_SIZE);
                    nvgFillPaint(vg, paint);
                    nvgFill(vg);
                    cursorX +=
                        EMOTE_SIZE + 4.0f;
                } else {
                    const float size =
                        textWidth(segment.text);
                    if (cursorX > x &&
                        cursorX + size > right)
                        nextLine();
                    nvgFillColor(
                        vg,
                        nvgRGB(245, 246, 250));
                    nvgText(
                        vg,
                        cursorX,
                        cursorY,
                        segment.text.c_str(),
                        nullptr);
                    cursorX += size;
                }
                continue;
            }

            const float size =
                textWidth(segment.text);
            if (cursorX > x &&
                cursorX + size > right)
                nextLine();
            if (line >= MAX_LINES) break;

            nvgFillColor(
                vg,
                nvgRGB(245, 246, 250));
            nvgText(
                vg,
                cursorX,
                cursorY,
                segment.text.c_str(),
                nullptr);
            cursorX += size;
        }

        nvgRestore(vg);
    }

private:
    struct Segment {
        std::string text;
        bool emote = false;
        brls::Image* image = nullptr;
    };

    std::string draft;
    std::vector<Segment> segments;
};

namespace {

brls::Box* makeActionButton(
    const std::string& text,
    brls::Label** output = nullptr) {
    auto* button = new brls::Box();
    button->setFocusable(true);
    button->setAxis(brls::Axis::ROW);
    button->setPadding(12, 22, 12, 22);
    button->setCornerRadius(8);
    button->setBackgroundColor(
        nvgRGB(45, 50, 65));
    button->setMarginRight(12);
    button->addGestureRecognizer(
        new brls::TapGestureRecognizer(button));

    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(18);
    label->setTextColor(
        nvgRGB(245, 246, 250));
    button->addView(label);

    if (output) *output = label;
    return button;
}

std::string trimmed(std::string value) {
    while (!value.empty() &&
        std::isspace(
            static_cast<unsigned char>(
                value.front())))
        value.erase(value.begin());

    while (!value.empty() &&
        std::isspace(
            static_cast<unsigned char>(
                value.back())))
        value.pop_back();

    return value;
}

class EmoteCell : public RecyclingGridItem {
public:
    EmoteCell() {
        this->setAxis(brls::Axis::COLUMN);
        this->setFocusable(true);
        this->setAlignItems(
            brls::AlignItems::CENTER);
        this->setJustifyContent(
            brls::JustifyContent::CENTER);
        this->setPadding(7, 7, 7, 7);
        this->setCornerRadius(8);
        this->setBackgroundColor(
            nvgRGB(31, 35, 46));

        picture = new brls::Image();
        picture->setWidth(52);
        picture->setHeight(52);
        picture->setImageFromRes(
            "img/video-card-bg.png");
        this->addView(picture);

        name = new brls::Label();
        name->setFontSize(13);
        name->setSingleLine(true);
        name->setHorizontalAlign(
            brls::HorizontalAlign::CENTER);
        name->setTextColor(
            nvgRGB(229, 231, 238));
        name->setMarginTop(4);
        name->setWidthPercentage(100);
        this->addView(name);

        this->getFocusEvent()->subscribe(
            [this](brls::View*) {
                if (focused)
                    focused(value);
            });
    }

    ~EmoteCell() override {
        Image::cancel(picture);
    }

    void prepareForReuse() override {
        Image::cancel(picture);
        picture->setImageFromRes(
            "img/video-card-bg.png");
        name->setText("");
        value = {};
        focused = nullptr;
    }

    void cacheForReuse() override {
        Image::cancel(picture);
        focused = nullptr;
    }

    void configure(
        const twitch::UserEmote& emote,
        std::function<void(
            const twitch::UserEmote&)> onFocus) {
        value = emote;
        focused = std::move(onFocus);
        name->setText(value.name);
        if (!value.imageUrl.empty())
            Image::with(
                picture,
                value.imageUrl);
    }

private:
    brls::Image* picture = nullptr;
    brls::Label* name = nullptr;
    twitch::UserEmote value;
    std::function<void(
        const twitch::UserEmote&)> focused;
};

class EmoteDataSource :
    public RecyclingGridDataSource {
public:
    using Selection = std::function<void(
        const twitch::UserEmote&)>;
    using Focus = std::function<void(
        const twitch::UserEmote&)>;
    EmoteDataSource(
        std::vector<twitch::UserEmote> values,
        Selection selected,
        Focus focused)
        : items(std::move(values)),
          selected(std::move(selected)),
          focused(std::move(focused)) {}

    size_t getItemCount() override {
        return items.size();
    }

    RecyclingGridItem* cellForRow(
        RecyclingView* recycler,
        size_t index) override {
        auto* cell = dynamic_cast<EmoteCell*>(
            recycler->dequeueReusableCell(
                "TwitchEmote"));
        cell->configure(
            items.at(index),
            focused);
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
    std::vector<twitch::UserEmote> items;
    Selection selected;
    Focus focused;
};

}  // namespace

TwitchChatComposer::TwitchChatComposer(
    std::string channel)
    : channel(std::move(channel)),
      recentEmotes(
          twitch::loadRecentEmotes()) {
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(
        brls::Application::contentWidth,
        brls::Application::contentHeight);
    this->setBackgroundColor(
        nvgRGB(15, 17, 23));
    this->setPadding(24, 34, 18, 34);

    auto* headingRow = new brls::Box();
    headingRow->setAxis(brls::Axis::ROW);
    headingRow->setAlignItems(
        brls::AlignItems::CENTER);
    headingRow->setMarginBottom(12);
    this->addView(headingRow);

    auto* heading = new brls::Label();
    heading->setText(
        "Twitch chat composer");
    heading->setFontSize(30);
    heading->setTextColor(
        nvgRGB(245, 246, 250));
    heading->setGrow(1.0f);
    headingRow->addView(heading);

    statusLabel = new brls::Label();
    statusLabel->setText(
        "Loading available emotes…");
    statusLabel->setFontSize(16);
    statusLabel->setTextColor(
        nvgRGB(160, 170, 190));
    headingRow->addView(statusLabel);

    draftButton = new brls::Box();
    draftButton->setFocusable(true);
    draftButton->setAxis(brls::Axis::COLUMN);
    draftButton->setHeight(112);
    draftButton->setPadding(12, 16, 10, 16);
    draftButton->setCornerRadius(8);
    draftButton->setBackgroundColor(
        nvgRGB(29, 33, 43));
    draftButton->setMarginBottom(12);
    draftButton->addGestureRecognizer(
        new brls::TapGestureRecognizer(
            draftButton));
    draftButton->registerClickAction(
        [this](brls::View*) {
            openKeyboard();
            return true;
        });
    this->addView(draftButton);

    auto* draftHeader = new brls::Box();
    draftHeader->setAxis(brls::Axis::ROW);
    draftHeader->setMarginBottom(5);
    draftButton->addView(draftHeader);

    auto* draftCaption = new brls::Label();
    draftCaption->setText(
        "Message draft");
    draftCaption->setFontSize(14);
    draftCaption->setTextColor(
        nvgRGB(145, 156, 177));
    draftCaption->setGrow(1.0f);
    draftHeader->addView(draftCaption);

    counterLabel = new brls::Label();
    counterLabel->setFontSize(14);
    counterLabel->setTextColor(
        nvgRGB(145, 156, 177));
    draftHeader->addView(counterLabel);

    draftPreview =
        new TwitchDraftPreview();
    draftPreview->setWidthPercentage(100);
    draftPreview->setHeight(62);
    draftButton->addView(draftPreview);

    auto* tabRow = new brls::Box();
    tabRow->setAxis(brls::Axis::ROW);
    tabRow->setMarginBottom(10);
    this->addView(tabRow);

    recentTab = makeActionButton("Recent");
    channelTab = makeActionButton("Channel");
    allTab = makeActionButton("All");

    recentTab->registerClickAction(
        [this](brls::View*) {
            setTab(Tab::Recent);
            return true;
        });
    channelTab->registerClickAction(
        [this](brls::View*) {
            setTab(Tab::Channel);
            return true;
        });
    allTab->registerClickAction(
        [this](brls::View*) {
            setTab(Tab::All);
            return true;
        });

    tabRow->addView(recentTab);
    tabRow->addView(channelTab);
    tabRow->addView(allTab);

    // The controls above the emote grid form an explicit vertical chain:
    // first emote row -> active filter tab -> message draft.
    recentTab->setCustomNavigationRoute(
        brls::FocusDirection::UP,
        draftButton);
    channelTab->setCustomNavigationRoute(
        brls::FocusDirection::UP,
        draftButton);
    allTab->setCustomNavigationRoute(
        brls::FocusDirection::UP,
        draftButton);

    channelTab->setVisibility(
        brls::Visibility::GONE);

    recentTab->setCustomNavigationRoute(
        brls::FocusDirection::UP,
        draftButton);
    channelTab->setCustomNavigationRoute(
        brls::FocusDirection::UP,
        draftButton);
    allTab->setCustomNavigationRoute(
        brls::FocusDirection::UP,
        draftButton);

    focusedEmoteLabel = new brls::Label();
    focusedEmoteLabel->setText(
        "Choose an emote with A");
    focusedEmoteLabel->setFontSize(16);
    focusedEmoteLabel->setTextColor(
        nvgRGB(167, 176, 195));
    focusedEmoteLabel->setMarginBottom(8);
    this->addView(focusedEmoteLabel);

    grid = new RecyclingGrid();
    grid->setGrow(1.0f);
    grid->spanCount = 8;
    grid->estimatedRowHeight = 98;
    grid->estimatedRowSpace = 10;
    grid->preFetchLine = 2;
    grid->setPadding(2, 2, 2, 2);
    grid->registerCell(
        "TwitchEmote",
        []() {
            return new EmoteCell();
        });
    grid->setIndexNavigationEnabled(true);
    grid->setTopBoundaryFocusRoute(
        [this]() -> brls::View* {
            switch (tab) {
            case Tab::Recent:
                return recentTab;
            case Tab::Channel:
                return channelSubscribed
                    ? channelTab
                    : allTab;
            case Tab::All:
            default:
                return allTab;
            }
        });
    this->addView(grid);

    auto* footer = new brls::Label();
    footer->setText(
        "A Insert   X Keyboard   L/R Tabs   "
        "ZR or + Send   B Back");
    footer->setFontSize(16);
    footer->setTextColor(
        nvgRGB(154, 165, 187));
    footer->setMarginTop(8);
    this->addView(footer);

    this->registerAction(
        "Keyboard",
        brls::BUTTON_X,
        [this](brls::View*) {
            openKeyboard();
            return true;
        });

    this->registerAction(
        "Previous tab",
        brls::BUTTON_LB,
        [this](brls::View*) {
            cycleTab(-1);
            return true;
        });

    this->registerAction(
        "Next tab",
        brls::BUTTON_RB,
        [this](brls::View*) {
            cycleTab(1);
            return true;
        });

    this->registerAction(
        "Send",
        brls::BUTTON_RT,
        [this](brls::View*) {
            sendMessage();
            return true;
        });

    this->registerAction(
        "Send",
        brls::BUTTON_START,
        [this](brls::View*) {
            sendMessage();
            return true;
        });

    this->registerAction(
        "Back",
        brls::BUTTON_B,
        [](brls::View*) {
            return brls::Application::popActivity();
        });

    updateDraft();
    updateTabs();
    refreshGrid();

    if (!twitch::hasUserEmotesScope()) {
        loading = false;
        statusLabel->setText(
            "Sign out and sign in again "
            "to load emotes");
    } else {
        this->ptrLock();
        twitch::loadUserEmotesAsync(
            this->channel,
            [this](
                twitch::UserEmoteCatalogue
                    catalogue) {
                emotes =
                    std::move(catalogue.emotes);
                channelSubscribed =
                    catalogue.channelSubscribed;
                subscriptionPermissionGranted =
                    catalogue.subscriptionPermissionGranted;
                loading = false;

                if (!subscriptionPermissionGranted) {
                    statusLabel->setText(
                        std::to_string(emotes.size()) +
                        " emotes available — sign in "
                        "again for channel emotes");
                } else {
                    statusLabel->setText(
                        std::to_string(emotes.size()) +
                        " emotes available");
                }

                reconcileRecentEmotes();
                updateChannelTabVisibility();
                updateDraft();
                refreshGrid();
                this->ptrUnlock();
            },
            [this](const std::string& error) {
                loading = false;
                statusLabel->setText(error);
                refreshGrid();
                this->ptrUnlock();
            });
    }

    brls::sync([this]() {
        brls::Application::giveFocus(
            draftButton);
    });
}

TwitchChatComposer::~TwitchChatComposer() {
    brls::Logger::debug(
        "TwitchChatComposer: deleted");
}

void TwitchChatComposer::openKeyboard() {
    auto* ime =
        brls::Application::getImeManager();
    if (!ime) {
        brls::Application::notify(
            "The software keyboard is unavailable");
        return;
    }

    ime->openForText(
        [this](const std::string& text) {
            draft = text.substr(
                0,
                std::min<size_t>(
                    text.size(),
                    500));
            updateDraft();

            // The system keyboard can leave focus on a control from the
            // underlying player activity. Explicitly return focus to the
            // composer so B and every composer shortcut work immediately.
            brls::sync([this]() {
                brls::Application::giveFocus(
                    draftButton);
            });
        },
        "Twitch chat",
        "Type message text, then return "
        "to choose emotes",
        500,
        draft,
        0);
}

void TwitchChatComposer::insertEmote(
    const twitch::UserEmote& emote) {
    if (emote.name.empty()) return;

    std::string addition;
    if (!draft.empty() &&
        !std::isspace(
            static_cast<unsigned char>(
                draft.back())))
        addition += ' ';
    addition += emote.name;
    addition += ' ';

    if (draft.size() + addition.size() > 500) {
        brls::Application::notify(
            "The Twitch message limit is "
            "500 characters");
        return;
    }

    draft += addition;
    twitch::rememberRecentEmote(emote);

    recentEmotes.erase(
        std::remove_if(
            recentEmotes.begin(),
            recentEmotes.end(),
            [&](const twitch::UserEmote& current) {
                return
                    (!emote.id.empty() &&
                     !current.id.empty() &&
                     emote.id == current.id) ||
                    current.name == emote.name;
            }),
        recentEmotes.end());

    recentEmotes.insert(
        recentEmotes.begin(),
        emote);

    if (recentEmotes.size() > 24)
        recentEmotes.resize(24);

    updateDraft();
    if (tab == Tab::Recent)
        refreshGrid();
}

void TwitchChatComposer::sendMessage() {
    if (sending) return;

    const std::string message =
        trimmed(draft);
    if (message.empty()) {
        brls::Application::notify(
            "Enter a chat message first");
        return;
    }

    sending = true;
    statusLabel->setText(
        "Sending message…");

    this->ptrLock();
    twitch::sendChatMessageAsync(
        channel,
        message,
        [this](const std::string&) {
            sending = false;
            statusLabel->setText(
                "Message sent");
            brls::Application::notify(
                "twiNX: chat message sent");
            this->ptrUnlock();
            brls::Application::popActivity();
        },
        [this](const std::string& error) {
            sending = false;
            statusLabel->setText(error);
            brls::Application::notify(
                "twiNX chat error: " + error);
            this->ptrUnlock();
        });
}

void TwitchChatComposer::setTab(Tab value) {
    if (value == Tab::Channel &&
        !channelSubscribed)
        value = Tab::All;

    tab = value;
    updateTabs();
    refreshGrid();
}

void TwitchChatComposer::cycleTab(
    int direction) {
    if (!channelSubscribed) {
        setTab(
            tab == Tab::Recent
                ? Tab::All
                : Tab::Recent);
        return;
    }

    int value = static_cast<int>(tab);
    value =
        (value + direction + 3) % 3;
    setTab(static_cast<Tab>(value));
}


void TwitchChatComposer::updateDraft() {
    draftPreview->configure(
        draft,
        emotes);
    counterLabel->setText(
        std::to_string(draft.size()) +
        " / 500");
}

void TwitchChatComposer::reconcileRecentEmotes() {
    std::vector<twitch::UserEmote> resolved;
    resolved.reserve(recentEmotes.size());

    for (const auto& recent : recentEmotes) {
        const auto found = std::find_if(
            emotes.begin(),
            emotes.end(),
            [&](const twitch::UserEmote& available) {
                if (!recent.id.empty() &&
                    !available.id.empty())
                    return recent.id ==
                        available.id;
                return recent.name ==
                    available.name;
            });

        if (found == emotes.end())
            continue;

        const bool duplicate =
            std::any_of(
                resolved.begin(),
                resolved.end(),
                [&](const twitch::UserEmote& current) {
                    return
                        (!found->id.empty() &&
                         !current.id.empty() &&
                         found->id == current.id) ||
                        found->name == current.name;
                });

        if (!duplicate)
            resolved.push_back(*found);

        if (resolved.size() >= 24)
            break;
    }

    recentEmotes =
        std::move(resolved);
}

void TwitchChatComposer::updateChannelTabVisibility() {
    channelTab->setVisibility(
        channelSubscribed
            ? brls::Visibility::VISIBLE
            : brls::Visibility::GONE);

    if (!channelSubscribed &&
        tab == Tab::Channel)
        tab = Tab::All;

    updateTabs();
}

void TwitchChatComposer::updateTabs() {
    const NVGcolor active =
        nvgRGB(102, 51, 153);
    const NVGcolor inactive =
        nvgRGB(45, 50, 65);

    recentTab->setBackgroundColor(
        tab == Tab::Recent
            ? active
            : inactive);
    channelTab->setBackgroundColor(
        tab == Tab::Channel
            ? active
            : inactive);
    allTab->setBackgroundColor(
        tab == Tab::All
            ? active
            : inactive);
}

std::vector<twitch::UserEmote>
TwitchChatComposer::visibleEmotes() const {
    if (tab == Tab::All)
        return emotes;

    if (tab == Tab::Channel) {
        if (!channelSubscribed)
            return {};

        std::vector<twitch::UserEmote> result;
        for (const auto& emote : emotes) {
            if (emote.channelEmote)
                result.push_back(emote);
        }
        return result;
    }

    return recentEmotes;
}

void TwitchChatComposer::refreshGrid() {
    const auto values = visibleEmotes();

    if (loading) {
        focusedEmoteLabel->setText(
            "Loading available emotes…");
    } else if (values.empty()) {
        switch (tab) {
        case Tab::Recent:
            focusedEmoteLabel->setText(
                "No recent emotes yet");
            break;
        case Tab::Channel:
            focusedEmoteLabel->setText(
                "No available emotes from "
                "this channel");
            break;
        case Tab::All:
        default:
            focusedEmoteLabel->setText(
                "No emotes available");
            break;
        }
    } else {
        focusedEmoteLabel->setText(
            std::to_string(values.size()) +
            " emotes");
    }

    grid->setVisibility(
        brls::Visibility::GONE);

    grid->setDataSource(
        new EmoteDataSource(
            values,
            [this](
                const twitch::UserEmote& emote) {
                insertEmote(emote);
            },
            [this](
                const twitch::UserEmote& emote) {
                focusedEmoteLabel->setText(
                    emote.name);
            }));

    // RecyclingGrid may retain an empty initial layout when an async catalogue
    // populates the active Recent tab. Toggling visibility forces a fresh
    // measurement and cell creation without recreating the composer.
    grid->setVisibility(
        brls::Visibility::VISIBLE);
    grid->invalidate();
}
