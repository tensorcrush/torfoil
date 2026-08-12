#include "util/archive.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "miniz.h"
#include "util/bytes.hpp"
#include "util/log.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace util {

namespace {

constexpr uint64_t kFat32FileLimit = 0xFFFFFFFFull;
constexpr size_t kChunk = 256 * 1024;

std::string lowered(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void make_dirs(const std::string& path) {
    // Chaque niveau séparément : mkdir ne crée pas les parents, et une archive
    // range volontiers ses fichiers à trois dossiers de profondeur.
    size_t pos = path.find('/');
    while (pos != std::string::npos) {
        const std::string part = path.substr(0, pos);
        if (!part.empty() && part.back() != ':') ::mkdir(part.c_str(), 0777);
        pos = path.find('/', pos + 1);
    }
    ::mkdir(path.c_str(), 0777);
}

// Un nom d'entrée d'archive n'est pas digne de confiance : « ../../ailleurs »
// écrirait hors du dossier demandé.
bool safe_entry_name(const std::string& name, std::string& out) {
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos) return false;
    if (name.find("..") != std::string::npos) return false;
    out.clear();
    out.reserve(name.size());
    for (char c : name) {
        if (c == '\\') { out += '/'; continue; }
        // Les caractères interdits par FAT32 sont remplacés plutôt que rejetés :
        // une archive faite sous Linux en contient sans le savoir.
        if (c == '"' || c == '*' || c == ':' || c == '<' || c == '>' || c == '?' || c == '|') {
            out += '_';
            continue;
        }
        out += c;
    }
    return !out.empty();
}

// Crée un fichier capable d'atteindre `size`. Au-delà de 4 Go sur FAT32, ce
// n'est pas un fichier ordinaire mais un fichier concaténé Horizon.
bool create_target(const std::string& path, uint64_t size, std::string* err) {
    if (size <= kFat32FileLimit) return true;
#ifdef __SWITCH__
    std::remove(path.c_str());
    const Result rc = fsdevCreateFile(path.c_str(), size, FsCreateOption_BigFile);
    if (R_SUCCEEDED(rc) || R_VALUE(rc) == 0x402) return true;
    util::log_fmt("extraction : fichier concaténé refusé (0x%x) pour %s", rc, path.c_str());
    return true;  // la carte n'est peut-être pas en FAT32 ; on tente en ordinaire
#else
    (void)path;
    (void)err;
    return true;
#endif
}

}  // namespace

std::string ExtractProgress::current_file() const {
    std::lock_guard<std::mutex> lock(mutex);
    return current;
}

std::string ExtractProgress::last_message() const {
    std::lock_guard<std::mutex> lock(mutex);
    return message;
}

int ExtractProgress::percent() const {
    const uint64_t total = bytes_total.load();
    if (total == 0) return 0;
    const uint64_t done = bytes_done.load();
    return static_cast<int>(done >= total ? 100 : (done * 100) / total);
}

bool looks_like_zip(const std::string& path) {
    return ends_with(lowered(path), ".zip");
}

bool extract_zip(const std::string& zip_path, const std::string& dest_dir,
                 ExtractProgress& progress) {
    auto finish = [&progress](bool ok, const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(progress.mutex);
            progress.message = message;
            progress.current.clear();
        }
        progress.ok.store(ok);
        progress.running.store(false);
        return ok;
    };

    progress.running.store(true);
    progress.cancel.store(false);
    progress.ok.store(false);
    progress.files_done.store(0);
    progress.files_total.store(0);
    progress.bytes_done.store(0);
    progress.bytes_total.store(0);

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
        return finish(false, "archive illisible : " + zip_path);
    }

    // Le total ne compte que ce qui sera vraiment écrit : inclure une entrée
    // refusée bloquerait la progression à 99 % sans que rien ne soit en panne.
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    uint64_t total = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        std::string kept;
        if (mz_zip_reader_file_stat(&zip, i, &st) && !mz_zip_reader_is_file_a_directory(&zip, i) &&
            safe_entry_name(st.m_filename, kept)) {
            total += st.m_uncomp_size;
        }
    }
    progress.files_total.store(count);
    progress.bytes_total.store(total);

    make_dirs(dest_dir);

    std::vector<uint8_t> buffer(kChunk);
    uint32_t written_files = 0;

    for (mz_uint i = 0; i < count; ++i) {
        if (progress.cancel.load()) {
            mz_zip_reader_end(&zip);
            return finish(false, "extraction interrompue");
        }

        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;

        std::string relative;
        if (!safe_entry_name(st.m_filename, relative)) {
            util::log_line("extraction : entrée ignorée « " + std::string(st.m_filename) + " »");
            continue;
        }
        const std::string target = dest_dir + "/" + relative;

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            make_dirs(target);
            progress.files_done.fetch_add(1);
            continue;
        }

        const size_t cut = target.rfind('/');
        if (cut != std::string::npos) make_dirs(target.substr(0, cut));

        {
            std::lock_guard<std::mutex> lock(progress.mutex);
            progress.current = relative;
        }

        std::string err;
        create_target(target, st.m_uncomp_size, &err);

        std::FILE* out = std::fopen(target.c_str(), "wb");
        if (!out) {
            mz_zip_reader_end(&zip);
            return finish(false, "écriture impossible : " + relative);
        }

        // Extraction en flux : une archive de plusieurs gigaoctets ne tient pas
        // en mémoire, et la console en a moins que le PC qui l'a fabriquée.
        struct Sink {
            std::FILE* file;
            ExtractProgress* progress;
            bool failed;
        } sink{out, &progress, false};

        auto write_cb = [](void* opaque, mz_uint64, const void* data, size_t size) -> size_t {
            Sink* s = static_cast<Sink*>(opaque);
            if (s->failed || s->progress->cancel.load()) return 0;
            if (std::fwrite(data, 1, size, s->file) != size) {
                s->failed = true;
                return 0;
            }
            s->progress->bytes_done.fetch_add(size);
            return size;
        };

        const mz_bool ok = mz_zip_reader_extract_to_callback(&zip, i, write_cb, &sink, 0);
        std::fclose(out);

        if (!ok || sink.failed) {
            mz_zip_reader_end(&zip);
            if (progress.cancel.load()) return finish(false, "extraction interrompue");
            return finish(false, "échec sur « " + relative + " »");
        }

        ++written_files;
        progress.files_done.fetch_add(1);
    }

    mz_zip_reader_end(&zip);
    util::log_fmt("extraction terminée : %u fichier(s) depuis %s", written_files, zip_path.c_str());
    return finish(true, std::to_string(written_files) + " fichier(s) extraits");
}

}  // namespace util
