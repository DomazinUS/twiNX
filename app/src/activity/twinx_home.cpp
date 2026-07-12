#include "activity/twinx_home.hpp"
#include "activity/twitch_channel.hpp"
#include "api/twitch.hpp"
#include "api/twitch_auth.hpp"
#include "api/twitch_helix.hpp"
#include "tab/remote_view.hpp"
#include "utils/image.hpp"
#include "utils/config.hpp"
#include "view/h_recycling.hpp"
#include "view/mpv_core.hpp"

#include <borealis/core/cache_helper.hpp>

#include <algorithm>
#include <cctype>
#include <functional>
#include <fmt/format.h>
#include <memory>
#include <utility>
#include <vector>

namespace {

class AboutScrollingFrame : public brls::ScrollingFrame {
public:
    void setFocusProxy(brls::View* view) { focusProxy = view; }

    void draw(
        NVGcontext* vg,
        float x,
        float y,
        float width,
        float height,
        brls::Style style,
        brls::FrameContext* ctx) override {
        // ScrollingFrame normally reads the right stick only while it or one
        // of its descendants owns focus. The About page keeps focus on the
        // fixed Back button, which sits outside the scroll frame. Bridge that
        // one local focus state during drawing so the stock right-stick
        // scrolling behavior remains available without changing global/home
        // navigation.
        const bool proxyIsFocused =
            focusProxy && brls::Application::getCurrentFocus() == focusProxy;
        const bool previousChildFocused = childFocused;
        if (proxyIsFocused) childFocused = true;

        brls::ScrollingFrame::draw(vg, x, y, width, height, style, ctx);

        childFocused = previousChildFocused;
    }

private:
    brls::View* focusProxy = nullptr;
};

class TwiNXActionButton : public brls::Box {
public:
    brls::View* getNextFocus(
        brls::FocusDirection direction,
        brls::View* currentView) override {
        if (direction == brls::FocusDirection::DOWN && downTarget) {
            if (auto* target = downTarget()) return target;
        }
        return brls::Box::getNextFocus(direction, currentView);
    }

    std::function<brls::View*()> downTarget;
};

class TwiNXProfileAvatar : public brls::View {
public:
    TwiNXProfileAvatar() {
        loader = new brls::Image();
        loader->setFreeTexture(false);
        this->setFocusable(false);
        this->setVisibility(brls::Visibility::GONE);
    }

    ~TwiNXProfileAvatar() override {
        Image::cancel(loader);
        delete loader;
    }

    void setProfileImage(const std::string& url) {
        if (url == currentUrl) {
            this->setVisibility(
                url.empty()
                    ? brls::Visibility::GONE
                    : brls::Visibility::VISIBLE);
            return;
        }

        currentUrl = url;
        Image::cancel(loader);

        if (currentUrl.empty()) {
            this->setVisibility(brls::Visibility::GONE);
            return;
        }

        this->setVisibility(brls::Visibility::VISIBLE);
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
        const float size = std::min(width, height);
        const float centerX = x + width * 0.5f;
        const float centerY = y + height * 0.5f;
        const float radius = size * 0.5f - 2.0f;

        nvgSave(vg);

        // Neutral Twitch-toned placeholder while the image is downloading.
        nvgBeginPath(vg);
        nvgCircle(vg, centerX, centerY, radius);
        nvgFillColor(vg, nvgRGB(91, 46, 145));
        nvgFill(vg);

        if (loader && loader->getTexture() > 0) {
            const float imageX = centerX - radius;
            const float imageY = centerY - radius;
            const float imageSize = radius * 2.0f;

            NVGpaint paint = nvgImagePattern(
                vg,
                imageX,
                imageY,
                imageSize,
                imageSize,
                0,
                loader->getTexture(),
                1.0f);

            nvgBeginPath(vg);
            nvgCircle(vg, centerX, centerY, radius);
            nvgFillPaint(vg, paint);
            nvgFill(vg);
        }

        nvgBeginPath(vg);
        nvgCircle(vg, centerX, centerY, radius);
        nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 90));
        nvgStrokeWidth(vg, 2.0f);
        nvgStroke(vg);

        nvgRestore(vg);
    }

private:
    brls::Image* loader = nullptr;
    std::string currentUrl;
};

