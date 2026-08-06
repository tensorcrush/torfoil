// Analyse de liens magnet (BEP 9 / BEP 53).
#pragma once

#include <string>
#include <vector>

#include "util/bytes.hpp"

namespace bt {

struct MagnetLink {
    util::Hash160 info_hash{};
    std::string display_name;              // dn= — indicatif, jamais de confiance
    std::vector<std::string> trackers;     // tr=
    std::vector<std::string> web_seeds;    // ws=
    uint64_t exact_length = 0;             // xl= (0 si absent)
};

// Accepte « xt=urn:btih: » en hex (40 car.) comme en base32 (32 car.).
// Les torrents v2 (urn:btmh) sont rejetés avec un message explicite.
bool parse_magnet(const std::string& uri, MagnetLink& out, std::string* err = nullptr);

}  // namespace bt
