#include "api/twitch.hpp"

#include "api/http.hpp"
#include "utils/thread.hpp"

#include <borealis.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace twitch {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

std::string settingsPath() {
#if defined(__SWITCH__)
    return "sdmc:/config/TwiNX/settings.json";
#else
    return "settings.json";
#endif
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

void applyConfigLine(Config& config, const std::string& rawLine) {
    std::string line = trim(rawLine);
    if (line.empty() || line.front() == '#' || line.front() == ';') return;

    const auto equals = line.find('=');
    if (equals == std::string::npos) {
        if (config.channel.empty()) config.channel = normalizeChannel(line);
        return;
    }

    std::string key = trim(line.substr(0, equals));
    std::string value = trim(line.substr(equals + 1));
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (key == "channel") config.channel = normalizeChannel(value);
    else if (key == "preferred_quality" || key == "quality") config.preferredQuality = value;
    else if (key == "player_type") config.playerType = value;
    else if (key == "oauth_client_id") config.oauthClientId = value;
    else if (key == "oauth_scopes") config.oauthScopes = value;
    else if (key == "gql_client_id" || key == "client_id") config.gqlClientId = value;
    else if (key == "playback_hash") config.playbackHash = value;
}

std::string postPlaybackToken(const Config& config) {
    json variables = {
        {"isLive", true},
        {"isVod", false},
        {"login", config.channel},
        {"vodID", ""},
        {"playerType", config.playerType.empty() ? json(nullptr) : json(config.playerType)},
    };

    json request = json::array({
        {
            {"operationName", "PlaybackAccessToken"},
            {"variables", variables},
            {"extensions", {
                {"persistedQuery", {
                    {"version", 1},
                    {"sha256Hash", config.playbackHash},
                }},
            }},
        },
    });

    HTTP::Header headers = {
        "Client-ID: " + config.gqlClientId,
        "Content-Type: application/json",
        "Accept: application/json",
        "Origin: https://www.twitch.tv",
        "Referer: https://www.twitch.tv/",
    };
    return HTTP::post(
        "https://gql.twitch.tv/gql",
        request.dump(),
        headers,
        HTTP::Timeout{15000});
}

std::pair<std::string, std::string> parsePlaybackToken(const std::string& response) {
    json root;
    try {
        root = json::parse(response);
    } catch (...) {
        throw std::runtime_error("Twitch GraphQL returned invalid JSON");
    }

    if (!root.is_array() || root.empty()) {
        throw std::runtime_error("Twitch GraphQL returned an unexpected response");
    }

    const auto& first = root.front();
    if (first.contains("errors") && first["errors"].is_array() && !first["errors"].empty()) {
        std::string message = "Twitch rejected the playback-token request";
        const auto& error = first["errors"].front();
        if (error.is_object() && error.contains("message") && error["message"].is_string())
            message += ": " + error["message"].get<std::string>();
        throw std::runtime_error(message);
    }

    const auto data = first.find("data");
    if (data == first.end() || !data->is_object())
        throw std::runtime_error("Twitch returned no playback-token data");

    const auto token = data->find("streamPlaybackAccessToken");
    if (token == data->end() || token->is_null())
        throw std::runtime_error("The channel is offline, unavailable, or Twitch returned no live token");

    const std::string value = token->value("value", "");
    const std::string signature = token->value("signature", "");
    if (value.empty() || signature.empty())
        throw std::runtime_error("Twitch returned an incomplete playback token");

    return {value, signature};
}

std::string buildMasterUrl(
    const Config& config,
    const std::string& token,
    const std::string& signature) {
    std::random_device device;
    const int randomValue = static_cast<int>(device() % 6);
    return "https://usher.ttvnw.net/api/channel/hls/" + config.channel +
        ".m3u8?player=twitchweb"
        "&token=" + percentEncode(token) +
        "&sig=" + percentEncode(signature) +
        "&allow_audio_only=true"
        "&allow_source=true"
        "&type=any"
        "&fast_bread=true"
        "&p=" + std::to_string(randomValue);
}

std::string getMasterPlaylist(const std::string& url) {
    HTTP::Header headers = {
        "Referer: https://player.twitch.tv",
        "Origin: https://player.twitch.tv",
        "Accept: application/vnd.apple.mpegurl, application/x-mpegURL, */*",
    };
    return HTTP::get(url, headers, HTTP::Timeout{15000});
}

}  // namespace

std::string loadPreferredQuality() {
    std::ifstream input(settingsPath());
    if (!input.is_open()) return {};

    try {
        const json root = json::parse(input);
        if (!root.is_object()) return {};
        return trim(root.value("preferred_quality", ""));
    } catch (...) {
        return {};
    }
}

bool savePreferredQuality(const std::string& quality) {
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

        root["preferred_quality"] = quality;
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
        return true;
    } catch (...) {
        return false;
    }
}

