// Métadonnées d'un torrent : soit lues dans un .torrent, soit reconstituées à
// partir du dictionnaire « info » récupéré auprès des pairs (BEP 9, cas magnet).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "util/bytes.hpp"

namespace bt {

struct FileEntry {
    // Chemin relatif, séparateurs '/', chaque composant déjà assaini pour exFAT.
    std::string path;
    uint64_t length = 0;
    // Décalage du début de ce fichier dans le flux concaténé du torrent.
    uint64_t offset = 0;
};

struct MetaInfo {
    util::Hash160 info_hash{};
    std::string name;
    uint32_t piece_length = 0;
    std::vector<util::Hash160> piece_hashes;
    std::vector<FileEntry> files;
    uint64_t total_size = 0;
    bool single_file = false;
    bool is_private = false;  // BEP 27 : pas de DHT/PEX sur ce torrent
    std::vector<std::string> trackers;

    bool valid() const { return piece_length > 0 && !piece_hashes.empty() && total_size > 0; }
    uint32_t piece_count() const { return static_cast<uint32_t>(piece_hashes.size()); }

    // Les pièces font toutes piece_length octets, sauf la dernière.
    uint32_t size_of_piece(uint32_t index) const;
};

// Parse un fichier .torrent complet (dictionnaire racine avec « info »).
bool parse_torrent(const std::string& buf, MetaInfo& out, std::string* err = nullptr);

// Parse un dictionnaire « info » nu, tel que reçu via ut_metadata.
// `expected` sert à vérifier que le SHA-1 correspond bien au magnet demandé —
// un pair malveillant peut renvoyer n'importe quoi.
bool parse_info_dict(const std::string& info_buf, const util::Hash160& expected, MetaInfo& out,
                     std::string* err = nullptr);

}  // namespace bt
