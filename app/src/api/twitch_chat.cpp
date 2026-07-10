#include "api/twitch_chat.hpp"

#include "api/http.hpp"
#include "api/twitch_auth.hpp"
#include "api/twitch_playlist.hpp"
#include "utils/thread.hpp"

#include <borealis.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace twitch {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr const char* DEFAULT_EVENTSUB_URL =
    "wss://eventsub.wss.twitch.tv/ws";
constexpr size_t MAX_DEDUP_IDS = 128;

std::string settingsPath() {
#if defined(__SWITCH__)
    return "sdmc:/config/TwiNX/settings.json";
#else
    return "settings.json";
#endif
}

bool hasScope(const OAuthSession& session, const std::string& required) {
    return std::find(session.scopes.begin(), session.scopes.end(), required) !=
        session.scopes.end();
}

std::string replaceTemplateValue(
    std::string input,
    const std::string& key,
    const std::string& value) {
    size_t position = 0;
    while ((position = input.find(key, position)) !=
        std::string::npos) {
        input.replace(position, key.size(), value);
        position += value.size();
    }
    return input;
}

std::string userEmoteImageUrl(
    std::string templatedUrl,
    const std::string& id) {
    if (templatedUrl.empty()) {
        templatedUrl =
            "https://static-cdn.jtvnw.net/"
            "emoticons/v2/{{id}}/{{format}}/"
            "{{theme_mode}}/{{scale}}";
    }

    templatedUrl = replaceTemplateValue(
        std::move(templatedUrl),
        "{{id}}",
        id);
    templatedUrl = replaceTemplateValue(
        std::move(templatedUrl),
        "{{format}}",
        "static");
    templatedUrl = replaceTemplateValue(
        std::move(templatedUrl),
        "{{theme_mode}}",
        "dark");
    templatedUrl = replaceTemplateValue(
        std::move(templatedUrl),
        "{{scale}}",
        "2.0");
    return templatedUrl;
}

std::string chatEmoteImageUrl(
    const std::string& id,
    const char* format) {
    if (id.empty()) return {};
    return
        "https://static-cdn.jtvnw.net/"
        "emoticons/v2/" +
        id + "/" + format + "/dark/1.0";
}

std::string utcTimestamp() {
    const auto now = std::time(nullptr);
    std::tm value{};
#if defined(_WIN32)
    gmtime_s(&value, &now);
#else
    gmtime_r(&now, &value);
#endif
    std::ostringstream output;
    output << std::put_time(&value, "%H:%M");
    return output.str();
}

std::string base64Encode(const uint8_t* input, size_t size) {
    static constexpr char TABLE[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((size + 2) / 3) * 4);

    for (size_t index = 0; index < size; index += 3) {
        const uint32_t first = input[index];
        const uint32_t second = index + 1 < size ? input[index + 1] : 0;
        const uint32_t third = index + 2 < size ? input[index + 2] : 0;
        const uint32_t value = (first << 16) | (second << 8) | third;

        result.push_back(TABLE[(value >> 18) & 0x3F]);
        result.push_back(TABLE[(value >> 12) & 0x3F]);
        result.push_back(index + 1 < size ? TABLE[(value >> 6) & 0x3F] : '=');
        result.push_back(index + 2 < size ? TABLE[value & 0x3F] : '=');
    }
    return result;
}

struct ParsedUrl {
    std::string curlUrl;
    std::string host;
    std::string path;
};

ParsedUrl parseWebSocketUrl(std::string url) {
    ParsedUrl result;

    if (url.rfind("wss://", 0) == 0) {
        result.curlUrl = "https://" + url.substr(6);
        url.erase(0, 6);
    } else if (url.rfind("https://", 0) == 0) {
        result.curlUrl = url;
        url.erase(0, 8);
    } else {
        throw std::runtime_error("Unsupported EventSub WebSocket URL");
    }

    const auto slash = url.find('/');
    result.host = slash == std::string::npos ? url : url.substr(0, slash);
    result.path = slash == std::string::npos ? "/" : url.substr(slash);
    if (result.host.empty()) throw std::runtime_error("Invalid EventSub host");
    return result;
}

void sleepBriefly() {
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
}

