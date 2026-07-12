#include "tab/remote_view.hpp"
#include "utils/debug_log.hpp"
#include "view/recycling_grid.hpp"
#include "view/svg_image.hpp"
#include "view/video_view.hpp"
#include "view/video_profile.hpp"
#include "view/mpv_core.hpp"
#include "view/music_view.hpp"
#include "activity/twitch_channel.hpp"
#include "api/twitch_helix.hpp"
#include "view/player_setting.hpp"
#include "view/twitch_chat_composer.hpp"
#include "api/twitch_chat.hpp"
#include "api/twitch.hpp"
#include "utils/thread.hpp"
#include "client/local.hpp"
#include "utils/misc.hpp"
#include "utils/config.hpp"
#include "utils/image.hpp"
#include "utils/orientation.hpp"

#include <utility>
#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <sstream>
#include <chrono>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <vector>
#include <limits>
#include <stb_image.h>

using namespace brls::literals;

namespace {

NVGcolor chatColor(const std::string& value) {
    if (value.size() != 7 || value.front() != '#')
        return nvgRGB(238, 240, 246);
    try {
        const int red = std::stoi(value.substr(1, 2), nullptr, 16);
        const int green = std::stoi(value.substr(3, 2), nullptr, 16);
        const int blue = std::stoi(value.substr(5, 2), nullptr, 16);
        return nvgRGB(red, green, blue);
    } catch (...) {
        return nvgRGB(238, 240, 246);
    }
}

class TwitchEmoteTexturePool {
public:
    static TwitchEmoteTexturePool& instance() {
        static auto* pool = new TwitchEmoteTexturePool();
        return *pool;
    }

    brls::Image* get(const std::string& url) {
        if (url.empty()) return nullptr;
        auto found = entries.find(url);
        if (found != entries.end()) return found->second;
        if (entries.size() >= MAX_ENTRIES) {
            if (!capacityWarningShown) {
                capacityWarningShown = true;
                brls::Logger::warning(
                    "twiNX: emote texture pool reached {} entries; new emotes use text fallback",
                    MAX_ENTRIES);
            }
            return nullptr;
        }
        auto* image = new brls::Image();
        image->setFreeTexture(false);
        entries.emplace(url, image);
        Image::with(image, url);
        return image;
    }

private:
    static constexpr size_t MAX_ENTRIES = 192;
    std::unordered_map<std::string, brls::Image*> entries;
    bool capacityWarningShown = false;
};

class TwitchAnimatedEmoteTexturePool {
public:
    struct Entry {
        std::vector<int> textures;
        std::vector<int> delaysMs;
        int totalDurationMs = 0;
        bool ready = false;
        bool failed = false;
        std::chrono::steady_clock::time_point startedAt{};
    };

    static TwitchAnimatedEmoteTexturePool& instance() {
        static auto* pool = new TwitchAnimatedEmoteTexturePool();
        return *pool;
    }

    Entry* get(const std::string& url) {
        if (url.empty()) return nullptr;

        auto found = entries.find(url);
        if (found != entries.end())
            return found->second.get();

        if (entries.size() >= MAX_ENTRIES) {
            if (!capacityWarningShown) {
                capacityWarningShown = true;
                brls::Logger::warning(
                    "twiNX: animated emote pool reached {} entries; "
                    "additional emotes use their static image",
                    MAX_ENTRIES);
            }
            return nullptr;
        }

        auto entry = std::make_shared<Entry>();
        Entry* result = entry.get();
        entries.emplace(url, entry);

        ThreadPool::instance().submit(
            [entry, url](HTTP& http) {
                try {
                    std::ostringstream body;
                    HTTP::set_option(
                        http,
                        HTTP::Timeout{12000});
                    http._get(url, &body);
                    const std::string data = body.str();

                    if (data.empty() ||
                        data.size() > MAX_DOWNLOAD_BYTES ||
                        data.size() > static_cast<size_t>(
                            std::numeric_limits<int>::max()))
                        throw std::runtime_error(
                            "animated emote download is empty or too large");

                    int* sourceDelays = nullptr;
                    int width = 0;
                    int height = 0;
                    int frameCount = 0;
                    int components = 0;
                    stbi_uc* decoded =
                        stbi_load_gif_from_memory(
                            reinterpret_cast<const stbi_uc*>(data.data()),
                            static_cast<int>(data.size()),
                            &sourceDelays,
                            &width,
                            &height,
                            &frameCount,
                            &components,
                            4);

                    if (!decoded || width <= 0 || height <= 0 ||
                        frameCount <= 0) {
                        if (sourceDelays)
                            stbi_image_free(sourceDelays);
                        if (decoded)
                            stbi_image_free(decoded);
                        throw std::runtime_error(
                            "animated emote GIF could not be decoded");
                    }

                    const size_t pixelsPerFrame =
                        static_cast<size_t>(width) *
                        static_cast<size_t>(height) * 4;
                    const size_t decodedBytes =
                        pixelsPerFrame *
                        static_cast<size_t>(frameCount);

                    if (width > MAX_DIMENSION ||
                        height > MAX_DIMENSION ||
                        frameCount > MAX_FRAMES ||
                        decodedBytes > MAX_DECODED_BYTES) {
                        stbi_image_free(decoded);
                        if (sourceDelays)
                            stbi_image_free(sourceDelays);
                        throw std::runtime_error(
                            "animated emote exceeds TwiNX safety limits");
                    }

                    auto pixels =
                        std::make_shared<std::vector<uint8_t>>(
                            decoded,
                            decoded + decodedBytes);
                    stbi_image_free(decoded);

                    std::vector<int> delays;
                    delays.reserve(static_cast<size_t>(frameCount));
                    for (int index = 0; index < frameCount; ++index) {
                        const int delay = sourceDelays
                            ? sourceDelays[index]
                            : 100;
                        delays.push_back(
                            std::clamp(delay, 20, 1000));
                    }
                    if (sourceDelays)
                        stbi_image_free(sourceDelays);

                    brls::sync(
                        [entry,
                         pixels,
                         delays = std::move(delays),
                         width,
                         height,
                         frameCount,
                         pixelsPerFrame]() mutable {
                            NVGcontext* vg =
                                brls::Application::getNVGContext();
                            if (!vg) {
                                entry->failed = true;
                                return;
                            }

                            std::vector<int> textures;
                            textures.reserve(
                                static_cast<size_t>(frameCount));
                            for (int index = 0;
                                 index < frameCount;
                                 ++index) {
                                const uint8_t* frame =
                                    pixels->data() +
                                    pixelsPerFrame *
                                        static_cast<size_t>(index);
                                const int texture =
                                    nvgCreateImageRGBA(
                                        vg,
                                        width,
                                        height,
                                        0,
                                        frame);
                                if (texture <= 0) {
                                    for (const int created : textures)
                                        nvgDeleteImage(vg, created);
                                    entry->failed = true;
                                    return;
                                }
                                textures.push_back(texture);
                            }

                            entry->textures = std::move(textures);
                            entry->delaysMs = std::move(delays);
                            entry->totalDurationMs = 0;
                            for (const int delay : entry->delaysMs)
                                entry->totalDurationMs += delay;
                            entry->startedAt =
                                std::chrono::steady_clock::now();
                            entry->ready =
                                !entry->textures.empty() &&
                                entry->totalDurationMs > 0;

                            brls::Logger::verbose(
                                "twiNX: animated emote loaded: {} frames",
                                entry->textures.size());
                        });
                } catch (const std::exception& exception) {
                    brls::Logger::warning(
                        "twiNX: animated emote fallback to static: {}",
                        exception.what());
                    brls::sync([entry]() {
                        entry->failed = true;
                    });
                } catch (...) {
                    brls::sync([entry]() {
                        entry->failed = true;
                    });
                }
            });

        return result;
    }

    int texture(Entry* entry) const {
        if (!entry || !entry->ready ||
            entry->textures.empty() ||
            entry->delaysMs.size() != entry->textures.size() ||
            entry->totalDurationMs <= 0)
            return 0;

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() -
                entry->startedAt)
                .count();
        int position = static_cast<int>(
            elapsed % entry->totalDurationMs);

        for (size_t index = 0;
             index < entry->textures.size();
             ++index) {
            if (position < entry->delaysMs[index])
                return entry->textures[index];
            position -= entry->delaysMs[index];
        }
        return entry->textures.back();
    }

private:
    static constexpr size_t MAX_ENTRIES = 48;
    static constexpr size_t MAX_DOWNLOAD_BYTES = 4 * 1024 * 1024;
    static constexpr size_t MAX_DECODED_BYTES = 16 * 1024 * 1024;
    static constexpr int MAX_DIMENSION = 128;
    static constexpr int MAX_FRAMES = 80;

    std::unordered_map<
        std::string,
        std::shared_ptr<Entry>> entries;
    bool capacityWarningShown = false;
};

class TwitchBadgeTexturePool {
public:
    static TwitchBadgeTexturePool& instance() {
        // Matches the stable emote architecture: loader
        // views live for the application lifetime, so
        // asynchronous completions never target deleted rows.
        static auto* pool =
            new TwitchBadgeTexturePool();
        return *pool;
    }

    brls::Image* get(const std::string& url) {
        if (url.empty()) return nullptr;

        auto found = entries.find(url);
        if (found != entries.end())
            return found->second;

        if (entries.size() >= MAX_ENTRIES) {
            if (!capacityWarningShown) {
                capacityWarningShown = true;
                brls::Logger::warning(
                    "twiNX: badge texture pool reached "
                    "{} entries; additional badges are "
                    "hidden",
                    MAX_ENTRIES);
            }
            return nullptr;
        }

        auto* image = new brls::Image();
        image->setFreeTexture(false);
        entries.emplace(url, image);
        Image::with(image, url);
        return image;
    }

private:
    static constexpr size_t MAX_ENTRIES = 96;
    std::unordered_map<
        std::string,
        brls::Image*> entries;
    bool capacityWarningShown = false;
};

class TwitchChatMessageView : public brls::View {
private:
    struct EmoteVisual {
        brls::Image* staticImage = nullptr;
        TwitchAnimatedEmoteTexturePool::Entry* animation = nullptr;
    };

public:
    void configure(
        const twitch::ChatMessage& value,
        const twitch::ChatPreferences& preferences,
        float availableWidth,
        float availableHeight) {
        clearEmotes();

        message = value;
        fontSize = preferences.fontSize;
        showTimestamp = preferences.timestamps;
        contentWidth = std::max(100.0f, availableWidth);
        contentHeightLimit = std::max(
            fontSize * 1.28f + 7.0f,
            availableHeight);

        badgeImages.clear();
        for (const auto& badge : message.badges) {
            badgeImages.push_back(
                TwitchBadgeTexturePool::instance().get(
                    badge.imageUrl));
        }

        for (const auto& fragment : message.fragments) {
            if (fragment.type != twitch::ChatFragmentType::Emote)
                continue;

            EmoteVisual visual;
            if (!fragment.emoteUrl.empty())
                visual.staticImage =
                    TwitchEmoteTexturePool::instance().get(
                        fragment.emoteUrl);

            if (preferences.emoteMode ==
                    twitch::ChatEmoteMode::Animated &&
                !fragment.animatedEmoteUrl.empty())
                visual.animation =
                    TwitchAnimatedEmoteTexturePool::instance().get(
                        fragment.animatedEmoteUrl);

            emoteImages.push_back(visual);
        }

        this->setWidth(contentWidth);
        this->setHeight(estimateHeight());
        this->setVisibility(brls::Visibility::VISIBLE);
        this->invalidate();
    }

