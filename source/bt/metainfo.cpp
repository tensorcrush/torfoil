#include "bt/metainfo.hpp"

#include <cstring>

#include "bt/bencode.hpp"
#include "util/sha1.hpp"

namespace bt {

namespace {

bool fail(std::string* err, const char* msg) {
    if (err) *err = msg;
    return false;
}

// Remplit name/piece_length/pieces/files à partir d'un dictionnaire « info ».
bool read_info(const Value& info, MetaInfo& out, std::string* err) {
    const Value* piece_len = info.find_int("piece length");
    if (!piece_len || piece_len->i <= 0 || piece_len->i > 256 * 1024 * 1024) {
        return fail(err, "torrent : « piece length » absent ou aberrant");
    }
    out.piece_length = static_cast<uint32_t>(piece_len->i);

    const Value* pieces = info.find_str("pieces");
    if (!pieces || pieces->s.empty() || pieces->s.size() % 20 != 0) {
        return fail(err, "torrent : table « pieces » invalide");
    }
    const size_t count = pieces->s.size() / 20;
    out.piece_hashes.resize(count);
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(out.piece_hashes[i].data(), pieces->s.data() + i * 20, 20);
    }

    const Value* name = info.find_str("name");
    if (!name || name->s.empty()) return fail(err, "torrent : « name » absent");
    out.name = util::sanitize_filename(name->s);

    out.is_private = info.int_or("private", 0) != 0;

    const Value* files = info.find_list("files");
    if (files) {
        // Torrent multi-fichiers : « name » devient le dossier racine.
        out.single_file = false;
        uint64_t offset = 0;
        for (const Value& f : files->l) {
            const Value* len = f.find_int("length");
            const Value* path = f.find_list("path");
            if (!len || len->i < 0 || !path || path->l.empty()) {
                return fail(err, "torrent : entrée de fichier invalide");
            }

            std::string joined;
            for (const Value& part : path->l) {
                if (!part.is_str()) return fail(err, "torrent : composant de chemin invalide");
                // Assainir composant par composant neutralise aussi les « .. »
                // et les séparateurs injectés (évasion de répertoire).
                const std::string clean = util::sanitize_filename(part.s);
                if (clean == "." || clean == "..") return fail(err, "torrent : chemin suspect");
                if (!joined.empty()) joined += '/';
                joined += clean;
            }

            FileEntry entry;
            entry.path = joined;
            entry.length = static_cast<uint64_t>(len->i);
            entry.offset = offset;
            offset += entry.length;
            out.files.push_back(std::move(entry));
        }
        out.total_size = offset;
    } else {
        const Value* len = info.find_int("length");
        if (!len || len->i < 0) return fail(err, "torrent : ni « files » ni « length »");
        out.single_file = true;
        FileEntry entry;
        entry.path = out.name;
        entry.length = static_cast<uint64_t>(len->i);
        entry.offset = 0;
        out.files.push_back(std::move(entry));
        out.total_size = entry.length;
    }

    if (out.total_size == 0) return fail(err, "torrent : taille totale nulle");

    // Cohérence : le nombre de pièces doit correspondre à la taille annoncée.
    const uint64_t expected_pieces =
        (out.total_size + out.piece_length - 1) / out.piece_length;
    if (expected_pieces != count) {
        return fail(err, "torrent : nombre de pièces incohérent avec la taille");
    }

    return true;
}

void collect_trackers(const Value& root, MetaInfo& out) {
    if (const Value* announce = root.find_str("announce"); announce && !announce->s.empty()) {
        out.trackers.push_back(announce->s);
    }

    // « announce-list » est une liste de niveaux (BEP 12), chacun une liste d'URL.
    if (const Value* list = root.find_list("announce-list")) {
        for (const Value& tier : list->l) {
            if (!tier.is_list()) continue;
            for (const Value& url : tier.l) {
                if (!url.is_str() || url.s.empty()) continue;
                bool already = false;
                for (const std::string& known : out.trackers) {
                    if (known == url.s) {
                        already = true;
                        break;
                    }
                }
                if (!already) out.trackers.push_back(url.s);
            }
        }
    }
}

}  // namespace

uint32_t MetaInfo::size_of_piece(uint32_t index) const {
    if (index + 1 < piece_count()) return piece_length;
    const uint64_t rest = total_size - static_cast<uint64_t>(index) * piece_length;
    return static_cast<uint32_t>(rest);
}

bool parse_torrent(const std::string& buf, MetaInfo& out, std::string* err) {
    Value root;
    if (!bdecode(buf, root, err)) return false;
    if (!root.is_dict()) return fail(err, "torrent : la racine n'est pas un dictionnaire");

    const Value* info = root.find_dict("info");
    if (!info) return fail(err, "torrent : dictionnaire « info » absent");

    // L'info_hash porte sur les octets d'origine, pas sur un ré-encodage.
    if (info->raw_end <= info->raw_begin || info->raw_end > buf.size()) {
        return fail(err, "torrent : étendue de « info » invalide");
    }
    out.info_hash = util::Sha1::of(reinterpret_cast<const uint8_t*>(buf.data()) + info->raw_begin,
                                   info->raw_end - info->raw_begin);

    if (!read_info(*info, out, err)) return false;
    collect_trackers(root, out);
    return true;
}

bool parse_info_dict(const std::string& info_buf, const util::Hash160& expected, MetaInfo& out,
                     std::string* err) {
    const util::Hash160 actual =
        util::Sha1::of(reinterpret_cast<const uint8_t*>(info_buf.data()), info_buf.size());
    if (actual != expected) {
        return fail(err, "métadonnées : info_hash ne correspond pas au magnet");
    }

    Value info;
    if (!bdecode(info_buf, info, err)) return false;
    if (!info.is_dict()) return fail(err, "métadonnées : « info » n'est pas un dictionnaire");

    out.info_hash = expected;
    return read_info(info, out, err);
}

}  // namespace bt