class RawWebSocket {
public:
    explicit RawWebSocket(const std::string& url) {
        const ParsedUrl parsed = parseWebSocketUrl(url);
        host = parsed.host;
        path = parsed.path;

        easy = curl_easy_init();
        if (!easy) throw std::runtime_error("Could not initialize chat transport");

        curl_easy_setopt(easy, CURLOPT_URL, parsed.curlUrl.c_str());
        curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 1L);
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 15000L);

        const CURLcode connected = curl_easy_perform(easy);
        if (connected != CURLE_OK)
            throw std::runtime_error(
                std::string("Chat connection failed: ") +
                curl_easy_strerror(connected));

        std::array<uint8_t, 16> nonce{};
        std::random_device random;
        for (auto& byte : nonce)
            byte = static_cast<uint8_t>(random() & 0xFF);

        const std::string key = base64Encode(nonce.data(), nonce.size());
        const std::string request =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "User-Agent: TwiNX/0.4.0 (Nintendo Switch)\r\n\r\n";

        sendRaw(
            reinterpret_cast<const uint8_t*>(request.data()),
            request.size());

        std::string response;
        std::array<uint8_t, 2048> buffer{};
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);

        while (response.find("\r\n\r\n") == std::string::npos) {
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("Timed out during chat WebSocket handshake");

            size_t received = 0;
            const CURLcode code =
                curl_easy_recv(easy, buffer.data(), buffer.size(), &received);
            if (code == CURLE_AGAIN) {
                sleepBriefly();
                continue;
            }
            if (code != CURLE_OK)
                throw std::runtime_error(
                    std::string("Chat handshake failed: ") +
                    curl_easy_strerror(code));

            response.append(
                reinterpret_cast<const char*>(buffer.data()),
                received);
        }

        if (response.find(" 101 ") == std::string::npos)
            throw std::runtime_error(
                "Twitch rejected the chat WebSocket handshake");

        const auto end = response.find("\r\n\r\n");
        if (end != std::string::npos && end + 4 < response.size()) {
            const auto* first =
                reinterpret_cast<const uint8_t*>(response.data() + end + 4);
            receiveBuffer.insert(
                receiveBuffer.end(),
                first,
                first + response.size() - end - 4);
        }
    }

    ~RawWebSocket() {
        if (easy) curl_easy_cleanup(easy);
    }

    RawWebSocket(const RawWebSocket&) = delete;
    RawWebSocket& operator=(const RawWebSocket&) = delete;

    bool nextText(
        std::string& text,
        std::atomic_bool& stopRequested) {
        while (!stopRequested.load()) {
            if (parseFrames(text)) return true;

            std::array<uint8_t, 8192> data{};
            size_t received = 0;
            const CURLcode code =
                curl_easy_recv(easy, data.data(), data.size(), &received);

            if (code == CURLE_AGAIN) {
                sleepBriefly();
                continue;
            }
            if (code != CURLE_OK)
                throw std::runtime_error(
                    std::string("Chat receive failed: ") +
                    curl_easy_strerror(code));
            if (received == 0)
                throw std::runtime_error("Twitch closed the chat connection");

            receiveBuffer.insert(
                receiveBuffer.end(),
                data.begin(),
                data.begin() + static_cast<std::ptrdiff_t>(received));
        }
        return false;
    }

    void close() {
        try {
            sendFrame(0x8, "");
        } catch (...) {
        }
    }

private:
    void sendRaw(const uint8_t* data, size_t size) {
        size_t offset = 0;
        while (offset < size) {
            size_t sent = 0;
            const CURLcode code =
                curl_easy_send(easy, data + offset, size - offset, &sent);
            if (code == CURLE_AGAIN) {
                sleepBriefly();
                continue;
            }
            if (code != CURLE_OK)
                throw std::runtime_error(
                    std::string("Chat send failed: ") +
                    curl_easy_strerror(code));
            offset += sent;
        }
    }

    void sendFrame(uint8_t opcode, const std::string& payload) {
        std::vector<uint8_t> frame;
        frame.push_back(static_cast<uint8_t>(0x80 | opcode));

        const size_t length = payload.size();
        if (length <= 125) {
            frame.push_back(static_cast<uint8_t>(0x80 | length));
        } else if (length <= 0xFFFF) {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(length & 0xFF));
        } else {
            frame.push_back(0x80 | 127);
            for (int shift = 56; shift >= 0; shift -= 8)
                frame.push_back(
                    static_cast<uint8_t>((length >> shift) & 0xFF));
        }

        std::random_device random;
        std::array<uint8_t, 4> mask = {
            static_cast<uint8_t>(random() & 0xFF),
            static_cast<uint8_t>(random() & 0xFF),
            static_cast<uint8_t>(random() & 0xFF),
            static_cast<uint8_t>(random() & 0xFF),
        };
        frame.insert(frame.end(), mask.begin(), mask.end());

        for (size_t index = 0; index < payload.size(); ++index)
            frame.push_back(
                static_cast<uint8_t>(payload[index]) ^ mask[index % 4]);

        sendRaw(frame.data(), frame.size());
    }

    bool parseFrames(std::string& text) {
        while (receiveBuffer.size() >= 2) {
            const uint8_t first = receiveBuffer[0];
            const uint8_t second = receiveBuffer[1];
            const bool final = (first & 0x80) != 0;
            const uint8_t opcode = first & 0x0F;
            const bool masked = (second & 0x80) != 0;

            uint64_t length = second & 0x7F;
            size_t header = 2;

            if (length == 126) {
                if (receiveBuffer.size() < 4) return false;
                length =
                    (static_cast<uint64_t>(receiveBuffer[2]) << 8) |
                    receiveBuffer[3];
                header = 4;
            } else if (length == 127) {
                if (receiveBuffer.size() < 10) return false;
                length = 0;
                for (size_t index = 2; index < 10; ++index)
                    length = (length << 8) | receiveBuffer[index];
                header = 10;
            }

            const size_t maskBytes = masked ? 4 : 0;
            if (length > 8 * 1024 * 1024)
                throw std::runtime_error("Chat frame is too large");
            if (receiveBuffer.size() < header + maskBytes + length)
                return false;

            std::array<uint8_t, 4> mask{};
            if (masked) {
                for (size_t index = 0; index < 4; ++index)
                    mask[index] = receiveBuffer[header + index];
            }

            const size_t payloadOffset = header + maskBytes;
            std::string payload;
            payload.resize(static_cast<size_t>(length));
            for (size_t index = 0; index < length; ++index) {
                uint8_t value = receiveBuffer[payloadOffset + index];
                if (masked) value ^= mask[index % 4];
                payload[index] = static_cast<char>(value);
            }

            receiveBuffer.erase(
                receiveBuffer.begin(),
                receiveBuffer.begin() +
                    static_cast<std::ptrdiff_t>(
                        payloadOffset + static_cast<size_t>(length)));

            if (opcode == 0x8) {
                throw std::runtime_error("Twitch closed the chat session");
            }
            if (opcode == 0x9) {
                sendFrame(0xA, payload);
                continue;
            }
            if (opcode == 0xA) continue;

            if (opcode == 0x1) {
                fragment = payload;
                fragmentOpcode = opcode;
            } else if (opcode == 0x0 && fragmentOpcode == 0x1) {
                fragment += payload;
            } else {
                continue;
            }

            if (final) {
                text = std::move(fragment);
                fragment.clear();
                fragmentOpcode = 0;
                return true;
            }
        }
        return false;
    }

    CURL* easy = nullptr;
    std::string host;
    std::string path;
    std::vector<uint8_t> receiveBuffer;
    std::string fragment;
    uint8_t fragmentOpcode = 0;
};

