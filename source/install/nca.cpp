#include "install/nca.hpp"

#include <mbedtls/aes.h>

#include <algorithm>
#include <cstring>

#include "util/bytes.hpp"

namespace install {

namespace {

constexpr uint64_t kMediaUnit = 0x200;
constexpr size_t kHeaderSize = 0xC00;

bool fail(std::string* err, const char* msg) {
    if (err) *err = msg;
    return false;
}

uint32_t rd_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

uint64_t rd_le64(const uint8_t* p) {
    return static_cast<uint64_t>(rd_le32(p)) | static_cast<uint64_t>(rd_le32(p + 4)) << 32;
}

// Le XTS de Nintendo numérote les secteurs en gros-boutiste sur 16 octets,
// là où mbedtls attend l'unité de données telle quelle. On construit donc le
// bloc à la main.
void make_tweak(uint8_t tweak[16], uint64_t sector) {
    std::memset(tweak, 0, 16);
    util::wr_be64(tweak + 8, sector);
}

// Compteur AES-CTR d'une section : les 8 premiers octets viennent du FS header
// (dans l'ordre inverse), les 8 suivants sont le décalage divisé par 16.
void section_ctr(uint8_t ctr[16], const uint8_t base[8], uint64_t absolute_offset) {
    for (int i = 0; i < 8; ++i) ctr[i] = base[7 - i];
    util::wr_be64(ctr + 8, absolute_offset >> 4);
}

bool aes_ecb_decrypt(const uint8_t key[16], const uint8_t* in, uint8_t* out, size_t blocks) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    bool ok = mbedtls_aes_setkey_dec(&ctx, key, 128) == 0;
    for (size_t i = 0; ok && i < blocks; ++i) {
        ok = mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, in + i * 16, out + i * 16) == 0;
    }
    mbedtls_aes_free(&ctx);
    return ok;
}

// Parseur PFS0 en mémoire (le conteneur est déjà déchiffré à ce stade).
struct MemEntry {
    std::string name;
    uint64_t offset;
    uint64_t size;
};

bool parse_pfs0_memory(const std::vector<uint8_t>& blob, std::vector<MemEntry>& out) {
    if (blob.size() < 0x10 || std::memcmp(blob.data(), "PFS0", 4) != 0) return false;

    const uint32_t count = rd_le32(blob.data() + 4);
    const uint32_t strings_size = rd_le32(blob.data() + 8);
    if (count == 0 || count > 1024) return false;

    const size_t table = 0x10;
    const size_t strings = table + static_cast<size_t>(count) * 0x18;
    const size_t data = strings + strings_size;
    if (data > blob.size()) return false;

    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = blob.data() + table + static_cast<size_t>(i) * 0x18;
        const uint64_t offset = rd_le64(e);
        const uint64_t size = rd_le64(e + 8);
        const uint32_t name_offset = rd_le32(e + 16);
        if (name_offset >= strings_size) continue;
        if (data + offset + size > blob.size()) continue;

        MemEntry entry;
        entry.name = std::string(reinterpret_cast<const char*>(blob.data() + strings + name_offset));
        entry.offset = data + offset;
        entry.size = size;
        out.push_back(std::move(entry));
    }
    return !out.empty();
}

}  // namespace

