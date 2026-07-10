#include "api/twitch_playlist.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <stdexcept>

namespace twitch {
namespace {

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string qualityToken(std::string value) {
    value = lower(std::move(value));
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return !std::isalnum(c);
    }), value.end());
    return value;
}

std::map<std::string, std::string> parseAttributes(const std::string& line) {
    std::map<std::string, std::string> result;
    const auto colon = line.find(':');
    if (colon == std::string::npos) return result;

    std::string body = line.substr(colon + 1);
    size_t cursor = 0;
    while (cursor < body.size()) {
        size_t equals = body.find('=', cursor);
        if (equals == std::string::npos) break;

        std::string key = trim(body.substr(cursor, equals - cursor));
        size_t valueStart = equals + 1;
        std::string value;

        if (valueStart < body.size() && body[valueStart] == '"') {
            const size_t endQuote = body.find('"', valueStart + 1);
            if (endQuote == std::string::npos) break;
            value = body.substr(valueStart + 1, endQuote - valueStart - 1);
            cursor = endQuote + 1;
        } else {
            const size_t comma = body.find(',', valueStart);
            value = trim(body.substr(valueStart,
                comma == std::string::npos ? std::string::npos : comma - valueStart));
            cursor = comma == std::string::npos ? body.size() : comma;
        }

        if (!key.empty()) result[key] = value;
        if (cursor < body.size() && body[cursor] == ',') ++cursor;
    }
    return result;
}

std::string absoluteUrl(const std::string& masterUrl, const std::string& candidate) {
    if (candidate.rfind("http://", 0) == 0 || candidate.rfind("https://", 0) == 0)
        return candidate;
    if (candidate.empty()) return {};

    const auto scheme = masterUrl.find("://");
    if (scheme == std::string::npos) return candidate;

    const auto firstPath = masterUrl.find('/', scheme + 3);
    const std::string origin =
        firstPath == std::string::npos ? masterUrl : masterUrl.substr(0, firstPath);

    if (candidate.front() == '/') return origin + candidate;

    const auto slash = masterUrl.rfind('/');
    if (slash == std::string::npos) return candidate;

    const std::string combined = masterUrl.substr(0, slash + 1) + candidate;
    const auto combinedScheme = combined.find("://");
    if (combinedScheme == std::string::npos) return combined;

    const auto combinedPath = combined.find('/', combinedScheme + 3);
    if (combinedPath == std::string::npos) return combined;

    const std::string combinedOrigin = combined.substr(0, combinedPath);
    std::vector<std::string> segments;
    std::istringstream pathInput(combined.substr(combinedPath + 1));
    for (std::string segment; std::getline(pathInput, segment, '/');) {
        if (segment.empty() || segment == ".") continue;
        if (segment == "..") {
            if (!segments.empty()) segments.pop_back();
            continue;
        }
        segments.push_back(segment);
    }

    std::string normalized = combinedOrigin + "/";
    for (size_t index = 0; index < segments.size(); ++index) {
        if (index) normalized += "/";
        normalized += segments[index];
    }
    return normalized;
}

std::string fallbackName(const std::map<std::string, std::string>& attrs) {
    std::string name;
    auto resolution = attrs.find("RESOLUTION");
    if (resolution != attrs.end()) {
        const auto x = resolution->second.find('x');
        name = x == std::string::npos ? resolution->second : resolution->second.substr(x + 1) + "p";
    }
    auto fps = attrs.find("FRAME-RATE");
    if (fps != attrs.end() && !fps->second.empty()) {
        try {
            const int rounded = static_cast<int>(std::stod(fps->second) + 0.5);
            if (!name.empty()) name += std::to_string(rounded);
        } catch (...) {
        }
    }
    return name.empty() ? "Variant" : name;
}

}  // namespace

