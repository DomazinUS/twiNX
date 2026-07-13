#include "api/twitch_helix.hpp"

#include "api/http.hpp"
#include "utils/thread.hpp"

#include <borealis.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace twitch {
namespace {

using json = nlohmann::json;

void replaceAll(std::string& value, const std::string& from, const std::string& to) {
    size_t position = 0;
    while ((position = value.find(from, position)) != std::string::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
    }
}

HTTP::Header helixHeaders(const Config& config, const OAuthSession& session) {
    return {
        "Authorization: Bearer " + session.accessToken,
        "Client-Id: " + config.oauthClientId,
        "Accept: application/json",
    };
}

void throwIfCancelled(const HTTP::Cancel& cancel) {
    if (cancel && cancel->load()) throw std::runtime_error("Request cancelled");
}

std::string helixGet(
    const Config& config,
    OAuthSession& session,
    const std::string& url,
    const HTTP::Cancel& cancel) {
    throwIfCancelled(cancel);
    try {
        if (cancel) {
            return HTTP::get(
                url, helixHeaders(config, session), HTTP::Timeout{15000}, cancel);
        }
        return HTTP::get(url, helixHeaders(config, session), HTTP::Timeout{15000});
    } catch (const std::exception& exception) {
        throwIfCancelled(cancel);
        const std::string message = exception.what();
        if (message.find("http status 401") == std::string::npos) throw;
        session.expiresAt = 0;
        saveOAuth(session);
        session = ensureOAuth(config);
        throwIfCancelled(cancel);
        if (cancel) {
            return HTTP::get(
                url, helixHeaders(config, session), HTTP::Timeout{15000}, cancel);
        }
        return HTTP::get(url, helixHeaders(config, session), HTTP::Timeout{15000});
    }
}

std::string pageCursor(const json& root) {
    if (!root.contains("pagination") || !root["pagination"].is_object()) return {};
    return root["pagination"].value("cursor", "");
}

std::string withCursor(std::string url, const std::string& cursor) {
    if (!cursor.empty()) url += "&after=" + percentEncode(cursor);
    return url;
}

UserProfile parseUserProfile(const json& item) {
    UserProfile profile;
    profile.id = item.value("id", "");
    profile.login = item.value("login", "");
    profile.displayName =
        item.value("display_name", profile.login);
    profile.description =
        item.value("description", "");
    profile.broadcasterType =
        item.value("broadcaster_type", "");
    profile.profileImageUrl =
        item.value("profile_image_url", "");
    profile.offlineImageUrl =
        item.value("offline_image_url", "");
    profile.createdAt =
        item.value("created_at", "");
    return profile;
}

UserProfile getCurrentUser(
    const Config& config,
    OAuthSession& session,
    const HTTP::Cancel& cancel) {
    const json root = json::parse(
        helixGet(
            config,
            session,
            "https://api.twitch.tv/helix/users",
            cancel));

    if (!root.contains("data") ||
        !root["data"].is_array() ||
        root["data"].empty())
        return {};

    return parseUserProfile(root["data"].front());
}

Stream parseStream(const json& item);

UserProfile getUserByLogin(
    const Config& config,
    OAuthSession& session,
    const std::string& login,
    const HTTP::Cancel& cancel) {
    const json root = json::parse(
        helixGet(
            config,
            session,
            "https://api.twitch.tv/helix/users?login=" +
                percentEncode(login),
            cancel));

    if (!root.contains("data") ||
        !root["data"].is_array() ||
        root["data"].empty())
        throw std::runtime_error(
            "Twitch channel was not found");

    return parseUserProfile(root["data"].front());
}

ChannelInformation parseChannelInformation(
    const json& item) {
    ChannelInformation result;
    result.broadcasterId =
        item.value("broadcaster_id", "");
    result.broadcasterLogin =
        item.value("broadcaster_login", "");
    result.broadcasterName =
        item.value(
            "broadcaster_name",
            result.broadcasterLogin);
    result.broadcasterLanguage =
        item.value("broadcaster_language", "");
    result.gameId =
        item.value("game_id", "");
    result.gameName =
        item.value("game_name", "");
    result.title =
        item.value("title", "");
    result.brandedContent =
        item.value(
            "is_branded_content", false);

    if (item.contains("tags") &&
        item["tags"].is_array()) {
        for (const auto& tag : item["tags"]) {
            if (tag.is_string())
                result.tags.push_back(
                    tag.get<std::string>());
        }
    }

    return result;
}

ChannelInformation getChannelInformation(
    const Config& config,
    OAuthSession& session,
    const std::string& broadcasterId,
    const HTTP::Cancel& cancel) {
    const json root = json::parse(
        helixGet(
            config,
            session,
            "https://api.twitch.tv/helix/channels?"
            "broadcaster_id=" +
                percentEncode(broadcasterId),
            cancel));

    if (!root.contains("data") ||
        !root["data"].is_array() ||
        root["data"].empty())
        return {};

    return parseChannelInformation(
        root["data"].front());
}

Stream getChannelLiveStream(
    const Config& config,
    OAuthSession& session,
    const std::string& broadcasterId,
    const HTTP::Cancel& cancel) {
    const json root = json::parse(
        helixGet(
            config,
            session,
            "https://api.twitch.tv/helix/streams?"
            "first=1&user_id=" +
                percentEncode(broadcasterId),
            cancel));

    if (!root.contains("data") ||
        !root["data"].is_array() ||
        root["data"].empty()) {
        Stream offline;
        offline.userId = broadcasterId;
        offline.isLive = false;
        return offline;
    }

    return parseStream(root["data"].front());
}

std::vector<ChannelVideo> getChannelVideos(
    const Config& config,
    OAuthSession& session,
    const std::string& broadcasterId,
    const HTTP::Cancel& cancel) {
    const json root = json::parse(
        helixGet(
            config,
            session,
            "https://api.twitch.tv/helix/videos?"
            "first=16&type=archive&user_id=" +
                percentEncode(broadcasterId),
            cancel));

    std::vector<ChannelVideo> result;
    if (!root.contains("data") ||
        !root["data"].is_array())
        return result;

    for (const auto& item : root["data"]) {
        ChannelVideo video;
        video.id = item.value("id", "");
        video.title = item.value("title", "");
        video.description =
            item.value("description", "");
        video.createdAt =
            item.value("created_at", "");
        video.publishedAt =
            item.value("published_at", "");
        video.url = item.value("url", "");
        video.thumbnailUrl =
            item.value("thumbnail_url", "");
        video.duration =
            item.value("duration", "");
        video.language =
            item.value("language", "");
        video.viewCount =
            item.value("view_count", 0);
        result.push_back(std::move(video));
    }

    return result;
}

std::vector<ChannelClip> getChannelClips(
    const Config& config,
    OAuthSession& session,
    const std::string& broadcasterId,
    const HTTP::Cancel& cancel) {
    const json root = json::parse(
        helixGet(
            config,
            session,
            "https://api.twitch.tv/helix/clips?"
            "first=16&broadcaster_id=" +
                percentEncode(broadcasterId),
            cancel));

    std::vector<ChannelClip> result;
    if (!root.contains("data") ||
        !root["data"].is_array())
        return result;

    for (const auto& item : root["data"]) {
        ChannelClip clip;
        clip.id = item.value("id", "");
        clip.title = item.value("title", "");
        clip.url = item.value("url", "");
        clip.embedUrl =
            item.value("embed_url", "");
        clip.creatorName =
            item.value("creator_name", "");
        clip.createdAt =
            item.value("created_at", "");
        clip.thumbnailUrl =
            item.value("thumbnail_url", "");
        clip.language =
            item.value("language", "");
        clip.viewCount =
            item.value("view_count", 0);
        clip.duration =
            item.value("duration", 0.0f);
        result.push_back(std::move(clip));
    }

    return result;
}

std::vector<ScheduleSegment> getChannelSchedule(
    const Config& config,
    OAuthSession& session,
    const std::string& broadcasterId,
    const HTTP::Cancel& cancel) {
    const json root = json::parse(
        helixGet(
            config,
            session,
            "https://api.twitch.tv/helix/schedule?"
            "first=8&broadcaster_id=" +
                percentEncode(broadcasterId),
            cancel));

    std::vector<ScheduleSegment> result;
    if (!root.contains("data") ||
        !root["data"].is_object() ||
        !root["data"].contains("segments") ||
        !root["data"]["segments"].is_array())
        return result;

    for (const auto& item :
         root["data"]["segments"]) {
        ScheduleSegment segment;
        segment.id = item.value("id", "");
        segment.startTime =
            item.value("start_time", "");
        segment.endTime =
            item.value("end_time", "");
        segment.title =
            item.value("title", "");

        if (item.contains("category") &&
            item["category"].is_object()) {
            segment.categoryName =
                item["category"].value(
                    "name", "");
        }

        segment.recurring =
            item.value(
                "is_recurring", false);
        result.push_back(std::move(segment));
    }

    return result;
}

std::vector<Category> getGamesByIds(
    const Config& config,
    OAuthSession& session,
    const std::vector<std::string>& ids,
    const HTTP::Cancel& cancel) {
    if (ids.empty()) return {};

    std::string url =
        "https://api.twitch.tv/helix/games?";
    bool first = true;
    for (const auto& id : ids) {
        if (id.empty()) continue;
        if (!first) url += '&';
        first = false;
        url += "id=" + percentEncode(id);
    }
    if (first) return {};

    const json root = json::parse(
        helixGet(
            config,
            session,
            url,
            cancel));

    std::vector<Category> result;
    if (!root.contains("data") ||
        !root["data"].is_array())
        return result;

    for (const auto& item : root["data"]) {
        Category category;
        category.id = item.value("id", "");
        category.name =
            item.value("name", "");
        category.boxArtUrl =
            item.value("box_art_url", "");
        result.push_back(std::move(category));
    }

    return result;
}

Stream parseStream(const json& item) {
    Stream stream;
    stream.id = item.value("id", "");
    stream.userId = item.value("user_id", item.value("broadcaster_id", ""));
    stream.userLogin = item.value("user_login", item.value("broadcaster_login", ""));
    stream.userName = item.value("user_name", item.value("display_name", stream.userLogin));
    stream.gameId = item.value("game_id", "");
    stream.gameName = item.value("game_name", "");
    stream.title = item.value("title", "");
    stream.thumbnailUrl = item.value("thumbnail_url", "");
    stream.language = item.value("language", "");
    stream.viewerCount = item.value("viewer_count", 0);
    stream.isLive = item.value("is_live", true);
    return stream;
}

StreamPage parseStreamPage(const std::string& response) {
    const json root = json::parse(response);
    StreamPage result;
    result.loaded = true;
    result.cursor = pageCursor(root);
    if (!root.contains("data") || !root["data"].is_array()) return result;
    for (const auto& item : root["data"]) result.items.push_back(parseStream(item));
    return result;
}

CategoryPage parseCategoryPage(const std::string& response) {
    const json root = json::parse(response);
    CategoryPage result;
    result.loaded = true;
    result.cursor = pageCursor(root);
    if (!root.contains("data") || !root["data"].is_array()) return result;
    for (const auto& item : root["data"]) {
        Category category;
        category.id = item.value("id", "");
        category.name = item.value("name", "");
        category.boxArtUrl = item.value("box_art_url", "");
        result.items.push_back(std::move(category));
    }
    return result;
}

StreamPage getPopular(
    const Config& config,
    OAuthSession& session,
    const std::string& cursor,
    const HTTP::Cancel& cancel) {
    return parseStreamPage(helixGet(config, session,
        withCursor("https://api.twitch.tv/helix/streams?first=30", cursor), cancel));
}

StreamPage getFollowed(
    const Config& config,
    OAuthSession& session,
    const std::string& cursor,
    const HTTP::Cancel& cancel) {
    StreamPage empty;
    empty.loaded = true;
    if (session.userId.empty()) return empty;
    return parseStreamPage(helixGet(config, session,
        withCursor(
            "https://api.twitch.tv/helix/streams/followed?first=30&user_id=" +
                percentEncode(session.userId),
            cursor),
        cancel));
}

StreamPage getFollowedChannelsPage(
    const Config& config,
    OAuthSession& session,
    const std::string& cursor,
    const HTTP::Cancel& cancel) {
    StreamPage result;
    result.loaded = true;
    if (session.userId.empty()) return result;

    const json root = json::parse(helixGet(
        config,
        session,
        withCursor(
            "https://api.twitch.tv/helix/channels/followed?first=30&user_id=" +
                percentEncode(session.userId),
            cursor),
        cancel));

    result.cursor = pageCursor(root);
    if (!root.contains("data") || !root["data"].is_array()) return result;

    for (const auto& item : root["data"]) {
        Stream channel;
        channel.userId = item.value("broadcaster_id", "");
        channel.userLogin = item.value("broadcaster_login", "");
        channel.userName = item.value("broadcaster_name", channel.userLogin);
        channel.isLive = false;
        if (!channel.userId.empty()) result.items.push_back(std::move(channel));
    }
    return result;
}

std::unordered_set<std::string> getLiveBroadcasterIds(
    const Config& config,
    OAuthSession& session,
    const std::vector<Stream>& channels,
    const HTTP::Cancel& cancel) {
    std::unordered_set<std::string> result;
    if (channels.empty()) return result;

    std::string url = "https://api.twitch.tv/helix/streams?first=100";
    for (const auto& channel : channels) {
        if (!channel.userId.empty())
            url += "&user_id=" + percentEncode(channel.userId);
    }

    const json root = json::parse(helixGet(config, session, url, cancel));
    if (!root.contains("data") || !root["data"].is_array()) return result;
    for (const auto& item : root["data"]) {
        const std::string id = item.value("user_id", "");
        if (!id.empty()) result.insert(id);
    }
    return result;
}

std::unordered_map<std::string, UserProfile> getUserProfilesByIds(
    const Config& config,
    OAuthSession& session,
    const std::vector<Stream>& channels,
    const HTTP::Cancel& cancel) {
    std::unordered_map<std::string, UserProfile> result;
    if (channels.empty()) return result;

    std::string url = "https://api.twitch.tv/helix/users?";
    bool first = true;
    for (const auto& channel : channels) {
        if (channel.userId.empty()) continue;
        if (!first) url += '&';
        first = false;
        url += "id=" + percentEncode(channel.userId);
    }
    if (first) return result;

    const json root = json::parse(helixGet(config, session, url, cancel));
    if (!root.contains("data") || !root["data"].is_array()) return result;
    for (const auto& item : root["data"]) {
        UserProfile profile = parseUserProfile(item);
        if (!profile.id.empty()) result.emplace(profile.id, std::move(profile));
    }
    return result;
}

StreamPage getOfflineFollowed(
    const Config& config,
    OAuthSession& session,
    const std::string& cursor,
    const HTTP::Cancel& cancel) {
    StreamPage result;
    result.loaded = true;
    if (session.userId.empty()) return result;

    // A page from Get Followed Channels may contain only live channels. Keep
    // advancing until at least one offline channel is found or pagination ends,
    // so the row does not incorrectly appear empty while offline follows exist.
    std::string nextCursor = cursor;
    do {
        StreamPage followed = getFollowedChannelsPage(
            config, session, nextCursor, cancel);
        nextCursor = followed.cursor;

        const auto liveIds = getLiveBroadcasterIds(
            config, session, followed.items, cancel);
        const auto profiles = getUserProfilesByIds(
            config, session, followed.items, cancel);

        for (auto& channel : followed.items) {
            if (liveIds.find(channel.userId) != liveIds.end()) continue;

            const auto profile = profiles.find(channel.userId);
            if (profile != profiles.end()) {
                channel.thumbnailUrl = profile->second.profileImageUrl;
                if (!profile->second.displayName.empty())
                    channel.userName = profile->second.displayName;
                if (!profile->second.login.empty())
                    channel.userLogin = profile->second.login;
            }
            channel.isLive = false;
            result.items.push_back(std::move(channel));
        }

        throwIfCancelled(cancel);
    } while (result.items.empty() && !nextCursor.empty());

    result.cursor = nextCursor;
    return result;
}

CategoryPage getCategories(
    const Config& config,
    OAuthSession& session,
    const std::string& cursor,
    const HTTP::Cancel& cancel) {
    return parseCategoryPage(helixGet(config, session,
        withCursor("https://api.twitch.tv/helix/games/top?first=30", cursor), cancel));
}

StreamPage getSearch(
    const Config& config,
    OAuthSession& session,
    const std::string& query,
    const std::string& cursor,
    const HTTP::Cancel& cancel) {
    StreamPage result = parseStreamPage(helixGet(config, session,
        withCursor(
            "https://api.twitch.tv/helix/search/channels?first=30&live_only=true&query=" +
                percentEncode(query),
            cursor),
        cancel));

    // Search Channels calls the broadcaster identifier `id`; Get Streams uses
    // `user_id`. Keep that distinction local to search result normalization.
    for (auto& item : result.items) item.userId = item.id;

    std::string liveDetailsUrl =
        "https://api.twitch.tv/helix/streams?first=100";
    std::unordered_set<std::string> requestedLiveIds;
    for (const auto& item : result.items) {
        if (!item.userId.empty() &&
            requestedLiveIds.insert(item.userId).second) {
            liveDetailsUrl += "&user_id=" + percentEncode(item.userId);
        }
    }

    if (!requestedLiveIds.empty()) {
        const StreamPage details = parseStreamPage(
            helixGet(config, session, liveDetailsUrl, cancel));
        std::unordered_map<std::string, int> viewerCounts;
        for (const auto& stream : details.items) {
            if (!stream.userId.empty()) {
                viewerCounts.emplace(stream.userId, stream.viewerCount);
            }
        }
        for (auto& item : result.items) {
            const auto count = viewerCounts.find(item.userId);
            if (count != viewerCounts.end()) item.viewerCount = count->second;
        }
    }
    return result;
}

StreamPage getCategoryStreams(
    const Config& config,
    OAuthSession& session,
    const std::string& gameId,
    const std::string& cursor,
    const HTTP::Cancel& cancel) {
    return parseStreamPage(helixGet(config, session,
        withCursor(
            "https://api.twitch.tv/helix/streams?first=30&game_id=" +
                percentEncode(gameId),
            cursor),
        cancel));
}



constexpr const char* TWITCH_WEB_CLIENT_ID =
    "kimne78kx3ncx6brgo4mv6wki5h1ko";

HTTP::Header gqlHeaders() {
    return {
        std::string("Client-ID: ") + TWITCH_WEB_CLIENT_ID,
        "Content-Type: application/json",
        "Accept: application/json",
        "User-Agent: Mozilla/5.0",
    };
}

json gqlPost(
    const json& body,
    const HTTP::Cancel& cancel) {
    throwIfCancelled(cancel);
    const std::string response = cancel
        ? HTTP::post(
              "https://gql.twitch.tv/gql",
              body.dump(),
              gqlHeaders(),
              HTTP::Timeout{15000},
              cancel)
        : HTTP::post(
              "https://gql.twitch.tv/gql",
              body.dump(),
              gqlHeaders(),
              HTTP::Timeout{15000});
    throwIfCancelled(cancel);
    return json::parse(response);
}

std::string appendQuery(
    const std::string& url,
    const std::string& query) {
    if (url.empty()) return {};
    return url +
        (url.find('?') == std::string::npos ? "?" : "&") +
        query;
}

MediaPlayback resolveVod(
    const std::string& videoId,
    const HTTP::Cancel& cancel) {
    if (videoId.empty())
        throw std::runtime_error("The Twitch VOD has no video ID");

    const json request = {
        {"operationName", "PlaybackAccessToken"},
        {"extensions",
         {{"persistedQuery",
           {{"version", 1},
            {"sha256Hash",
             "ed230aa1e33e07eebb8928504583da78a5173989fadfb1ac94be06a04f3cdbe9"}}}}},
        {"variables",
         {{"isLive", false},
          {"login", ""},
          {"isVod", true},
          {"vodID", videoId},
          {"playerType", "embed"},
          {"platform", "site"}}},
    };

    const json root = gqlPost(request, cancel);
    if (root.contains("errors") && root["errors"].is_array() &&
        !root["errors"].empty()) {
        throw std::runtime_error(
            root["errors"].front().value(
                "message", "Twitch rejected VOD playback"));
    }

    if (!root.contains("data") ||
        !root["data"].is_object() ||
        !root["data"].contains("videoPlaybackAccessToken") ||
        !root["data"]["videoPlaybackAccessToken"].is_object()) {
        throw std::runtime_error(
            "Twitch did not return a VOD playback token");
    }

    const auto& access =
        root["data"]["videoPlaybackAccessToken"];
    const std::string signature =
        access.value("signature", "");
    const std::string token =
        access.value("value", "");
    if (signature.empty() || token.empty())
        throw std::runtime_error(
            "The VOD is unavailable or access-restricted");

    const auto nonce = static_cast<unsigned long long>(
        brls::getCPUTimeUsec() % 1000000ULL);

    MediaPlayback result;
    result.quality = "auto";
    result.url =
        "https://usher.ttvnw.net/vod/v2/" +
        percentEncode(videoId) +
        ".m3u8?nauthsig=" + percentEncode(signature) +
        "&nauth=" + percentEncode(token) +
        "&allow_source=true&allow_audio_only=true" +
        "&playlist_include_framerate=true" +
        "&platform=web&player_backend=mediaplayer" +
        "&supported_codecs=h264&p=" +
        std::to_string(nonce);
    return result;
}

MediaPlayback resolveClip(
    const std::string& clipSlug,
    const HTTP::Cancel& cancel) {
    if (clipSlug.empty())
        throw std::runtime_error("The Twitch clip has no slug");

    const json request = {
        {"operationName", "VideoAccessToken_Clip"},
        {"extensions",
         {{"persistedQuery",
           {{"version", 1},
            {"sha256Hash",
             "993d9a5131f15a37bd16f32342c44ed1e0b1a9b968c6afdb662d2cddd595f6c5"}}}}},
        {"variables",
         {{"slug", clipSlug},
          {"platform", "web"}}},
    };

    const json root = gqlPost(request, cancel);
    if (root.contains("errors") && root["errors"].is_array() &&
        !root["errors"].empty()) {
        throw std::runtime_error(
            root["errors"].front().value(
                "message", "Twitch rejected clip playback"));
    }

    if (!root.contains("data") ||
        !root["data"].is_object() ||
        !root["data"].contains("clip") ||
        !root["data"]["clip"].is_object()) {
        throw std::runtime_error(
            "Twitch did not return clip playback data");
    }

    const auto& clip = root["data"]["clip"];
    if (!clip.contains("playbackAccessToken") ||
        !clip["playbackAccessToken"].is_object() ||
        !clip.contains("videoQualities") ||
        !clip["videoQualities"].is_array()) {
        throw std::runtime_error(
            "The clip is unavailable or access-restricted");
    }

    const auto& access = clip["playbackAccessToken"];
    const std::string signature =
        access.value("signature", "");
    const std::string token =
        access.value("value", "");
    if (signature.empty() || token.empty())
        throw std::runtime_error(
            "Twitch did not return clip playback authorization");

    std::string bestUrl;
    std::string bestQuality;
    int bestHeight = -1;
    int bestFrameRate = -1;

    for (const auto& quality : clip["videoQualities"]) {
        if (!quality.is_object()) continue;
        const std::string source =
            quality.value("sourceURL", "");
        const std::string label =
            quality.value("quality", "");
        int frameRate = 0;
        const auto frameRateIt = quality.find("frameRate");
        if (frameRateIt != quality.end() &&
            frameRateIt->is_number())
            frameRate = static_cast<int>(
                frameRateIt->get<double>());
        if (source.empty()) continue;

        int height = 0;
        try {
            height = std::stoi(label);
        } catch (...) {
            height = 0;
        }

        if (bestUrl.empty() || height > bestHeight ||
            (height == bestHeight && frameRate > bestFrameRate)) {
            bestUrl = source;
            bestQuality = label.empty()
                ? "auto"
                : label + "p" +
                    (frameRate > 0
                         ? std::to_string(frameRate)
                         : "");
            bestHeight = height;
            bestFrameRate = frameRate;
        }
    }

    if (bestUrl.empty())
        throw std::runtime_error(
            "Twitch returned no playable clip quality");

    MediaPlayback result;
    result.quality = bestQuality;
    result.url = appendQuery(
        bestUrl,
        "sig=" + percentEncode(signature) +
            "&token=" + percentEncode(token));
    return result;
}

std::string sectionError(const std::exception& exception) {
    const std::string message = exception.what();
    return message.empty() ? "Unknown request error" : message;
}

template <typename Result, typename Work, typename Callback>
void loadPageAsync(
    const Config& config,
    Work work,
    Callback success,
    ErrorCallback failure,
    HTTP::Cancel cancel,
    const std::string& unknownError) {
    ThreadPool::instance().submit(
        [config, work = std::move(work), success = std::move(success),
            failure = std::move(failure), cancel = std::move(cancel), unknownError](HTTP&) mutable {
            try {
                OAuthSession session = ensureOAuth(config);
                throwIfCancelled(cancel);
                Result result = work(config, session, cancel);
                saveOAuth(session);
                throwIfCancelled(cancel);
                brls::sync(std::bind(success, std::move(result)));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(failure, std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(failure, unknownError));
            }
        });
}

}  // namespace