bool nca_read_header(FileReader& file, uint64_t nca_offset, const KeySet& keys, NcaInfo& out,
                     std::string* err) {
    const uint8_t* header_key = keys.header_key();
    if (!header_key) return fail(err, "header_key absente de prod.keys");

    std::vector<uint8_t> encrypted(kHeaderSize);
    if (!file.read(nca_offset, encrypted.data(), encrypted.size())) {
        return fail(err, "en-tête NCA illisible");
    }

    std::vector<uint8_t> header(kHeaderSize);
    mbedtls_aes_xts_context xts;
    mbedtls_aes_xts_init(&xts);
    if (mbedtls_aes_xts_setkey_dec(&xts, header_key, 256) != 0) {
        mbedtls_aes_xts_free(&xts);
        return fail(err, "clé XTS invalide");
    }

    for (size_t sector = 0; sector * kMediaUnit < kHeaderSize; ++sector) {
        uint8_t tweak[16];
        make_tweak(tweak, sector);
        if (mbedtls_aes_crypt_xts(&xts, MBEDTLS_AES_DECRYPT, kMediaUnit, tweak,
                                  encrypted.data() + sector * kMediaUnit,
                                  header.data() + sector * kMediaUnit) != 0) {
            mbedtls_aes_xts_free(&xts);
            return fail(err, "déchiffrement de l'en-tête impossible");
        }
    }
    mbedtls_aes_xts_free(&xts);

    if (std::memcmp(header.data() + 0x200, "NCA3", 4) != 0) {
        // NCA2/NCA0 : formats antérieurs, plus produits depuis des années.
        return fail(err, "NCA non reconnu (prod.keys erronées ou format ancien)");
    }

    out.content_type = header[0x205];
    out.key_area_index = header[0x207];
    out.content_size = rd_le64(header.data() + 0x208);
    out.title_id = rd_le64(header.data() + 0x210);

    const uint8_t gen_old = header[0x206];
    const uint8_t gen_new = header[0x220];
    uint8_t generation = std::max(gen_old, gen_new);
    if (generation > 0) --generation;  // la génération 1 partage l'index 0
    out.key_generation = generation;

    std::memcpy(out.rights_id, header.data() + 0x230, 16);
    out.has_rights_id = std::any_of(out.rights_id, out.rights_id + 16,
                                    [](uint8_t b) { return b != 0; });

    // Zone de clés : AES-ECB avec la clé d'application de cette génération.
    const uint8_t* kaek = keys.key_area_key_application(generation);
    if (out.key_area_index != 0) {
        // Ocean/System : jeux tiers particuliers et titres système.
        return fail(err, "type de clé de zone non géré (jeu système ?)");
    }
    if (!kaek) return fail(err, "key_area_key_application manquante pour cette génération");
    if (!aes_ecb_decrypt(kaek, header.data() + 0x300, out.decrypted_key_area, 4)) {
        return fail(err, "déchiffrement de la zone de clés impossible");
    }

    for (int i = 0; i < 4; ++i) {
        const uint8_t* entry = header.data() + 0x240 + i * 0x10;
        const uint32_t start = rd_le32(entry);
        const uint32_t end = rd_le32(entry + 4);
        if (end <= start) continue;

        NcaSection& section = out.sections[i];
        section.present = true;
        section.offset = static_cast<uint64_t>(start) * kMediaUnit;
        section.size = static_cast<uint64_t>(end - start) * kMediaUnit;

        const uint8_t* fs = header.data() + 0x400 + i * 0x200;
        section.fs_type = fs[0x02];
        section.hash_type = fs[0x03];
        section.encryption_type = fs[0x04];
        section_ctr(section.ctr, fs + 0x140, section.offset);

        // HierarchicalSha256 : le PFS0 se trouve dans la dernière couche.
        if (section.hash_type == 2) {
            const uint8_t* hash_data = fs + 0x08;
            const uint32_t layers = rd_le32(hash_data + 0x24);
            if (layers >= 1 && layers <= 5) {
                const uint8_t* region = hash_data + 0x28 + (layers - 1) * 0x10;
                section.content_offset = rd_le64(region);
                section.content_size = rd_le64(region + 8);
            }
        }
    }

    return true;
}

bool nca_read_section(FileReader& file, uint64_t nca_offset, const NcaInfo& nca, int section_index,
                      uint64_t rel_offset, void* buffer, size_t length) {
    if (section_index < 0 || section_index >= 4) return false;
    const NcaSection& section = nca.sections[section_index];
    if (!section.present || length == 0) return false;
    if (rel_offset + length > section.size) return false;

    const uint64_t absolute = section.offset + rel_offset;

    if (section.encryption_type == 1) {  // aucune
        return file.read(nca_offset + absolute, buffer, length);
    }
    if (section.encryption_type != 3) return false;  // XTS/BKTR non gérés ici

    // AES-CTR impose de travailler sur des blocs de 16 octets alignés.
    const uint64_t aligned_start = absolute & ~static_cast<uint64_t>(0xf);
    const size_t lead = static_cast<size_t>(absolute - aligned_start);
    const size_t span = ((lead + length) + 0xf) & ~static_cast<size_t>(0xf);

    std::vector<uint8_t> raw(span);
    if (!file.read(nca_offset + aligned_start, raw.data(), span)) return false;

    // Les 8 premiers octets du compteur sont propres à la section ; seuls les
    // 8 derniers dépendent du décalage lu.
    uint8_t ctr[16];
    std::memcpy(ctr, section.ctr, 8);
    util::wr_be64(ctr + 8, aligned_start >> 4);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, nca.decrypted_key_area + 0x20, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return false;
    }

    std::vector<uint8_t> plain(span);
    size_t nc_off = 0;
    uint8_t stream_block[16]{};
    const int rc = mbedtls_aes_crypt_ctr(&ctx, span, &nc_off, ctr, stream_block, raw.data(),
                                         plain.data());
    mbedtls_aes_free(&ctx);
    if (rc != 0) return false;

    std::memcpy(buffer, plain.data() + lead, length);
    return true;
}