    void clear() {
        clearEmotes();
        message = {};
        this->setHeight(0);
        this->setVisibility(brls::Visibility::GONE);
    }

    void draw(
        NVGcontext* vg,
        float x,
        float y,
        float width,
        float height,
        brls::Style style,
        brls::FrameContext* ctx) override {
        if (message.text.empty()) return;

        // Never allow one message to paint into the next row, even if a
        // malformed/very wide fragment produces an unexpected wrap.
        nvgSave(vg);
        nvgIntersectScissor(vg, x, y, width, height);

        const float lineHeight = fontSize * 1.28f;
        const float emoteSize =
            std::max(18.0f, fontSize * 1.35f);
        const float badgeSize =
            std::max(15.0f, fontSize * 1.05f);
        const float right = x + width;
        float cursorX = x;
        float cursorY = y + fontSize;
        int line = 0;
        const int maxLines = maxLineCount();

        nvgFontFaceId(vg, brls::Application::getDefaultFont());
        nvgFontSize(vg, fontSize);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);

        auto textWidth = [&](const std::string& value) {
            if (value.empty()) return 0.0f;
            float bounds[4]{};
            return nvgTextBounds(
                vg, 0, 0, value.c_str(), nullptr, bounds);
        };

        auto nextLine = [&]() {
            cursorX = x;
            cursorY += lineHeight;
            ++line;
        };

        auto drawWord = [&](const std::string& value, NVGcolor color) {
            if (value.empty() || line >= maxLines) return;
            const float wordWidth = textWidth(value);

            if (cursorX > x && cursorX + wordWidth > right)
                nextLine();
            if (line >= maxLines) return;

            nvgFillColor(vg, color);
            nvgText(vg, cursorX, cursorY, value.c_str(), nullptr);
            cursorX += wordWidth;
        };

        auto drawWrappedText = [&](const std::string& value, NVGcolor color) {
            std::string token;
            for (size_t index = 0; index < value.size(); ++index) {
                token.push_back(value[index]);
                const bool boundary =
                    value[index] == ' ' ||
                    value[index] == '\n' ||
                    index + 1 == value.size();

                if (!boundary) continue;

                if (!token.empty() && token.back() == '\n') {
                    token.pop_back();
                    drawWord(token, color);
                    token.clear();
                    nextLine();
                } else {
                    drawWord(token, color);
                    token.clear();
                }

                if (line >= maxLines) break;
            }
        };

        if (showTimestamp && !message.timestamp.empty()) {
            drawWord(
                "[" + message.timestamp + "] ",
                nvgRGB(145, 154, 174));
        }

        const size_t displayedBadges =
            std::min<size_t>(
                message.badges.size(),
                6);

        for (size_t badgeIndex = 0;
             badgeIndex < displayedBadges;
             ++badgeIndex) {
            if (cursorX > x &&
                cursorX + badgeSize > right)
                nextLine();
            if (line >= maxLines) break;

            brls::Image* image =
                badgeIndex < badgeImages.size()
                    ? badgeImages[badgeIndex]
                    : nullptr;

            if (image && image->getTexture() > 0) {
                const float imageY =
                    cursorY - fontSize -
                    (badgeSize - fontSize) * 0.4f;
                NVGpaint paint = nvgImagePattern(
                    vg,
                    cursorX,
                    imageY,
                    badgeSize,
                    badgeSize,
                    0,
                    image->getTexture(),
                    1.0f);
                nvgBeginPath(vg);
                nvgRect(
                    vg,
                    cursorX,
                    imageY,
                    badgeSize,
                    badgeSize);
                nvgFillPaint(vg, paint);
                nvgFill(vg);
            }

            // Reserve the same space while an image is
            // still downloading to avoid text jumping.
            cursorX += badgeSize + 3.0f;
        }

        drawWord(
            message.userName + ": ",
            chatColor(message.color));

        size_t emoteIndex = 0;
        for (const auto& fragment : message.fragments) {
            if (line >= maxLines) break;

            if (fragment.type == twitch::ChatFragmentType::Emote) {
                if (cursorX > x && cursorX + emoteSize > right)
                    nextLine();
                if (line >= maxLines) break;

                const EmoteVisual* visual =
                    emoteIndex < emoteImages.size()
                        ? &emoteImages[emoteIndex]
                        : nullptr;
                ++emoteIndex;

                int texture = 0;
                if (visual && visual->animation)
                    texture =
                        TwitchAnimatedEmoteTexturePool::instance().texture(
                            visual->animation);
                if (texture <= 0 && visual &&
                    visual->staticImage)
                    texture = visual->staticImage->getTexture();

                if (texture > 0) {
                    const float imageY =
                        cursorY - fontSize - (emoteSize - fontSize) * 0.35f;
                    NVGpaint paint = nvgImagePattern(
                        vg,
                        cursorX,
                        imageY,
                        emoteSize,
                        emoteSize,
                        0,
                        texture,
                        1.0f);
                    nvgBeginPath(vg);
                    nvgRect(vg, cursorX, imageY, emoteSize, emoteSize);
                    nvgFillPaint(vg, paint);
                    nvgFill(vg);
                    cursorX += emoteSize + 3.0f;
                } else {
                    drawWord(
                        fragment.text + " ",
                        nvgRGB(245, 246, 250));
                }
            } else {
                drawWrappedText(
                    fragment.text,
                    nvgRGB(245, 246, 250));
            }
        }

        nvgBeginPath(vg);
        nvgMoveTo(vg, x, y + height - 1);
        nvgLineTo(vg, right, y + height - 1);
        nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 18));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);

        nvgRestore(vg);
    }

private:
    int maxLineCount() const {
        const float lineHeight = fontSize * 1.28f;
        return std::max(
            1,
            static_cast<int>(
                std::floor(
                    std::max(0.0f, contentHeightLimit - 7.0f) /
                    std::max(1.0f, lineHeight))));
    }

    float estimateHeight() const {
        const float lineHeight = fontSize * 1.28f;
        const float emoteSize =
            std::max(18.0f, fontSize * 1.35f);
        const float badgeSize =
            std::max(15.0f, fontSize * 1.05f);
        const int maxLines = maxLineCount();

        NVGcontext* vg = brls::Application::getNVGContext();
        if (!vg)
            return lineHeight + 7.0f;

        nvgSave(vg);
        nvgFontFaceId(
            vg,
            brls::Application::getDefaultFont());
        nvgFontSize(vg, fontSize);
        nvgTextAlign(
            vg,
            NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);

        auto textWidth = [&](const std::string& value) {
            if (value.empty()) return 0.0f;
            float bounds[4]{};
            return nvgTextBounds(
                vg,
                0,
                0,
                value.c_str(),
                nullptr,
                bounds);
        };

        float cursor = 0.0f;
        int lineCount = 1;

        auto nextLine = [&]() {
            cursor = 0.0f;
            lineCount =
                std::min(lineCount + 1, maxLines);
        };

        auto consumeWord = [&](const std::string& value) {
            if (value.empty() || lineCount >= maxLines + 1)
                return;

            const float wordWidth = textWidth(value);

            if (cursor > 0.0f &&
                cursor + wordWidth > contentWidth)
                nextLine();

            cursor += wordWidth;
        };

        auto consumeWrappedText =
            [&](const std::string& value) {
                std::string token;

                for (size_t index = 0;
                     index < value.size();
                     ++index) {
                    token.push_back(value[index]);

                    const bool boundary =
                        value[index] == ' ' ||
                        value[index] == '\n' ||
                        index + 1 == value.size();

                    if (!boundary) continue;

                    if (!token.empty() &&
                        token.back() == '\n') {
                        token.pop_back();
                        consumeWord(token);
                        token.clear();

                        if (lineCount < maxLines)
                            nextLine();
                    } else {
                        consumeWord(token);
                        token.clear();
                    }

                    if (lineCount >= maxLines)
                        break;
                }
            };

        if (showTimestamp &&
            !message.timestamp.empty()) {
            consumeWord(
                "[" + message.timestamp + "] ");
        }

        const size_t displayedBadges =
            std::min<size_t>(
                message.badges.size(),
                6);

        for (size_t badgeIndex = 0;
             badgeIndex < displayedBadges;
             ++badgeIndex) {
            if (cursor > 0.0f &&
                cursor + badgeSize > contentWidth)
                nextLine();

            cursor += badgeSize + 3.0f;
        }

        consumeWord(message.userName + ": ");

        for (const auto& fragment : message.fragments) {
            if (lineCount >= maxLines) break;

            if (fragment.type ==
                twitch::ChatFragmentType::Emote) {
                if (cursor > 0.0f &&
                    cursor + emoteSize > contentWidth)
                    nextLine();

                cursor += emoteSize + 3.0f;
            } else {
                consumeWrappedText(fragment.text);
            }
        }

        nvgRestore(vg);

        return std::clamp(
                   lineCount,
                   1,
                   maxLines) *
                   lineHeight +
               7.0f;
    }

    void clearEmotes() {
        // Shared loader views remain alive in the bounded app-lifetime pool.
        emoteImages.clear();
    }

    twitch::ChatMessage message;
    std::vector<brls::Image*> badgeImages;
    std::vector<EmoteVisual> emoteImages;
    float fontSize = 16.0f;
    float contentWidth = 300.0f;
    float contentHeightLimit = brls::Application::contentHeight;
    bool showTimestamp = false;
};

class TwitchChatPanel : public brls::Box {
public:
    TwitchChatPanel(
        bool docked,
        std::function<void()> sendRequested,
        std::function<bool()> backRequested)
        : docked(docked),
          sendRequested(std::move(sendRequested)),
          backRequested(std::move(backRequested)) {
        this->setAxis(brls::Axis::COLUMN);
        this->setWidth(docked ? 330.0f : 360.0f);
        panelHeight = brls::Application::contentHeight;
        this->setHeight(panelHeight);
        if (!docked)
            this->setPositionType(brls::PositionType::ABSOLUTE);
        this->setPadding(18, 12, 12, 12);
        this->setVisibility(brls::Visibility::GONE);
        this->setFocusable(false);

        title = new brls::Label();
        title->setText("LIVE CHAT");
        title->setFontSize(18);
        title->setTextColor(nvgRGB(245, 246, 250));
        title->setMarginBottom(7);
        this->addView(title);

        status = new brls::Label();
        status->setText("");
        status->setFontSize(13);
        status->setTextColor(nvgRGB(173, 181, 199));
        status->setMarginBottom(7);
        status->setVisibility(brls::Visibility::GONE);
        this->addView(status);

        messagesBox = new brls::Box();
        messagesBox->setAxis(brls::Axis::COLUMN);
        messagesBox->setGrow(1.0f);
        this->addView(messagesBox);

        for (size_t index = 0; index < ROW_POOL; ++index) {
            auto* row = new TwitchChatMessageView();
            row->setVisibility(brls::Visibility::GONE);
            messagesBox->addView(row);
            rows.push_back(row);
        }

        composeButton = new brls::Button();
        composeButton->setText("Send message");
        composeButton->setFontSize(15);
        composeButton->setHeight(46);
        composeButton->setStyle(&brls::BUTTONSTYLE_BORDERED);
        composeButton->setMarginTop(8);
        composeButton->setVisibility(brls::Visibility::GONE);
        composeButton->registerClickAction(
            [this](brls::View*) {
                if (this->sendRequested) this->sendRequested();
                return true;
            });

        // The chat panel is outside VideoView's focus tree. If this button
        // receives focus after closing a composer/IME, route B to the same
        // playback back action instead of swallowing it.
        composeButton->registerAction(
            "Back",
            brls::BUTTON_B,
            [this](brls::View*) {
                return this->backRequested
                    ? this->backRequested()
                    : false;
            });

        this->addView(composeButton);
    }