using BadgeImageMap =
    std::unordered_map<std::string, std::string>;

std::string badgeKey(
    const std::string& setId,
    const std::string& versionId) {
    return setId + "\x1f" + versionId;
}

void mergeBadgeCatalogue(
    const std::string& response,
    BadgeImageMap& images) {
    const json root = json::parse(response);
    if (!root.contains("data") ||
        !root["data"].is_array())
        return;

    for (const auto& set : root["data"]) {
        const std::string setId =
            set.value("set_id", "");
        if (setId.empty() ||
            !set.contains("versions") ||
            !set["versions"].is_array())
            continue;

        for (const auto& version : set["versions"]) {
            const std::string versionId =
                version.value("id", "");
            if (versionId.empty()) continue;

            std::string image =
                version.value("image_url_2x", "");
            if (image.empty())
                image = version.value(
                    "image_url_1x", "");
            if (image.empty()) continue;

            images[badgeKey(setId, versionId)] =
                std::move(image);
        }
    }
}

BadgeImageMap loadBadgeCatalogue(
    const Config& config,
    const OAuthSession& session,
    const std::string& broadcasterId) {
    BadgeImageMap result;

    const HTTP::Header headers = {
        "Authorization: Bearer " +
            session.accessToken,
        "Client-Id: " +
            config.oauthClientId,
        "Accept: application/json",
    };

    try {
        mergeBadgeCatalogue(
            HTTP::get(
                "https://api.twitch.tv/"
                "helix/chat/badges/global",
                headers,
                HTTP::Timeout{15000}),
            result);
    } catch (const std::exception& exception) {
        brls::Logger::warning(
            "TwiNX: global Twitch badge catalogue "
            "could not be loaded: {}",
            exception.what());
    }

    try {
        mergeBadgeCatalogue(
            HTTP::get(
                "https://api.twitch.tv/"
                "helix/chat/badges?"
                "broadcaster_id=" +
                    percentEncode(broadcasterId),
                headers,
                HTTP::Timeout{15000}),
            result);
    } catch (const std::exception& exception) {
        brls::Logger::warning(
            "TwiNX: channel Twitch badge catalogue "
            "could not be loaded: {}",
            exception.what());
    }

    brls::Logger::info(
        "TwiNX: loaded {} Twitch badge versions",
        result.size());

    return result;
}

std::string resolveBroadcasterId(
    const Config& config,
    const OAuthSession& session,
    const std::string& channel) {
    const HTTP::Header headers = {
        "Authorization: Bearer " + session.accessToken,
        "Client-Id: " + config.oauthClientId,
        "Accept: application/json",
    };
    const std::string response = HTTP::get(
        "https://api.twitch.tv/helix/users?login=" +
            percentEncode(channel),
        headers,
        HTTP::Timeout{15000});
    const json root = json::parse(response);
    if (!root.contains("data") ||
        !root["data"].is_array() ||
        root["data"].empty())
        throw std::runtime_error("Twitch channel was not found");

    const std::string id = root["data"].front().value("id", "");
    if (id.empty())
        throw std::runtime_error("Twitch returned no broadcaster ID");
    return id;
}

