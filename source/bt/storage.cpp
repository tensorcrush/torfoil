#include "bt/storage.hpp"

#include <mutex>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "util/log.hpp"
#include "util/sha1.hpp"

namespace bt {

namespace {

bool fail(std::string* err, const std::string& msg) {
    if (err) *err = msg;
    return false;
}

// Traduit errno en quelque chose d'exploitable. « écriture impossible » sans
// cause, c'est exactement le message qui a fait perdre du temps.
std::string explain(int code, const std::string& path) {
    std::string base;
    switch (code) {
        case ENOSPC: base = "carte pleine"; break;
        case EFBIG:  base = "fichier trop gros pour ce système de fichiers "
                            "(carte en FAT32 ? la limite y est de 4 Go)"; break;
        case EACCES:
        case EPERM:  base = "accès refusé"; break;
        case EIO:    base = "erreur d'entrée/sortie (carte retirée ou défaillante)"; break;
        case ENOENT: base = "chemin introuvable"; break;
        case EMFILE:
        case ENFILE: base = "trop de fichiers ouverts"; break;
        case ENAMETOOLONG: base = "nom de fichier trop long"; break;
        default:     base = std::string("erreur système ") + std::to_string(code); break;
    }
    return base + " — " + path;
}

// Nom de fichier seul, pour un message d'erreur lisible à l'écran.
std::string basename_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

Storage::~Storage() {
    close();
}

bool Storage::ensure_directories(const std::string& abs_path, std::string* err) {
    // Crée chaque niveau intermédiaire ; le dernier composant est le fichier.
    size_t pos = abs_path.find('/');
    while (pos != std::string::npos) {
        const std::string dir = abs_path.substr(0, pos);
        // Ignore la racine du device ("sdmc:").
        if (!dir.empty() && dir.back() != ':') {
            if (::mkdir(dir.c_str(), 0777) != 0 && errno != EEXIST) {
                return fail(err, "impossible de créer le dossier " + dir);
            }
        }
        pos = abs_path.find('/', pos + 1);
    }
    return true;
}

bool Storage::open(const MetaInfo& meta, const std::string& base_dir, std::string* err) {
    close();

    if (!meta.valid()) return fail(err, "métadonnées incomplètes");

    meta_ = meta;
    base_dir_ = base_dir;
    if (!base_dir_.empty() && base_dir_.back() == '/') base_dir_.pop_back();

    // Un torrent multi-fichiers vit dans un sous-dossier portant son nom.
    const std::string root = meta_.single_file ? base_dir_ : base_dir_ + "/" + meta_.name;

    // Espace libre vérifié d'entrée : découvrir qu'il manque 10 Go après trois
    // heures de téléchargement n'aide personne.
    // Le « && vfs.f_blocks > 0 » n'est pas décoratif : si la console ne renseigne
    // pas ces champs, tout vaut zéro et on refuserait le torrent en annonçant
    // une carte pleine qui ne l'est pas. Mieux vaut ne pas vérifier que mentir.
    struct statvfs vfs{};
    if (::statvfs(base_dir_.c_str(), &vfs) == 0 && vfs.f_frsize > 0 && vfs.f_blocks > 0) {
        const uint64_t free_bytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
        if (free_bytes < meta_.total_size) {
            const uint64_t need_mb = (meta_.total_size - free_bytes) / (1024 * 1024);
            return fail(err, "espace insuffisant sur la carte : il manque " +
                                 std::to_string(need_mb) + " Mo");
        }
    }

    files_.reserve(meta_.files.size());
    for (const FileEntry& f : meta_.files) {
        FileSlot slot;
        slot.abs_path = root + "/" + f.path;
        slot.offset = f.offset;
        slot.length = f.length;
        if (!ensure_directories(slot.abs_path, err)) return false;

        // Taille déjà présente : elle décide s'il faudra agrandir avant d'écrire,
        // et permet de reprendre un téléchargement interrompu.
        struct stat st{};
        slot.allocated = (::stat(slot.abs_path.c_str(), &st) == 0)
                             ? static_cast<uint64_t>(st.st_size)
                             : 0;

        // Un fichier déjà présent avant cette ouverture peut contenir des pièces
        // valides ; un fichier que nous venons de créer, non.
        if (slot.allocated > 0) created_fresh_ = false;

        if (slot.length > large_file_threshold_) {
            // Fait maintenant, pas à 3,9 Go de téléchargement.
            if (!prepare_large_file(slot, err)) return false;
        } else if (slot.allocated == 0) {
            std::FILE* fp = std::fopen(slot.abs_path.c_str(), "wb");
            if (!fp) return fail(err, "création impossible : " + explain(errno, slot.abs_path));
            std::fclose(fp);
        }

        // Le fichier doit être ouvrable en écriture, sinon autant le savoir ici.
        std::FILE* probe = std::fopen(slot.abs_path.c_str(), "r+b");
        if (!probe) return fail(err, "ouverture impossible : " + explain(errno, slot.abs_path));
        std::fclose(probe);

        files_.push_back(std::move(slot));
    }

    return true;
}

// Un jeu Switch dépasse presque toujours 4 Go, et une carte formatée en FAT32 —
// le cas par défaut, et celui que beaucoup gardent volontairement parce que
// l'exFAT de la console a mauvaise réputation — refuse catégoriquement un tel
// fichier. C'est très exactement la cause du « écriture SD impossible ».
//
// Horizon prévoit le coup : le « fichier concaténé » est un dossier marqué d'un
// attribut spécial, que la couche système présente ensuite comme un fichier
// unique. On écrit et on lit dessus normalement, le découpage en tranches de
// 4 Go est invisible. C'est le mécanisme qu'utilisent les installeurs de NSP.
bool Storage::prepare_large_file(FileSlot& slot, std::string* err) {
    // Déjà à sa taille définitive : c'est soit un fichier concaténé créé par une
    // session précédente, soit un système de fichiers sans limite. Rien à faire.
    if (slot.allocated >= slot.length) return true;

    // Le cas qui a coûté cher : le fichier EXISTE, mais c'est un fichier
    // ordinaire laissé par une version antérieure. Sortir ici « parce qu'il
    // existe » le condamnait à rester ordinaire — donc à buter sur les 4 Go de
    // FAT32 à chaque téléchargement, sans que rien ne le signale à l'ouverture.
    if (slot.allocated > 0) {
        if (can_grow_to_full_size(slot)) return true;  // exFAT : aucun souci

        util::log_fmt("fichier ordinaire incompatible (%llu Mo déjà là, %llu Mo requis) : "
                      "recréation, progression perdue — %s",
                      static_cast<unsigned long long>(slot.allocated / (1024 * 1024)),
                      static_cast<unsigned long long>(slot.length / (1024 * 1024)),
                      slot.abs_path.c_str());
        recreated_ = true;
        slot.allocated = 0;
    }

    return create_large_file(slot, err);
}

// La décision est commune aux deux plateformes ; seul le moyen de créer un
// fichier de plus de 4 Go diffère.
bool Storage::create_large_file(FileSlot& slot, std::string* err) {
#ifdef __SWITCH__
    // Le fichier ordinaire ne conviendra pas : on le retire pour laisser la
    // place au dossier concaténé.
    std::remove(slot.abs_path.c_str());

    const Result rc = fsdevCreateFile(slot.abs_path.c_str(), slot.length, FsCreateOption_BigFile);
    if (R_SUCCEEDED(rc) || R_VALUE(rc) == 0x402) {  // 0x402 = existe déjà
        // Créé à sa taille finale : plus rien à agrandir ensuite.
        slot.allocated = slot.length;
        util::log_fmt("fichier concaténé prêt (%llu Go) : %s",
                      static_cast<unsigned long long>(slot.length / (1024 * 1024 * 1024)),
                      slot.abs_path.c_str());
        return true;
    }

    // Le fichier concaténé n'est utile que sur FAT32. Sur une carte qui n'en a
    // pas besoin, Horizon peut refuser la demande — ce n'est alors pas une
    // erreur, il suffit d'un fichier ordinaire.
    util::log_fmt("fichier concaténé refusé (0x%x), tentative en fichier ordinaire : %s", rc,
                  slot.abs_path.c_str());

    std::FILE* fp = std::fopen(slot.abs_path.c_str(), "wb");
    if (!fp) {
        return fail(err, "création du fichier volumineux « " + basename_of(slot.abs_path) +
                             " » (" + std::to_string(slot.length / (1024 * 1024 * 1024)) +
                             " Go) impossible : " + explain(errno, slot.abs_path));
    }
    std::fclose(fp);

    // Un fichier ordinaire doit prouver qu'il peut atteindre la taille voulue,
    // sinon on échouera plus tard, au pire moment.
    slot.allocated = 0;
    if (!can_grow_to_full_size(slot)) {
        return fail(err, "la carte SD ne peut pas contenir « " + basename_of(slot.abs_path) +
                             " » (" + std::to_string(slot.length / (1024 * 1024 * 1024)) +
                             " Go) : elle est en FAT32 et le mécanisme de fichier découpé a "
                             "été refusé. La reformater en exFAT résout le problème.");
    }
    return true;
#else
    // Hors console, il n'existe pas de fichier concaténé : un fichier ordinaire
    // est la seule option. Mais on vérifie qu'il pourra atteindre sa taille —
    // sans quoi l'échec surviendrait au milieu du téléchargement, ce qui est
    // précisément ce qu'on cherche à ne plus faire.
    std::FILE* fp = std::fopen(slot.abs_path.c_str(), "wb");
    if (!fp) return fail(err, "création impossible : " + explain(errno, slot.abs_path));
    std::fclose(fp);

    slot.allocated = 0;
    if (!can_grow_to_full_size(slot)) {
        return fail(err, "le système de fichiers ne peut pas contenir « " +
                             basename_of(slot.abs_path) + " » (" +
                             util::human_size(slot.length) +
                             "). Une carte en FAT32 plafonne à 4 Go par fichier ; "
                             "la reformater en exFAT résout le problème.");
    }
    return true;
#endif
}

// Le fichier peut-il atteindre sa taille finale ? Sur exFAT oui, sur FAT32 non
// dès qu'on dépasse 4 Go. On pose la question au système de fichiers plutôt que
// d'essayer de deviner son format : c'est lui qui décide, et sa réponse est
// définitive.
bool Storage::can_grow_to_full_size(FileSlot& slot) {
    if (growth_probe_) return growth_probe_(slot.abs_path, slot.length);

    std::FILE* fp = std::fopen(slot.abs_path.c_str(), "r+b");
    if (!fp) return false;

    errno = 0;
    const bool ok = ftruncate(fileno(fp), static_cast<off_t>(slot.length)) == 0;
    if (ok) {
        // On le remet à sa taille réelle : l'agrandissement se fera au fil de
        // l'eau, et un fichier gonflé à 23 Go de zéros compliquerait la reprise.
        (void)!ftruncate(fileno(fp), static_cast<off_t>(slot.allocated));
    }
    std::fclose(fp);
    return ok;
}

// Le système de fichiers de la console refuse une écriture au-delà de la fin du
// fichier : il faut l'agrandir d'abord. C'est la cause du fameux « écriture SD
// impossible » — le fichier était créé vide, et la toute première pièce tombait
// presque toujours au-delà de zéro.
bool Storage::ensure_allocated(FileSlot& slot, uint64_t needed) {
    if (needed <= slot.allocated) return true;
    if (!slot.handle) return false;

    // On alloue par tranches de 32 Mo plutôt qu'octet par octet : agrandir un
    // fichier coûte une mise à jour de la table d'allocation, autant ne pas la
    // payer à chaque bloc de 16 Ko.
    constexpr uint64_t kGrowChunk = 32ull * 1024 * 1024;
    uint64_t target = ((needed + kGrowChunk - 1) / kGrowChunk) * kGrowChunk;
    if (target > slot.length) target = slot.length;

    std::fflush(slot.handle);
    if (ftruncate(fileno(slot.handle), static_cast<off_t>(target)) != 0) {
        last_error_ = explain(errno, slot.abs_path);
        return false;
    }
    slot.allocated = target;
    return true;
}

void Storage::close() {
    // Ne jamais perdre une pièce en vol : ce qui est en mémoire part sur la
    // carte avant qu'on ferme quoi que ce soit.
    for (PieceBuffer& buf : buffers_) {
        if (!buf.data.empty()) flush_buffer(buf, nullptr);
    }
    buffers_.clear();

    for (FileSlot& slot : files_) {
        if (slot.handle) {
            std::fflush(slot.handle);
            std::fclose(slot.handle);
            slot.handle = nullptr;
        }
    }
    files_.clear();
    open_handles_ = 0;
    clock_ = 0;
    created_fresh_ = true;
}

void Storage::evict_if_needed() {
    if (open_handles_ < kMaxOpenHandles) return;

    FileSlot* oldest = nullptr;
    for (FileSlot& slot : files_) {
        if (!slot.handle) continue;
        if (!oldest || slot.last_used < oldest->last_used) oldest = &slot;
    }
    if (oldest) {
        // Vider le tampon avant de fermer : sinon une écriture qui échoue ici
        // passe inaperçue et la pièce est déclarée bonne alors qu'elle manque.
        if (std::fflush(oldest->handle) != 0) last_error_ = explain(errno, oldest->abs_path);
        std::fclose(oldest->handle);
        oldest->handle = nullptr;
        --open_handles_;
    }
}

std::FILE* Storage::acquire(FileSlot& slot) {
    if (!slot.handle) {
        evict_if_needed();
        slot.handle = std::fopen(slot.abs_path.c_str(), "r+b");
        if (!slot.handle) return nullptr;
        ++open_handles_;
    }
    slot.last_used = ++clock_;
    return slot.handle;
}

template <typename Fn>
bool Storage::for_each_span(uint64_t abs_offset, size_t len, Fn fn) {
    if (abs_offset + len > meta_.total_size) return false;

    // Recherche du premier fichier concerné (les offsets sont croissants).
    size_t idx = 0;
    {
        size_t lo = 0;
        size_t hi = files_.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (files_[mid].offset + files_[mid].length <= abs_offset) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        idx = lo;
    }

    size_t remaining = len;
    uint64_t cursor = abs_offset;

    while (remaining > 0 && idx < files_.size()) {
        FileSlot& slot = files_[idx];
        if (slot.length == 0) {
            ++idx;
            continue;
        }

        const uint64_t local = cursor - slot.offset;
        const uint64_t avail = slot.length - local;
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(avail, remaining));

        if (!fn(slot, local, chunk)) return false;

        cursor += chunk;
        remaining -= chunk;
        ++idx;
    }