std::string mediaThumbnail(
    const std::string& pattern,
    int width,
    int height) {
    std::string result = pattern;
    replaceAll(
        result,
        "%{width}",
        std::to_string(width));
    replaceAll(
        result,
        "%{height}",
        std::to_string(height));
    replaceAll(
        result,
        "{width}",
        std::to_string(width));
    replaceAll(
        result,
        "{height}",
        std::to_string(height));
    return result;
}

std::string streamThumbnail(const std::string& pattern, int width, int height) {
    std::string result = pattern;
    replaceAll(result, "{width}", std::to_string(width));
    replaceAll(result, "{height}", std::to_string(height));
    return result;
}

std::string categoryThumbnail(const std::string& pattern, int width, int height) {
    return streamThumbnail(pattern, width, height);
}

void loadChannelProfileAsync(
    const Config& config,
    const std::string& login,
    UserProfileCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    ThreadPool::instance().submit(
        [config,
         login,
         success = std::move(success),
         failure = std::move(failure),
         cancel = std::move(cancel)](HTTP&) mutable {
            try {
                OAuthSession session =
                    ensureOAuth(config);
                throwIfCancelled(cancel);
                UserProfile profile =
                    getUserByLogin(
                        config,
                        session,
                        login,
                        cancel);
                saveOAuth(session);
                throwIfCancelled(cancel);
                brls::sync(std::bind(
                    success,
                    std::move(profile)));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(
                    failure,
                    std::string(
                        exception.what())));
            } catch (...) {
                brls::sync(std::bind(
                    failure,
                    std::string(
                        "Unknown Twitch channel error")));
            }
        });
}