brls::Box* makeButton(const std::string& text, brls::Label** outLabel = nullptr) {
    auto* button = new TwiNXActionButton();
    button->setFocusable(true);
    button->setAxis(brls::Axis::ROW);
    button->setPadding(12, 22, 12, 22);
    button->setCornerRadius(8);
    button->setBackgroundColor(nvgRGB(91, 46, 145));
    button->setMarginRight(12);
    button->addGestureRecognizer(new brls::TapGestureRecognizer(button));

    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(19);
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

std::string streamKey(const twitch::Stream& item) {
    if (!item.userLogin.empty()) return item.userLogin;
    if (!item.userId.empty()) return item.userId;
    return item.id;
}

std::string categoryKey(const twitch::Category& item) {
    return item.id.empty() ? item.name : item.id;
}

std::string joinErrors(const twitch::HomeData& data) {
    std::vector<std::string> errors;
    if (!data.followedError.empty()) errors.push_back("live followed channels");
    if (!data.offlineFollowedError.empty()) errors.push_back("offline followed channels");
    if (!data.popularError.empty()) errors.push_back("popular streams");
    if (!data.categoriesError.empty()) errors.push_back("categories");

    std::string result;
    for (size_t index = 0; index < errors.size(); ++index) {
        if (index) result += index + 1 == errors.size() ? " and " : ", ";
        result += errors[index];
    }
    return result;
}

class StreamCell : public RecyclingGridItem {
public:
    StreamCell() {
        this->setAxis(brls::Axis::COLUMN);
        this->setFocusable(true);
        this->setCornerRadius(8);
        this->setBackgroundColor(nvgRGB(32, 35, 45));
        this->setPadding(0, 0, 10, 0);

        picture = new brls::Image();
        picture->setHeight(142);
        picture->setWidthPercentage(100);
        picture->setImageFromRes("img/video-card-bg.png");
        this->addView(picture);

        title = new brls::Label();
        title->setFontSize(18);
        title->setSingleLine(true);
        title->setMarginTop(8);
        title->setMarginLeft(10);
        title->setMarginRight(10);
        title->setTextColor(nvgRGB(245, 246, 250));
        this->addView(title);

        subtitle = new brls::Label();
        subtitle->setFontSize(15);
        subtitle->setSingleLine(true);
        subtitle->setMarginTop(3);
        subtitle->setMarginLeft(10);
        subtitle->setMarginRight(10);
        subtitle->setTextColor(nvgRGB(170, 177, 193));
        this->addView(subtitle);

        this->getFocusEvent()->subscribe([this](brls::View*) {
            if (onFocused) onFocused();
        });
    }

    brls::View* getNextFocus(
        brls::FocusDirection direction,
        brls::View* currentView) override {
        if (direction == brls::FocusDirection::UP && upTarget) {
            if (auto* target = upTarget()) return target;
        }
        return RecyclingGridItem::getNextFocus(direction, currentView);
    }

    ~StreamCell() override { Image::cancel(picture); }

    void prepareForReuse() override {
        Image::cancel(picture);
        picture->setImageFromRes("img/video-card-bg.png");
        title->setText("");
        subtitle->setText("");
        onFocused = nullptr;
        upTarget = nullptr;
    }

    void cacheForReuse() override {
        Image::cancel(picture);
        onFocused = nullptr;
        upTarget = nullptr;
    }

    brls::Image* picture = nullptr;
    brls::Label* title = nullptr;
    brls::Label* subtitle = nullptr;
    std::function<void()> onFocused;
    std::function<brls::View*()> upTarget;
};

class CategoryCell : public RecyclingGridItem {
public:
    CategoryCell() {
        this->setAxis(brls::Axis::COLUMN);
        this->setFocusable(true);
        this->setCornerRadius(8);
        this->setBackgroundColor(nvgRGB(32, 35, 45));
        this->setPadding(0, 0, 10, 0);

        picture = new brls::Image();
        picture->setHeight(205);
        picture->setWidthPercentage(100);
        picture->setImageFromRes("img/video-card-bg.png");
        this->addView(picture);

        title = new brls::Label();
        title->setFontSize(17);
        title->setSingleLine(true);
        title->setMarginTop(8);
        title->setMarginLeft(8);
        title->setMarginRight(8);
        title->setTextColor(nvgRGB(245, 246, 250));
        this->addView(title);

        this->getFocusEvent()->subscribe([this](brls::View*) {
            if (onFocused) onFocused();
        });
    }

    brls::View* getNextFocus(
        brls::FocusDirection direction,
        brls::View* currentView) override {
        if (direction == brls::FocusDirection::UP && upTarget) {
            if (auto* target = upTarget()) return target;
        }
        return RecyclingGridItem::getNextFocus(direction, currentView);
    }

    ~CategoryCell() override { Image::cancel(picture); }

    void prepareForReuse() override {
        Image::cancel(picture);
        picture->setImageFromRes("img/video-card-bg.png");
        title->setText("");
        onFocused = nullptr;
        upTarget = nullptr;
    }

    void cacheForReuse() override {
        Image::cancel(picture);
        onFocused = nullptr;
        upTarget = nullptr;
    }

    brls::Image* picture = nullptr;
    brls::Label* title = nullptr;
    std::function<void()> onFocused;
    std::function<brls::View*()> upTarget;
};

class StreamDataSource : public RecyclingGridDataSource {
public:
    using Selection = std::function<void(const twitch::Stream&)>;
    using Focus = std::function<void(const twitch::Stream&, size_t)>;

    StreamDataSource(
        std::vector<twitch::Stream> data,
        Selection selected,
        Focus focused,
        std::function<brls::View*()> up)
        : items(std::move(data)),
          onSelected(std::move(selected)),
          onFocused(std::move(focused)),
          upTarget(up) {}

    size_t getItemCount() override { return items.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        auto* cell = dynamic_cast<StreamCell*>(recycler->dequeueReusableCell("Stream"));
        const auto& item = items.at(index);
        cell->upTarget = upTarget;
        cell->title->setText(item.userName.empty() ? item.userLogin : item.userName);
        std::string detail = item.gameName;
        if (item.isLive) {
            if (!detail.empty()) detail += " · ";
            detail += viewersText(item.viewerCount);
        } else {
            if (!detail.empty()) detail += " · ";
            detail += "Offline";
        }
        cell->subtitle->setText(detail);
        if (!item.thumbnailUrl.empty()) {
            Image::with(cell->picture, twitch::streamThumbnail(item.thumbnailUrl));
        }

        const twitch::Stream focusedItem = item;
        const Focus focusedCallback = onFocused;
        cell->onFocused = [focusedItem, index, focusedCallback]() {
            if (focusedCallback) focusedCallback(focusedItem, index);
        };
        return cell;
    }

    void onItemSelected(brls::Box*, size_t index) override {
        if (onSelected) onSelected(items.at(index));
    }

    void clearData() override { items.clear(); }

    size_t append(std::vector<twitch::Stream> incoming) {
        size_t added = 0;
        for (auto& item : incoming) {
            const std::string key = streamKey(item);
            const auto duplicate = std::find_if(items.begin(), items.end(), [&](const twitch::Stream& current) {
                return streamKey(current) == key;
            });
            if (duplicate != items.end()) continue;
            items.push_back(std::move(item));
            ++added;
        }
        return added;
    }

    size_t find(const std::string& key) const {
        for (size_t index = 0; index < items.size(); ++index) {
            if (streamKey(items[index]) == key) return index;
        }
        return items.size();
    }

private:
    std::vector<twitch::Stream> items;
    Selection onSelected;
    Focus onFocused;
    std::function<brls::View*()> upTarget;
};

class CategoryDataSource : public RecyclingGridDataSource {
public:
    using Selection = std::function<void(const twitch::Category&)>;
    using Focus = std::function<void(const twitch::Category&, size_t)>;

    CategoryDataSource(
        std::vector<twitch::Category> data,
        Selection selected,
        Focus focused,
        std::function<brls::View*()> up)
        : items(std::move(data)),
          onSelected(std::move(selected)),
          onFocused(std::move(focused)),
          upTarget(up) {}

    size_t getItemCount() override { return items.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        auto* cell = dynamic_cast<CategoryCell*>(recycler->dequeueReusableCell("Category"));
        const auto& item = items.at(index);
        cell->upTarget = upTarget;
        cell->title->setText(item.name);
        if (!item.boxArtUrl.empty()) {
            Image::with(cell->picture, twitch::categoryThumbnail(item.boxArtUrl));
        }

        const twitch::Category focusedItem = item;
        const Focus focusedCallback = onFocused;
        cell->onFocused = [focusedItem, index, focusedCallback]() {
            if (focusedCallback) focusedCallback(focusedItem, index);
        };
        return cell;
    }

    void onItemSelected(brls::Box*, size_t index) override {
        if (onSelected) onSelected(items.at(index));
    }

    void clearData() override { items.clear(); }

    size_t append(std::vector<twitch::Category> incoming) {
        size_t added = 0;
        for (auto& item : incoming) {
            const std::string key = categoryKey(item);
            const auto duplicate = std::find_if(items.begin(), items.end(), [&](const twitch::Category& current) {
                return categoryKey(current) == key;
            });
            if (duplicate != items.end()) continue;
            items.push_back(std::move(item));
            ++added;
        }
        return added;
    }

    size_t find(const std::string& key) const {
        for (size_t index = 0; index < items.size(); ++index) {
            if (categoryKey(items[index]) == key) return index;
        }
        return items.size();
    }

private:
    std::vector<twitch::Category> items;
    Selection onSelected;
    Focus onFocused;
    std::function<brls::View*()> upTarget;
};

}  // namespace

class TwitchRow : public brls::Box {
public:
    enum class Kind { Streams, Categories };
    using FocusCallback = std::function<void(const std::string&, const std::string&)>;
    using StreamFocusCallback = std::function<void(const twitch::Stream&)>;

    TwitchRow(std::string identifier, const std::string& text, Kind rowKind, FocusCallback focus)
        : id(std::move(identifier)), kind(rowKind), focusCallback(std::move(focus)) {
        this->setAxis(brls::Axis::COLUMN);
        this->setMarginBottom(18);

        header = new brls::Header();
        header->setTitle(text);
        header->setHeight(38);
        this->addView(header);

        recycler = new HRecyclerFrame();
        recycler->setHeight(kind == Kind::Streams ? 225 : 285);
        recycler->estimatedRowWidth = kind == Kind::Streams ? 315 : 175;
        recycler->estimatedRowSpace = 18;
        recycler->setPadding(0, 4, 0, 4);
        if (kind == Kind::Streams) {
            recycler->registerCell("Stream", []() { return new StreamCell(); });
        } else {
            recycler->registerCell("Category", []() { return new CategoryCell(); });
        }
        this->addView(recycler);

        emptyBox = new brls::Box();
        emptyBox->setAxis(brls::Axis::ROW);
        emptyBox->setHeight(70);
        emptyBox->setPadding(14, 18, 14, 18);
        emptyBox->setCornerRadius(8);
        emptyBox->setBackgroundColor(nvgRGB(28, 31, 40));
        emptyLabel = new brls::Label();
        emptyLabel->setFontSize(17);
        emptyLabel->setTextColor(nvgRGB(170, 177, 193));
        emptyBox->addView(emptyLabel);
        emptyBox->setVisibility(brls::Visibility::GONE);
        this->addView(emptyBox);

        this->setVisibility(brls::Visibility::GONE);
    }