std::vector<Quality> parseMasterPlaylist(
    const std::string& masterUrl,
    const std::string& playlistText) {
    std::vector<Quality> result;
    result.push_back({"auto", "Auto", masterUrl});

    std::map<std::string, std::string> groupNames;
    std::istringstream input(playlistText);
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(trim(line));
    }

    for (const auto& line : lines) {
        if (line.rfind("#EXT-X-MEDIA:", 0) != 0) continue;
        const auto attrs = parseAttributes(line);
        auto group = attrs.find("GROUP-ID");
        auto name = attrs.find("NAME");
        if (group != attrs.end() && name != attrs.end())
            groupNames[group->second] = name->second;
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("#EXT-X-STREAM-INF:", 0) != 0) continue;
        const auto attrs = parseAttributes(lines[i]);

        size_t next = i + 1;
        while (next < lines.size() &&
               (lines[next].empty() || lines[next].front() == '#')) {
            ++next;
        }
        if (next >= lines.size()) continue;

        std::string id;
        for (const char* key : {"VIDEO", "GROUP-ID"}) {
            auto it = attrs.find(key);
            if (it != attrs.end() && !it->second.empty()) {
                id = it->second;
                break;
            }
        }
        if (id.empty()) {
            auto resolution = attrs.find("RESOLUTION");
            id = resolution == attrs.end() ? "variant-" + std::to_string(result.size())
                                           : resolution->second;
        }

        std::string name;
        auto knownName = groupNames.find(id);
        if (knownName != groupNames.end()) name = knownName->second;
        if (name.empty()) name = fallbackName(attrs);

        const std::string url = absoluteUrl(masterUrl, lines[next]);
        if (url.empty()) continue;

        const auto duplicate = std::find_if(result.begin(), result.end(),
            [&](const Quality& quality) { return quality.id == id || quality.url == url; });
        if (duplicate == result.end()) result.push_back({id, name, url});
    }

    return result;
}

Quality selectQuality(
    const std::vector<Quality>& qualities,
    const std::string& preferred) {
    if (qualities.empty()) throw std::runtime_error("Twitch returned no playable qualities");

    const std::string wanted = lower(preferred);
    const std::string wantedToken = qualityToken(preferred);

    if (wanted.empty() || wanted == "auto") {
        auto automatic = std::find_if(qualities.begin(), qualities.end(), [](const Quality& quality) {
            return lower(quality.id) == "auto";
        });
        if (automatic != qualities.end()) return *automatic;
    }

    auto exact = std::find_if(qualities.begin(), qualities.end(), [&](const Quality& quality) {
        return lower(quality.id) == wanted || lower(quality.name) == wanted ||
               qualityToken(quality.id) == wantedToken ||
               qualityToken(quality.name) == wantedToken;
    });
    if (exact != qualities.end()) return *exact;

    if (wantedToken == "source" || wantedToken == "chunked") {
        auto source = std::find_if(qualities.begin(), qualities.end(), [](const Quality& quality) {
            return qualityToken(quality.id) == "chunked" ||
                   qualityToken(quality.name).find("source") != std::string::npos;
        });
        if (source != qualities.end()) return *source;
    }

    if (wantedToken == "audioonly") {
        auto audio = std::find_if(qualities.begin(), qualities.end(), [](const Quality& quality) {
            return qualityToken(quality.id).find("audioonly") != std::string::npos ||
                   qualityToken(quality.name).find("audioonly") != std::string::npos;
        });
        if (audio != qualities.end()) return *audio;
    }

    // If a frame-rate-specific option is unavailable, prefer the same
    // resolution before falling back to the first concrete variant.
    std::string resolutionToken = wantedToken;
    if (resolutionToken.size() > 2 &&
        resolutionToken.compare(resolutionToken.size() - 2, 2, "60") == 0)
        resolutionToken.erase(resolutionToken.size() - 2);
    if (!resolutionToken.empty()) {
        auto sameResolution = std::find_if(qualities.begin(), qualities.end(), [&](const Quality& quality) {
            const std::string id = qualityToken(quality.id);
            const std::string name = qualityToken(quality.name);
            return id.find(resolutionToken) != std::string::npos ||
                   name.find(resolutionToken) != std::string::npos;
        });
        if (sameResolution != qualities.end()) return *sameResolution;
    }

    auto nonAuto = std::find_if(qualities.begin(), qualities.end(), [](const Quality& quality) {
        return lower(quality.id) != "auto";
    });
    return nonAuto == qualities.end() ? qualities.front() : *nonAuto;
}

std::string percentEncode(const std::string& value) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 3);
    for (unsigned char c : value) {
        const bool safe = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(HEX[(c >> 4) & 0x0F]);
            result.push_back(HEX[c & 0x0F]);
        }
    }
    return result;
}

std::string normalizeChannel(std::string value) {
    value = trim(value);
    const std::string prefix1 = "https://www.twitch.tv/";
    const std::string prefix2 = "https://twitch.tv/";
    if (value.rfind(prefix1, 0) == 0) value.erase(0, prefix1.size());
    else if (value.rfind(prefix2, 0) == 0) value.erase(0, prefix2.size());

    const auto query = value.find_first_of("?#/");
    if (query != std::string::npos) value.erase(query);

    value = lower(trim(value));
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return !(std::isalnum(c) || c == '_');
    }), value.end());
    return value;
}

}  // namespace twitch