    void apply(const twitch::ChatPreferences& value) {
        preferences = value;
        panelWidth = static_cast<float>(
            docked ? value.dockedWidth : value.overlayWidth);
        this->setWidth(panelWidth);

        if (docked) {
            panelHeight = brls::Application::contentHeight;
            this->setHeight(panelHeight);
            this->setBackgroundColor(nvgRGBA(25, 32, 47, 255));
        } else {
            const bool compact =
                value.overlaySize == twitch::ChatOverlaySize::Compact;
            panelHeight = compact
                ? std::max(
                      280.0f,
                      brls::Application::contentHeight * 0.5f)
                : brls::Application::contentHeight;
            this->setHeight(panelHeight);

            const float inset = compact ? 12.0f : 0.0f;
            const bool onLeft =
                value.overlayPosition ==
                    twitch::ChatOverlayPosition::TopLeft ||
                value.overlayPosition ==
                    twitch::ChatOverlayPosition::BottomLeft;
            const bool onBottom =
                compact &&
                (value.overlayPosition ==
                     twitch::ChatOverlayPosition::BottomRight ||
                 value.overlayPosition ==
                     twitch::ChatOverlayPosition::BottomLeft);

            this->setPositionTop(
                onBottom ? brls::View::AUTO : inset);
            this->setPositionBottom(
                onBottom ? inset : brls::View::AUTO);
            this->setPositionLeft(
                onLeft ? inset : brls::View::AUTO);
            this->setPositionRight(
                onLeft ? brls::View::AUTO : inset);

            const int alpha =
                std::clamp(value.opacity * 255 / 100, 0, 255);
            this->setBackgroundColor(nvgRGBA(12, 14, 20, alpha));
        }

        composeButton->setVisibility(
            value.participation ==
                    twitch::ChatParticipation::Interactive
                ? brls::Visibility::VISIBLE
                : brls::Visibility::GONE);

        refresh();
    }

    void setStatus(const std::string& value) {
        if (value.empty() ||
            value == "Live chat" ||
            value == "Chat disabled") {
            status->setText("");
            status->setVisibility(brls::Visibility::GONE);
            return;
        }

        status->setText(value);
        status->setVisibility(brls::Visibility::VISIBLE);
    }

    void append(twitch::ChatMessage message) {
        messages.push_back(std::move(message));
        while (messages.size() > MAX_MESSAGES)
            messages.pop_front();
        refresh();
    }

    float width() const { return panelWidth; }

    void setPortraitFrame(float width, float height) {
        panelWidth = width;
        panelHeight = height;
        this->setWidth(width);
        this->setHeight(height);
        this->setBackgroundColor(nvgRGBA(18, 23, 34, 255));
        refresh();
    }

private:
    void refresh() {
        for (auto* row : rows) row->clear();
        if (messages.empty()) return;

        const float innerWidth =
            std::max(120.0f, panelWidth - 24.0f);
        const float reserved =
            18.0f + 28.0f +
            (status->getVisibility() == brls::Visibility::VISIBLE
                ? 28.0f
                : 0.0f) +
            (composeButton->getVisibility() == brls::Visibility::VISIBLE
                ? 58.0f
                : 0.0f);
        const float availableHeight =
            std::max(
                120.0f,
                panelHeight - reserved);

        std::vector<const twitch::ChatMessage*> selected;
        float consumed = 0.0f;

        for (auto iterator = messages.rbegin();
             iterator != messages.rend() &&
             selected.size() < rows.size();
             ++iterator) {
            // Configure a temporary row measurement using the exact same
            // NanoVG layout rules as the renderer. This prevents the panel
            // from selecting more messages than the available height can
            // actually contain.
            rows.front()->configure(
                *iterator,
                preferences,
                innerWidth,
                availableHeight);
            const float measuredHeight =
                rows.front()->getHeight();
            rows.front()->clear();

            if (!selected.empty() &&
                consumed + measuredHeight > availableHeight)
                break;

            selected.push_back(&*iterator);
            consumed += measuredHeight;
        }

        std::reverse(selected.begin(), selected.end());
        for (size_t index = 0;
             index < selected.size() && index < rows.size();
             ++index) {
            rows[index]->configure(
                *selected[index],
                preferences,
                innerWidth,
                availableHeight);
        }
    }

    static constexpr size_t ROW_POOL = 20;
    static constexpr size_t MAX_MESSAGES = 72;

    bool docked = false;
    float panelWidth = 330.0f;
    float panelHeight = brls::Application::contentHeight;
    twitch::ChatPreferences preferences;
    std::function<void()> sendRequested;
    std::function<bool()> backRequested;
    std::deque<twitch::ChatMessage> messages;
    brls::Label* title = nullptr;
    brls::Label* status = nullptr;
    brls::Box* messagesBox = nullptr;
    brls::Button* composeButton = nullptr;
    std::vector<TwitchChatMessageView*> rows;
};

struct ChatUiState {
    std::atomic_bool alive{true};
    TwitchChatPanel* overlayPanel = nullptr;
    TwitchChatPanel* dockedPanel = nullptr;

    void append(twitch::ChatMessage message) {
        if (overlayPanel) overlayPanel->append(message);
        if (dockedPanel) dockedPanel->append(std::move(message));
    }

    void setStatus(const std::string& status) {
        if (overlayPanel) overlayPanel->setStatus(status);
        if (dockedPanel) dockedPanel->setStatus(status);
    }
};


struct TwitchPlaybackRecoveryState {
    std::atomic_bool alive{true};
    std::atomic_bool recovering{false};
    std::atomic_bool retryScheduled{false};
    std::atomic_bool reconfigArmed{false};
    std::atomic_bool hardwareDecode{false};
    std::atomic_bool hybridSoftware{false};
    std::atomic_bool hybridRestoreEligible{false};
    std::atomic_bool decoderSwitching{false};
    // Hardware-only recovery must not replace the playlist until MPV has
    // confirmed that the old NVTEGRA-backed file has stopped. This prevents
    // a new load from racing an in-flight hardware decode job.
    std::atomic_bool waitingForHardwareStop{false};
    std::atomic_int pendingRecoveryAttempt{0};
    std::atomic_uint hardwareStopGeneration{0};
    std::atomic_int decoderMode{
        static_cast<int>(twitch::DecoderMode::Software)};
    std::atomic_int attempt{0};
    std::atomic_int reconfigCount{0};
    std::atomic_uint restoreGeneration{0};
    std::chrono::steady_clock::time_point lastLoadedAt{};
    std::chrono::steady_clock::time_point lastReconfigRecovery{};
    std::string channel;
};

constexpr int TWITCH_RECOVERY_MAX_ATTEMPTS = 5;
constexpr int HYBRID_RESTORE_STABLE_SECONDS = 8;
constexpr int HYBRID_REARM_SECONDS = 3;

void performTwitchPlaybackRecovery(
    const std::shared_ptr<TwitchPlaybackRecoveryState>& state,
    int attempt);

void resolveTwitchReplacementPlaylist(
    const std::shared_ptr<TwitchPlaybackRecoveryState>& state,
    int attempt);

void continueHardwareRecoveryAfterStop(
    const std::shared_ptr<TwitchPlaybackRecoveryState>& state);

void scheduleHybridHardwareRestore(
    const std::shared_ptr<TwitchPlaybackRecoveryState>& state) {
    if (!state || !state->alive.load() ||
        !state->hybridSoftware.load() ||
        !state->hybridRestoreEligible.load())
        return;

    const unsigned int generation =
        state->restoreGeneration.fetch_add(1) + 1;

    brls::Logger::info(
        "twiNX Hybrid: normal presentation returned; waiting {} seconds "
        "before restoring hardware decoding",
        HYBRID_RESTORE_STABLE_SECONDS);

    ThreadPool::instance().submit(
        [state, generation](HTTP&) {
            for (int elapsed = 0;
                 elapsed < HYBRID_RESTORE_STABLE_SECONDS &&
                 state->alive.load();
                 ++elapsed) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (!state->alive.load()) return;

            brls::sync([state, generation]() {
                if (!state->alive.load() ||
                    generation != state->restoreGeneration.load() ||
                    state->recovering.load() ||
                    twitch::loadDecoderMode() !=
                        twitch::DecoderMode::Hybrid ||
                    !state->hybridSoftware.load() ||
                    !state->hybridRestoreEligible.load())
                    return;

                state->decoderSwitching.store(true);
                state->reconfigArmed.store(false);
                state->hybridRestoreEligible.store(false);

                auto& mpv = MPVCore::instance();
                mpv.command("set", "vd-lavc-dr", "no");
                mpv.command(
                    "set",
                    "hwdec",
                    MPVCore::PLAYER_HWDEC_METHOD.c_str());
                mpv.command("set", "pause", "no");

                brls::Logger::warning(
                    "twiNX Hybrid: requested hardware decoder restoration");

                ThreadPool::instance().submit(
                    [state, generation](HTTP&) {
                        for (int elapsed = 0;
                             elapsed < HYBRID_REARM_SECONDS &&
                             state->alive.load();
                             ++elapsed) {
                            std::this_thread::sleep_for(
                                std::chrono::seconds(1));
                        }

                        if (!state->alive.load()) return;

                        brls::sync([state, generation]() {
                            if (!state->alive.load() ||
                                generation !=
                                    state->restoreGeneration.load())
                                return;

                            auto& mpv = MPVCore::instance();
                            const std::string active =
                                mpv.getString("hwdec-current");

                            state->decoderSwitching.store(false);
                            state->lastLoadedAt =
                                std::chrono::steady_clock::now();

                            if (active.empty() || active == "no") {
                                state->hybridSoftware.store(true);
                                state->hardwareDecode.store(false);
                                state->reconfigArmed.store(true);
                                mpv.command("set", "hwdec", "no");
                                brls::Application::notify(
                                    "twiNX Hybrid: hardware restore failed; "
                                    "continuing in software");
                                brls::Logger::warning(
                                    "twiNX Hybrid: hardware decoder did not "
                                    "reactivate; software remains active");
                                return;
                            }

                            state->hybridSoftware.store(false);
                            state->hardwareDecode.store(true);
                            state->reconfigArmed.store(true);
#if defined(TWINX_PLAYBACK_PERF_DEBUG)
                            mpv.logHardwareDecoderState(
                                "hybrid-hardware-restored");
#endif
                            brls::Application::notify(
                                "twiNX Hybrid: hardware decoding restored");
                        });
                    });
            });
        });
}