    void setTitle(const std::string& text) { header->setTitle(text); }
    void onStreamFocused(StreamFocusCallback callback) { streamFocusCallback = std::move(callback); }

    void setStreams(
        std::vector<twitch::Stream> items,
        std::function<void(const twitch::Stream&)> selected,
        const std::string& emptyText,
        std::function<brls::View*()> upTarget = {}) {
        count = items.size();
        streamSource = new StreamDataSource(std::move(items), std::move(selected),
            [this](const twitch::Stream& stream, size_t) {
                if (focusCallback) focusCallback(id, streamKey(stream));
                if (streamFocusCallback) streamFocusCallback(stream);
            }, upTarget);
        categorySource = nullptr;
        recycler->setDataSource(streamSource);
        showResult(count > 0, emptyText);
    }

    void setCategories(
        std::vector<twitch::Category> items,
        std::function<void(const twitch::Category&)> selected,
        const std::string& emptyText,
        std::function<brls::View*()> upTarget = {}) {
        count = items.size();
        categorySource = new CategoryDataSource(std::move(items), std::move(selected),
            [this](const twitch::Category& category, size_t) {
                if (focusCallback) focusCallback(id, categoryKey(category));
            }, upTarget);
        streamSource = nullptr;
        recycler->setDataSource(categorySource);
        showResult(count > 0, emptyText);
    }

    size_t appendStreams(std::vector<twitch::Stream> items) {
        if (!streamSource) return 0;
        const size_t added = streamSource->append(std::move(items));
        count += added;
        if (added) recycler->notifyDataChanged();
        updateSubtitle();
        return added;
    }

    size_t appendCategories(std::vector<twitch::Category> items) {
        if (!categorySource) return 0;
        const size_t added = categorySource->append(std::move(items));
        count += added;
        if (added) recycler->notifyDataChanged();
        updateSubtitle();
        return added;
    }

    void setHasMore(bool value) {
        hasMore = value;
        updateSubtitle();
    }

    void onNextPage(bool enabled, const std::function<void()>& callback) {
        recycler->onNextPage(enabled ? callback : std::function<void()>{});
    }

    void clear() {
        this->setVisibility(brls::Visibility::GONE);
        recycler->onNextPage(std::function<void()>{});
        recycler->clearData();
        streamSource = nullptr;
        categorySource = nullptr;
        count = 0;
        hasMore = false;
    }

    brls::View* defaultFocus() { return recycler->getDefaultFocus(); }

