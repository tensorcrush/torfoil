// Lecture d'un NCA — juste ce qu'il faut pour installer.
//
// Point important : installer un jeu ne demande PAS de déchiffrer son contenu.
// Les NCA sont recopiés tels quels dans le stockage de la console. Seul le NCA
// « meta » doit être ouvert, parce qu'il contient le CNMT — la liste des
// contenus et l'identifiant du titre, sans lesquels rien n'est enregistrable.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "install/archive.hpp"
#include "install/keys.hpp"

namespace install {

enum NcaContentType : uint8_t {
    kNcaProgram = 0,
    kNcaMeta = 1,
    kNcaControl = 2,
    kNcaManual = 3,
    kNcaData = 4,
    kNcaPublicData = 5,
};

struct NcaSection {
    bool present = false;
    uint64_t offset = 0;  // absolu dans le NCA
    uint64_t size = 0;
    uint8_t fs_type = 0;
    uint8_t hash_type = 0;
    uint8_t encryption_type = 0;
    uint8_t ctr[16]{};
    uint64_t content_offset = 0;  // début du PFS0, relatif au début de section
    uint64_t content_size = 0;
};

struct NcaInfo {
    uint8_t content_type = 0;
    uint8_t key_generation = 0;
    uint8_t key_area_index = 0;
    uint64_t content_size = 0;
    uint64_t title_id = 0;
    bool has_rights_id = false;
    uint8_t rights_id[16]{};
    uint8_t decrypted_key_area[0x40]{};
    NcaSection sections[4];
};

// `nca_offset` : position du NCA dans le fichier (NSP ou XCI).
bool nca_read_header(FileReader& file, uint64_t nca_offset, const KeySet& keys, NcaInfo& out,
                     std::string* err = nullptr);

// Lit `length` octets déchiffrés à `rel_offset` dans la section indiquée.
bool nca_read_section(FileReader& file, uint64_t nca_offset, const NcaInfo& nca, int section,
                      uint64_t rel_offset, void* buffer, size_t length);

// Extrait le fichier .cnmt d'un NCA de type Meta.
bool nca_extract_cnmt(FileReader& file, uint64_t nca_offset, const NcaInfo& nca,
                      std::vector<uint8_t>& out, std::string* err = nullptr);

// --- CNMT ---

struct ContentRecord {
    uint8_t hash[32]{};
    uint8_t nca_id[16]{};
    uint64_t size = 0;
    uint8_t type = 0;       // \ref NcmContentType
    uint8_t id_offset = 0;
};

struct CnmtInfo {
    uint64_t title_id = 0;
    uint32_t version = 0;
    uint8_t meta_type = 0;  // 0x80 Application, 0x81 Patch, 0x82 AddOnContent
    uint8_t attributes = 0;
    std::vector<uint8_t> extended_header;
    std::vector<ContentRecord> contents;
    std::vector<uint8_t> content_meta_infos;  // recopiés tels quels
    uint16_t content_meta_count = 0;

    // Identifiant de l'application à laquelle rattacher l'enregistrement.
    uint64_t application_id() const;
};

bool parse_cnmt(const std::vector<uint8_t>& blob, CnmtInfo& out, std::string* err = nullptr);

}  // namespace install