void loadChannelPageAsync(
    const Config& config,
    const std::string& login,
    ChannelPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    ThreadPool::instance().submit(
        [config,
         login,
         success = std::move(success),
         failure = std::move(failure),
         cancel = std::move(cancel)](HTTP&) mutable {
            try {
                OAuthSession session =
                    ensureOAuth(config);
                throwIfCancelled(cancel);

                ChannelPageData result;
                result.profile =
                    getUserByLogin(
                        config,
                        session,
                        login,
                        cancel);
                throwIfCancelled(cancel);

                result.information =
                    getChannelInformation(
                        config,
                        session,
                        result.profile.id,
                        cancel);

                result.liveStream =
                    getChannelLiveStream(
                        config,
                        session,
                        result.profile.id,
                        cancel);
                result.live =
                    result.liveStream.isLive &&
                    !result.liveStream.id.empty();

                try {
                    result.videos =
                        getChannelVideos(
                            config,
                            session,
                            result.profile.id,
                            cancel);
                } catch (const std::exception& exception) {
                    throwIfCancelled(cancel);
                    result.videosError =
                        exception.what();
                }

                try {
                    result.clips =
                        getChannelClips(
                            config,
                            session,
                            result.profile.id,
                            cancel);
                } catch (const std::exception& exception) {
                    throwIfCancelled(cancel);
                    result.clipsError =
                        exception.what();
                }

                try {
                    result.schedule =
                        getChannelSchedule(
                            config,
                            session,
                            result.profile.id,
                            cancel);
                } catch (const std::exception& exception) {
                    throwIfCancelled(cancel);
                    // Channels without a published schedule commonly return
                    // an empty/404-style response. Keep the rest of the page.
                    result.scheduleError =
                        exception.what();
                }

                std::vector<std::string> gameIds;
                if (!result.information.gameId.empty())
                    gameIds.push_back(
                        result.information.gameId);
                if (!result.liveStream.gameId.empty() &&
                    std::find(
                        gameIds.begin(),
                        gameIds.end(),
                        result.liveStream.gameId) ==
                        gameIds.end())
                    gameIds.push_back(
                        result.liveStream.gameId);

                try {
                    result.categories =
                        getGamesByIds(
                            config,
                            session,
                            gameIds,
                            cancel);
                } catch (const std::exception&) {
                    throwIfCancelled(cancel);
                }

                saveOAuth(session);
                throwIfCancelled(cancel);
                brls::sync(std::bind(
                    success,
                    std::move(result)));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(
                    failure,
                    std::string(
                        exception.what())));
            } catch (...) {
                brls::sync(std::bind(
                    failure,
                    std::string(
                        "Unknown Twitch channel page error")));
            }
        });
}