    bool focusKey(const std::string& key) {
        size_t index = count;
        if (streamSource) index = streamSource->find(key);
        if (categorySource) index = categorySource->find(key);
        if (index >= count) return false;
        recycler->selectRowAt(index, false);
        if (auto* focus = recycler->getDefaultFocus()) {
            brls::Application::giveFocus(focus);
            return true;
        }
        return false;
    }

private:
    void showResult(bool hasItems, const std::string& emptyText) {
        this->setVisibility(brls::Visibility::VISIBLE);
        recycler->setVisibility(hasItems ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        emptyLabel->setText(emptyText);
        emptyBox->setVisibility(hasItems ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        updateSubtitle();
    }

    void updateSubtitle() {
        header->setSubtitle(std::to_string(count) + (hasMore ? "+" : ""));
    }

    std::string id;
    Kind kind;
    FocusCallback focusCallback;
    StreamFocusCallback streamFocusCallback;
    brls::Header* header = nullptr;
    HRecyclerFrame* recycler = nullptr;
    brls::Box* emptyBox = nullptr;
    brls::Label* emptyLabel = nullptr;
    StreamDataSource* streamSource = nullptr;
    CategoryDataSource* categorySource = nullptr;
    size_t count = 0;
    bool hasMore = false;
};

TwiNXHome::TwiNXHome() {
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(nvgRGB(16, 18, 24));

    // Borealis already uses an LRU texture cache. Tighten its absolute runtime
    // cap for this thumbnail-heavy client so stale channel/category artwork is
    // evicted instead of accumulating throughout a long browsing session.
    brls::LRUCache<std::string, size_t>::DEFAULT_CAPACITY = 0;
    brls::TextureCache::instance().cache.setCapacity(192);

    auto* top = new brls::Box();
    top->setAxis(brls::Axis::COLUMN);
    top->setPadding(24, 48, 12, 48);
    this->addView(top);

    auto* headingRow = new brls::Box();
    headingRow->setAxis(brls::Axis::ROW);
    headingRow->setAlignItems(brls::AlignItems::FLEX_START);
    headingRow->setWidthPercentage(100);
    top->addView(headingRow);

    auto* headingText = new brls::Box();
    headingText->setAxis(brls::Axis::COLUMN);
    headingText->setGrow(1.0f);
    headingRow->addView(headingText);

    auto* title = new brls::Label();
    title->setText("twiNX · Twitch for Nintendo Switch");
    title->setFontSize(33);
    title->setTextColor(nvgRGB(240, 242, 248));
    headingText->addView(title);

    accountLabel = new brls::Label();
    accountLabel->setFontSize(18);
    accountLabel->setTextColor(nvgRGB(180, 187, 203));
    accountLabel->setMarginTop(5);
    headingText->addView(accountLabel);

    profileAvatar = new TwiNXProfileAvatar();
    profileAvatar->setWidth(76);
    profileAvatar->setHeight(76);
    profileAvatar->setMarginLeft(24);
    headingRow->addView(profileAvatar);

    statusLabel = new brls::Label();
    statusLabel->setFontSize(17);
    statusLabel->setTextColor(nvgRGB(166, 173, 190));
    statusLabel->setMarginTop(6);
    statusLabel->setMarginBottom(14);
    top->addView(statusLabel);

    auto* actions = new brls::Box();
    actions->setAxis(brls::Axis::ROW);
    top->addView(actions);

    loginButton = makeButton("Sign in", &loginButtonLabel);
    loginButton->registerClickAction([this](brls::View*) {
        if (twitch::loadOAuth().signedIn()) logout(); else beginLogin();
        return true;
    });
    actions->addView(loginButton);

    searchButton = makeButton("Search");
    searchButton->registerClickAction([this](brls::View*) {
        search();
        return true;
    });
    actions->addView(searchButton);

    refreshButton = makeButton("Refresh");
    refreshButton->registerClickAction([this](brls::View*) {
        refreshHome();
        return true;
    });
    actions->addView(refreshButton);

    aboutButton = makeButton("About");
    aboutButton->registerClickAction([this](brls::View*) {
        showAbout();
        return true;
    });
    actions->addView(aboutButton);

    errorBox = new brls::Box();
    errorBox->setAxis(brls::Axis::ROW);
    errorBox->setAlignItems(brls::AlignItems::CENTER);
    errorBox->setPadding(12, 16, 12, 16);
    errorBox->setMarginTop(12);
    errorBox->setCornerRadius(8);
    errorBox->setBackgroundColor(nvgRGB(75, 35, 42));
    errorBox->setVisibility(brls::Visibility::GONE);

    errorLabel = new brls::Label();
    errorLabel->setGrow(1.0f);
    errorLabel->setFontSize(16);
    errorLabel->setTextColor(nvgRGB(245, 220, 224));
    errorBox->addView(errorLabel);

    retryButton = makeButton("Retry");
    retryButton->setMarginRight(0);
    retryButton->registerClickAction([this](brls::View*) {
        if (retryAction) retryAction();
        return true;
    });
    errorBox->addView(retryButton);
    top->addView(errorBox);

    homeScroll = new brls::ScrollingFrame();
    auto* scroll = homeScroll;
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setFocusable(false);
    scroll->setGrow(1.0f);
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPadding(4, 48, 26, 48);
    scroll->setContentView(content);
    this->addView(scroll);

    heroBox = new brls::Box();
    heroBox->setAxis(brls::Axis::ROW);
    heroBox->setHeight(278);
    heroBox->setCornerRadius(12);
    heroBox->setBackgroundColor(nvgRGB(25, 27, 36));
    heroBox->setMarginBottom(20);
    heroBox->setVisibility(brls::Visibility::GONE);

    auto* heroInfo = new brls::Box();
    heroInfo->setAxis(brls::Axis::COLUMN);
    heroInfo->setWidthPercentage(44);
    heroInfo->setHeightPercentage(100);
    heroInfo->setPadding(24, 26, 24, 26);
    heroInfo->setBackgroundColor(nvgRGBA(18, 20, 28, 245));
    heroBox->addView(heroInfo);

    heroLiveLabel = new brls::Label();
    heroLiveLabel->setFontSize(16);
    heroLiveLabel->setTextColor(nvgRGB(230, 95, 130));
    heroLiveLabel->setMarginBottom(8);
    heroInfo->addView(heroLiveLabel);

    heroChannelLabel = new brls::Label();
    heroChannelLabel->setFontSize(27);
    heroChannelLabel->setTextColor(nvgRGB(248, 249, 252));
    heroChannelLabel->setSingleLine(true);
    heroChannelLabel->setMarginBottom(8);
    heroInfo->addView(heroChannelLabel);

    heroTitleLabel = new brls::Label();
    heroTitleLabel->setFontSize(20);
    heroTitleLabel->setTextColor(nvgRGB(232, 235, 242));
    heroTitleLabel->setMarginBottom(12);
    heroInfo->addView(heroTitleLabel);

    heroMetaLabel = new brls::Label();
    heroMetaLabel->setFontSize(17);
    heroMetaLabel->setTextColor(nvgRGB(172, 180, 198));
    heroInfo->addView(heroMetaLabel);

    heroPreview = new brls::Image();
    heroPreview->setGrow(1.0f);
    heroPreview->setHeightPercentage(100);
    heroPreview->setImageFromRes("img/video-card-bg.png");
    heroBox->addView(heroPreview);

    content->addView(heroBox);

    auto remember = [this](const std::string& row, const std::string& key) {
        rememberFocus(row, key);
    };
    followedRow = new TwitchRow("followed", "Followed channels — live", TwitchRow::Kind::Streams, remember);
    offlineFollowedRow = new TwitchRow("offline-followed", "Followed channels — offline", TwitchRow::Kind::Streams, remember);
    popularRow = new TwitchRow("popular", "Popular live streams", TwitchRow::Kind::Streams, remember);
    categoryStreamsRow = new TwitchRow("category-streams", "Category streams", TwitchRow::Kind::Streams, remember);
    categoriesRow = new TwitchRow("categories", "Top categories", TwitchRow::Kind::Categories, remember);
    searchRow = new TwitchRow("search", "Search results", TwitchRow::Kind::Streams, remember);

    // The hero is the visual context for the followed-channel row. Keep the
    // page pinned to its absolute top while moving horizontally between those
    // cards, so changing the selected channel never crops the hero banner.
    auto updateFollowedStream = [this](const twitch::Stream& stream) {
        updateHero(stream);

        if (homeScroll) {
            // The first stream row belongs visually to the hero at the top of
            // the page. CENTERED scrolling moves that row underneath the fixed
            // account/actions bar; the content is clipped, but Borealis draws
            // its focus border globally, making the cyan cursor overlap the
            // bar. Disable automatic centering while this row owns focus and
            // pin the page to the true top instead.
            homeScroll->setScrollingBehavior(
                brls::ScrollingBehavior::NATURAL);
            homeScroll->setContentOffsetY(0.0f, false);
        }

        // Focus propagation can finish later in the same UI pass. Reassert the
        // top offset after it completes so an already-started animation cannot
        // leave the highlight partially behind the fixed header.
        brls::sync([this]() {
            if (homeScroll)
                homeScroll->setContentOffsetY(0.0f, false);
        });
    };

    auto updateFocusedStream =
        [this](const twitch::Stream& stream) {
            if (homeScroll)
                homeScroll->setScrollingBehavior(
                    brls::ScrollingBehavior::CENTERED);
            updateHero(stream);
        };

    followedRow->onStreamFocused(updateFollowedStream);
    popularRow->onStreamFocused(updateFocusedStream);
    categoryStreamsRow->onStreamFocused(updateFocusedStream);
    searchRow->onStreamFocused(updateFocusedStream);

    content->addView(followedRow);
    content->addView(offlineFollowedRow);
    content->addView(popularRow);
    content->addView(categoryStreamsRow);
    content->addView(categoriesRow);
    content->addView(searchRow);

    auto firstVisibleContentFocus = [this]() -> brls::View* {
        for (auto* row : {
                 followedRow,
                 offlineFollowedRow,
                 popularRow,
                 categoryStreamsRow,
                 categoriesRow,
                 searchRow}) {
            if (auto* focus = row->defaultFocus()) return focus;
        }
        return nullptr;
    };

    for (auto* button : {loginButton, searchButton, refreshButton, aboutButton}) {
        if (auto* actionButton = dynamic_cast<TwiNXActionButton*>(button))
            actionButton->downTarget = firstVisibleContentFocus;
    }

    this->registerAction("Search", brls::BUTTON_Y, [this](brls::View*) {
        search();
        return true;
    });
    this->registerAction("Refresh", brls::BUTTON_X, [this](brls::View*) {
        refreshHome();
        return true;
    });
    this->registerAction("Configured channel", brls::BUTTON_RB, [this](brls::View*) {
        playConfiguredChannel();
        return true;
    });

    updateAccount();
    if (twitch::loadOAuth().signedIn()) refreshHome();
    else statusLabel->setText("Sign in with Twitch to load followed channels, streams, categories, and search.");
    brls::sync([this]() { brls::Application::giveFocus(loginButton); });
}

TwiNXHome::~TwiNXHome() {
    if (activeRequest) activeRequest->store(true);
    if (heroPreview) Image::cancel(heroPreview);
}

void TwiNXHome::updateHero(const twitch::Stream& stream) {
    if (!heroBox || !heroPreview) return;

    Image::cancel(heroPreview);
    heroPreview->setImageFromRes("img/video-card-bg.png");
    if (!stream.thumbnailUrl.empty())
        Image::with(heroPreview, twitch::streamThumbnail(stream.thumbnailUrl, 820, 461));

    const std::string channel = stream.userName.empty() ? stream.userLogin : stream.userName;
    heroLiveLabel->setText(stream.isLive ? "LIVE · " + viewersText(stream.viewerCount) : "OFFLINE");
    heroChannelLabel->setText(channel.empty() ? "Twitch" : channel);
    heroTitleLabel->setText(stream.title.empty() ? "Live on Twitch" : stream.title);

    std::string meta = stream.gameName;
    if (!stream.language.empty()) {
        if (!meta.empty()) meta += " · ";
        meta += stream.language;
    }
    heroMetaLabel->setText(meta.empty() ? "Select a stream to watch" : meta);
    heroBox->setVisibility(brls::Visibility::VISIBLE);
}

void TwiNXHome::clearHero() {
    if (!heroBox || !heroPreview) return;
    Image::cancel(heroPreview);
    heroPreview->setImageFromRes("img/video-card-bg.png");
    heroBox->setVisibility(brls::Visibility::GONE);
}

void TwiNXHome::setBusy(bool value, const std::string& message) {
    busy = value;
    if (!message.empty()) statusLabel->setText(message);
}

void TwiNXHome::updateAccount() {
    const auto session = twitch::loadOAuth();
    if (session.signedIn()) {
        accountLabel->setText("Signed in as " +
            (session.displayName.empty() ? session.login : session.displayName));
        loginButtonLabel->setText("Sign out");
    } else {
        accountLabel->setText("Not signed in");
        loginButtonLabel->setText("Sign in");
        updateProfileImage("");
    }
}

void TwiNXHome::updateProfileImage(const std::string& url) {
    if (!profileAvatar) return;
    static_cast<TwiNXProfileAvatar*>(
        profileAvatar)->setProfileImage(url);
}

uint64_t TwiNXHome::beginBrowsingRequest(const std::string& message) {
    // A browsing refresh/search may replace an older browsing operation. Login
    // and playback resolution remain non-replaceable because they use separate
    // protocols and must complete their own lifecycle cleanly.
    if (busy && !activeRequest) return 0;
    if (activeRequest) activeRequest->store(true);

    activeRequest = std::make_shared<std::atomic_bool>(false);
    const uint64_t id = ++requestGeneration;
    setBusy(true, message);
    this->ptrLock();
    return id;
}

bool TwiNXHome::finishBrowsingRequest(uint64_t requestId) {
    this->ptrUnlock();
    if (requestId != requestGeneration) return false;
    activeRequest.reset();
    busy = false;
    return true;
}

void TwiNXHome::cancelBrowsingRequest() {
    if (!activeRequest) return;
    activeRequest->store(true);
    activeRequest.reset();
    ++requestGeneration;
    busy = false;
}

void TwiNXHome::showRequestError(const std::string& message, std::function<void()> retry) {
    errorLabel->setText(message);
    retryAction = std::move(retry);
    retryButton->setVisibility(retryAction ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    errorBox->setVisibility(brls::Visibility::VISIBLE);
}

void TwiNXHome::clearRequestError() {
    retryAction = nullptr;
    errorBox->setVisibility(brls::Visibility::GONE);
}

void TwiNXHome::rememberFocus(const std::string& row, const std::string& key) {
    lastFocusedRow = row;
    lastFocusedKey = key;
}

void TwiNXHome::restoreContentFocus() {
    if (lastFocusedRow == "followed" && followedRow->focusKey(lastFocusedKey)) return;
    if (lastFocusedRow == "offline-followed" && offlineFollowedRow->focusKey(lastFocusedKey)) return;
    if (lastFocusedRow == "popular" && popularRow->focusKey(lastFocusedKey)) return;
    if (lastFocusedRow == "category-streams" && categoryStreamsRow->focusKey(lastFocusedKey)) return;
    if (lastFocusedRow == "categories" && categoriesRow->focusKey(lastFocusedKey)) return;
    if (lastFocusedRow == "search" && searchRow->focusKey(lastFocusedKey)) return;
    focusFirstContentRow();
}

void TwiNXHome::focusFirstContentRow() {
    if (auto* focus = followedRow->defaultFocus()) {
        brls::Application::giveFocus(focus);
        return;
    }
    if (auto* focus = offlineFollowedRow->defaultFocus()) {
        brls::Application::giveFocus(focus);
        return;
    }
    if (auto* focus = popularRow->defaultFocus()) {
        brls::Application::giveFocus(focus);
        return;
    }
    if (auto* focus = categoriesRow->defaultFocus()) {
        brls::Application::giveFocus(focus);
        return;
    }
    brls::Application::giveFocus(refreshButton);
}

void TwiNXHome::beginLogin() {
    if (busy) return;
    clearRequestError();
    setBusy(true, "Requesting a Twitch activation code…");
    this->ptrLock();
    twitch::loginDeviceAsync(
        twitch::loadConfig(),
        [this](twitch::DeviceAuthorization code) {
            statusLabel->setText(
                "On your phone or computer open twitch.tv/activate · Code: " +
                code.userCode + " · Waiting for authorization…");
        },
        [this](twitch::OAuthSession) {
            setBusy(false, "Twitch sign-in completed.");
            updateAccount();
            this->ptrUnlock();
            refreshHome();
        },
        [this](const std::string& error) {
            setBusy(false, "Login error: " + error);
            showRequestError("Twitch sign-in failed: " + error, [this]() { beginLogin(); });
            brls::Application::notify("twiNX: " + error);
            this->ptrUnlock();
        });
}

void TwiNXHome::logout() {
    if (busy && !activeRequest) return;
    cancelBrowsingRequest();
    twitch::clearOAuth();
    clearRows();
    clearRequestError();
    updateProfileImage("");
    updateAccount();
    statusLabel->setText("Signed out. Select Sign in to reconnect your Twitch account.");
}

void TwiNXHome::refreshHome() {
    if (!twitch::loadOAuth().signedIn()) {
        statusLabel->setText("Sign in to Twitch first.");
        brls::Application::giveFocus(loginButton);
        return;
    }

    const uint64_t requestId = beginBrowsingRequest("Loading Twitch home…");
    if (!requestId) return;
    clearRequestError();
    const HTTP::Cancel requestCancel = activeRequest;

    twitch::loadHomeAsync(
        twitch::loadConfig(),
        [this, requestId](twitch::HomeData data) {
            if (!finishBrowsingRequest(requestId)) return;
            showHomeData(std::move(data));
            restoreContentFocus();
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            setBusy(false, "Home error: " + error);
            showRequestError("Could not refresh Twitch home: " + error,
                [this]() { refreshHome(); });
            brls::Application::notify("twiNX: " + error);
        },
        requestCancel);
}

void TwiNXHome::showHomeData(twitch::HomeData data) {
    updateAccount();
    updateProfileImage(
        data.profile.profileImageUrl);
    auto play = [this](const twitch::Stream& stream) { playChannel(stream); };

    if (!data.followed.items.empty()) updateHero(data.followed.items.front());
    else if (!data.popular.items.empty()) updateHero(data.popular.items.front());
    else clearHero();

    followedCursor = data.followed.loaded ? data.followed.cursor : "";
    offlineFollowedCursor = data.offlineFollowed.loaded ? data.offlineFollowed.cursor : "";
    popularCursor = data.popular.loaded ? data.popular.cursor : "";
    categoriesCursor = data.categories.loaded ? data.categories.cursor : "";

    const bool followedHasItems = !data.followed.items.empty();
    const bool offlineFollowedHasItems = !data.offlineFollowed.items.empty();
    const bool popularHasItems = !data.popular.items.empty();
    const bool categoriesHaveItems = !data.categories.items.empty();

    auto focusTopActions = [this]() -> brls::View* {
        if (homeScroll) homeScroll->setContentOffsetY(0.0f, false);
        return loginButton;
    };

    std::function<brls::View*()> followedUpTarget;
    std::function<brls::View*()> offlineFollowedUpTarget;
    std::function<brls::View*()> popularUpTarget;
    std::function<brls::View*()> categoriesUpTarget;

    if (followedHasItems) {
        followedUpTarget = focusTopActions;
    } else if (offlineFollowedHasItems) {
        offlineFollowedUpTarget = focusTopActions;
    } else if (popularHasItems) {
        popularUpTarget = focusTopActions;
    } else if (categoriesHaveItems) {
        categoriesUpTarget = focusTopActions;
    }

    followedRow->setStreams(std::move(data.followed.items), play,
        data.followedError.empty()
            ? "None of your followed channels are live right now."
            : "Live followed channels could not be loaded. Use Retry to try again.",
        followedUpTarget);
    offlineFollowedRow->setStreams(std::move(data.offlineFollowed.items), play,
        data.offlineFollowedError.empty()
            ? "No offline followed channels were returned."
            : "Offline followed channels could not be loaded. Use Retry to try again.",
        offlineFollowedUpTarget);
    popularRow->setStreams(std::move(data.popular.items), play,
        data.popularError.empty()
            ? "Twitch returned no popular live streams."
            : "Popular streams could not be loaded. Use Retry to try again.",
        popularUpTarget);
    categoriesRow->setCategories(std::move(data.categories.items),
        [this](const twitch::Category& category) { openCategory(category); },
        data.categoriesError.empty()
            ? "Twitch returned no top categories."
            : "Top categories could not be loaded. Use Retry to try again.",
        categoriesUpTarget);

    followedRow->setHasMore(!followedCursor.empty());
    offlineFollowedRow->setHasMore(!offlineFollowedCursor.empty());
    popularRow->setHasMore(!popularCursor.empty());
    categoriesRow->setHasMore(!categoriesCursor.empty());
    followedRow->onNextPage(!followedCursor.empty(), [this]() { loadMoreFollowed(); });
    offlineFollowedRow->onNextPage(!offlineFollowedCursor.empty(), [this]() { loadMoreOfflineFollowed(); });
    popularRow->onNextPage(!popularCursor.empty(), [this]() { loadMorePopular(); });
    categoriesRow->onNextPage(!categoriesCursor.empty(), [this]() { loadMoreCategories(); });

    categoryStreamsRow->clear();
    searchRow->clear();
    categoryStreamsCursor.clear();
    searchCursor.clear();
    currentCategoryId.clear();
    currentCategoryName.clear();
    currentSearchQuery.clear();

    if (data.hasErrors()) {
        const std::string sections = joinErrors(data);
        setBusy(false, "Home updated, but " + sections + " could not be loaded.");
        showRequestError("Some Twitch sections failed to load: " + sections + ".",
            [this]() { refreshHome(); });
    } else {
        clearRequestError();
        setBusy(false, "Home updated.");
    }
}

void TwiNXHome::showAbout() {
    auto* page = new brls::Box();
    page->setAxis(brls::Axis::COLUMN);
    page->setDimensions(
        brls::Application::contentWidth,
        brls::Application::contentHeight);
    page->setBackgroundColor(nvgRGB(16, 18, 24));

    auto* heading = new brls::Box();
    heading->setAxis(brls::Axis::ROW);
    heading->setAlignItems(brls::AlignItems::CENTER);
    heading->setPadding(24, 48, 18, 48);
    heading->setWidthPercentage(100);
    page->addView(heading);

    auto* headingText = new brls::Box();
    headingText->setAxis(brls::Axis::COLUMN);
    headingText->setGrow(1.0f);
    heading->addView(headingText);

    auto* title = new brls::Label();
    title->setText("About twiNX");
    title->setFontSize(34);
    title->setTextColor(nvgRGB(244, 246, 251));
    headingText->addView(title);

    auto* subtitle = new brls::Label();
    subtitle->setText(
        "Version " + AppVersion::getVersion() +
        " · Twitch for Nintendo Switch");
    subtitle->setFontSize(17);
    subtitle->setTextColor(nvgRGB(171, 179, 196));
    subtitle->setMarginTop(4);
    headingText->addView(subtitle);

    auto* backButton = makeButton("Back");
    backButton->setMarginRight(0);
    backButton->registerClickAction([](brls::View*) {
        return brls::Application::popActivity();
    });
    heading->addView(backButton);

    auto* scroll = new AboutScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setFocusable(false);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setFocusProxy(backButton);
    page->addView(scroll);

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPadding(6, 48, 42, 48);
    scroll->setContentView(content);

    auto addSection = [content](const std::string& text) {
        auto* label = new brls::Label();
        label->setText(text);
        label->setFontSize(25);
        label->setTextColor(nvgRGB(242, 243, 248));
        label->setMarginTop(18);
        label->setMarginBottom(8);
        content->addView(label);
    };

    auto addBody = [content](
                       const std::string& text,
                       NVGcolor color = nvgRGB(194, 200, 214)) {
        auto* label = new brls::Label();
        label->setText(text);
        label->setFontSize(18);
        label->setTextColor(color);
        label->setMarginBottom(8);
        content->addView(label);
    };

    auto* creditCard = new brls::Box();
    creditCard->setAxis(brls::Axis::COLUMN);
    creditCard->setPadding(18, 22, 18, 22);
    creditCard->setCornerRadius(10);
    creditCard->setBackgroundColor(nvgRGB(28, 31, 41));
    creditCard->setMarginBottom(8);
    content->addView(creditCard);

    auto* createdBy = new brls::Label();
    createdBy->setText("Created by HiroshiYamauchi");
    createdBy->setFontSize(24);
    createdBy->setTextColor(nvgRGB(238, 220, 255));
    creditCard->addView(createdBy);

    auto* projectDescription = new brls::Label();
    projectDescription->setText(
        "twiNX is a controller-first, independent Twitch client built "
        "specifically for Nintendo Switch homebrew.");
    projectDescription->setFontSize(18);
    projectDescription->setTextColor(nvgRGB(190, 197, 212));
    projectDescription->setMarginTop(7);
    creditCard->addView(projectDescription);

    addSection("Features");
    addBody(
        "• Twitch sign-in and followed-channel browsing\n"
        "• Separate live and offline followed-channel rows\n"
        "• Popular streams, categories and channel search\n"
        "• Detailed channel pages with broadcasts, clips and categories\n"
        "• Live, VOD and clip playback with quality selection\n"
        "• Embedded-player live resolution without the commercial-break slate\n"
        "• Guarded software, hardware and Hybrid decoder modes\n"
        "• Automatic Joy-Con portrait orientation in either direction\n"
        "• Portrait stream, chat, draft, emote sheet and inline touch keyboard\n"
        "• Live chat with badges, static/animated emotes and message composer\n"
        "• Adjustable touch-keyboard haptic feedback\n"
        "• Docked and overlay chat, compact mode and four-corner placement\n"
        "• Controller-first navigation designed for portrait, handheld and TV play");

    addSection("Changelog");
    addBody(
        "0.9.0 — Added automatic smartphone-style portrait mode with a persistent "
        "chat composer, inline keyboard, emote sheet and adjustable keyboard haptics. "
        "Added a touch-accessible Send button, validated animated emotes, hardened "
        "MPV/FFmpeg/NVTEGRA hardware playback and switched live resolution to Twitch's "
        "embedded-player identity, avoiding the generic commercial-break presentation.\n\n"
        "0.8.1 — Corrected twiNX branding throughout the interface and added "
        "this About page with features, complete release history and credits.\n\n"
        "0.8.0 — Added the offline followed-channels row.\n\n"
        "0.7.9 — Fixed long-message wrapping and added compact overlay chat "
        "with selectable corner placement.\n\n"
        "0.7.8 — Added incoming channel/subscriber emote rendering and an "
        "experimental animated-emote preference.\n\n"
        "0.7.7 — Improved player lifecycle ownership, fullscreen VOD/clip "
        "layout, channel-page focus and network stability.\n\n"
        "0.7.6 — Fixed a multithreaded libcurl DNS-cache race.\n\n"
        "0.7.5 — Added finite-index navigation to the chat composer emote grid.\n\n"
        "0.7.4 — Reworked composer navigation boundaries.\n\n"
        "0.7.3 — Improved composer cursor behavior and focus rendering.\n\n"
        "0.7.2 — Added the experimental Hybrid decoder and header/emote fixes.\n\n"
        "0.7.1 — Added native VOD and clip playback plus navigation fixes.\n\n"
        "0.7.0 — Added Twitch channel pages and channel-page navigation.");

    addBody(
        "0.6.0 — Chat Composer and Emote Picker.\n\n"
        "0.5.4 — Image Content-Type Guard.\n\n"
        "0.5.3 — Added native Twitch badges and the Plus-button chat-layout shortcut.\n\n"
        "0.5.2 — Chat Text Layout Fix.\n\n"
        "0.5.1 — Chat and Emote Stability.\n\n"
        "0.5.0 — First substantial chat feature release.\n\n"
        "0.4.8 — Decoder Mode Selector.\n\n"
        "0.4.7 — Hardware Decode Transition.\n\n"
        "0.4.6 — Twitch software-decode stability and hardware decoder recovery.\n\n"
        "0.4.5 — Docked Video Fit.\n\n"
        "0.4.4 — MPVCore-level ad recovery.\n\n"
        "0.4.3 — Commercial-Break Recovery.\n\n"
        "0.4.2 — TV-Style Docked Chat.\n\n"
        "0.4.1 — Chat Behind Player Controls.\n\n"
        "0.4.0 — Read-Only Live Chat.");

    addBody(
        "0.3.2 — Reset the home scroll to the absolute top.\n\n"
        "0.3.1 — TV Home and Player Quality.\n\n"
        "0.3.0 — Channel Details and Quality Selection.\n\n"
        "0.2.5 — Deterministic Focus Fix.\n\n"
        "0.2.4 — Focus and Icon Correction.\n\n"
        "0.2.3 — Navigation and Branding.\n\n"
        "0.2.2 — Browsing Stability.\n\n"
        "0.2.1 — UX Fix.\n\n"
        "0.2.0 — Login and Browsing Update.\n\n"
        "0.1.0 — First proof of concept.");

    addSection("Credits");
    addBody(
        "Design, direction and project ownership: HiroshiYamauchi\n\n"
        "Built with Twitch APIs and chat services, mpv, FFmpeg, Borealis, "
        "libcurl, libnx and devkitPro. Twire provided reference behavior for "
        "embedded-player stream resolution. Thanks to the Nintendo Switch "
        "homebrew and open-source communities that make projects like "
        "twiNX possible.");

    addSection("Project note");
    addBody(
        "twiNX is an independent homebrew project. It is not affiliated "
        "with, endorsed by or sponsored by Twitch Interactive or Nintendo.",
        nvgRGB(150, 158, 176));

    page->registerAction("Back", brls::BUTTON_B, [](brls::View*) {
        return brls::Application::popActivity();
    });

    brls::Application::pushActivity(new brls::Activity(page));
    brls::sync([backButton]() {
        brls::Application::giveFocus(backButton);
    });
}

void TwiNXHome::search() {
    if (busy && !activeRequest) return;
    if (!twitch::loadOAuth().signedIn()) {
        statusLabel->setText("Sign in before searching Twitch.");
        return;
    }
    brls::Application::getImeManager()->openForText(
        [this](std::string query) {
            query.erase(query.begin(), std::find_if(query.begin(), query.end(), [](unsigned char c) {
                return !std::isspace(c);
            }));
            while (!query.empty() && std::isspace(static_cast<unsigned char>(query.back()))) {
                query.pop_back();
            }
            if (!query.empty()) searchFor(query);
        },
        "Search Twitch", "Enter a channel name", 100, "");
}

void TwiNXHome::searchFor(const std::string& query) {
    const uint64_t requestId = beginBrowsingRequest("Searching Twitch for “" + query + "”…");
    if (!requestId) return;
    clearRequestError();
    const HTTP::Cancel requestCancel = activeRequest;

    twitch::searchChannelsPageAsync(
        twitch::loadConfig(), query, "",
        [this, requestId, query](twitch::StreamPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            currentSearchQuery = query;
            searchCursor = page.cursor;
            searchRow->setTitle("Search results · " + query);
            searchRow->setStreams(std::move(page.items),
                [this](const twitch::Stream& stream) { playChannel(stream); },
                "No Twitch channels matched “" + query + "”.");
            searchRow->setHasMore(!searchCursor.empty());
            searchRow->onNextPage(!searchCursor.empty(), [this]() { loadMoreSearch(); });
            clearRequestError();
            setBusy(false, "Search complete.");
            if (auto* focus = searchRow->defaultFocus()) brls::Application::giveFocus(focus);
        },
        [this, requestId, query](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            setBusy(false, "Search error: " + error);
            showRequestError("Search failed: " + error,
                [this, query]() { searchFor(query); });
            brls::Application::notify("twiNX: " + error);
        },
        requestCancel);
}

void TwiNXHome::openCategory(const twitch::Category& category) {
    const uint64_t requestId = beginBrowsingRequest("Loading " + category.name + " streams…");
    if (!requestId) return;
    clearRequestError();
    const HTTP::Cancel requestCancel = activeRequest;

    twitch::loadCategoryStreamsPageAsync(
        twitch::loadConfig(), category.id, "",
        [this, requestId, category](twitch::StreamPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            currentCategoryId = category.id;
            currentCategoryName = category.name;
            categoryStreamsCursor = page.cursor;
            if (!page.items.empty()) updateHero(page.items.front());
            categoryStreamsRow->setTitle("Live in " + category.name);
            categoryStreamsRow->setStreams(std::move(page.items),
                [this](const twitch::Stream& stream) { playChannel(stream); },
                "No live channels are currently streaming " + category.name + ".");
            categoryStreamsRow->setHasMore(!categoryStreamsCursor.empty());
            categoryStreamsRow->onNextPage(!categoryStreamsCursor.empty(), [this]() { loadMoreCategoryStreams(); });
            clearRequestError();
            setBusy(false, "Category loaded.");
            if (auto* focus = categoryStreamsRow->defaultFocus()) brls::Application::giveFocus(focus);
        },
        [this, requestId, category](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            setBusy(false, "Category error: " + error);
            showRequestError("Could not load " + category.name + ": " + error,
                [this, category]() { openCategory(category); });
            brls::Application::notify("twiNX: " + error);
        },
        requestCancel);
}

void TwiNXHome::loadMoreFollowed() {
    if (followedCursor.empty() || busy) return;
    const std::string cursor = followedCursor;
    const uint64_t requestId = beginBrowsingRequest("Loading more followed channels…");
    if (!requestId) return;
    const HTTP::Cancel requestCancel = activeRequest;
    twitch::loadFollowedPageAsync(twitch::loadConfig(), cursor,
        [this, requestId](twitch::StreamPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = followedRow->appendStreams(std::move(page.items));
            followedCursor = page.cursor;
            followedRow->setHasMore(!followedCursor.empty());
            followedRow->onNextPage(!followedCursor.empty(), [this]() { loadMoreFollowed(); });
            clearRequestError();
            setBusy(false, added ? "More followed channels loaded." : "No additional followed channels.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more followed channels: " + error,
                [this]() { loadMoreFollowed(); });
            setBusy(false, "Pagination error: " + error);
        }, requestCancel);
}

void TwiNXHome::loadMoreOfflineFollowed() {
    if (offlineFollowedCursor.empty() || busy) return;
    const std::string cursor = offlineFollowedCursor;
    const uint64_t requestId = beginBrowsingRequest("Loading more offline followed channels…");
    if (!requestId) return;
    const HTTP::Cancel requestCancel = activeRequest;
    twitch::loadOfflineFollowedPageAsync(twitch::loadConfig(), cursor,
        [this, requestId](twitch::StreamPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = offlineFollowedRow->appendStreams(std::move(page.items));
            offlineFollowedCursor = page.cursor;
            offlineFollowedRow->setHasMore(!offlineFollowedCursor.empty());
            offlineFollowedRow->onNextPage(!offlineFollowedCursor.empty(),
                [this]() { loadMoreOfflineFollowed(); });
            clearRequestError();
            setBusy(false, added
                ? "More offline followed channels loaded."
                : "No additional offline followed channels.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more offline followed channels: " + error,
                [this]() { loadMoreOfflineFollowed(); });
            setBusy(false, "Pagination error: " + error);
        }, requestCancel);
}

void TwiNXHome::loadMorePopular() {
    if (popularCursor.empty() || busy) return;
    const std::string cursor = popularCursor;
    const uint64_t requestId = beginBrowsingRequest("Loading more popular streams…");
    if (!requestId) return;
    const HTTP::Cancel requestCancel = activeRequest;
    twitch::loadPopularPageAsync(twitch::loadConfig(), cursor,
        [this, requestId](twitch::StreamPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = popularRow->appendStreams(std::move(page.items));
            popularCursor = page.cursor;
            popularRow->setHasMore(!popularCursor.empty());
            popularRow->onNextPage(!popularCursor.empty(), [this]() { loadMorePopular(); });
            clearRequestError();
            setBusy(false, added ? "More popular streams loaded." : "No additional popular streams.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more popular streams: " + error,
                [this]() { loadMorePopular(); });
            setBusy(false, "Pagination error: " + error);
        }, requestCancel);
}

void TwiNXHome::loadMoreCategories() {
    if (categoriesCursor.empty() || busy) return;
    const std::string cursor = categoriesCursor;
    const uint64_t requestId = beginBrowsingRequest("Loading more categories…");
    if (!requestId) return;
    const HTTP::Cancel requestCancel = activeRequest;
    twitch::loadCategoriesPageAsync(twitch::loadConfig(), cursor,
        [this, requestId](twitch::CategoryPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = categoriesRow->appendCategories(std::move(page.items));
            categoriesCursor = page.cursor;
            categoriesRow->setHasMore(!categoriesCursor.empty());
            categoriesRow->onNextPage(!categoriesCursor.empty(), [this]() { loadMoreCategories(); });
            clearRequestError();
            setBusy(false, added ? "More categories loaded." : "No additional categories.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more categories: " + error,
                [this]() { loadMoreCategories(); });
            setBusy(false, "Pagination error: " + error);
        }, requestCancel);
}

void TwiNXHome::loadMoreSearch() {
    if (searchCursor.empty() || currentSearchQuery.empty() || busy) return;
    const std::string cursor = searchCursor;
    const std::string query = currentSearchQuery;
    const uint64_t requestId = beginBrowsingRequest("Loading more search results…");
    if (!requestId) return;
    const HTTP::Cancel requestCancel = activeRequest;
    twitch::searchChannelsPageAsync(twitch::loadConfig(), query, cursor,
        [this, requestId](twitch::StreamPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = searchRow->appendStreams(std::move(page.items));
            searchCursor = page.cursor;
            searchRow->setHasMore(!searchCursor.empty());
            searchRow->onNextPage(!searchCursor.empty(), [this]() { loadMoreSearch(); });
            clearRequestError();
            setBusy(false, added ? "More search results loaded." : "No additional search results.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more search results: " + error,
                [this]() { loadMoreSearch(); });
            setBusy(false, "Pagination error: " + error);
        }, requestCancel);
}

void TwiNXHome::loadMoreCategoryStreams() {
    if (categoryStreamsCursor.empty() || currentCategoryId.empty() || busy) return;
    const std::string cursor = categoryStreamsCursor;
    const std::string gameId = currentCategoryId;
    const uint64_t requestId = beginBrowsingRequest("Loading more " + currentCategoryName + " streams…");
    if (!requestId) return;
    const HTTP::Cancel requestCancel = activeRequest;
    twitch::loadCategoryStreamsPageAsync(twitch::loadConfig(), gameId, cursor,
        [this, requestId](twitch::StreamPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = categoryStreamsRow->appendStreams(std::move(page.items));
            categoryStreamsCursor = page.cursor;
            categoryStreamsRow->setHasMore(!categoryStreamsCursor.empty());
            categoryStreamsRow->onNextPage(!categoryStreamsCursor.empty(), [this]() { loadMoreCategoryStreams(); });
            clearRequestError();
            setBusy(false, added ? "More category streams loaded." : "No additional category streams.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more category streams: " + error,
                [this]() { loadMoreCategoryStreams(); });
            setBusy(false, "Pagination error: " + error);
        }, requestCancel);
}

void TwiNXHome::openChannelPage(
    const twitch::Stream& stream) {
    const std::string login =
        stream.userLogin.empty()
            ? stream.userName
            : stream.userLogin;

    if (login.empty()) {
        statusLabel->setText(
            "This result did not contain a "
            "Twitch channel login.");
        return;
    }

    brls::Application::pushActivity(
        new brls::Activity(
            new TwitchChannelPage(login)));
}

void TwiNXHome::playChannel(const twitch::Stream& stream) {
    if (busy) return;
    if (!stream.isLive) {
        openChannelPage(stream);
        return;
    }

    auto config = twitch::loadConfig();
    config.channel = stream.userLogin;
    config.preferredQuality = twitch::loadPreferredQuality();
    if (config.preferredQuality.empty()) config.preferredQuality = "source";
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
            RemoteView::play(url, "Twitch - " + channel, twitch::mpvExtra(), channel);
        },
        [this](const std::string& error) {
            setBusy(false, "Playback error: " + error);
            brls::Application::notify("twiNX: " + error);
            this->ptrUnlock();
        });
}

void TwiNXHome::playConfiguredChannel() {
    auto config = twitch::loadConfig();
    if (config.channel.empty()) {
        statusLabel->setText("No channel is configured in /switch/twinx.txt");
        return;
    }
    twitch::Stream stream;
    stream.userLogin = config.channel;
    stream.userName = config.channel;
    stream.isLive = true;
    playChannel(stream);
}

void TwiNXHome::clearRows() {
    clearHero();
    followedRow->clear();
    offlineFollowedRow->clear();
    popularRow->clear();
    categoryStreamsRow->clear();
    categoriesRow->clear();
    searchRow->clear();
    followedCursor.clear();
    offlineFollowedCursor.clear();
    popularCursor.clear();
    categoriesCursor.clear();
    searchCursor.clear();
    categoryStreamsCursor.clear();
}
