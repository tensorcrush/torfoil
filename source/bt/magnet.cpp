#include "bt/magnet.hpp"

#include <cstdlib>

namespace bt {

namespace {

bool fail(std::string* err, const char* msg) {
    if (err) *err = msg;
    return false;
}

}  // namespace

bool parse_magnet(const std::string& uri, MagnetLink& out, std::string* err) {
    const std::string trimmed = util::trim(uri);
    if (!util::starts_with(trimmed, "magnet:?")) {
        return fail(err, "magnet : le lien doit commencer par « magnet:? »");
    }

    bool has_hash = false;
    const std::string query = trimmed.substr(8);

    for (const std::string& raw_param : util::split(query, '&')) {
        const size_t eq = raw_param.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = raw_param.substr(0, eq);
        const std::string value = util::url_decode(raw_param.substr(eq + 1));
        if (value.empty()) continue;

        if (key == "xt" || util::starts_with(key, "xt.")) {
            if (util::starts_with(value, "urn:btih:")) {
                const std::string id = value.substr(9);
                if (id.size() == 40) {
                    if (!util::from_hex(id, out.info_hash.data(), out.info_hash.size())) {
                        return fail(err, "magnet : info_hash hexadécimal invalide");
                    }
                } else if (id.size() == 32) {
                    if (!util::from_base32(id, out.info_hash.data(), out.info_hash.size())) {
                        return fail(err, "magnet : info_hash base32 invalide");
                    }
                } else {
                    return fail(err, "magnet : longueur d'info_hash inattendue");
                }
                has_hash = true;
            } else if (util::starts_with(value, "urn:btmh:")) {
                // BitTorrent v2 : arbres de Merkle SHA-256, protocole différent.
                return fail(err, "magnet : torrent v2 (btmh) non pris en charge");
            }
        } else if (key == "dn") {
            out.display_name = value;
        } else if (key == "tr") {
            out.trackers.push_back(value);
        } else if (key == "ws") {
            out.web_seeds.push_back(value);
        } else if (key == "xl") {
            const long long len = std::strtoll(value.c_str(), nullptr, 10);
            if (len > 0) out.exact_length = static_cast<uint64_t>(len);
        }
    }

    if (!has_hash) return fail(err, "magnet : paramètre « xt=urn:btih: » absent");
    return true;
}

}  // namespace bt
