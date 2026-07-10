#include "api/twitch_auth.hpp"

#include "api/http.hpp"
#include "utils/thread.hpp"

#include <borealis.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace twitch {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

struct RawResponse {
    long status = 0;
    std::string body;
};

std::string oauthPath() {
#if defined(__SWITCH__)
    return "sdmc:/config/TwiNX/oauth.json";
#else
    return "oauth.json";
#endif
}

size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    const size_t count = size * nmemb;
    body->append(ptr, count);
    return count;
}

RawResponse postFormRaw(const std::string& url, const HTTP::Form& form) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Could not initialize OAuth network request");

    RawResponse response;
    const std::string data = HTTP::encode_form(form);
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(data.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "twiNX/0.8.1 (Nintendo Switch)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    const CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        const std::string error = curl_easy_strerror(result);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        throw std::runtime_error("OAuth request failed: " + error);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

std::vector<std::string> parseScopes(const json& value) {
    std::vector<std::string> scopes;
    if (value.is_array()) {
        for (const auto& item : value)
            if (item.is_string()) scopes.push_back(item.get<std::string>());
    } else if (value.is_string()) {
        scopes.push_back(value.get<std::string>());
    }
    return scopes;
}

OAuthSession parseTokenResponse(const std::string& body) {
    const json root = json::parse(body);
    OAuthSession session;
    session.accessToken = root.value("access_token", "");
    session.refreshToken = root.value("refresh_token", "");
    session.scopes = parseScopes(root.value("scope", json::array()));
    const int expiresIn = root.value("expires_in", 0);
    session.expiresAt = static_cast<long long>(std::time(nullptr)) + expiresIn;
    if (session.accessToken.empty())
        throw std::runtime_error("Twitch returned no access token");
    return session;
}

OAuthSession fetchIdentity(const Config& config, OAuthSession session) {
    HTTP::Header headers = {
        "Authorization: Bearer " + session.accessToken,
        "Client-Id: " + config.oauthClientId,
        "Accept: application/json",
    };
    const std::string response = HTTP::get(
        "https://api.twitch.tv/helix/users", headers, HTTP::Timeout{15000});
    const json root = json::parse(response);
    if (!root.contains("data") || !root["data"].is_array() || root["data"].empty())
        throw std::runtime_error("Twitch returned no account information");

    const auto& user = root["data"].front();
    session.userId = user.value("id", "");
    session.login = user.value("login", "");
    session.displayName = user.value("display_name", session.login);
    session.profileImageUrl = user.value("profile_image_url", "");
    if (session.userId.empty())
        throw std::runtime_error("Twitch account response did not contain a user ID");
    return session;
}

DeviceAuthorization startDeviceAuthorization(const Config& config) {
    if (config.oauthClientId.empty())
        throw std::runtime_error("twiNX OAuth Client ID is not configured");

    const RawResponse response = postFormRaw(
        "https://id.twitch.tv/oauth2/device",
        {{"client_id", config.oauthClientId}, {"scopes", config.oauthScopes}});
    if (response.status >= 400)
        throw std::runtime_error("Twitch device authorization failed (HTTP " +
                                 std::to_string(response.status) + ")");

    const json root = json::parse(response.body);
    DeviceAuthorization result;
    result.deviceCode = root.value("device_code", "");
    result.userCode = root.value("user_code", "");
    result.verificationUri = root.value("verification_uri", "");
    result.expiresIn = root.value("expires_in", 0);
    result.interval = root.value("interval", 5);
    if (result.deviceCode.empty() || result.userCode.empty())
        throw std::runtime_error("Twitch returned an incomplete device authorization response");
    return result;
}

OAuthSession pollDeviceAuthorization(const Config& config, const DeviceAuthorization& auth) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(auth.expiresIn);
    int interval = auth.interval > 0 ? auth.interval : 5;

    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        const RawResponse response = postFormRaw(
            "https://id.twitch.tv/oauth2/token",
            {{"client_id", config.oauthClientId},
             {"scopes", config.oauthScopes},
             {"device_code", auth.deviceCode},
             {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}});

        if (response.status < 400) return parseTokenResponse(response.body);

        std::string message;
        try {
            message = json::parse(response.body).value("message", "");
        } catch (...) {
        }
        if (message == "authorization_pending") continue;
        if (message == "slow_down") {
            interval += 5;
            continue;
        }
        if (message.empty()) message = "HTTP " + std::to_string(response.status);
        throw std::runtime_error("Twitch authorization failed: " + message);
    }
    throw std::runtime_error("Twitch activation code expired. Start login again.");
}

