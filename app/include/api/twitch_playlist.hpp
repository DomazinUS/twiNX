#pragma once

#include <string>
#include <vector>

namespace twitch {

struct Quality {
    std::string id;
    std::string name;
    std::string url;
};

std::vector<Quality> parseMasterPlaylist(
    const std::string& masterUrl,
    const std::string& playlistText);

Quality selectQuality(
    const std::vector<Quality>& qualities,
    const std::string& preferred = "source");

std::string percentEncode(const std::string& value);
std::string normalizeChannel(std::string value);

}  // namespace twitch