void createChatSubscription(
    const Config& config,
    const OAuthSession& session,
    const std::string& broadcasterId,
    const std::string& webSocketSessionId) {
    const json body = {
        {"type", "channel.chat.message"},
        {"version", "1"},
        {"condition",
            {
                {"broadcaster_user_id", broadcasterId},
                {"user_id", session.userId},
            }},
        {"transport",
            {
                {"method", "websocket"},
                {"session_id", webSocketSessionId},
            }},
    };

    const HTTP::Header headers = {
        "Authorization: Bearer " + session.accessToken,
        "Client-Id: " + config.oauthClientId,
        "Content-Type: application/json",
        "Accept: application/json",
    };
    HTTP::post(
        "https://api.twitch.tv/helix/eventsub/subscriptions",
        body.dump(),
        headers,
        HTTP::Timeout{15000});
}

}  // namespace

ChatPreferences loadChatPreferences() {
    ChatPreferences result;
    std::ifstream input(settingsPath());
    if (!input.is_open()) return result;

    try {
        const json root = json::parse(input);
        if (!root.is_object()) return result;
        const int mode = root.value(
            "chat_mode",
            static_cast<int>(ChatMode::RightPanel));
        if (mode >= static_cast<int>(ChatMode::Off) &&
            mode <= static_cast<int>(ChatMode::Overlay))
            result.mode = static_cast<ChatMode>(mode);
        result.fontSize = std::clamp(root.value("chat_font_size", 16), 12, 22);
        result.opacity = std::clamp(root.value("chat_opacity", 82), 20, 100);
        result.dockedWidth = std::clamp(
            root.value("chat_docked_width", 330), 280, 420);
        result.overlayWidth = std::clamp(
            root.value("chat_overlay_width", 360), 280, 480);
        const int participation = root.value(
            "chat_participation",
            static_cast<int>(ChatParticipation::ReadOnly));
        if (participation >= static_cast<int>(ChatParticipation::ReadOnly) &&
            participation <= static_cast<int>(ChatParticipation::Interactive))
            result.participation =
                static_cast<ChatParticipation>(participation);

        const int composerMode = root.value(
            "chat_composer_mode",
            static_cast<int>(ChatComposerMode::TwiNXComposer));
        if (composerMode >=
                static_cast<int>(ChatComposerMode::NintendoKeyboard) &&
            composerMode <=
                static_cast<int>(ChatComposerMode::TwiNXComposer))
            result.composerMode =
                static_cast<ChatComposerMode>(composerMode);

        const int emoteMode = root.value(
            "chat_emote_mode",
            static_cast<int>(ChatEmoteMode::Static));
        if (emoteMode >= static_cast<int>(ChatEmoteMode::Static) &&
            emoteMode <= static_cast<int>(ChatEmoteMode::Animated))
            result.emoteMode = static_cast<ChatEmoteMode>(emoteMode);

        const int overlaySize = root.value(
            "chat_overlay_size",
            static_cast<int>(ChatOverlaySize::FullHeight));
        if (overlaySize >= static_cast<int>(ChatOverlaySize::FullHeight) &&
            overlaySize <= static_cast<int>(ChatOverlaySize::Compact))
            result.overlaySize = static_cast<ChatOverlaySize>(overlaySize);

        const int overlayPosition = root.value(
            "chat_overlay_position",
            static_cast<int>(ChatOverlayPosition::TopRight));
        if (overlayPosition >=
                static_cast<int>(ChatOverlayPosition::TopRight) &&
            overlayPosition <=
                static_cast<int>(ChatOverlayPosition::BottomLeft))
            result.overlayPosition =
                static_cast<ChatOverlayPosition>(overlayPosition);

        result.timestamps = root.value("chat_timestamps", false);
    } catch (...) {
    }
    return result;
}

bool saveChatPreferences(const ChatPreferences& preferences) {
    try {
        const fs::path path(settingsPath());
        if (!path.parent_path().empty())
            fs::create_directories(path.parent_path());

        json root = json::object();
        {
            std::ifstream input(path);
            if (input.is_open()) {
                try {
                    const json existing = json::parse(input);
                    if (existing.is_object()) root = existing;
                } catch (...) {
                }
            }
        }

        root["chat_mode"] = static_cast<int>(preferences.mode);
        root["chat_participation"] =
            static_cast<int>(preferences.participation);
        root["chat_composer_mode"] =
            static_cast<int>(preferences.composerMode);
        root["chat_emote_mode"] =
            static_cast<int>(preferences.emoteMode);
        root["chat_overlay_size"] =
            static_cast<int>(preferences.overlaySize);
        root["chat_overlay_position"] =
            static_cast<int>(preferences.overlayPosition);
        root["chat_font_size"] = preferences.fontSize;
        root["chat_opacity"] = preferences.opacity;
        root["chat_docked_width"] = preferences.dockedWidth;
        root["chat_overlay_width"] = preferences.overlayWidth;
        root["chat_timestamps"] = preferences.timestamps;

        const fs::path temporary = path.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output.is_open()) return false;
            output << root.dump(2) << '\n';
            if (!output.good()) return false;
        }

        std::error_code error;
        fs::remove(path, error);
        error.clear();
        fs::rename(temporary, path, error);
        if (error) {
            fs::remove(temporary, error);
            return false;
        }

        chatPreferencesEvent()->fire(preferences);
        return true;
    } catch (...) {
        return false;
    }
}

brls::Event<ChatPreferences>* chatPreferencesEvent() {
    static brls::Event<ChatPreferences> event;
    return &event;
}