OAuthSession refreshOAuth(const Config& config, const OAuthSession& current) {
    if (current.refreshToken.empty())
        throw std::runtime_error("Twitch session cannot be refreshed. Sign in again.");

    const RawResponse response = postFormRaw(
        "https://id.twitch.tv/oauth2/token",
        {{"client_id", config.oauthClientId},
         {"grant_type", "refresh_token"},
         {"refresh_token", current.refreshToken}});
    if (response.status >= 400) {
        std::string message;
        try {
            message = json::parse(response.body).value("message", "");
        } catch (...) {
        }
        if (message.empty()) message = "HTTP " + std::to_string(response.status);
        throw std::runtime_error("Twitch session refresh failed: " + message);
    }

    OAuthSession refreshed = parseTokenResponse(response.body);
    refreshed.userId = current.userId;
    refreshed.login = current.login;
    refreshed.displayName = current.displayName;
    refreshed.profileImageUrl = current.profileImageUrl;
    refreshed = fetchIdentity(config, std::move(refreshed));
    saveOAuth(refreshed);
    return refreshed;
}

}  // namespace

OAuthSession loadOAuth() {
    std::ifstream input(oauthPath());
    if (!input.is_open()) return {};
    try {
        const json root = json::parse(input);
        OAuthSession session;
        session.accessToken = root.value("access_token", "");
        session.refreshToken = root.value("refresh_token", "");
        session.scopes = parseScopes(root.value("scopes", json::array()));
        session.userId = root.value("user_id", "");
        session.login = root.value("login", "");
        session.displayName = root.value("display_name", "");
        session.profileImageUrl = root.value("profile_image_url", "");
        session.expiresAt = root.value("expires_at", 0LL);
        return session;
    } catch (...) {
        return {};
    }
}

void saveOAuth(const OAuthSession& session) {
    const fs::path path(oauthPath());
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    const json root = {
        {"access_token", session.accessToken},
        {"refresh_token", session.refreshToken},
        {"scopes", session.scopes},
        {"user_id", session.userId},
        {"login", session.login},
        {"display_name", session.displayName},
        {"profile_image_url", session.profileImageUrl},
        {"expires_at", session.expiresAt},
    };
    std::ofstream output(path, std::ios::trunc);
    output << root.dump(2);
}

void clearOAuth() {
    std::error_code error;
    fs::remove(oauthPath(), error);
}

OAuthSession ensureOAuth(const Config& config) {
    OAuthSession session = loadOAuth();
    if (!session.signedIn())
        throw std::runtime_error("Sign in to Twitch first");

    const auto now = static_cast<long long>(std::time(nullptr));
    if (session.expiresAt <= now + 60) session = refreshOAuth(config, session);
    return session;
}

void loginDeviceAsync(
    const Config& config,
    DeviceCodeCallback deviceCode,
    AuthCallback success,
    ErrorCallback failure) {
    ThreadPool::instance().submit(
        [config, deviceCode = std::move(deviceCode), success = std::move(success),
            failure = std::move(failure)](HTTP&) mutable {
            try {
                const DeviceAuthorization authorization = startDeviceAuthorization(config);
                brls::sync(std::bind(deviceCode, authorization));
                OAuthSession session = pollDeviceAuthorization(config, authorization);
                session = fetchIdentity(config, std::move(session));
                saveOAuth(session);
                brls::sync(std::bind(success, session));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(failure, std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(failure, std::string("Unknown Twitch login error")));
            }
        });
}

}  // namespace twitch