void finishTwitchRecoveryFailure(
    const std::shared_ptr<TwitchPlaybackRecoveryState>& state,
    const std::string& error) {
    state->retryScheduled.store(false);
    state->recovering.store(false);
    state->attempt.store(0);

    if (state->alive.load()) {
        brls::Application::notify(
            "twiNX could not resume the live stream: " + error);
    }
}

void scheduleTwitchPlaybackRetry(
    const std::shared_ptr<TwitchPlaybackRecoveryState>& state,
    const std::string& error) {
    if (!state || !state->alive.load()) return;

    bool expected = false;
    if (!state->retryScheduled.compare_exchange_strong(expected, true))
        return;

    const int nextAttempt = state->attempt.fetch_add(1) + 1;
    if (nextAttempt > TWITCH_RECOVERY_MAX_ATTEMPTS) {
        finishTwitchRecoveryFailure(state, error);
        return;
    }

    const int delaySeconds = std::min(nextAttempt, 4);
    ThreadPool::instance().submit(
        [state, nextAttempt, delaySeconds](HTTP&) {
            for (int elapsed = 0;
                 elapsed < delaySeconds && state->alive.load();
                 ++elapsed) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (!state->alive.load()) return;

            brls::sync([state, nextAttempt]() {
                if (!state->alive.load() ||
                    !state->recovering.load())
                    return;

                state->retryScheduled.store(false);
                performTwitchPlaybackRecovery(state, nextAttempt);
            });
        });
}

void useTwitchSoftwareDecoder(
    const std::shared_ptr<TwitchPlaybackRecoveryState>& state,
    const std::string& reason) {
    auto& mpv = MPVCore::instance();
    mpv.command("set", "pause", "yes");
    mpv.command("drop-buffers");
    mpv.command("set", "vd-lavc-dr", "no");
    mpv.command("set", "hwdec", "no");

    if (state) {
        state->hybridSoftware.store(true);
        state->hardwareDecode.store(false);
        state->restoreGeneration.fetch_add(1);
    }

    brls::Logger::warning(
        "twiNX Hybrid: switched to software decoding ({})",
        reason);
}

void resolveTwitchReplacementPlaylist(
    const std::shared_ptr<TwitchPlaybackRecoveryState>& state,
    int attempt) {
    if (!state || !state->alive.load() || !state->recovering.load())
        return;

    auto config = twitch::loadConfig();
    config.channel = state->channel;

    twitch::resolveLiveAsync(
        config,
        [state](twitch::Resolution result) {
            if (!state->alive.load() || !state->recovering.load()) return;

            MPVCore::BOTTOM_BAR = false;
            MPVCore::instance().setUrl(
                result.selected.url,
                twitch::mpvExtra());

            // Recovery remains active until MPV_LOADED confirms that this
            // replacement playlist actually opened.
        },
        [state, attempt](const std::string& error) {
            if (!state->alive.load()) return;
            scheduleTwitchPlaybackRetry(state, error);
        });
}

void continueHardwareRecoveryAfterStop(
    const std::shared_ptr<TwitchPlaybackRecoveryState>& state) {
    if (!state || !state->alive.load() || !state->recovering.load())
        return;

    bool expected = true;
    if (!state->waitingForHardwareStop.compare_exchange_strong(
            expected,
            false))
        return;

    const int attempt = state->pendingRecoveryAttempt.load();
    brls::Logger::warning(
        "twiNX: old hardware-decoded Twitch file stopped; "
        "resolving a fresh playlist without toggling hwdec");
    resolveTwitchReplacementPlaylist(state, attempt);
}

void scheduleHardwareStopWatchdog(
    const std::shared_ptr<TwitchPlaybackRecoveryState>& state,
    unsigned int generation) {
    ThreadPool::instance().submit(
        [state, generation](HTTP&) {
            for (int elapsed = 0; elapsed < 5 && state->alive.load(); ++elapsed)
                std::this_thread::sleep_for(std::chrono::seconds(1));

            if (!state->alive.load()) return;

            brls::sync([state, generation]() {
                if (!state->alive.load() ||
                    !state->recovering.load() ||
                    !state->waitingForHardwareStop.load() ||
                    generation != state->hardwareStopGeneration.load())
                    return;

                if (MPVCore::instance().isStopped()) {
                    brls::Logger::warning(
                        "twiNX: hardware stop confirmation event was delayed; "
                        "continuing after MPV reported the file stopped");
                    continueHardwareRecoveryAfterStop(state);
                    return;
                }

                state->waitingForHardwareStop.store(false);
                finishTwitchRecoveryFailure(
                    state,
                    "the hardware decoder did not stop cleanly");
                brls::Logger::error(
                    "twiNX: aborted hardware recovery because MPV did not "
                    "confirm a clean stop within 5 seconds");
            });
        });
}

void performTwitchPlaybackRecovery(
    const std::shared_ptr<TwitchPlaybackRecoveryState>& state,
    int attempt) {
    if (!state || !state->alive.load()) return;

    const twitch::DecoderMode mode = twitch::loadDecoderMode();
    state->decoderMode.store(static_cast<int>(mode));

    if (mode == twitch::DecoderMode::Hardware) {
        auto& mpv = MPVCore::instance();
        state->hardwareDecode.store(true);
        state->hybridSoftware.store(false);
        state->reconfigArmed.store(false);
        state->pendingRecoveryAttempt.store(attempt);

        // Never toggle hwdec or discard decoder buffers while NVTEGRA may
        // still be waiting on an in-flight frame. Stop the old file first and
        // load the replacement only after MPV confirms MPV_STOP.
        if (mpv.isStopped()) {
            state->waitingForHardwareStop.store(false);
            brls::Logger::warning(
                "twiNX: hardware-decoded Twitch file had already ended; "
                "resolving a fresh playlist");
            resolveTwitchReplacementPlaylist(state, attempt);
            return;
        }

        state->waitingForHardwareStop.store(true);
        const unsigned int generation =
            state->hardwareStopGeneration.fetch_add(1) + 1;

        brls::Logger::warning(
            "twiNX: stopping the old hardware-decoded Twitch file before "
            "loading its replacement");
        mpv.stop();
        scheduleHardwareStopWatchdog(state, generation);
        return;
    }

    state->waitingForHardwareStop.store(false);
    state->hardwareStopGeneration.fetch_add(1);

    if (mode == twitch::DecoderMode::Hybrid) {
        // The first dangerous Twitch transition moves Hybrid to software.
        // Subsequent recovery events keep software active until the return to
        // the normal presentation is confirmed and stable.
        useTwitchSoftwareDecoder(
            state,
            state->hybridRestoreEligible.load()
                ? "return transition"
                : "commercial/presentation transition");
    } else {
        auto& mpv = MPVCore::instance();
        mpv.command("set", "pause", "yes");
        mpv.command("drop-buffers");
        mpv.command("set", "hwdec", "no");
        state->hardwareDecode.store(false);
        state->hybridSoftware.store(false);
    }

    resolveTwitchReplacementPlaylist(state, attempt);
}

bool requestTwitchPlaybackRecovery(
    const std::shared_ptr<TwitchPlaybackRecoveryState>& state,
    MpvEventEnum event) {
#if defined(TWINX_PLAYBACK_DEBUG)
    twinx::debug::log(
        "TW_RECOVERY",
        "request event=%s(%d) state=%p alive=%d recovering=%d waiting_stop=%d switching=%d",
        twinx::debug::playerEventName(static_cast<int>(event)),
        static_cast<int>(event),
        state.get(),
        state && state->alive.load() ? 1 : 0,
        state && state->recovering.load() ? 1 : 0,
        state && state->waitingForHardwareStop.load() ? 1 : 0,
        state && state->decoderSwitching.load() ? 1 : 0);
#endif
    if (!state || !state->alive.load()) return true;

    if (state->decoderSwitching.load()) {
        brls::Logger::debug(
            "twiNX Hybrid: ignoring decoder-generated event {}",
            static_cast<int>(event));
        return true;
    }

    if (state->waitingForHardwareStop.load()) {
        brls::Logger::debug(
            "twiNX: ignoring event {} while waiting for the old "
            "hardware-decoded file to stop",
            static_cast<int>(event));
        return true;
    }

    // Decoder reset itself can generate VIDEO_RECONFIG. Ignore those while a
    // recovery is already in progress instead of creating recursive retries.
    if (event == MpvEventEnum::VIDEO_RECONFIG &&
        state->recovering.load())
        return true;

    const twitch::DecoderMode mode = twitch::loadDecoderMode();
    state->decoderMode.store(static_cast<int>(mode));

    // Hardware-only mode must never force a decoder teardown or playlist
    // replacement after MPV has begun processing a Twitch presentation
    // transition. The Switch NVTEGRA path can still own frames inside
    // mp_image_hw_download at this point. Let MPV finish naturally; if the
    // presentation ends or errors, VideoView will show its normal controlled
    // end/error UI instead of racing the hardware decoder.
    if (mode == twitch::DecoderMode::Hardware) {
        brls::Logger::warning(
            "twiNX Hardware: automatic live recovery suppressed for event {} "
            "to avoid tearing down an active NVTEGRA frame",
            static_cast<int>(event));
        return false;
    }

    // A second transition while Hybrid is already in software is our signal
    // that Twitch has moved away from the inserted presentation. Hardware is
    // restored only after the replacement stream has loaded and stayed stable.
    if (mode == twitch::DecoderMode::Hybrid &&
        state->hybridSoftware.load()) {
        state->hybridRestoreEligible.store(true);
    }

    bool expected = false;
    if (state->recovering.compare_exchange_strong(expected, true)) {
        state->attempt.store(0);
        state->retryScheduled.store(false);
        state->restoreGeneration.fetch_add(1);

        brls::Logger::warning(
            "twiNX: Twitch live playback interrupted (event {}), "
            "resolving a fresh playlist",
            static_cast<int>(event));
        brls::Application::notify(
            mode == twitch::DecoderMode::Hybrid
                ? "twiNX Hybrid: stabilizing the live stream in software…"
                : "twiNX: resuming the live stream…");

        performTwitchPlaybackRecovery(state, 0);
    } else {
        // If the replacement playlist fails before MPV_LOADED, schedule one
        // controlled retry rather than starting overlapping resolver jobs.
        scheduleTwitchPlaybackRetry(
            state,
            "the replacement playlist did not open");
    }

    return true;
}