bool hasUserEmotesScope() {
    const OAuthSession session = loadOAuth();
    return hasScope(session, "user:read:emotes");
}

std::vector<UserEmote> loadRecentEmotes() {
    std::ifstream input(settingsPath());
    if (!input.is_open()) return {};

    try {
        const json root = json::parse(input);
        if (!root.is_object() ||
            !root.contains("chat_recent_emotes") ||
            !root["chat_recent_emotes"].is_array())
            return {};

        std::vector<UserEmote> result;
        for (const auto& value :
             root["chat_recent_emotes"]) {
            UserEmote emote;

            // Backward compatibility with 0.6.0/0.6.1, which stored
            // only the emote name.
            if (value.is_string()) {
                emote.name =
                    value.get<std::string>();
            } else if (value.is_object()) {
                emote.id =
                    value.value("id", "");
                emote.name =
                    value.value("name", "");
                emote.ownerId =
                    value.value("owner_id", "");
                emote.emoteType =
                    value.value("emote_type", "");
                emote.imageUrl =
                    value.value("image_url", "");
                emote.channelEmote =
                    value.value(
                        "channel_emote", false);
            }

            if (emote.name.empty())
                continue;

            result.push_back(std::move(emote));
            if (result.size() >= 24)
                break;
        }

        return result;
    } catch (...) {
        return {};
    }
}

void rememberRecentEmote(const UserEmote& emote) {
    if (emote.name.empty()) return;

    try {
        const fs::path path(settingsPath());
        if (!path.parent_path().empty())
            fs::create_directories(
                path.parent_path());

        json root = json::object();
        {
            std::ifstream input(path);
            if (input.is_open()) {
                try {
                    const json existing =
                        json::parse(input);
                    if (existing.is_object())
                        root = existing;
                } catch (...) {
                }
            }
        }

        auto serialize = [](const UserEmote& value) {
            return json{
                {"id", value.id},
                {"name", value.name},
                {"owner_id", value.ownerId},
                {"emote_type", value.emoteType},
                {"image_url", value.imageUrl},
                {"channel_emote", value.channelEmote},
            };
        };

        json saved = json::array();
        saved.push_back(serialize(emote));

        if (root.contains("chat_recent_emotes") &&
            root["chat_recent_emotes"].is_array()) {
            for (const auto& value :
                 root["chat_recent_emotes"]) {
                UserEmote current;

                if (value.is_string()) {
                    current.name =
                        value.get<std::string>();
                } else if (value.is_object()) {
                    current.id =
                        value.value("id", "");
                    current.name =
                        value.value("name", "");
                    current.ownerId =
                        value.value("owner_id", "");
                    current.emoteType =
                        value.value("emote_type", "");
                    current.imageUrl =
                        value.value("image_url", "");
                    current.channelEmote =
                        value.value(
                            "channel_emote", false);
                }

                if (current.name.empty())
                    continue;

                const bool duplicate =
                    (!emote.id.empty() &&
                     !current.id.empty() &&
                     emote.id == current.id) ||
                    current.name == emote.name;

                if (duplicate)
                    continue;

                saved.push_back(
                    serialize(current));
                if (saved.size() >= 24)
                    break;
            }
        }

        root["chat_recent_emotes"] =
            std::move(saved);

        const fs::path temporary =
            path.string() + ".tmp";
        {
            std::ofstream output(
                temporary,
                std::ios::trunc);
            if (!output.is_open()) return;
            output << root.dump(2) << '\n';
            if (!output.good()) return;
        }

        std::error_code error;
        fs::remove(path, error);
        error.clear();
        fs::rename(temporary, path, error);
        if (error)
            fs::remove(temporary, error);
    } catch (...) {
    }
}

