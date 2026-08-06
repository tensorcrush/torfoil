// Conteneurs Nintendo Switch : PFS0 (intérieur d'un NSP) et HFS0 (partitions
// d'un XCI). Même idée dans les deux cas — un en-tête, une table d'entrées, une
// table de chaînes, puis les données.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace install {

// Lecture à accès aléatoire sur un fichier de la carte SD. Un XCI de 32 Go ne
// tient évidemment pas en mémoire : tout se fait par fenêtres.
class FileReader {
public:
    FileReader() = default;
    ~FileReader();

    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    bool open(const std::string& path);
    void close();

    bool read(uint64_t offset, void* buffer, size_t length);
    uint64_t size() const { return size_; }
    bool is_open() const { return handle_ != nullptr; }
    const std::string& path() const { return path_; }

private:
    std::FILE* handle_ = nullptr;
    uint64_t size_ = 0;
    std::string path_;
};

struct ArchiveEntry {
    std::string name;
    uint64_t offset = 0;  // absolu dans le fichier
    uint64_t size = 0;
};

// `base` est le décalage du conteneur dans le fichier (0 pour un NSP).
bool parse_pfs0(FileReader& file, uint64_t base, std::vector<ArchiveEntry>& out,
                std::string* err = nullptr);
bool parse_hfs0(FileReader& file, uint64_t base, std::vector<ArchiveEntry>& out,
                std::string* err = nullptr);

// Localise la partition « secure » d'un XCI : c'est elle qui porte les NCA.
bool find_xci_secure_partition(FileReader& file, uint64_t& secure_base, std::string* err = nullptr);

// Ouvre indifféremment un NSP ou un XCI et renvoie la liste des NCA/tickets.
bool list_installable_contents(FileReader& file, std::vector<ArchiveEntry>& out,
                               std::string* err = nullptr);

}  // namespace install