void handleTwitchVideoReconfig(
    const std::shared_ptr<TwitchPlaybackRecoveryState>& state) {
#if defined(TWINX_PLAYBACK_DEBUG)
    twinx::debug::log(
        "TW_RECONFIG",
        "enter state=%p alive=%d switching=%d armed=%d recovering=%d count=%d",
        state.get(),
        state && state->alive.load() ? 1 : 0,
        state && state->decoderSwitching.load() ? 1 : 0,
        state && state->reconfigArmed.load() ? 1 : 0,
        state && state->recovering.load() ? 1 : 0,
        state ? state->reconfigCount.load() : -1);
#endif
    if (!state || !state->alive.load() ||
        state->decoderSwitching.load())
        return;

    const twitch::DecoderMode mode = twitch::loadDecoderMode();
    state->decoderMode.store(static_cast<int>(mode));

    if (mode == twitch::DecoderMode::Software) return;

    const int count = state->reconfigCount.fetch_add(1) + 1;
    const auto now = std::chrono::steady_clock::now();

    // Ignore startup/reload reconfiguration. A later Twitch presentation
    // change (such as live -> commercial or commercial -> live) is the
    // dangerous transition.
    if (!state->reconfigArmed.load() ||
        state->lastLoadedAt.time_since_epoch().count() == 0 ||
        now - state->lastLoadedAt < std::chrono::seconds(5)) {
        brls::Logger::debug(
            "twiNX: ignoring startup video reconfig {}",
            count);
        return;
    }

    if (state->lastReconfigRecovery.time_since_epoch().count() != 0 &&
        now - state->lastReconfigRecovery < std::chrono::seconds(8)) {
        brls::Logger::debug(
            "twiNX: throttling repeated video reconfig {}",
            count);
        return;
    }

    if (mode == twitch::DecoderMode::Hardware) {
        // Diagnostic/safety behavior: do not stop, reload, drop buffers, or
        // change hwdec in response to VIDEO_RECONFIG. Any of those operations
        // can race a hardware frame currently being downloaded by MPV.
        brls::Logger::warning(
            "twiNX Hardware: Twitch video reconfiguration {} detected; "
            "leaving MPV/NVTEGRA untouched",
            count);
        return;
    }

    state->lastReconfigRecovery = now;

    if (mode == twitch::DecoderMode::Hybrid) {
        if (state->hybridSoftware.load()) {
            state->hybridRestoreEligible.store(true);
            brls::Logger::warning(
                "twiNX Hybrid: second Twitch presentation transition "
                "detected; hardware restore will be attempted after stable "
                "software playback");
        } else {
            brls::Logger::warning(
                "twiNX Hybrid: Twitch presentation transition detected; "
                "switching to software before consuming the new frames");
        }
    } else {
        brls::Logger::warning(
            "twiNX: post-startup Twitch video reconfiguration detected; "
            "resetting hardware decoder before new frames are consumed");
    }

    requestTwitchPlaybackRecovery(
        state,
        MpvEventEnum::VIDEO_RECONFIG);
}

const char* configuredVideoRotation() {
    switch (MPVCore::VIDEO_ROTATION) {
        case 1: return "90";
        case 2: return "180";
        case 3: return "270";
        default: return "0";
    }
}

void resetPlayerVideoLayout() {
    auto& mpv = MPVCore::instance();
    mpv.command("set", "video-rotate", configuredVideoRotation());
    mpv.command("set", "video-margin-ratio-left", "0.0");
    mpv.command("set", "video-margin-ratio-right", "0.0");
    mpv.command("set", "video-margin-ratio-top", "0.0");
    mpv.command("set", "video-margin-ratio-bottom", "0.0");
    mpv.command("set", "video-zoom", "0.0");
    mpv.command("set", "video-pan-x", "0.0");
    mpv.command("set", "video-pan-y", "0.0");
    mpv.command("set", "video-align-x", "0");
    mpv.command("set", "video-align-y", "0");
    mpv.command("set", "video-recenter", "yes");
    mpv.setAspect(MPVCore::VIDEO_ASPECT);
}

std::atomic_uint64_t nextPlaybackSession{0};
std::atomic_uint64_t activePlaybackSession{0};

}  // namespace

class RemotePlayer : public brls::Box {
public:
    RemotePlayer(const remote::DirEntry& item, std::string twitchChannel = "")
        : twitchChannel(std::move(twitchChannel)),
          playbackSessionId(++nextPlaybackSession) {
        activePlaybackSession.store(playbackSessionId);
#if defined(TWINX_PLAYBACK_DEBUG)
        twinx::debug::log(
            "TW_PLAYER",
            "construct session=%llu channel=%s item_type=%d item_name=%s",
            static_cast<unsigned long long>(playbackSessionId),
            this->twitchChannel.empty() ? "<non-twitch>" : this->twitchChannel.c_str(),
            static_cast<int>(item.type),
            item.name.c_str());
#endif
        MPVCore::LIVE_STREAM_RECOVERY = !this->twitchChannel.empty();
        resetPlayerVideoLayout();
        float width = brls::Application::contentWidth;
        float height = brls::Application::contentHeight;
        view->setDimensions(width, height);
        view->setGrow(1.0f);
        view->setId("video");
        view->setTitie(item.name);
        view->hideVideoQuality();
        this->setDimensions(width, height);
        this->setAxis(brls::Axis::ROW);
        this->addView(view);

        auto composeChat = [this]() {
            openChatComposer();
        };

        auto closePlayback = []() {
            return VideoView::close(true);
        };

        chatOverlayPanel =
            new TwitchChatPanel(
                false,
                composeChat,
                closePlayback);
        view->setContentOverlay(chatOverlayPanel);

        chatDockedPanel =
            new TwitchChatPanel(
                true,
                composeChat,
                closePlayback);
        this->addView(chatDockedPanel);

        chatUi = std::make_shared<ChatUiState>();
        chatUi->overlayPanel = chatOverlayPanel;
        chatUi->dockedPanel = chatDockedPanel;

        if (this->twitchChannel.empty()) {
            chatOverlayPanel->setVisibility(brls::Visibility::GONE);
            chatDockedPanel->setVisibility(brls::Visibility::GONE);
            view->setDimensions(width, height);
        }

        if (!this->twitchChannel.empty()) {
            MPVCore::LIVE_STREAM_RECOVERY = true;

            const twitch::DecoderMode decoderMode =
                twitch::loadDecoderMode();
            const bool hardwareDecode =
                twitch::decoderUsesHardware(decoderMode);
#if defined(TWINX_PLAYBACK_DEBUG)
            twinx::debug::log(
                "TW_PLAYER",
                "session=%llu decoder_mode=%d hardware=%d",
                static_cast<unsigned long long>(playbackSessionId),
                static_cast<int>(decoderMode),
                hardwareDecode ? 1 : 0);
#endif
            if (hardwareDecode) {
                MPVCore::instance().command(
                    "set",
                    "hwdec",
                    MPVCore::PLAYER_HWDEC_METHOD.c_str());
                MPVCore::instance().command(
                    "set",
                    "vd-lavc-dr",
                    "no");
                brls::Logger::warning(
                    decoderMode == twitch::DecoderMode::Hybrid
                        ? "twiNX: experimental Twitch Hybrid decoding enabled"
                        : "twiNX: experimental Twitch hardware decoding enabled");
            } else {
                MPVCore::instance().command("set", "hwdec", "no");
                brls::Logger::info(
                    "twiNX: stable Twitch software decoding enabled");
            }

            playbackRecovery =
                std::make_shared<TwitchPlaybackRecoveryState>();
            playbackRecovery->channel = this->twitchChannel;
            playbackRecovery->hardwareDecode.store(hardwareDecode);
            playbackRecovery->decoderMode.store(
                static_cast<int>(decoderMode));
            playbackRecovery->hybridSoftware.store(false);
            playbackRecovery->hybridRestoreEligible.store(false);

            std::weak_ptr<TwitchPlaybackRecoveryState> weakRecovery =
                playbackRecovery;
            view->setPlaybackRecoveryHandler(
                [weakRecovery](MpvEventEnum event) {
                    auto state = weakRecovery.lock();
                    if (!state) return true;
                    return requestTwitchPlaybackRecovery(state, event);
                });

            chatPreferenceSubscribeID =
                twitch::chatPreferencesEvent()->subscribe(
                    [this](twitch::ChatPreferences preferences) {
                        applyChatPreferences(preferences);
                    });
            applyChatPreferences(
                twitch::loadChatPreferences());

            auto& orientationController =
                twinx::portrait::OrientationController::instance();
            orientationSubscribeID =
                orientationController.getOrientationChanged()->subscribe(
                    [this](twinx::portrait::DisplayOrientation orientation) {
                        applyDisplayOrientation(orientation);
                    });
            applyDisplayOrientation(orientationController.orientation());

            chatModeAction = this->registerAction(
                "Chat layout",
                brls::BUTTON_START,
                [this](brls::View*) {
                    cycleChatMode();
                    return true;
                },
                false);

            channelSubscribeID =
                view->getChannelEvent()->subscribe(
                    [this]() {
                        openChannelPage();
                    });
            channelEventSubscribed = true;

            channelProfileCancel =
                std::make_shared<std::atomic_bool>(
                    false);
            this->ptrLock();
            twitch::loadChannelProfileAsync(
                twitch::loadConfig(),
                this->twitchChannel,
                [this](twitch::UserProfile profile) {
                    channelProfile =
                        std::move(profile);
                    view->showChannelButton(
                        channelProfile.profileImageUrl);
                    this->ptrUnlock();
                },
                [this](const std::string& error) {
                    brls::Logger::warning(
                        "twiNX: player channel avatar "
                        "could not be loaded: {}",
                        error);
                    view->hideChannelButton();
                    this->ptrUnlock();
                },
                channelProfileCancel);
        }

        if (item.type == remote::EntryType::PLAYLIST) {
            view->hideVideoProgressSlider();
        } else if (item.name.size() > 0) {
            titles.push_back(item.name);
        }

        auto& mpv = MPVCore::instance();
        eventSubscribeID = mpv.getEvent()->subscribe([this](MpvEventEnum event) {
            if (playerShuttingDown ||
                activePlaybackSession.load() != playbackSessionId)
                return;

#if defined(TWINX_PLAYBACK_DEBUG)
            twinx::debug::log(
                "TW_EVENT",
                "session=%llu event=%s(%d) recovery=%p recovering=%d armed=%d "
                "hw=%d hybrid_sw=%d waiting_stop=%d",
                static_cast<unsigned long long>(playbackSessionId),
                twinx::debug::playerEventName(static_cast<int>(event)),
                static_cast<int>(event),
                playbackRecovery.get(),
                playbackRecovery && playbackRecovery->recovering.load() ? 1 : 0,
                playbackRecovery && playbackRecovery->reconfigArmed.load() ? 1 : 0,
                playbackRecovery && playbackRecovery->hardwareDecode.load() ? 1 : 0,
                playbackRecovery && playbackRecovery->hybridSoftware.load() ? 1 : 0,
                playbackRecovery && playbackRecovery->waitingForHardwareStop.load() ? 1 : 0);
#endif

            auto& mpv = MPVCore::instance();
            switch (event) {
            case MpvEventEnum::MPV_LOADED: {
                if (!this->twitchChannel.empty()) {
                    applyTwitchVideoLayout(currentChatPreferences);
                    if (playbackRecovery) {
                        const twitch::DecoderMode decoderMode =
                            twitch::loadDecoderMode();
                        const bool hardwareDecode =
                            twitch::decoderUsesHardware(decoderMode) &&
                            !playbackRecovery->hybridSoftware.load();

                        playbackRecovery->decoderMode.store(
                            static_cast<int>(decoderMode));
                        playbackRecovery->hardwareDecode.store(
                            hardwareDecode);
                        playbackRecovery->lastLoadedAt =
                            std::chrono::steady_clock::now();
                        playbackRecovery->waitingForHardwareStop.store(false);
                        playbackRecovery->hardwareStopGeneration.fetch_add(1);
                        playbackRecovery->reconfigArmed.store(
                            decoderMode != twitch::DecoderMode::Software);

                        if (decoderMode == twitch::DecoderMode::Software) {
                            playbackRecovery->hybridSoftware.store(false);
                            playbackRecovery->hybridRestoreEligible.store(false);
                            MPVCore::instance().command(
                                "set",
                                "hwdec",
                                "no");
                        } else if (decoderMode == twitch::DecoderMode::Hybrid &&
                                   playbackRecovery->hybridSoftware.load()) {
                            MPVCore::instance().command(
                                "set",
                                "hwdec",
                                "no");
#if defined(TWINX_PLAYBACK_PERF_DEBUG)
                            MPVCore::instance().logHardwareDecoderState(
                                "hybrid-software-transition");
#endif
                        } else {
#if defined(TWINX_PLAYBACK_PERF_DEBUG)
                            MPVCore::instance().logHardwareDecoderState(
                                decoderMode == twitch::DecoderMode::Hybrid
                                    ? "hybrid-hardware"
                                    : "twitch-experimental-hardware");
#endif

                            const std::string activeHwdec =
                                MPVCore::instance().getString(
                                    "hwdec-current");
                            if (activeHwdec == "no") {
                                brls::Application::notify(
                                    decoderMode == twitch::DecoderMode::Hybrid
                                        ? "twiNX Hybrid: hardware did not activate; "
                                          "continuing in software"
                                        : "twiNX warning: experimental hardware "
                                          "decoding did not activate");
                                if (decoderMode == twitch::DecoderMode::Hybrid) {
                                    playbackRecovery->hybridSoftware.store(true);
                                    playbackRecovery->hardwareDecode.store(false);
                                }
                            }
                        }
                    }
                }

                const bool recoveryCompleted =
                    playbackRecovery &&
                    playbackRecovery->recovering.exchange(false);
                if (recoveryCompleted) {
                    playbackRecovery->retryScheduled.store(false);
                    playbackRecovery->attempt.store(0);
                    brls::Application::notify(
                        twitch::loadDecoderMode() == twitch::DecoderMode::Hybrid
                            ? "twiNX Hybrid: live stream stabilized in software"
                            : "twiNX: live stream resumed");
                }

                if (playbackRecovery &&
                    twitch::loadDecoderMode() == twitch::DecoderMode::Hybrid &&
                    playbackRecovery->hybridSoftware.load() &&
                    playbackRecovery->hybridRestoreEligible.load()) {
                    scheduleHybridHardwareRestore(playbackRecovery);
                }

                if (titles.empty()) this->loadList();
                view->getProfile()->init("Local");
                const char* flag = MPVCore::SUBS_FALLBACK ? "select" : "auto";
                for (auto& it : this->subtitles) {
                    mpv.command("sub-add", it.second.c_str(), flag, it.first.c_str());
                }
                break;
            }
            case MpvEventEnum::MPV_STOP:
                if (playbackRecovery &&
                    playbackRecovery->waitingForHardwareStop.load())
                    continueHardwareRecoveryAfterStop(playbackRecovery);
                break;
            case MpvEventEnum::VIDEO_RECONFIG:
                if (playbackRecovery)
                    handleTwitchVideoReconfig(playbackRecovery);
                break;
            default:;
            }
        });
        settingSubscribeID = view->getSettingEvent()->subscribe([this]() {
            brls::View* setting = new PlayerSetting(nullptr, this->twitchChannel);
            brls::Application::pushActivity(new brls::Activity(setting));
        });
    }