void loadUserEmotesAsync(
    const std::string& channel,
    UserEmotesCallback success,
    ErrorCallback failure) {
    ThreadPool::instance().submit(
        [channel,
         success = std::move(success),
         failure = std::move(failure)](HTTP&) mutable {
            try {
                const Config config = loadConfig();
                const OAuthSession session =
                    ensureOAuth(config);

                if (!hasScope(
                        session,
                        "user:read:emotes")) {
                    throw std::runtime_error(
                        "Emote picker requires signing out "
                        "and signing in again once");
                }

                std::string broadcasterId;
                const std::string normalized =
                    normalizeChannel(channel);
                if (!normalized.empty()) {
                    broadcasterId =
                        resolveBroadcasterId(
                            config,
                            session,
                            normalized);
                }

                const HTTP::Header headers = {
                    "Authorization: Bearer " +
                        session.accessToken,
                    "Client-Id: " +
                        config.oauthClientId,
                    "Accept: application/json",
                };

                const bool subscriptionPermissionGranted =
                    hasScope(
                        session,
                        "user:read:subscriptions");

                bool channelSubscribed =
                    !broadcasterId.empty() &&
                    session.userId == broadcasterId;

                if (!channelSubscribed &&
                    !broadcasterId.empty() &&
                    subscriptionPermissionGranted) {
                    try {
                        const std::string subscriptionUrl =
                            "https://api.twitch.tv/"
                            "helix/subscriptions/user?"
                            "broadcaster_id=" +
                            percentEncode(broadcasterId) +
                            "&user_id=" +
                            percentEncode(session.userId);

                        const json subscriptionRoot =
                            json::parse(
                                HTTP::get(
                                    subscriptionUrl,
                                    headers,
                                    HTTP::Timeout{15000}));

                        channelSubscribed =
                            subscriptionRoot.contains("data") &&
                            subscriptionRoot["data"].is_array() &&
                            !subscriptionRoot["data"].empty();
                    } catch (const std::exception& exception) {
                        // Twitch returns 404 when the user is not subscribed.
                        // Any other failure is logged and treated
                        // conservatively as not subscribed.
                        const std::string reason =
                            exception.what();
                        if (reason != "http status 404") {
                            brls::Logger::warning(
                                "TwiNX: subscription check failed: {}",
                                reason);
                        }
                        channelSubscribed = false;
                    }
                }

                std::vector<UserEmote> emotes;
                std::set<std::string> seen;
                std::string cursor;
                constexpr size_t MAX_EMOTES = 400;

                while (emotes.size() < MAX_EMOTES) {
                    std::string url =
                        "https://api.twitch.tv/"
                        "helix/chat/emotes/user?"
                        "user_id=" +
                        percentEncode(session.userId);

                    // broadcaster_id is intentionally supplied only for
                    // confirmed subscribers/owners. This prevents Twitch from
                    // returning channel-specific/follower candidates that the
                    // current account cannot reliably use or render.
                    if (channelSubscribed &&
                        !broadcasterId.empty()) {
                        url += "&broadcaster_id=" +
                            percentEncode(
                                broadcasterId);
                    }

                    if (!cursor.empty()) {
                        url += "&after=" +
                            percentEncode(cursor);
                    }

                    const json root = json::parse(
                        HTTP::get(
                            url,
                            headers,
                            HTTP::Timeout{15000}));

                    const std::string imageTemplate =
                        root.value("template", "");

                    if (root.contains("data") &&
                        root["data"].is_array()) {
                        for (const auto& source :
                             root["data"]) {
                            UserEmote emote;
                            emote.id =
                                source.value("id", "");
                            emote.name =
                                source.value("name", "");
                            emote.ownerId =
                                source.value(
                                    "owner_id", "");
                            emote.emoteType =
                                source.value(
                                    "emote_type", "");

                            bool supportsStatic = true;
                            if (source.contains("format") &&
                                source["format"].is_array()) {
                                supportsStatic = false;
                                for (const auto& value :
                                     source["format"]) {
                                    if (value.is_string() &&
                                        value.get<std::string>() ==
                                            "static") {
                                        supportsStatic = true;
                                        break;
                                    }
                                }
                            }

                            if (emote.id.empty() ||
                                emote.name.empty() ||
                                !supportsStatic ||
                                !seen.insert(
                                    emote.id).second)
                                continue;

                            emote.imageUrl =
                                userEmoteImageUrl(
                                    imageTemplate,
                                    emote.id);
                            emote.channelEmote =
                                channelSubscribed &&
                                !broadcasterId.empty() &&
                                emote.ownerId ==
                                    broadcasterId;

                            // When not subscribed, discard broadcaster-owned
                            // candidates entirely. General/global/account
                            // emotes remain available under All and Recent.
                            if (!channelSubscribed &&
                                !broadcasterId.empty() &&
                                emote.ownerId ==
                                    broadcasterId)
                                continue;

                            emotes.push_back(
                                std::move(emote));
                            if (emotes.size() >=
                                MAX_EMOTES)
                                break;
                        }
                    }

                    cursor.clear();
                    if (root.contains("pagination") &&
                        root["pagination"].is_object()) {
                        cursor =
                            root["pagination"].value(
                                "cursor", "");
                    }
                    if (cursor.empty()) break;
                }

                std::stable_sort(
                    emotes.begin(),
                    emotes.end(),
                    [](const UserEmote& left,
                       const UserEmote& right) {
                        if (left.channelEmote !=
                            right.channelEmote)
                            return left.channelEmote >
                                right.channelEmote;
                        return left.name < right.name;
                    });

                UserEmoteCatalogue catalogue;
                catalogue.emotes = std::move(emotes);
                catalogue.channelSubscribed =
                    channelSubscribed;
                catalogue.subscriptionPermissionGranted =
                    subscriptionPermissionGranted;

                brls::sync(std::bind(
                    success,
                    std::move(catalogue)));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(
                    failure,
                    std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(
                    failure,
                    std::string(
                        "Unknown Twitch emote error")));
            }
        });
}

bool hasChatWriteScope() {
    const OAuthSession session = loadOAuth();
    return hasScope(session, "user:write:chat");
}