bool nca_extract_cnmt(FileReader& file, uint64_t nca_offset, const NcaInfo& nca,
                      std::vector<uint8_t>& out, std::string* err) {
    if (nca.content_type != kNcaMeta) return fail(err, "ce NCA n'est pas un NCA meta");
    if (nca.has_rights_id) {
        return fail(err, "NCA meta protégé par ticket : non pris en charge");
    }

    const NcaSection& section = nca.sections[0];
    if (!section.present || section.content_size == 0) {
        return fail(err, "section de métadonnées absente");
    }
    if (section.content_size > 4 * 1024 * 1024) return fail(err, "PFS0 meta démesuré");

    std::vector<uint8_t> pfs0(static_cast<size_t>(section.content_size));
    if (!nca_read_section(file, nca_offset, nca, 0, section.content_offset, pfs0.data(),
                          pfs0.size())) {
        return fail(err, "lecture du PFS0 meta impossible");
    }

    std::vector<MemEntry> entries;
    if (!parse_pfs0_memory(pfs0, entries)) return fail(err, "PFS0 meta illisible");

    for (const MemEntry& entry : entries) {
        if (entry.name.size() < 5) continue;
        if (entry.name.compare(entry.name.size() - 5, 5, ".cnmt") != 0) continue;
        if (entry.size > 1024 * 1024) return fail(err, "CNMT démesuré");

        out.assign(pfs0.begin() + static_cast<long>(entry.offset),
                   pfs0.begin() + static_cast<long>(entry.offset + entry.size));
        return true;
    }

    return fail(err, "aucun fichier .cnmt dans le NCA meta");
}

uint64_t CnmtInfo::application_id() const {
    switch (meta_type) {
        case 0x81:  // Patch : l'identifiant du patch vaut celui du jeu + 0x800
            return title_id & ~static_cast<uint64_t>(0xfff);
        case 0x82:  // AddOnContent (DLC)
            return (title_id & ~static_cast<uint64_t>(0xfff)) - 0x1000;
        default:
            return title_id;
    }
}

bool parse_cnmt(const std::vector<uint8_t>& blob, CnmtInfo& out, std::string* err) {
    if (blob.size() < 0x20) return fail(err, "CNMT tronqué");

    out.title_id = rd_le64(blob.data());
    out.version = rd_le32(blob.data() + 0x08);
    out.meta_type = blob[0x0C];

    const uint16_t extended_size = static_cast<uint16_t>(blob[0x0E] | blob[0x0F] << 8);
    const uint16_t content_count = static_cast<uint16_t>(blob[0x10] | blob[0x11] << 8);
    out.content_meta_count = static_cast<uint16_t>(blob[0x12] | blob[0x13] << 8);
    out.attributes = blob[0x14];

    const size_t records_offset = 0x20 + extended_size;
    const size_t records_size = static_cast<size_t>(content_count) * 0x38;
    if (records_offset + records_size > blob.size()) return fail(err, "CNMT incohérent");

    out.extended_header.assign(blob.begin() + 0x20, blob.begin() + 0x20 + extended_size);

    out.contents.clear();
    out.contents.reserve(content_count);
    for (uint16_t i = 0; i < content_count; ++i) {
        const uint8_t* r = blob.data() + records_offset + static_cast<size_t>(i) * 0x38;

        ContentRecord record;
        std::memcpy(record.hash, r, 32);
        std::memcpy(record.nca_id, r + 32, 16);
        // Taille sur 6 octets, little-endian.
        record.size = 0;
        for (int b = 5; b >= 0; --b) record.size = record.size << 8 | r[48 + b];
        record.type = r[54];
        record.id_offset = r[55];

        // Les fragments delta ne s'installent pas.
        if (record.type == 6) continue;
        out.contents.push_back(record);
    }

    const size_t metas_offset = records_offset + records_size;
    const size_t metas_size = static_cast<size_t>(out.content_meta_count) * 0x10;
    if (metas_offset + metas_size <= blob.size()) {
        out.content_meta_infos.assign(blob.begin() + static_cast<long>(metas_offset),
                                      blob.begin() + static_cast<long>(metas_offset + metas_size));
    } else {
        out.content_meta_count = 0;
    }

    if (out.contents.empty()) return fail(err, "CNMT sans contenu installable");
    return true;
}

}  // namespace install