    ~RemotePlayer() override {
#if defined(TWINX_PLAYBACK_DEBUG)
        twinx::debug::log(
            "TW_PLAYER",
            "destruct session=%llu channel=%s owns=%d",
            static_cast<unsigned long long>(playbackSessionId),
            this->twitchChannel.empty() ? "<non-twitch>" : this->twitchChannel.c_str(),
            activePlaybackSession.load() == playbackSessionId ? 1 : 0);
#endif
        playerShuttingDown = true;
        const bool ownsPlayback =
            activePlaybackSession.load() == playbackSessionId;
        if (ownsPlayback)
            activePlaybackSession.store(0);

        if (ownsPlayback && !this->twitchChannel.empty()) {
            auto resetPreferences = currentChatPreferences;
            resetPreferences.mode = twitch::ChatMode::Off;
            applyTwitchVideoLayout(resetPreferences);
        }

        if (ownsPlayback) {
            MPVCore::LIVE_STREAM_RECOVERY = false;
            resetPlayerVideoLayout();

            const char* configuredHwdec = MPVCore::HARDWARE_DEC
                ? MPVCore::PLAYER_HWDEC_METHOD.c_str()
                : "no";
            MPVCore::instance().command(
                "set",
                "hwdec",
                configuredHwdec);
        }

        if (playbackRecovery) {
            playbackRecovery->alive.store(false);
            playbackRecovery->recovering.store(false);
            playbackRecovery->retryScheduled.store(false);
            playbackRecovery->reconfigArmed.store(false);
            playbackRecovery->decoderSwitching.store(false);
            playbackRecovery->waitingForHardwareStop.store(false);
            playbackRecovery->hardwareStopGeneration.fetch_add(1);
            playbackRecovery->restoreGeneration.fetch_add(1);
        }
        view->setPlaybackRecoveryHandler({});

        if (chatSendAction != ACTION_NONE) {
            this->unregisterAction(chatSendAction);
            chatSendAction = ACTION_NONE;
        }

        if (chatModeAction != ACTION_NONE) {
            this->unregisterAction(chatModeAction);
            chatModeAction = ACTION_NONE;
        }

        if (chatUi) chatUi->alive.store(false);
        chatClient.reset();
        if (!this->twitchChannel.empty()) {
            twitch::chatPreferencesEvent()->unsubscribe(
                chatPreferenceSubscribeID);
            if (orientationSubscribeID) {
                twinx::portrait::OrientationController::instance()
                    .getOrientationChanged()->unsubscribe(*orientationSubscribeID);
                orientationSubscribeID.reset();
            }
        }

        if (portraitLayout) {
            brls::Application::setContentOrientation(
                brls::ContentOrientation::LANDSCAPE);
            portraitLayout = false;
        }

        auto& mpv = MPVCore::instance();
        mpv.getEvent()->unsubscribe(eventSubscribeID);
        view->getPlayEvent()->unsubscribe(playSubscribeID);
        if (channelProfileCancel)
            channelProfileCancel->store(true);
        if (channelEventSubscribed)
            view->getChannelEvent()->unsubscribe(
                channelSubscribeID);
        view->getSettingEvent()->unsubscribe(settingSubscribeID);
        if (this->twitchChannel.empty()) {
            mpv.command("write-watch-later-config");
        } else {
            brls::Logger::debug(
                "twiNX: skipped watch-later write for Twitch live playback");
        }
    }

    void willDisappear(bool resetState) override {
#ifdef ANDROID
        if (brls::Application::getThemeVariant() == brls::ThemeVariant::LIGHT)
            brls::Application::getTheme().addColor("brls/clear", nvgRGBA(235, 235, 235, 255));
        else
            brls::Application::getTheme().addColor("brls/clear", nvgRGBA(45, 45, 45, 255));
#endif
    }

    void willAppear(bool resetState) override {
#ifdef ANDROID
        brls::Application::getTheme().addColor("brls/clear", nvgRGBA(0, 0, 0, 0));
#endif
        if (!this->twitchChannel.empty())
            restorePlaybackFocus();
    }

    void setList(const DirList& list, size_t index, const std::string& extra) {
        // æ’­æ”¾åˆ—è¡¨
        DirList urls;
        for (size_t i = 1; i < list.size(); i++) {
            auto& it = list.at(i);
            if (it.type == remote::EntryType::VIDEO) {
                if (i == index) index = urls.size();
                titles.push_back(it.name);
                urls.push_back(it);
            }
        }
        if (titles.size() > 1) view->setList(titles, index);

        playSubscribeID = view->getPlayEvent()->subscribe([this, list, urls, extra](int index) {
            if (index < 0 || index >= (int)urls.size()) {
                return VideoView::close(true);
            }
            MPVCore::instance().reset();
            auto& item = urls.at(index);

            std::string name = item.name;
            auto pos = name.find_last_of(".");
            if (pos != std::string::npos) {
                name = name.substr(0, pos);
            }

            this->subtitles.clear();
            for (auto& s : list) {
                if (s.type == remote::EntryType::SUBTITLE) {
                    if (!s.name.rfind(name, 0)) {
                        this->subtitles.insert(std::make_pair(s.name.substr(pos), s.url()));
                    }
                }
            }
            this->url = item.url();
            MPVCore::instance().setUrl(this->url, extra);
            view->setTitie(name);
            return true;
        });

        view->getPlayEvent()->fire(index);
    }

    void setUrl(const std::string& path, const std::string& extra = "") {
        playSubscribeID = view->getPlayEvent()->subscribe([](int index) { return VideoView::close(true); });
 MPVCore::instance().setUrl(path, extra);
    }

    void loadList() {
        auto& mpv = MPVCore::instance();
        int64_t count = mpv.getInt("playlist-count");
        for (int64_t n = 0; n < count; n++) {
            auto key = fmt::format("playlist/{}/title", n);
            titles.push_back(mpv.getString(key));
        }
        if (titles.size() > 1) view->setList(titles, 0);
        view->setTitie(titles.front());

        playSubscribeID = view->getPlayEvent()->subscribe([this, &mpv](int index) {
            if (index < 0 || index >= (int)titles.size()) {
                return VideoView::close();
            }
            MPVCore::instance().reset();
            view->setTitie(titles.at(index));
            mpv.command("playlist-play-index", std::to_string(index).c_str());
            return true;
        });
    }

    void deactivatePlaybackForNavigation() {
        if (playerShuttingDown) return;
        playerShuttingDown = true;

        if (activePlaybackSession.load() == playbackSessionId)
            activePlaybackSession.store(0);

        MPVCore::LIVE_STREAM_RECOVERY = false;
        if (playbackRecovery) {
            playbackRecovery->alive.store(false);
            playbackRecovery->recovering.store(false);
            playbackRecovery->retryScheduled.store(false);
            playbackRecovery->reconfigArmed.store(false);
            playbackRecovery->decoderSwitching.store(false);
            playbackRecovery->restoreGeneration.fetch_add(1);
        }
        view->setPlaybackRecoveryHandler({});

        if (chatUi) chatUi->alive.store(false);
        stopChat();
        resetPlayerVideoLayout();

        auto& mpv = MPVCore::instance();
        mpv.stop();
        const char* configuredHwdec = MPVCore::HARDWARE_DEC
            ? MPVCore::PLAYER_HWDEC_METHOD.c_str()
            : "no";
        mpv.command("set", "hwdec", configuredHwdec);
    }

