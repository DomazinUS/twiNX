#pragma once

#include "api/http.hpp"
#include "api/twitch.hpp"
#include "api/twitch_auth.hpp"

#include <functional>
#include <string>
#include <vector>

namespace twitch {

struct UserProfile {
    std::string id;
    std::string login;
    std::string displayName;
    std::string description;
    std::string broadcasterType;
    std::string profileImageUrl;
    std::string offlineImageUrl;
    std::string createdAt;
};

struct ChannelInformation {
    std::string broadcasterId;
    std::string broadcasterLogin;
    std::string broadcasterName;
    std::string broadcasterLanguage;
    std::string gameId;
    std::string gameName;
    std::string title;
    std::vector<std::string> tags;
    bool brandedContent = false;
};

struct ChannelVideo {
    std::string id;
    std::string title;
    std::string description;
    std::string createdAt;
    std::string publishedAt;
    std::string url;
    std::string thumbnailUrl;
    std::string duration;
    std::string language;
    int viewCount = 0;
};

struct ChannelClip {
    std::string id;
    std::string title;
    std::string url;
    std::string embedUrl;
    std::string creatorName;
    std::string createdAt;
    std::string thumbnailUrl;
    std::string language;
    int viewCount = 0;
    float duration = 0.0f;
};


struct MediaPlayback {
    std::string url;
    std::string quality;
};

struct ScheduleSegment {
    std::string id;
    std::string startTime;
    std::string endTime;
    std::string title;
    std::string categoryName;
    bool recurring = false;
};


struct Stream {
    std::string id;
    std::string userId;
    std::string userLogin;
    std::string userName;
    std::string gameId;
    std::string gameName;
    std::string title;
    std::string thumbnailUrl;
    std::string language;
    int viewerCount = 0;
    bool isLive = true;
};

struct Category {
    std::string id;
    std::string name;
    std::string boxArtUrl;
};

struct ChannelPageData {
    UserProfile profile;
    ChannelInformation information;
    Stream liveStream;
    bool live = false;
    std::vector<ChannelVideo> videos;
    std::vector<ChannelClip> clips;
    std::vector<Category> categories;
    std::vector<ScheduleSegment> schedule;
    std::string videosError;
    std::string clipsError;
    std::string scheduleError;
};


struct StreamPage {
    std::vector<Stream> items;
    std::string cursor;
    bool loaded = false;
};

struct CategoryPage {
    std::vector<Category> items;
    std::string cursor;
    bool loaded = false;
};

struct HomeData {
    OAuthSession account;
    UserProfile profile;
    StreamPage followed;
    StreamPage offlineFollowed;
    StreamPage popular;
    CategoryPage categories;
    std::string followedError;
    std::string offlineFollowedError;
    std::string popularError;
    std::string categoriesError;

    bool hasErrors() const {
        return !followedError.empty() || !offlineFollowedError.empty() ||
               !popularError.empty() || !categoriesError.empty();
    }

    bool hasAnySection() const {
        return followed.loaded || offlineFollowed.loaded ||
               popular.loaded || categories.loaded;
    }
};

using HomeCallback = std::function<void(HomeData)>;
using UserProfileCallback =
    std::function<void(UserProfile)>;
using ChannelPageCallback =
    std::function<void(ChannelPageData)>;
using StreamsCallback = std::function<void(std::vector<Stream>)>;
using CategoriesCallback = std::function<void(std::vector<Category>)>;
using StreamPageCallback = std::function<void(StreamPage)>;
using CategoryPageCallback = std::function<void(CategoryPage)>;
using MediaPlaybackCallback = std::function<void(MediaPlayback)>;

void loadChannelProfileAsync(
    const Config& config,
    const std::string& login,
    UserProfileCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);

void loadChannelPageAsync(
    const Config& config,
    const std::string& login,
    ChannelPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);

void resolveVodAsync(
    const std::string& videoId,
    MediaPlaybackCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);

void resolveClipAsync(
    const std::string& clipSlug,
    MediaPlaybackCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);

void loadHomeAsync(
    const Config& config,
    HomeCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);

void searchChannelsAsync(
    const Config& config,
    const std::string& query,
    StreamsCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);

void loadCategoryStreamsAsync(
    const Config& config,
    const std::string& gameId,
    StreamsCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);

void loadFollowedPageAsync(
    const Config& config,
    const std::string& cursor,
    StreamPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);

void loadOfflineFollowedPageAsync(
    const Config& config,
    const std::string& cursor,
    StreamPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);

void loadPopularPageAsync(
    const Config& config,
    const std::string& cursor,
    StreamPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);

void loadCategoriesPageAsync(
    const Config& config,
    const std::string& cursor,
    CategoryPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);

void searchChannelsPageAsync(
    const Config& config,
    const std::string& query,
    const std::string& cursor,
    StreamPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);

void loadCategoryStreamsPageAsync(
    const Config& config,
    const std::string& gameId,
    const std::string& cursor,
    StreamPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);

std::string mediaThumbnail(
    const std::string& pattern,
    int width = 320,
    int height = 180);
std::string streamThumbnail(const std::string& pattern, int width = 320, int height = 180);
std::string categoryThumbnail(const std::string& pattern, int width = 188, int height = 250);

}  // namespace twitch