void resolveVodAsync(
    const std::string& videoId,
    MediaPlaybackCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    ThreadPool::instance().submit(
        [videoId,
         success = std::move(success),
         failure = std::move(failure),
         cancel = std::move(cancel)](HTTP&) mutable {
            try {
                MediaPlayback result =
                    resolveVod(videoId, cancel);
                throwIfCancelled(cancel);
                brls::sync(std::bind(
                    success,
                    std::move(result)));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(
                    failure,
                    std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(
                    failure,
                    std::string(
                        "Unknown Twitch VOD playback error")));
            }
        });
}

void resolveClipAsync(
    const std::string& clipSlug,
    MediaPlaybackCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    ThreadPool::instance().submit(
        [clipSlug,
         success = std::move(success),
         failure = std::move(failure),
         cancel = std::move(cancel)](HTTP&) mutable {
            try {
                MediaPlayback result =
                    resolveClip(clipSlug, cancel);
                throwIfCancelled(cancel);
                brls::sync(std::bind(
                    success,
                    std::move(result)));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(
                    failure,
                    std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(
                    failure,
                    std::string(
                        "Unknown Twitch clip playback error")));
            }
        });
}

void loadHomeAsync(
    const Config& config,
    HomeCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    ThreadPool::instance().submit(
        [config, success = std::move(success), failure = std::move(failure),
            cancel = std::move(cancel)](HTTP&) mutable {
            try {
                HomeData result;
                result.account = ensureOAuth(config);
                throwIfCancelled(cancel);

                // Profile artwork is optional. A transient image/profile
                // request failure must never block the stream/category home.
                try {
                    result.profile =
                        getCurrentUser(
                            config,
                            result.account,
                            cancel);
                } catch (const std::exception& exception) {
                    throwIfCancelled(cancel);
                    brls::Logger::warning(
                        "TwiNX: profile image could not be loaded: {}",
                        exception.what());
                }

                try {
                    result.followed = getFollowed(config, result.account, "", cancel);
                } catch (const std::exception& exception) {
                    throwIfCancelled(cancel);
                    result.followedError = sectionError(exception);
                }

                try {
                    result.offlineFollowed =
                        getOfflineFollowed(config, result.account, "", cancel);
                } catch (const std::exception& exception) {
                    throwIfCancelled(cancel);
                    result.offlineFollowedError = sectionError(exception);
                }

                try {
                    result.popular = getPopular(config, result.account, "", cancel);
                } catch (const std::exception& exception) {
                    throwIfCancelled(cancel);
                    result.popularError = sectionError(exception);
                }

                try {
                    result.categories = getCategories(config, result.account, "", cancel);
                } catch (const std::exception& exception) {
                    throwIfCancelled(cancel);
                    result.categoriesError = sectionError(exception);
                }

                saveOAuth(result.account);
                throwIfCancelled(cancel);
                if (!result.hasAnySection()) {
                    throw std::runtime_error(
                        "Twitch did not return any home sections. Check the connection and retry.");
                }
                brls::sync(std::bind(success, std::move(result)));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(failure, std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(failure, std::string("Unknown Twitch browsing error")));
            }
        });
}