    void openChannelPage() {
        if (twitchChannel.empty() || navigatingToChannel) return;
        navigatingToChannel = true;

        const std::string channel = twitchChannel;
        deactivatePlaybackForNavigation();

        if (!brls::Application::popActivity(
                brls::TransitionAnimation::NONE)) {
            navigatingToChannel = false;
            return;
        }

        // Push on the next UI turn so the player activity and VideoView are
        // fully destroyed before the channel page becomes active.
        brls::sync([channel]() {
            brls::Application::pushActivity(
                new brls::Activity(
                    new TwitchChannelPage(channel)),
                brls::TransitionAnimation::NONE);
        });
    }

    void restorePlaybackFocus() {
        brls::sync([this]() {
            if (!playerShuttingDown &&
                activePlaybackSession.load() == playbackSessionId)
                brls::Application::giveFocus(view);
        });
    }

    void cycleChatMode() {
        auto preferences =
            twitch::loadChatPreferences();

        const char* description = "Off";
        switch (preferences.mode) {
        case twitch::ChatMode::Off:
            preferences.mode =
                twitch::ChatMode::Overlay;
            description = "Overlay";
            break;

        case twitch::ChatMode::Overlay:
            preferences.mode =
                twitch::ChatMode::RightPanel;
            description = "Right panel";
            break;

        case twitch::ChatMode::RightPanel:
        default:
            preferences.mode =
                twitch::ChatMode::Off;
            description = "Off";
            break;
        }

        if (!twitch::saveChatPreferences(
                preferences)) {
            brls::Application::notify(
                "twiNX: could not change chat layout");
            return;
        }

        brls::Application::notify(
            std::string("Chat layout: ") +
            description);
    }

    void updateChatInteractionAction(
        const twitch::ChatPreferences& preferences) {
        const bool interactive =
            preferences.participation ==
            twitch::ChatParticipation::Interactive;

        if (interactive &&
            chatSendAction == ACTION_NONE) {
            chatSendAction = this->registerAction(
                "Chat",
                brls::BUTTON_LT,
                [this](brls::View*) {
                    openChatComposer();
                    return true;
                },
                false);
        } else if (!interactive &&
            chatSendAction != ACTION_NONE) {
            this->unregisterAction(chatSendAction);
            chatSendAction = ACTION_NONE;
        }
    }

    void openChatComposer() {
        const auto preferences =
            twitch::loadChatPreferences();

        if (preferences.participation !=
            twitch::ChatParticipation::Interactive) {
            brls::Application::notify(
                "Enable Interactive under Chat participation first");
            return;
        }

        if (!twitch::hasChatWriteScope()) {
            brls::Application::notify(
                "Sign out and sign in again once to enable "
                "interactive chat");
            return;
        }

        if (preferences.composerMode ==
            twitch::ChatComposerMode::NintendoKeyboard) {
            auto* ime =
                brls::Application::getImeManager();
            if (!ime) {
                brls::Application::notify(
                    "The software keyboard is unavailable");
                return;
            }

            ime->openForText(
                [this](std::string message) {
                    restorePlaybackFocus();

                    if (message.empty()) return;

                    brls::Application::notify(
                        "twiNX: sending chat message…");

                    twitch::sendChatMessageAsync(
                        twitchChannel,
                        message,
                        [](const std::string&) {
                            brls::Application::notify(
                                "twiNX: chat message sent");
                        },
                        [](const std::string& error) {
                            brls::Application::notify(
                                "twiNX chat error: " + error);
                        });
                },
                "Twitch chat",
                "Send a message as your signed-in Twitch account",
                500,
                "");
            return;
        }

        brls::Application::pushActivity(
            new brls::Activity(
                new TwitchChatComposer(
                    twitchChannel)));
    }

    void stopChat() {
        chatClient.reset();
        if (chatUi) chatUi->setStatus("Chat disabled");
    }

    void startChat() {
        if (chatClient || twitchChannel.empty()) return;

        std::weak_ptr<ChatUiState> weak = chatUi;
        chatClient = std::make_unique<twitch::ChatClient>(
            twitchChannel,
            [weak](twitch::ChatMessage message) {
                brls::sync([weak, message = std::move(message)]() mutable {
                    auto state = weak.lock();
                    if (!state || !state->alive.load()) return;
                    state->append(std::move(message));
                });
            },
            [weak](const std::string& status) {
                brls::sync([weak, status]() {
                    auto state = weak.lock();
                    if (!state || !state->alive.load()) return;
                    state->setStatus(status);
                });
            });
    }

    void applyDisplayOrientation(
        twinx::portrait::DisplayOrientation orientation) {
        portraitOrientation = orientation;
        portraitLayout =
            orientation != twinx::portrait::DisplayOrientation::Landscape;

        brls::Application::setContentOrientation(
            orientation == twinx::portrait::DisplayOrientation::PortraitClockwise
                ? brls::ContentOrientation::PORTRAIT_CLOCKWISE
                : orientation == twinx::portrait::DisplayOrientation::PortraitCounterClockwise
                    ? brls::ContentOrientation::PORTRAIT_COUNTER_CLOCKWISE
                    : brls::ContentOrientation::LANDSCAPE);

        applyChatPreferences(currentChatPreferences);
    }

    void applyTwitchVideoLayout(
        const twitch::ChatPreferences& preferences) {
        const twitch::ChatMode mode = preferences.mode;
        auto& mpv = MPVCore::instance();

        if (portraitLayout) {
            const float occupied = std::clamp(
                portraitVideoHeight /
                    std::max(1.0f, brls::Application::contentHeight),
                0.0f,
                1.0f);
            const float unused = 1.0f - occupied;
            const bool clockwise =
                portraitOrientation ==
                twinx::portrait::DisplayOrientation::PortraitClockwise;

            const std::string left =
                fmt::format("{:.6f}", clockwise ? unused : 0.0f);
            const std::string right =
                fmt::format("{:.6f}", clockwise ? 0.0f : unused);

            mpv.command("set", "video-margin-ratio-left", left.c_str());
            mpv.command("set", "video-margin-ratio-right", right.c_str());
            mpv.command("set", "video-margin-ratio-top", "0.0");
            mpv.command("set", "video-margin-ratio-bottom", "0.0");
            mpv.command("set", "video-rotate", clockwise ? "90" : "270");
            mpv.command("set", "keepaspect", "yes");
            mpv.command("set", "keepaspect-window", "no");
            mpv.command("set", "video-aspect-override", "no");
            mpv.command("set", "panscan", "0.0");
            mpv.command("set", "video-zoom", "0.0");
            mpv.command("set", "video-pan-x", "0.0");
            mpv.command("set", "video-pan-y", "0.0");
            mpv.command("set", "video-align-x", "0");
            mpv.command("set", "video-align-y", "0");
            mpv.command("set", "video-recenter", "yes");
            return;
        }

        mpv.command("set", "video-rotate", configuredVideoRotation());

        // MPV renders directly into the full Switch framebuffer. Making the
        // Borealis VideoView narrower changes the OSD bounds, but does not by
        // itself constrain MPV's video rectangle. Reserve the docked chat area
        // through MPV's own window-relative video margin.
        const float contentWidth = brls::Application::contentWidth;
        const float rightMargin =
            mode == twitch::ChatMode::RightPanel && contentWidth > 0.0f
                ? static_cast<float>(preferences.dockedWidth) /
                    contentWidth
                : 0.0f;

        const std::string rightMarginValue =
            fmt::format("{:.6f}", rightMargin);

        mpv.command(
            "set",
            "video-margin-ratio-left",
            "0.0");
        mpv.command(
            "set",
            "video-margin-ratio-right",
            rightMarginValue.c_str());
        mpv.command(
            "set",
            "video-margin-ratio-top",
            "0.0");
        mpv.command(
            "set",
            "video-margin-ratio-bottom",
            "0.0");

        mpv.command("set", "video-zoom", "0.0");
        mpv.command("set", "video-pan-x", "0.0");
        mpv.command("set", "video-pan-y", "0.0");
        mpv.command("set", "video-align-y", "0");
        mpv.command("set", "video-recenter", "yes");

        if (mode == twitch::ChatMode::RightPanel) {
            // Fit the complete source image into the remaining area. Any
            // aspect-ratio mismatch becomes black letterboxing rather than
            // cropping the stream behind the chat panel.
            mpv.command("set", "keepaspect", "yes");
            mpv.command("set", "keepaspect-window", "no");
            mpv.command("set", "video-aspect-override", "no");
            mpv.command("set", "panscan", "0.0");
            mpv.command("set", "video-align-x", "-1");

            brls::Logger::info(
                "twiNX: docked video fit enabled, right margin ratio {}",
                rightMarginValue);
        } else {
            // Restore the user's normal player aspect choice when leaving the
            // dedicated docked layout.
            mpv.command("set", "video-align-x", "0");
            mpv.setAspect(MPVCore::VIDEO_ASPECT);

            brls::Logger::info(
                "twiNX: docked video fit disabled");
        }
    }