void sendChatMessageAsync(
    const std::string& channel,
    const std::string& message,
    ChatSendCallback success,
    ErrorCallback failure) {
    ThreadPool::instance().submit(
        [channel, message, success = std::move(success),
            failure = std::move(failure)](HTTP&) mutable {
            try {
                const std::string normalized = normalizeChannel(channel);
                if (normalized.empty())
                    throw std::runtime_error("No Twitch channel is active");
                if (message.empty())
                    throw std::runtime_error("The chat message is empty");
                if (message.size() > 500)
                    throw std::runtime_error(
                        "Twitch chat messages are limited to 500 characters");

                const Config config = loadConfig();
                const OAuthSession session = ensureOAuth(config);
                if (!hasScope(session, "user:write:chat"))
                    throw std::runtime_error(
                        "Chat interaction requires signing out and signing "
                        "in again once");

                const std::string broadcasterId =
                    resolveBroadcasterId(config, session, normalized);

                const json body = {
                    {"broadcaster_id", broadcasterId},
                    {"sender_id", session.userId},
                    {"message", message},
                };

                const HTTP::Header headers = {
                    "Authorization: Bearer " + session.accessToken,
                    "Client-Id: " + config.oauthClientId,
                    "Content-Type: application/json",
                    "Accept: application/json",
                };

                const std::string response = HTTP::post(
                    "https://api.twitch.tv/helix/chat/messages",
                    body.dump(),
                    headers,
                    HTTP::Timeout{15000});

                const json root = json::parse(response);
                if (!root.contains("data") ||
                    !root["data"].is_array() ||
                    root["data"].empty())
                    throw std::runtime_error(
                        "Twitch returned no chat-send result");

                const auto& result = root["data"].front();
                if (!result.value("is_sent", false)) {
                    std::string reason = "Twitch rejected the message";
                    if (result.contains("drop_reason") &&
                        result["drop_reason"].is_object()) {
                        reason = result["drop_reason"].value(
                            "message", reason);
                    }
                    throw std::runtime_error(reason);
                }

                const std::string messageId =
                    result.value("message_id", "");
                brls::sync(std::bind(success, messageId));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(
                    failure, std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(
                    failure,
                    std::string("Unknown Twitch chat-send error")));
            }
        });
}

ChatClient::ChatClient(
    std::string channel,
    ChatMessageCallback message,
    ChatStatusCallback status)
    : channel(normalizeChannel(std::move(channel))),
      messageCallback(std::move(message)),
      statusCallback(std::move(status)) {
#ifdef BOREALIS_USE_STD_THREAD
    thread = std::make_shared<std::thread>(&ChatClient::threadEntry, this);
#else
    if (pthread_create(&thread, nullptr, &ChatClient::threadEntry, this) == 0)
        threadStarted = true;
#endif
}

ChatClient::~ChatClient() {
    stop();
}

void ChatClient::stop() {
    if (stopRequested.exchange(true)) return;
#ifdef BOREALIS_USE_STD_THREAD
    if (thread && thread->joinable()) thread->join();
    thread.reset();
#else
    if (threadStarted) {
        pthread_join(thread, nullptr);
        threadStarted = false;
    }
#endif
}

void* ChatClient::threadEntry(void* context) {
    static_cast<ChatClient*>(context)->run();
    return nullptr;
}

