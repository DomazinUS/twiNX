#pragma once

#include "client.hpp"
#include <api/http.hpp>

namespace remote {

class Apache : public Client {
public:
    Apache(const AppRemote &conf);
    std::vector<DirEntry> list(const std::string &path) override;

private:
    HTTP c;
    std::string host;
};

}  // namespace remote