    void applyChatPreferences(const twitch::ChatPreferences& preferences) {
        if (playerShuttingDown ||
            activePlaybackSession.load() != playbackSessionId ||
            twitchChannel.empty() ||
            !chatOverlayPanel ||
            !chatDockedPanel)
            return;

        currentChatMode = preferences.mode;
        currentChatPreferences = preferences;

        chatOverlayPanel->apply(preferences);
        chatDockedPanel->apply(preferences);
        updateChatInteractionAction(preferences);

        const float width = brls::Application::contentWidth;
        const float height = brls::Application::contentHeight;

        if (portraitLayout) {
            this->setAxis(brls::Axis::COLUMN);
            this->setDimensions(width, height);
            portraitVideoHeight = std::min(width * 9.0f / 16.0f, height * 0.42f);
            view->setGrow(0.0f);
            view->setDimensions(width, portraitVideoHeight);
            chatOverlayPanel->setVisibility(brls::Visibility::GONE);
            chatDockedPanel->setVisibility(brls::Visibility::VISIBLE);
            chatDockedPanel->setPortraitFrame(
                width,
                std::max(240.0f, height - portraitVideoHeight));
            startChat();
            applyTwitchVideoLayout(preferences);
            this->invalidate();
            return;
        }

        this->setAxis(brls::Axis::ROW);
        this->setDimensions(width, height);
        view->setGrow(1.0f);
        applyTwitchVideoLayout(preferences);

        switch (preferences.mode) {
        case twitch::ChatMode::Off:
            chatOverlayPanel->setVisibility(brls::Visibility::GONE);
            chatDockedPanel->setVisibility(brls::Visibility::GONE);
            view->setDimensions(width, height);
            stopChat();
            break;

        case twitch::ChatMode::RightPanel:
            // TV layout: the complete video is scaled into the remaining
            // width, and chat occupies its own column rather than covering it.
            chatOverlayPanel->setVisibility(brls::Visibility::GONE);
            chatDockedPanel->setVisibility(brls::Visibility::VISIBLE);
            view->setDimensions(
                std::max(
                    640.0f,
                    width -
                        static_cast<float>(
                            preferences.dockedWidth)),
                height);
            startChat();
            break;

        case twitch::ChatMode::Overlay:
            chatDockedPanel->setVisibility(brls::Visibility::GONE);
            chatOverlayPanel->setVisibility(brls::Visibility::VISIBLE);
            view->setDimensions(width, height);
            startChat();
            break;
        }

        this->invalidate();
    }

private:
    VideoView* view = new VideoView();
    std::string url;
    std::string twitchChannel;
    std::vector<std::string> titles;
    std::unordered_map<std::string, std::string> subtitles;
    MPVEvent::Subscription eventSubscribeID;
    brls::Event<int>::Subscription playSubscribeID;
    brls::VoidEvent::Subscription settingSubscribeID;
    brls::VoidEvent::Subscription channelSubscribeID;
    bool channelEventSubscribed = false;
    HTTP::Cancel channelProfileCancel;
    twitch::UserProfile channelProfile;
    brls::Event<twitch::ChatPreferences>::Subscription chatPreferenceSubscribeID;
    std::optional<brls::Event<twinx::portrait::DisplayOrientation>::Subscription>
        orientationSubscribeID;
    TwitchChatPanel* chatOverlayPanel = nullptr;
    TwitchChatPanel* chatDockedPanel = nullptr;
    std::shared_ptr<ChatUiState> chatUi;
    std::shared_ptr<TwitchPlaybackRecoveryState> playbackRecovery;
    std::unique_ptr<twitch::ChatClient> chatClient;
    twitch::ChatMode currentChatMode = twitch::ChatMode::Off;
    twitch::ChatPreferences currentChatPreferences;
    brls::ActionIdentifier chatSendAction = ACTION_NONE;
    brls::ActionIdentifier chatModeAction = ACTION_NONE;
    uint64_t playbackSessionId = 0;
    bool portraitLayout = false;
    float portraitVideoHeight = 405.0f;
    twinx::portrait::DisplayOrientation portraitOrientation =
        twinx::portrait::DisplayOrientation::Landscape;
    bool playerShuttingDown = false;
    bool navigatingToChannel = false;
};

class FileCard : public RecyclingGridItem {
public:
    FileCard() { this->inflateFromXMLRes("xml/view/dir_entry.xml"); }

    void setCard(const remote::DirEntry& item) {
        if (item.type == remote::EntryType::UP) {
            this->icon->setImageFromSVGRes("icon/ico-folder-up.svg");
            this->name->setText("main/remote/up"_i18n);
            this->size->setText("");
            return;
        }
        this->name->setText(item.name);
        if (item.type == remote::EntryType::DIR) {
            this->icon->setImageFromSVGRes("icon/ico-folder.svg");
            this->size->setText("main/remote/folder"_i18n);
            return;
        }
        if (item.type == remote::EntryType::DEVICE) {
            this->icon->setImageFromSVGRes("icon/ico-folder.svg");
            this->size->setText(item.path);
            return;
        }
        this->size->setText(misc::formatSize(item.fileSize));
        switch (item.type) {
        case remote::EntryType::VIDEO:
            this->icon->setImageFromSVGRes("icon/ico-file-video.svg");
            break;
        case remote::EntryType::AUDIO:
            this->icon->setImageFromSVGRes("icon/ico-file-audio.svg");
            break;
        case remote::EntryType::IMAGE:
            this->icon->setImageFromSVGRes("icon/ico-file-image.svg");
            break;
        case remote::EntryType::PLAYLIST:
            this->icon->setImageFromSVGRes("icon/ico-list.svg");
            break;
        default:
            this->icon->setImageFromSVGRes("icon/ico-file.svg");
        }
    }

private:
    BRLS_BIND(SVGImage, icon, "file/icon");
    BRLS_BIND(brls::Label, name, "file/name");
    BRLS_BIND(brls::Label, size, "file/misc");
};

static std::set<std::string> videoExt = {
    ".mp4", ".mkv", ".avi", ".flv", ".mov", ".wmv", ".webm", ".rm", ".rmvb", ".mpg"};
static std::set<std::string> audioExt = {".mp3", ".flac", ".wav", ".ogg", ".m4a", ".aac", ".wma", ".ape"};
static std::set<std::string> imageExt = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp"};
static std::set<std::string> playlistExt = {".m3u", ".m3u8"};
static std::set<std::string> subtitleExt = {".srt", ".ass", ".ssa", ".sub", ".smi"};

class FileDataSource : public RecyclingGridDataSource {
public:
    FileDataSource(const DirList& r, RemoteView::Client c) : list(std::move(r)), client(c) {
        for (auto& it : this->list) {
            if (it.type != remote::EntryType::FILE) continue;

            auto pos = it.name.find_last_of('.');
            if (pos == std::string::npos) continue;
            std::string ext = it.name.substr(pos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (videoExt.count(ext)) {
                it.type = remote::EntryType::VIDEO;
            } else if (audioExt.count(ext)) {
                it.type = remote::EntryType::AUDIO;
            } else if (imageExt.count(ext)) {
                it.type = remote::EntryType::IMAGE;
            } else if (subtitleExt.count(ext)) {
                it.type = remote::EntryType::SUBTITLE;
            } else if (playlistExt.count(ext)) {
                it.type = remote::EntryType::PLAYLIST;
            }
        }
    }

    size_t getItemCount() override { return this->list.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        FileCard* cell = dynamic_cast<FileCard*>(recycler->dequeueReusableCell("Cell"));
        auto& item = this->list.at(index);
        cell->setCard(item);
        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto& item = this->list.at(index);
        if (item.type == remote::EntryType::UP) {
            recycler->getParent()->dismiss();
            return;
        }

        if (item.type == remote::EntryType::DIR || item.type == remote::EntryType::DEVICE) {
            auto* view = dynamic_cast<RemoteView*>(recycler->getParent());
            if (view) view->push(item.path);
            return;
        }

        if (item.type == remote::EntryType::VIDEO) {
            RemotePlayer* view = new RemotePlayer(item);
            view->setList(this->list, index, client->extraOption());
            brls::Application::pushActivity(new brls::Activity(view), brls::TransitionAnimation::NONE);
            return;
        }

        if (item.type == remote::EntryType::AUDIO) {
            DirList urls;
            for (size_t i = 1; i < this->list.size(); i++) {
                auto& it = this->list.at(i);
                if (it.type == remote::EntryType::AUDIO) {
                    if (i == index) index = urls.size();
                    urls.push_back(it);
                }
            }
            MusicView::instance().load(urls, index, client->extraOption());
            return;
        }

        if (item.type == remote::EntryType::IMAGE) {
            return;
        }

        if (item.type == remote::EntryType::PLAYLIST) {
            RemotePlayer* view = new RemotePlayer(item);
            MPVCore::instance().setUrl(item.url(), client->extraOption());
            brls::Application::pushActivity(new brls::Activity(view), brls::TransitionAnimation::NONE);
        }
    }

    void clearData() override { this->list.clear(); }

private:
    DirList list;
    RemoteView::Client client;
};

UmsView::UmsView() : RemoteView(std::make_shared<remote::Local>()) {
    RecyclingGrid* view = this->newRecycler();
    this->stack.push_back(view);
    this->setContent(view);

    auto ev = Ums::instance().getEvent();
    deviceSubscribeID = ev->subscribe([this, view](const Ums::DeviceList& r) {
        DirList dirs;
        dirs.reserve(r.size());
        for (auto& it : r) {
            remote::DirEntry entry;
            entry.type = it.id < 0 ? remote::EntryType::DIR : remote::EntryType::DEVICE;
            entry.name = it.name;
            entry.path = it.mount + "/";
            dirs.push_back(entry);
        }
        view->setDataSource(new FileDataSource(dirs, this->client));
    });
    ev->fire(Ums::instance().getDevice());
}

UmsView::~UmsView() { Ums::instance().getEvent()->unsubscribe(deviceSubscribeID); }

RemoteView::RemoteView(Client c) : client(c) { brls::Logger::debug("RemoteView: create"); }

RemoteView::~RemoteView() {
    brls::Logger::debug("RemoteView: deleted");
    this->setDimensions(View::AUTO, View::AUTO);
    PlayerSetting::selectedSubtitle = 0;
    PlayerSetting::selectedAudio = 0;

    /// é€šçŸ¥ MusicView å·²å…³é—­
    MusicView::instance().setParent(nullptr);
}

brls::View* RemoteView::getDefaultFocus() { return this->recycler; }

void RemoteView::push(const std::string& path) {
    RecyclingGrid* view = this->newRecycler();
    this->stack.push_back(view);
    this->setContent(view);

    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN, &path]() {
        try {
            auto r = client->list(path);
            brls::sync([ASYNC_TOKEN, r]() {
                ASYNC_RELEASE
                this->recycler->setDataSource(new FileDataSource(r, client));
                if (this->stack.size() > 1) brls::Application::giveFocus(this->recycler);
            });
        } catch (const std::exception& ex) {
            std::string error = ex.what();
            brls::sync([ASYNC_TOKEN, error]() {
                ASYNC_RELEASE
                this->recycler->setError(error);
            });
        }
    });
}

void RemoteView::dismiss(std::function<void(void)> cb) {
    if (this->stack.size() > 1) {
        brls::View* lastView = this->recycler;
        this->stack.pop_back();
        this->setContent(this->stack.back());
        cb();
        lastView->freeView();
    } else if (brls::Application::getInputType() == brls::InputType::TOUCH) {
        brls::View::dismiss(cb);
    } else {
        AutoTabFrame::focus2Sidebar(this);
    }
}

void RemoteView::setContent(RecyclingGrid* view) {
    if (this->recycler) {
        this->removeView(this->recycler, false);
        this->recycler = nullptr;
    }

    this->recycler = view;
    this->recycler->setDimensions(View::AUTO, View::AUTO);
    this->recycler->setGrow(1.0f);
    this->addView(this->recycler);
    brls::Application::giveFocus(this->recycler);
}

RecyclingGrid* RemoteView::newRecycler() {
    RecyclingGrid* view = new RecyclingGrid();
    view->spanCount = 1;
    view->estimatedRowHeight = 48;
    view->estimatedRowSpace = 10;
    view->setDefaultCellFocus(1);
    view->registerCell("Cell", []() { return new FileCard(); });
    view->registerAction("hints/back"_i18n, brls::BUTTON_B, [this](...) {
        this->dismiss();
        return true;
    });
    return view;
}

void RemoteView::play(const std::string& path, const std::string& name, const std::string& extra, const std::string& twitchChannel) {
    // Every playback launch starts from a clean global MPV presentation. This
    // prevents a live chat margin or recovery session leaking into VODs/clips.
    MPVCore::LIVE_STREAM_RECOVERY = false;
    MPVCore::instance().stop();
    resetPlayerVideoLayout();

    RemotePlayer* view = new RemotePlayer({remote::EntryType::VIDEO, name, path}, twitchChannel);
    brls::Application::pushActivity(new brls::Activity(view), brls::TransitionAnimation::NONE);
    view->setUrl(path, extra);
}