DecoderMode loadDecoderMode() {
    std::ifstream input(settingsPath());
    if (!input.is_open()) return DecoderMode::Software;

    try {
        const json root = json::parse(input);
        if (!root.is_object()) return DecoderMode::Software;

        if (root.contains("twitch_decoder_mode")) {
            const auto& stored = root.at("twitch_decoder_mode");
            if (stored.is_number_integer()) {
                const int value = stored.get<int>();
                if (value >= static_cast<int>(DecoderMode::Software) &&
                    value <= static_cast<int>(DecoderMode::Hybrid))
                    return static_cast<DecoderMode>(value);
            } else if (stored.is_string()) {
                std::string value = stored.get<std::string>();
                std::transform(value.begin(), value.end(), value.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (value == "hardware") return DecoderMode::Hardware;
                if (value == "hybrid") return DecoderMode::Hybrid;
                return DecoderMode::Software;
            }
        }

        // Migrate the former two-state preference without changing an existing
        // user's selection: true remains Hardware, false remains Software.
        return root.value("twitch_hardware_decode", false)
            ? DecoderMode::Hardware
            : DecoderMode::Software;
    } catch (...) {
        return DecoderMode::Software;
    }
}

bool decoderUsesHardware(DecoderMode mode) {
    return mode == DecoderMode::Hardware || mode == DecoderMode::Hybrid;
}

bool saveDecoderMode(DecoderMode mode) {
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

        root["twitch_decoder_mode"] = static_cast<int>(mode);
        // Keep the legacy key coherent for older builds that read the same SD
        // card. Hybrid is hardware-capable, so it maps to true there.
        root["twitch_hardware_decode"] = decoderUsesHardware(mode);

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
        return true;
    } catch (...) {
        return false;
    }
}

bool loadHardwareDecodeEnabled() {
    return decoderUsesHardware(loadDecoderMode());
}

bool saveHardwareDecodeEnabled(bool enabled) {
    return saveDecoderMode(
        enabled ? DecoderMode::Hardware : DecoderMode::Software);
}

Config loadConfig() {
    Config config;
    const std::vector<std::string> candidates = {
#if defined(__SWITCH__)
        "sdmc:/switch/twinx.txt",
        "sdmc:/config/TwiNX/twinx.txt",
#endif
        "twinx.txt",
    };

    for (const auto& path : candidates) {
        std::ifstream input(path);
        if (!input.is_open()) continue;
        for (std::string line; std::getline(input, line);) applyConfigLine(config, line);
        if (!config.channel.empty()) break;
    }
    const std::string savedQuality = loadPreferredQuality();
    if (!savedQuality.empty()) config.preferredQuality = savedQuality;
    config.channel = normalizeChannel(config.channel);
    return config;
}

Resolution resolveLive(const Config& config) {
    Config clean = config;
    // The embedded-player identity avoids Twitch's stitched commercial
    // presentation and its disruptive media timeline transitions. Force it
    // here so a legacy twinx.txt value cannot silently restore site playback.
    clean.playerType = "embed";
    brls::Logger::info("Twitch live resolver player type: {}", clean.playerType);
    clean.channel = normalizeChannel(clean.channel);
    if (clean.channel.empty())
        throw std::runtime_error("No Twitch channel is configured in /switch/twinx.txt");
    if (clean.gqlClientId.empty() || clean.playbackHash.empty())
        throw std::runtime_error("The GraphQL client ID or playback hash is empty");

    const auto response = postPlaybackToken(clean);
    const auto [token, signature] = parsePlaybackToken(response);
    const std::string masterUrl = buildMasterUrl(clean, token, signature);
    const std::string playlist = getMasterPlaylist(masterUrl);
    auto qualities = parseMasterPlaylist(masterUrl, playlist);

    if (qualities.size() <= 1) {
        throw std::runtime_error(
            "Twitch returned a master playlist without playable variants");
    }

    Resolution result;
    result.channel = clean.channel;
    result.masterUrl = masterUrl;
    result.selected = selectQuality(qualities, clean.preferredQuality);
    result.qualities = std::move(qualities);
    return result;
}

void resolveLiveAsync(
    const Config& config,
    ResolveCallback success,
    ErrorCallback failure) {
    ThreadPool::instance().submit(
        [config, success = std::move(success), failure = std::move(failure)](HTTP&) mutable {
            try {
                auto result = resolveLive(config);
                brls::sync(std::bind(std::move(success), std::move(result)));
            } catch (const std::exception& exception) {
                const std::string message = exception.what();
                brls::sync(std::bind(std::move(failure), message));
            } catch (...) {
                brls::sync(std::bind(
                    std::move(failure),
                    std::string("Unknown Twitch resolver error")));
            }
        });
}

std::string mpvExtra() {
    std::vector<std::string> options = {
        "network-timeout=15",
        "cache=yes",
        "cache-secs=8",
        "referrer=\"https://player.twitch.tv\"",
        "user-agent=\"Mozilla/5.0 (Nintendo Switch; twiNX/0.9.0)\"",
        "http-header-fields=\"Origin: https://player.twitch.tv\"",
    };

    const DecoderMode decoderMode = loadDecoderMode();
    if (decoderUsesHardware(decoderMode)) {
        // Hardware and Hybrid both begin with the Switch hardware decoder.
        // Direct rendering stays disabled to reduce decoder-owned staging
        // buffer lifetime across Twitch HLS presentation changes.
        options.push_back("vd-lavc-dr=no");
    } else {
        // Stable software mode never inherits the app-wide hardware setting.
        options.push_back("hwdec=no");
    }
#if defined(__SWITCH__)
    options.push_back("tls-ca-file=\"romfs:/cert/cacert.pem\"");
#endif

    std::string result;
    for (const auto& option : options) {
        if (!result.empty()) result += ",";
        result += option;
    }
    return result;
}

std::string describeQualities(const std::vector<Quality>& qualities) {
    std::string result;
    for (const auto& quality : qualities) {
        if (quality.id == "auto") continue;
        if (!result.empty()) result += ", ";
        result += quality.name;
    }
    return result.empty() ? "none" : result;
}

}  // namespace twitch
