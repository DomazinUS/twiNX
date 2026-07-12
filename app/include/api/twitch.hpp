#pragma once

#include "api/twitch_playlist.hpp"

#include <functional>
#include <string>
#include <vector>

namespace twitch {

struct Config {
    std::string channel;
    std::string preferredQuality = "source";
    std::string playerType = "embed";

    // Public Twitch application ID used by OAuth Device Code Flow and Helix.
    std::string oauthClientId = "1a0hyv0sxnwpkvde7pjyo4gpwcnwj9";
    std::string oauthScopes = "user:read:follows user:read:chat user:write:chat user:read:emotes user:read:subscriptions";

    // Twitch web GraphQL identifiers used by the native playback resolver.
    // They are configurable because Twitch can replace them.
    std::string gqlClientId = "kd1unb4b3q4t58fwlpcbzcbnm76a8fp";
    std::string playbackHash =
        "0828119ded1c13477966434e15800ff57ddacf13ba1911c129dc2200705b0712";
};

struct Resolution {
    std::string channel;
    std::string masterUrl;
    std::vector<Quality> qualities;
    Quality selected;
};

using ResolveCallback = std::function<void(Resolution)>;
using ErrorCallback = std::function<void(const std::string&)>;

Config loadConfig();
std::string loadPreferredQuality();
bool savePreferredQuality(const std::string& quality);

enum class DecoderMode {
    Software = 0,
    Hardware = 1,
    Hybrid = 2,
};

// Software decoding remains the safest mode. Hardware is the original
// experimental path. Hybrid starts in hardware, falls back to software for a
// Twitch presentation transition, and only restores hardware after a second
// transition has completed and playback has remained stable.
DecoderMode loadDecoderMode();
bool saveDecoderMode(DecoderMode mode);
bool decoderUsesHardware(DecoderMode mode);

// Backwards-compatible wrappers for older call sites and settings files.
bool loadHardwareDecodeEnabled();
bool saveHardwareDecodeEnabled(bool enabled);
Resolution resolveLive(const Config& config);
void resolveLiveAsync(
    const Config& config,
    ResolveCallback success,
    ErrorCallback failure);

std::string mpvExtra();
std::string describeQualities(const std::vector<Quality>& qualities);

}  // namespace twitch