void searchChannelsPageAsync(
    const Config& config,
    const std::string& query,
    const std::string& cursor,
    StreamPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    loadPageAsync<StreamPage>(config,
        [query, cursor](const Config& c, OAuthSession& session, const HTTP::Cancel& requestCancel) {
            return getSearch(c, session, query, cursor, requestCancel);
        },
        std::move(success), std::move(failure), std::move(cancel),
        "Unknown Twitch search error");
}

void searchChannelsAsync(
    const Config& config,
    const std::string& query,
    StreamsCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    searchChannelsPageAsync(config, query, "",
        [success = std::move(success)](StreamPage page) mutable {
            success(std::move(page.items));
        },
        std::move(failure), std::move(cancel));
}

void loadCategoryStreamsPageAsync(
    const Config& config,
    const std::string& gameId,
    const std::string& cursor,
    StreamPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    loadPageAsync<StreamPage>(config,
        [gameId, cursor](const Config& c, OAuthSession& session, const HTTP::Cancel& requestCancel) {
            return getCategoryStreams(c, session, gameId, cursor, requestCancel);
        },
        std::move(success), std::move(failure), std::move(cancel),
        "Unknown category request error");
}

void loadCategoryStreamsAsync(
    const Config& config,
    const std::string& gameId,
    StreamsCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    loadCategoryStreamsPageAsync(config, gameId, "",
        [success = std::move(success)](StreamPage page) mutable {
            success(std::move(page.items));
        },
        std::move(failure), std::move(cancel));
}

