#pragma once

#include "api/http.hpp"

#include <borealis.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace twitch {
struct Stream;
struct Category;
struct HomeData;
}

class TwitchRow;

class TwiNXHome : public brls::Box {
public:
    TwiNXHome();
    ~TwiNXHome() override;

private:
    void setBusy(bool value, const std::string& message = "");
    void updateAccount();
    void updateProfileImage(const std::string& url);
    void beginLogin();
    void logout();
    void refreshHome();
    void search();
    void showAbout();
    void searchFor(const std::string& query);
    void openCategory(const twitch::Category& category);
    void openChannelPage(const twitch::Stream& stream);
    void playChannel(const twitch::Stream& stream);
    void updateHero(const twitch::Stream& stream);
    void clearHero();
    void playConfiguredChannel();
    void showHomeData(twitch::HomeData data);
    void clearRows();

    uint64_t beginBrowsingRequest(const std::string& message);
    bool finishBrowsingRequest(uint64_t requestId);
    void cancelBrowsingRequest();
    void showRequestError(const std::string& message, std::function<void()> retry);
    void clearRequestError();

    void rememberFocus(const std::string& row, const std::string& key);
    void restoreContentFocus();
    void focusFirstContentRow();

    void loadMoreFollowed();
    void loadMoreOfflineFollowed();
    void loadMorePopular();
    void loadMoreCategories();
    void loadMoreSearch();
    void loadMoreCategoryStreams();

    brls::Label* accountLabel = nullptr;
    brls::Label* statusLabel = nullptr;
    brls::Label* loginButtonLabel = nullptr;
    brls::View* profileAvatar = nullptr;
    brls::Box* loginButton = nullptr;
    brls::Box* searchButton = nullptr;
    brls::Box* refreshButton = nullptr;
    brls::Box* aboutButton = nullptr;

    brls::Box* errorBox = nullptr;
    brls::Label* errorLabel = nullptr;
    brls::Box* retryButton = nullptr;

    brls::ScrollingFrame* homeScroll = nullptr;

    brls::Box* heroBox = nullptr;
    brls::Image* heroPreview = nullptr;
    brls::Label* heroLiveLabel = nullptr;
    brls::Label* heroChannelLabel = nullptr;
    brls::Label* heroTitleLabel = nullptr;
    brls::Label* heroMetaLabel = nullptr;
    std::function<void()> retryAction;

    TwitchRow* followedRow = nullptr;
    TwitchRow* offlineFollowedRow = nullptr;
    TwitchRow* popularRow = nullptr;
    TwitchRow* categoryStreamsRow = nullptr;
    TwitchRow* categoriesRow = nullptr;
    TwitchRow* searchRow = nullptr;

    HTTP::Cancel activeRequest;
    uint64_t requestGeneration = 0;

    std::string followedCursor;
    std::string offlineFollowedCursor;
    std::string popularCursor;
    std::string categoriesCursor;
    std::string searchCursor;
    std::string categoryStreamsCursor;
    std::string currentSearchQuery;
    std::string currentCategoryId;
    std::string currentCategoryName;

    std::string lastFocusedRow;
    std::string lastFocusedKey;

    bool busy = false;
};
