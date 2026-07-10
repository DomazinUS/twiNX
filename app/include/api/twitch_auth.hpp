#pragma once

#include "api/twitch.hpp"

#include <functional>
#include <string>
#include <vector>

namespace twitch {

struct DeviceAuthorization {
    std::string deviceCode;
    std::string userCode;
    std::string verificationUri;
    int expiresIn = 0;
    int interval = 5;
};

struct OAuthSession {
    std::string accessToken;
    std::string refreshToken;
    std::vector<std::string> scopes;
    std::string userId;
    std::string login;
    std::string displayName;
    std::string profileImageUrl;
    long long expiresAt = 0;

    bool signedIn() const { return !accessToken.empty() && !userId.empty(); }
};

using DeviceCodeCallback = std::function<void(DeviceAuthorization)>;
using AuthCallback = std::function<void(OAuthSession)>;

OAuthSession loadOAuth();
void saveOAuth(const OAuthSession& session);
void clearOAuth();
OAuthSession ensureOAuth(const Config& config);

void loginDeviceAsync(
    const Config& config,
    DeviceCodeCallback deviceCode,
    AuthCallback success,
    ErrorCallback failure);

}  // namespace twitch