void ChatClient::run() {
    int retrySeconds = 1;
    std::string connectionUrl = DEFAULT_EVENTSUB_URL;
    std::deque<std::string> recentIds;
    std::set<std::string> recentIdSet;

    while (!stopRequested.load()) {
        try {
            if (statusCallback) statusCallback("Connecting to Twitch chat…");

            const Config config = loadConfig();
            const OAuthSession session = ensureOAuth(config);
            if (!hasScope(session, "user:read:chat")) {
                if (statusCallback)
                    statusCallback(
                        "Chat permission required: sign out, then sign in again.");
                return;
            }

            const std::string broadcasterId =
                resolveBroadcasterId(config, session, channel);
            const BadgeImageMap badgeImages =
                loadBadgeCatalogue(
                    config,
                    session,
                    broadcasterId);

            RawWebSocket socket(connectionUrl);
            connectionUrl = DEFAULT_EVENTSUB_URL;
            retrySeconds = 1;
            bool subscribed = false;

            while (!stopRequested.load()) {
                std::string raw;
                if (!socket.nextText(raw, stopRequested)) break;

                const json root = json::parse(raw);
                const std::string messageType =
                    root.value("metadata", json::object())
                        .value("message_type", "");
                const std::string messageId =
                    root.value("metadata", json::object())
                        .value("message_id", "");

                if (!messageId.empty()) {
                    if (recentIdSet.count(messageId)) continue;
                    recentIdSet.insert(messageId);
                    recentIds.push_back(messageId);
                    while (recentIds.size() > MAX_DEDUP_IDS) {
                        recentIdSet.erase(recentIds.front());
                        recentIds.pop_front();
                    }
                }

                if (messageType == "session_welcome") {
                    const std::string sessionId =
                        root["payload"]["session"].value("id", "");
                    if (sessionId.empty())
                        throw std::runtime_error(
                            "Twitch chat welcome contained no session ID");
                    createChatSubscription(
                        config,
                        session,
                        broadcasterId,
                        sessionId);
                    subscribed = true;
                    if (statusCallback) statusCallback("Live chat");
                    continue;
                }

                if (messageType == "session_keepalive") {
                    if (subscribed && statusCallback)
                        statusCallback("Live chat");
                    continue;
                }

                if (messageType == "session_reconnect") {
                    connectionUrl =
                        root["payload"]["session"].value(
                            "reconnect_url",
                            DEFAULT_EVENTSUB_URL);
                    if (statusCallback)
                        statusCallback("Reconnecting to Twitch chat…");
                    socket.close();
                    break;
                }

                if (messageType == "revocation") {
                    const std::string reason =
                        root["payload"]["subscription"].value(
                            "status",
                            "revoked");
                    throw std::runtime_error(
                        "Twitch chat subscription " + reason);
                }

                if (messageType != "notification") continue;

                const std::string subscriptionType =
                    root.value("metadata", json::object())
                        .value("subscription_type", "");
                if (subscriptionType != "channel.chat.message") continue;

                const auto& event = root["payload"]["event"];
                ChatMessage message;
                message.id = messageId;
                message.userName =
                    event.value(
                        "chatter_user_name",
                        event.value("chatter_user_login", "viewer"));
                message.color = event.value("color", "");
                message.timestamp = utcTimestamp();

                const json* badgeArray = nullptr;
                if (event.contains("badges") &&
                    event["badges"].is_array()) {
                    badgeArray = &event["badges"];
                } else if (
                    event.contains("source_badges") &&
                    event["source_badges"].is_array()) {
                    badgeArray = &event["source_badges"];
                }

                if (badgeArray) {
                    for (const auto& source : *badgeArray) {
                        ChatBadge badge;
                        badge.setId =
                            source.value("set_id", "");
                        badge.id =
                            source.value("id", "");
                        badge.info =
                            source.value("info", "");

                        const auto found =
                            badgeImages.find(
                                badgeKey(
                                    badge.setId,
                                    badge.id));
                        if (found != badgeImages.end())
                            badge.imageUrl =
                                found->second;

                        if (!badge.setId.empty() &&
                            !badge.id.empty())
                            message.badges.push_back(
                                std::move(badge));

                        // The official clients keep the badge
                        // prefix compact. A hard cap also protects
                        // narrow layouts from pathological payloads.
                        if (message.badges.size() >= 8)
                            break;
                    }
                }

                if (event.contains("message") &&
                    event["message"].is_object()) {
                    const auto& payloadMessage = event["message"];
                    message.text = payloadMessage.value("text", "");

                    if (payloadMessage.contains("fragments") &&
                        payloadMessage["fragments"].is_array()) {
                        for (const auto& source : payloadMessage["fragments"]) {
                            ChatFragment fragment;
                            fragment.text = source.value("text", "");
                            const std::string type =
                                source.value("type", "text");

                            if (type == "emote" &&
                                source.contains("emote") &&
                                source["emote"].is_object()) {
                                fragment.type = ChatFragmentType::Emote;
                                const auto& emote = source["emote"];
                                fragment.emoteId = emote.value("id", "");

                                bool staticAvailable = true;
                                bool animatedAvailable = false;
                                if (emote.contains("format") &&
                                    emote["format"].is_array()) {
                                    staticAvailable = false;
                                    for (const auto& value : emote["format"]) {
                                        if (!value.is_string()) continue;
                                        const std::string format =
                                            value.get<std::string>();
                                        if (format == "static")
                                            staticAvailable = true;
                                        else if (format == "animated")
                                            animatedAvailable = true;
                                    }
                                }

                                if (!fragment.emoteId.empty()) {
                                    // Rendering an incoming emote is based only
                                    // on the message fragment metadata. The local
                                    // viewer does not need permission to send it.
                                    if (staticAvailable)
                                        fragment.emoteUrl =
                                            chatEmoteImageUrl(
                                                fragment.emoteId,
                                                "static");
                                    if (animatedAvailable)
                                        fragment.animatedEmoteUrl =
                                            chatEmoteImageUrl(
                                                fragment.emoteId,
                                                "animated");
                                }
                            } else if (type == "mention") {
                                fragment.type = ChatFragmentType::Mention;
                            } else if (type == "cheermote") {
                                fragment.type = ChatFragmentType::Cheermote;
                            } else {
                                fragment.type = ChatFragmentType::Text;
                            }

                            message.fragments.push_back(std::move(fragment));
                        }
                    }
                }

                if (message.fragments.empty() && !message.text.empty()) {
                    ChatFragment fallback;
                    fallback.type = ChatFragmentType::Text;
                    fallback.text = message.text;
                    message.fragments.push_back(std::move(fallback));
                }

                if (!message.text.empty() && messageCallback)
                    messageCallback(std::move(message));
            }
        } catch (const std::exception& exception) {
            if (stopRequested.load()) break;
            if (statusCallback)
                statusCallback(
                    std::string("Chat reconnecting: ") + exception.what());
        } catch (...) {
            if (stopRequested.load()) break;
            if (statusCallback)
                statusCallback("Chat reconnecting after an unknown error");
        }

        if (stopRequested.load()) break;
        for (int second = 0;
             second < retrySeconds && !stopRequested.load();
             ++second)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        retrySeconds = std::min(retrySeconds * 2, 20);
    }
}

}  // namespace twitch