    return remaining == 0;
}

Storage::PieceBuffer* Storage::buffer_for(uint32_t piece, bool create) {
    for (PieceBuffer& buf : buffers_) {
        if (buf.index == piece && !buf.data.empty()) {
            buf.last_used = ++clock_;
            return &buf;
        }
    }
    if (!create) return nullptr;

    const uint32_t size = meta_.size_of_piece(piece);
    if (size == 0 || size > kMaxBufferedPiece) return nullptr;

    // Budget mémoire. Point important : quand il est saturé on refuse de
    // bufferiser cette pièce-là, on n'évince PAS une pièce vivante pour lui
    // faire de la place. Évincer reviendrait à écrire sur la carte une pièce à
    // moitié faite, puis à la relire pour la vérifier — plus cher que l'écriture
    // directe qu'on cherchait justement à éviter. Avec plus de pairs que de
    // pièces bufferisables, cette éviction se déclenchait en boucle.
    uint64_t used = 0;
    for (const PieceBuffer& buf : buffers_) used += buf.data.size();
    if (used + size > kBufferBudget) return nullptr;

    PieceBuffer fresh;
    fresh.index = piece;
    fresh.data.assign(size, 0);
    fresh.have.assign((size + kBlockSize - 1) / kBlockSize, false);
    fresh.last_used = ++clock_;
    buffers_.push_back(std::move(fresh));
    return &buffers_.back();
}

// Écrit sur la carte les seuls blocs que ce tampon détient : le reste de la
// pièce peut déjà s'y trouver (reprise), il ne faut pas l'écraser avec des zéros.
bool Storage::flush_buffer(PieceBuffer& buf, std::string* err) {
    const uint32_t size = static_cast<uint32_t>(buf.data.size());

    uint32_t run_start = 0;
    bool in_run = false;

    for (uint32_t b = 0; b <= buf.have.size(); ++b) {
        const bool present = b < buf.have.size() && buf.have[b];
        if (present && !in_run) {
            run_start = b * kBlockSize;
            in_run = true;
        } else if (!present && in_run) {
            const uint32_t run_end = std::min(b * kBlockSize, size);
            if (!write_through(buf.index, run_start, buf.data.data() + run_start,
                               run_end - run_start, err)) {
                return false;
            }
            in_run = false;
        }
    }

    buf.data.clear();
    buf.have.clear();
    buf.blocks_present = 0;
    return true;
}

bool Storage::commit_piece(uint32_t piece, std::string* err) {
    for (auto it = buffers_.begin(); it != buffers_.end(); ++it) {
        if (it->index != piece || it->data.empty()) continue;
        const bool ok = flush_buffer(*it, err);
        buffers_.erase(it);
        return ok;
    }
    return true;  // déjà écrite au fil de l'eau
}

void Storage::discard_piece(uint32_t piece) {
    for (auto it = buffers_.begin(); it != buffers_.end(); ++it) {
        if (it->index != piece) continue;
        buffers_.erase(it);
        return;
    }
}

bool Storage::write_block(uint32_t piece, uint32_t offset, const uint8_t* data, size_t len,
                          std::string* err) {
    if (piece >= meta_.piece_count()) return fail(err, "pièce hors limites");
    const uint32_t piece_size = meta_.size_of_piece(piece);
    if (offset > piece_size || len > piece_size - offset) {
        return fail(err, "bloc hors limites");
    }

    // Chemin rapide : la pièce s'assemble en mémoire.
    if (offset % kBlockSize == 0 && len > 0) {
        if (PieceBuffer* buf = buffer_for(piece, true)) {
            const uint32_t first = offset / kBlockSize;
            // Rien n'oblige un pair à envoyer exactement 16 Ko. Ne marquer que
            // le premier bloc laissait le reste des octets en mémoire sans
            // jamais les écrire : la pièce paraissait incomplète, était relue
            // depuis la carte, et échouait à la vérification pour des données
            // qu'on avait pourtant bien reçues.
            const uint32_t last = static_cast<uint32_t>((offset + len - 1) / kBlockSize);
            if (last < buf->have.size()) {
                std::memcpy(buf->data.data() + offset, data, len);
                for (uint32_t b = first; b <= last; ++b) {
                    if (buf->have[b]) continue;
                    buf->have[b] = true;
                    ++buf->blocks_present;
                }
                return true;
            }
        }
    }

    return write_through(piece, offset, data, len, err);
}

bool Storage::write_through(uint32_t piece, uint32_t offset, const uint8_t* data, size_t len,
                            std::string* err) {
    std::lock_guard<std::recursive_mutex> lock(file_mutex_);
    const uint64_t abs = static_cast<uint64_t>(piece) * meta_.piece_length + offset;
    const uint8_t* cursor = data;

    last_error_.clear();

    const bool ok = for_each_span(abs, len, [&](FileSlot& slot, uint64_t local, size_t chunk) {
        std::FILE* fp = acquire(slot);
        if (!fp) {
            if (last_error_.empty()) last_error_ = explain(errno, slot.abs_path);
            return false;
        }
        if (!ensure_allocated(slot, local + chunk)) return false;

        // fseeko : un XCI dépasse largement les 4 Go, un offset 32 bits ne suffit pas.
        errno = 0;
        if (fseeko(fp, static_cast<off_t>(local), SEEK_SET) != 0) {
            last_error_ = explain(errno, slot.abs_path);
            return false;
        }
        if (std::fwrite(cursor, 1, chunk, fp) != chunk) {
            last_error_ = explain(errno ? errno : ENOSPC, slot.abs_path);
            return false;
        }
        cursor += chunk;
        return true;
    });

    if (!ok) {
        return fail(err, last_error_.empty()
                             ? std::string("écriture SD impossible (pièce ") +
                                   std::to_string(piece) + ")"
                             : "écriture SD : " + last_error_);
    }
    return true;
}

bool Storage::read_block(uint32_t piece, uint32_t offset, uint8_t* data, size_t len) {
    std::lock_guard<std::recursive_mutex> lock(file_mutex_);
    const uint64_t abs = static_cast<uint64_t>(piece) * meta_.piece_length + offset;
    uint8_t* cursor = data;

    return for_each_span(abs, len, [&](FileSlot& slot, uint64_t local, size_t chunk) {
        std::FILE* fp = acquire(slot);
        if (!fp) return false;
        // fseeko : un XCI dépasse largement les 4 Go, un offset 32 bits ne suffit pas.
        if (fseeko(fp, static_cast<off_t>(local), SEEK_SET) != 0) return false;
        if (std::fread(cursor, 1, chunk, fp) != chunk) return false;
        cursor += chunk;
        return true;
    });
}

bool Storage::take_complete_piece(uint32_t piece, std::vector<uint8_t>& out) {
    for (auto it = buffers_.begin(); it != buffers_.end(); ++it) {
        if (it->index != piece || it->data.empty()) continue;
        // Partielle : le reste dort déjà sur la carte, il faudra l'y relire.
        // Ce cas ne se produit qu'en reprise, on le laisse au chemin synchrone.
        if (it->blocks_present != it->have.size()) return false;
        out = std::move(it->data);
        buffers_.erase(it);
        return true;
    }
    return false;
}

bool Storage::hash_matches(uint32_t piece, const uint8_t* data, size_t len) const {
    if (piece >= meta_.piece_count()) return false;
    if (len != meta_.size_of_piece(piece)) return false;
    util::Sha1 sha;
    sha.update(data, len);
    return sha.digest() == meta_.piece_hashes[piece];
}

bool Storage::write_piece(uint32_t piece, const uint8_t* data, size_t len, std::string* err) {
    return write_through(piece, 0, data, len, err);
}

bool Storage::verify_piece(uint32_t piece) {
    if (piece >= meta_.piece_count()) return false;

    const uint32_t size = meta_.size_of_piece(piece);
    util::Sha1 sha;

    // La pièce est encore en mémoire et complète : on hache directement. C'est
    // le cas courant en téléchargement, et ça évite de relire jusqu'à 16 Mo
    // sur la carte pour chaque pièce.
    if (PieceBuffer* buf = buffer_for(piece, false)) {
        if (buf->blocks_present == buf->have.size()) {
            sha.update(buf->data.data(), size);
            return sha.digest() == meta_.piece_hashes[piece];
        }
        // Partielle : le reste est déjà sur la carte (blocs arrivés avant que la
        // pièce soit bufferisée). On écrit ce qu'on détient pour que la
        // relecture soit cohérente, puis on vérifie depuis la carte.
        if (!flush_buffer(*buf, nullptr)) return false;
        discard_piece(piece);
    }

    std::vector<uint8_t> chunk(kHashChunk);
    uint32_t done = 0;
    while (done < size) {
        const uint32_t take = std::min<uint32_t>(kHashChunk, size - done);
        if (!read_block(piece, done, chunk.data(), take)) return false;
        sha.update(chunk.data(), take);
        done += take;
    }

    return sha.digest() == meta_.piece_hashes[piece];
}

bool Storage::scan_existing(std::vector<bool>& out_have,
                            const std::function<bool(uint32_t, uint32_t)>& progress) {
    const uint32_t total = meta_.piece_count();
    out_have.assign(total, false);

    for (uint32_t i = 0; i < total; ++i) {
        out_have[i] = verify_piece(i);
        if (progress && !progress(i + 1, total)) return false;
    }
    return true;
}

std::string Storage::largest_file_path() const {
    const FileSlot* best = nullptr;
    for (const FileSlot& slot : files_) {
        if (!best || slot.length > best->length) best = &slot;
    }
    return best ? best->abs_path : std::string{};
}

}  // namespace bt
