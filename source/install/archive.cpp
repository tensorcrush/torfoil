#include "install/archive.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "util/bytes.hpp"

namespace install {

namespace {

bool fail(std::string* err, const std::string& msg) {
    if (err) *err = msg;
    return false;
}

// Les structures Nintendo sont en little-endian.
uint32_t rd_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

uint64_t rd_le64(const uint8_t* p) {
    return static_cast<uint64_t>(rd_le32(p)) | static_cast<uint64_t>(rd_le32(p + 4)) << 32;
}

// Table d'entrées + table de chaînes, commune à PFS0 et HFS0 : seule la taille
// d'une entrée et la position des champs changent.
bool parse_container(FileReader& file, uint64_t base, const char magic[4], size_t entry_size,
                     std::vector<ArchiveEntry>& out, std::string* err) {
    uint8_t header[0x10];
    if (!file.read(base, header, sizeof(header))) return fail(err, "en-tête illisible");
    if (std::memcmp(header, magic, 4) != 0) return fail(err, "signature de conteneur inattendue");

    const uint32_t count = rd_le32(header + 4);
    const uint32_t string_table_size = rd_le32(header + 8);
    if (count == 0 || count > 4096) return fail(err, "nombre d'entrées aberrant");
    if (string_table_size > 1024 * 1024) return fail(err, "table de chaînes aberrante");

    const uint64_t table_offset = base + 0x10;
    const uint64_t strings_offset = table_offset + static_cast<uint64_t>(count) * entry_size;
    const uint64_t data_offset = strings_offset + string_table_size;

    std::vector<uint8_t> table(static_cast<size_t>(count) * entry_size);
    if (!file.read(table_offset, table.data(), table.size())) {
        return fail(err, "table d'entrées illisible");
    }

    std::vector<char> strings(string_table_size + 1, '\0');
    if (string_table_size > 0 && !file.read(strings_offset, strings.data(), string_table_size)) {
        return fail(err, "table de chaînes illisible");
    }

    out.clear();
    out.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = table.data() + static_cast<size_t>(i) * entry_size;
        const uint64_t offset = rd_le64(e);
        const uint64_t size = rd_le64(e + 8);
        const uint32_t name_offset = rd_le32(e + 16);

        if (name_offset >= string_table_size) continue;
        if (data_offset + offset + size > file.size()) continue;  // entrée tronquée

        ArchiveEntry entry;
        entry.name = std::string(strings.data() + name_offset);
        entry.offset = data_offset + offset;
        entry.size = size;
        out.push_back(std::move(entry));
    }

    if (out.empty()) return fail(err, "conteneur vide");
    return true;
}

}  // namespace

FileReader::~FileReader() {
    close();
}

bool FileReader::open(const std::string& path) {
    close();
    handle_ = std::fopen(path.c_str(), "rb");
    if (!handle_) return false;

    if (fseeko(handle_, 0, SEEK_END) != 0) {
        close();
        return false;
    }
    const off_t end = ftello(handle_);
    if (end < 0) {
        close();
        return false;
    }
    size_ = static_cast<uint64_t>(end);
    path_ = path;
    return true;
}

void FileReader::close() {
    if (handle_) {
        std::fclose(handle_);
        handle_ = nullptr;
    }
    size_ = 0;
}

bool FileReader::read(uint64_t offset, void* buffer, size_t length) {
    if (!handle_ || length == 0) return length == 0;
    if (offset + length > size_) return false;
    if (fseeko(handle_, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
    return std::fread(buffer, 1, length, handle_) == length;
}

bool parse_pfs0(FileReader& file, uint64_t base, std::vector<ArchiveEntry>& out,
                std::string* err) {
    return parse_container(file, base, "PFS0", 0x18, out, err);
}

bool parse_hfs0(FileReader& file, uint64_t base, std::vector<ArchiveEntry>& out,
                std::string* err) {
    return parse_container(file, base, "HFS0", 0x40, out, err);
}

bool find_xci_secure_partition(FileReader& file, uint64_t& secure_base, std::string* err) {
    uint8_t header[0x200];
    if (!file.read(0, header, sizeof(header))) return fail(err, "en-tête XCI illisible");
    if (std::memcmp(header + 0x100, "HEAD", 4) != 0) return fail(err, "ce n'est pas un XCI");

    const uint64_t root_offset = rd_le64(header + 0x130);

    std::vector<ArchiveEntry> partitions;
    if (!parse_hfs0(file, root_offset, partitions, err)) return false;

    for (const ArchiveEntry& part : partitions) {
        if (part.name != "secure") continue;
        secure_base = part.offset;
        return true;
    }
    return fail(err, "partition « secure » absente du XCI");
}

bool list_installable_contents(FileReader& file, std::vector<ArchiveEntry>& out,
                               std::string* err) {
    // Lire le minimum disponible plutôt qu'un bloc fixe : un NSP tronqué —
    // téléchargement interrompu, copie ratée — doit donner un diagnostic clair
    // et pas « fichier illisible ».
    uint8_t probe[0x200];
    std::memset(probe, 0, sizeof(probe));

    if (file.size() < 0x10) {
        return fail(err, "fichier trop petit pour être un NSP ou un XCI (" +
                             std::to_string(file.size()) + " octets) — copie incomplète ?");
    }

    const size_t probe_len = static_cast<size_t>(std::min<uint64_t>(sizeof(probe), file.size()));
    if (!file.read(0, probe, probe_len)) return fail(err, "fichier illisible");

    if (std::memcmp(probe, "PFS0", 4) == 0) return parse_pfs0(file, 0, out, err);

    if (probe_len >= 0x104 && std::memcmp(probe + 0x100, "HEAD", 4) == 0) {
        uint64_t secure = 0;
        if (!find_xci_secure_partition(file, secure, err)) return false;
        return parse_hfs0(file, secure, out, err);
    }

    // Dire ce qu'on a trouvé plutôt que « format inconnu » : sur un fichier issu
    // d'un torrent, un en-tête de zéros signifie simplement que les premières
    // pièces ne sont pas encore arrivées — le fichier n'est pas en cause.
    bool all_zero = true;
    for (size_t i = 0; i < 16 && i < probe_len; ++i) {
        if (probe[i] != 0) all_zero = false;
    }
    if (all_zero) {
        return fail(err, "début du fichier encore vide — le téléchargement n'a pas "
                         "récupéré les premières pièces");
    }

    char magic[16];
    std::snprintf(magic, sizeof(magic), "%02X %02X %02X %02X", probe[0], probe[1], probe[2],
                  probe[3]);
    return fail(err, std::string("format inconnu : attendu « PFS0 » (NSP) ou « HEAD » (XCI), "
                                 "trouvé ") + magic);
}

}  // namespace install