void loadFollowedPageAsync(
    const Config& config,
    const std::string& cursor,
    StreamPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    loadPageAsync<StreamPage>(config,
        [cursor](const Config& c, OAuthSession& session, const HTTP::Cancel& requestCancel) {
            return getFollowed(c, session, cursor, requestCancel);
        },
        std::move(success), std::move(failure), std::move(cancel),
        "Unknown followed-streams request error");
}

void loadOfflineFollowedPageAsync(
    const Config& config,
    const std::string& cursor,
    StreamPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    loadPageAsync<StreamPage>(config,
        [cursor](const Config& c, OAuthSession& session, const HTTP::Cancel& requestCancel) {
            return getOfflineFollowed(c, session, cursor, requestCancel);
        },
        std::move(success), std::move(failure), std::move(cancel),
        "Unknown offline followed-channels request error");
}

void loadPopularPageAsync(
    const Config& config,
    const std::string& cursor,
    StreamPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    loadPageAsync<StreamPage>(config,
        [cursor](const Config& c, OAuthSession& session, const HTTP::Cancel& requestCancel) {
            return getPopular(c, session, cursor, requestCancel);
        },
        std::move(success), std::move(failure), std::move(cancel),
        "Unknown popular-streams request error");
}

void loadCategoriesPageAsync(
    const Config& config,
    const std::string& cursor,
    CategoryPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    loadPageAsync<CategoryPage>(config,
        [cursor](const Config& c, OAuthSession& session, const HTTP::Cancel& requestCancel) {
            return getCategories(c, session, cursor, requestCancel);
        },
        std::move(success), std::move(failure), std::move(cancel),
        "Unknown categories request error");
}

}  // namespace twitch
