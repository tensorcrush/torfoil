#include "bt/session.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>

#include "bt/magnet.hpp"
#include "bt/peer.hpp"
#include "util/log.hpp"
#include "bt/piece_picker.hpp"
#include "bt/piece_writer.hpp"
#include "bt/storage.hpp"
#include "util/clock.hpp"
#include "util/sha1.hpp"

namespace bt {

namespace {

// Beaucoup de ces emplacements sont occupés par des tentatives qui échoueront :
// il en faut plus que le nombre de pairs réellement voulus pour que la file
// avance. Chaque connexion coûte ~64 Ko de tampons, l'ordre de grandeur reste
// négligeable même en mode applet.
constexpr size_t kMaxPeersPerTorrent = 60;
// Nombre de blocs de 16 Ko demandés d'avance à un même pair. Trop bas, le débit
// est plafonné par le temps d'aller-retour : à 12 blocs et 100 ms de latence on
// ne dépasse pas 2 Mo/s par pair, quelle que soit la bande passante disponible.
constexpr uint32_t kMaxRequestsInFlight = 64;
constexpr uint64_t kBlockTimeoutMs = 45 * 1000;
constexpr uint64_t kMetadataRetryMs = 8 * 1000;
constexpr uint64_t kPeerRetryCooldownMs = 120 * 1000;
// Patience accordée à un pair qui nous étrangle sans rien avoir servi.
constexpr uint64_t kChokeGiveUpMs = 40 * 1000;
// Delai avant de retenter d'ouvrir des sockets apres un refus du systeme.
constexpr uint64_t kCeilingProbeMs = 20 * 1000;
// Sauvegarde périodique du point de reprise. Court, parce qu'une console peut
// être éteinte brutalement et que le fichier ne pèse qu'un bit par pièce.
constexpr uint64_t kResumeSaveIntervalMs = 10 * 1000;
constexpr uint32_t kResumeMagic = 0x54465231;  // "TFR1"

bool file_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

}  // namespace

const char* state_label(TorrentState state) {
    switch (state) {
        case TorrentState::FetchingMetadata: return "Métadonnées";
        case TorrentState::Checking: return "Vérification";
        case TorrentState::Downloading: return "Téléchargement";
        case TorrentState::Seeding: return "Partage";
        case TorrentState::Paused: return "En pause";
        case TorrentState::Queued: return "En attente";
        case TorrentState::Completed: return "Terminé";
        case TorrentState::Failed: return "Erreur";
    }
    return "?";
}

// ===========================================================================
// Torrent
// ===========================================================================

class Torrent : public PeerHandler {
public:
    Torrent(Session& session, const MetaInfo& meta, bool meta_complete)
        : session_(session), meta_(meta), meta_complete_(meta_complete) {
        hash_hex_ = util::to_hex(meta_.info_hash);
        name_ = meta_.name.empty() ? hash_hex_.substr(0, 16) : meta_.name;
        state_ = meta_complete_ ? TorrentState::Checking : TorrentState::FetchingMetadata;
    }

    ~Torrent() override {
        // Le fil d'écriture d'abord : il touche au stockage, qui disparaît avec
        // cet objet.
        writer_.stop();
        peers_.clear();
    }

    // Ferme toutes les connexions en rendant au picker les blocs qu'elles
    // devaient servir. Vider peers_ directement les laisserait réservés à des
    // pairs disparus jusqu'à expiration — 45 secondes de progression perdues à
    // chaque bascule de VPN ou mise en pause.
    void drop_all_peers() {
        if (peers_.empty()) return;
        if (meta_complete_) {
            for (const auto& peer : peers_) {
                for (const BlockRequest& r : peer->outstanding()) {
                    picker_.on_request_dropped(r.piece, r.offset, r.length);
                }
                if (peer->pieces().size() > 0) picker_.peer_removed(peer->pieces());
            }
        }
        peers_.clear();
    }

    const std::string& hash_hex() const { return hash_hex_; }
    TorrentState state() const { return state_; }

    void set_paused(bool paused) {
        user_paused_ = paused;
        apply_gate();
    }

    // Mise en attente par la file, qui n'est pas une mise en pause : reprendre
    // un torrent que la file avait rangé doit le rendre à la file, pas le
    // lancer de force devant les autres.
    void set_queued(bool queued) {
        queued_ = queued;
        apply_gate();
    }

    bool user_paused() const { return user_paused_; }

    // Occupe-t-il un des créneaux de téléchargement ?
    //
    // Seul ce qui tire sur le réseau compte. Un torrent qui partage ne demande
    // rien, il répond. Et une relecture ne touche que la carte : la ranger dans
    // la file ferait attendre derrière un téléchargement en cours un torrent
    // déjà complet, qui n'avait qu'à se relire pour le découvrir.
    bool holds_slot() const {
        return state_ == TorrentState::Downloading || state_ == TorrentState::FetchingMetadata;
    }

    bool waiting_in_queue() const { return state_ == TorrentState::Queued; }

    // ---- boucle moteur ----

    void tick(uint64_t now_ms, const std::shared_ptr<net::Transport>& transport) {
        if (state_ == TorrentState::Paused || state_ == TorrentState::Queued ||
            state_ == TorrentState::Failed) {
            return;
        }
        if (!transport || !transport->ready()) {
            // Plus de réseau, ou VPN exigé et absent : on lâche tout et on
            // attend. Rien ne peut sortir tant qu'un chemin n'existe pas.
            drop_all_peers();
            if (session_.privacy().require_vpn && message_.empty()) {
                message_ = "en attente du VPN (exigé dans les réglages)";
            }
            return;
        }

        // Nettoyage des connexions mortes.
        peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                    [](const std::unique_ptr<PeerConnection>& p) {
                                        return p->closed();
                                    }),
                     peers_.end());

        for (auto& peer : peers_) peer->tick(now_ms);

        // Un pair qui nous étrangle sans rien nous avoir donné occupe une place
        // pour rien. Or la place est la ressource rare : sur ce protocole on
        // n'obtient de débit qu'en étant « déchoké », et plus on essaie de pairs
        // différents, plus on tombe sur ceux qui ont une place libre. Le lâcher
        // vaut mieux que l'attendre.
        for (auto& peer : peers_) {
            if (peer->state() != PeerConnection::State::Ready) continue;
            if (!peer->peer_choking() || peer->bytes_downloaded() > 0) continue;
            const uint64_t since = peer->choking_since_ms();
            if (since != 0 && now_ms - since > kChokeGiveUpMs) {
                peer->close("étrangle sans rien donner");
            }
        }

        connect_more(now_ms, *transport);

        if (!meta_complete_) {
            drive_metadata(now_ms);
            return;
        }

        if (state_ == TorrentState::Checking) return;  // traité par run_check()

        collect_written_pieces();
        if (state_ == TorrentState::Failed) return;

        picker_.expire_requests(now_ms, kBlockTimeoutMs);

        for (auto& peer : peers_) {
            if (peer->state() != PeerConnection::State::Ready) continue;
            request_blocks(*peer, now_ms);
        }

        if (now_ms - last_resume_save_ms_ > kResumeSaveIntervalMs) {
            save_resume();
            last_resume_save_ms_ = now_ms;
        }

        update_rates(now_ms);
    }

    void gather_poll(std::vector<net::PollItem>& items, std::vector<PeerConnection*>& owners) {
        for (auto& peer : peers_) {
            if (peer->closed()) continue;
            net::PollItem item;
            peer->fill_poll(item);
            if (!item.tcp) continue;
            items.push_back(item);
            owners.push_back(peer.get());
        }
    }

    // ---- métadonnées / démarrage ----

    bool prepare_storage(const std::string& dir, std::string* err) {
        dir_ = dir;
        if (!storage_.open(meta_, dir_, err)) {
            state_ = TorrentState::Failed;
            message_ = err ? *err : "ouverture du stockage impossible";
            util::log_line("stockage refusé pour « " + name_ + " » : " + message_);
            return false;
        }
        util::log_fmt("stockage prêt : %s, %llu fichiers, %llu Mo%s", name_.c_str(),
                      static_cast<unsigned long long>(meta_.files.size()),
                      static_cast<unsigned long long>(meta_.total_size / (1024 * 1024)),
                      storage_.created_fresh() ? " (nouveaux fichiers)" : " (reprise possible)");
        picker_.init(meta_);
        primary_file_ = storage_.largest_file_path();
        writer_.start(storage_);

        // Point de reprise vide, écrit tout de suite.
        //
        // Sans lui, un torrent interrompu avant la première sauvegarde
        // périodique repartait en relecture intégrale de la carte au lancement
        // suivant. Et cette relecture ne pouvait RIEN trouver : on préalloue les
        // fichiers à leur taille finale, si bien que « le fichier fait 35 Go »
        // ne prouve plus qu'il contient quoi que ce soit. On hachait 35 Go de
        // zéros pendant un quart d'heure pour conclure qu'on n'avait rien.
        //
        // Ce fichier est la preuve que le contenu vient de nous. Son absence
        // signifie désormais ce qu'elle devrait : des fichiers d'origine
        // inconnue, seul cas où une relecture se justifie.
        if (storage_.created_fresh() && !file_exists(resume_path())) save_resume();
        return true;
    }

    // Reprise : on fait confiance au fichier .torfoil s'il colle, sinon on
    // relit toute la carte SD (long mais sûr).
    void run_check() {
        // Le drapeau « passer la vérification » est remis à zéro ICI, à
        // l'entrée. Il était auparavant effacé seulement à la sortie de la
        // relecture, si bien qu'un appui sur B en dehors de toute vérification
        // restait armé et faisait sauter la prochaine — celle qui, elle,
        // servait peut-être vraiment.
        session_.skip_check_.store(false);

        if (storage_.recreated()) {
            // Un fichier a été recréé : le point de reprise décrirait des pièces
            // qui n'existent plus. Le garder ferait croire le torrent à moitié
            // fini, et il ne se terminerait jamais.
            std::remove(resume_path().c_str());
            message_ = "fichier recréé pour la carte SD — téléchargement repris de zéro";
            util::log_line("point de reprise invalidé : " + message_);
            checked_ = true;
            state_ = TorrentState::Downloading;
            apply_gate();
            return;
        }

        if (load_resume()) {
            message_ = "reprise";
        } else if (!storage_.created_fresh() && any_data_on_disk()) {
            // Relire toute la carte peut prendre un quart d'heure sur un gros
            // torrent : on affiche l'avancement, sans quoi l'application paraît
            // figée et on la tue.
            std::vector<bool> have;
            const uint32_t total_pieces = meta_.piece_count();
            const bool complete = storage_.scan_existing(have, [&](uint32_t done, uint32_t total) {
                (void)total;
                check_progress_ = total_pieces ? static_cast<float>(done) / total_pieces : 0.0f;
                message_ = "vérification " +
                           std::to_string(static_cast<int>(check_progress_ * 100)) +
                           "% (B pour passer)";
                session_.publish_status();
                // Interrompre est sans danger : ce qui a été vérifié est acquis,
                // le reste sera redemandé au réseau.
                return !session_.skip_check_.load() && session_.running_.load();
            });
            for (uint32_t i = 0; i < have.size(); ++i) {
                if (have[i]) picker_.set_have(i);
            }
            if (!complete) {
                util::log_line("vérification interrompue — le reste sera retéléchargé");
            }
            session_.skip_check_.store(false);
            save_resume();
        }
        check_progress_ = 0.0f;
        checked_ = true;

        state_ = picker_.is_complete() ? TorrentState::Seeding : TorrentState::Downloading;
        if (picker_.is_complete()) message_ = "terminé";
        apply_gate();
    }

    // ---- trackers ----

    bool wants_announce(uint64_t now_ms) const {
        if (state_ == TorrentState::Paused || state_ == TorrentState::Queued ||
            state_ == TorrentState::Failed) {
            return false;
        }
        return now_ms >= next_announce_ms_;
    }

    void mark_announced(uint64_t now_ms, uint32_t interval_s) {
        next_announce_ms_ = now_ms + static_cast<uint64_t>(interval_s) * 1000;
    }

    const std::vector<std::string>& trackers() const { return meta_.trackers; }

    // L'événement d'annonce n'est pas décoratif : « started » à chaque passage
    // fait croire au tracker à un nouveau client toutes les demi-heures, et
    // « completed » est ce qui alimente son compteur de sources complètes. On
    // les émet donc chacun une fois, au bon moment.
    AnnounceEvent next_event() {
        if (!announce_started_) return AnnounceEvent::Started;
        if (meta_complete_ && picker_.is_complete() && !completed_announced_) {
            return AnnounceEvent::Completed;
        }
        return AnnounceEvent::None;
    }

    void note_event_sent(AnnounceEvent event) {
        if (event == AnnounceEvent::Started) announce_started_ = true;
        if (event == AnnounceEvent::Completed) completed_announced_ = true;
    }

    AnnounceRequest make_request(const std::array<uint8_t, 20>& peer_id, AnnounceEvent event) const {
        AnnounceRequest req;
        req.info_hash = meta_.info_hash;
        req.peer_id = peer_id;
        req.port = 6881;
        req.downloaded = picker_.bytes_done();
        req.uploaded = uploaded_;
        req.left = meta_complete_ ? (meta_.total_size - picker_.bytes_done()) : 0;
        req.event = event;
        req.key = announce_key_;
        return req;
    }

    void apply_announce(const AnnounceResult& result, uint64_t now_ms) {
        if (!result.ok) {
            // Un tracker mort sur cinq n'est pas un problème tant que le DHT
            // fournit des pairs. On ne le remonte que si on n'a rien d'autre,
            // sinon l'écran affiche une erreur pendant que ça télécharge.
            if (known_peers_.empty()) message_ = result.failure;
            return;
        }
        message_.clear();
        seeders_ = std::max(seeders_, result.seeders);
        leechers_ = std::max(leechers_, result.leechers);

        // Le tracker dit à quel rythme il veut être réinterrogé. L'ignorer et
        // repasser toutes les demi-heures, c'est se faire bannir par ceux qui
        // en demandent moins, et perdre des pairs chez ceux qui en demandent
        // plus. La planification provisoire posée à l'émission (courte, pour
        // pouvoir réessayer vite en cas d'échec) est ici remplacée par la vraie.
        const uint32_t interval = std::max<uint32_t>(60, result.interval_s);
        next_announce_ms_ = now_ms + static_cast<uint64_t>(interval) * 1000;

        add_peers(result.peers);
    }

    // Point d'entrée commun aux trois sources de pairs : trackers, DHT et PEX.
    void add_peers(const std::vector<net::Endpoint>& found) {
        if (!found.empty()) message_.clear();
        for (const net::Endpoint& ep : found) {
            if (!ep.valid()) continue;
            const bool known = std::any_of(known_peers_.begin(), known_peers_.end(),
                                           [&](const net::Endpoint& e) { return e == ep; });
            if (!known) known_peers_.push_back(ep);
        }
    }

    // Un pair nous a envoyé son carnet d'adresses (ut_pex).
    void on_peers_discovered(PeerConnection&, const std::vector<net::Endpoint>& found) override {
        if (!session_.privacy().enable_pex || meta_.is_private) return;
        add_peers(found);
    }

    const util::Hash160& info_hash() const { return meta_.info_hash; }
    bool needs_peers() const { return known_peers_.size() < 60; }
    bool is_private() const { return meta_.is_private; }

    // ---- état ----

    TorrentStatus status() const {
        TorrentStatus s;
        s.hash_hex = hash_hex_;
        s.name = name_;
        s.state = state_;
        s.total_size = meta_complete_ ? meta_.total_size : 0;
        s.downloaded = meta_complete_ ? picker_.bytes_done() : 0;
        s.uploaded = uploaded_;
        s.progress = state_ == TorrentState::Checking
                         ? check_progress_
                         : (meta_complete_ ? picker_.progress() : metadata_progress());
        s.rate_down = rate_down_;
        s.rate_up = rate_up_;
        s.peers_known = static_cast<uint32_t>(known_peers_.size());
        s.seeders = seeders_;
        s.leechers = leechers_;
        s.message = message_;
        s.save_path = dir_;
        // Tant que le nom n'est pas connu (magnet sans métadonnées), rien sur
        // la carte n'appartient encore à ce torrent : on laisse vide plutôt que
        // de désigner le dossier commun, qui reviendrait à revendiquer les
        // fichiers de tous les autres.
        s.content_root = meta_.name.empty() ? std::string() : dir_ + "/" + meta_.name;
        s.primary_file = primary_file_;

        s.is_private = meta_.is_private;
        s.trackers = meta_.trackers;
        s.pieces_rejected = corrupt_pieces_;
        if (meta_complete_) {
            s.pieces_total = meta_.piece_count();
            s.pieces_done = picker_.have().count();
            s.piece_length = meta_.piece_length;
            s.file_count = static_cast<uint32_t>(meta_.files.size());
            s.magnet = Session::magnet_for(meta_);
        }

        for (const auto& peer : peers_) {
            if (peer->state() != PeerConnection::State::Ready) continue;
            ++s.peers_connected;
            if (peer->peer_choking()) ++s.peers_choking;
            s.blocks_in_flight += peer->in_flight();
        }

        if (rate_down_ > 0 && meta_complete_ && s.total_size > s.downloaded) {
            s.eta_s = (s.total_size - s.downloaded) / rate_down_;
        }
        return s;
    }

    void delete_files() {
        storage_.close();
        // Suppression best-effort : on retire les fichiers du torrent, pas le
        // dossier de téléchargement lui-même.
        for (const FileEntry& f : meta_.files) {
            const std::string path =
                (meta_.single_file ? dir_ : dir_ + "/" + meta_.name) + "/" + f.path;
            ::remove(path.c_str());
        }
        ::remove(resume_path().c_str());
    }

    // ---- PeerHandler ----

    void on_peer_ready(PeerConnection& peer) override {
        if (meta_complete_) {
            peer.set_piece_count(meta_.piece_count());
            if (picker_.have().count() > 0) peer.send_bitfield(picker_.have());
            peer.send_interested(!picker_.is_complete());
        }
        // On accepte de servir : Mullvad ne redirige pas de port, donc autant
        // être utile aux pairs auxquels on s'est connecté soi-même. Sauf si le
        // partage est refusé — dans ce cas on étrangle d'emblée plutôt que de
        // laisser les pairs gaspiller des requêtes qu'on ignorera.
        peer.send_choke(session_.privacy().no_upload);
    }

    void on_peer_bitfield(PeerConnection& peer) override {
        if (!meta_complete_) return;
        picker_.peer_added(peer.pieces());
    }

    void on_peer_have(PeerConnection& peer, uint32_t piece) override {
        if (!meta_complete_) return;
        picker_.peer_has(piece);
    }

    void on_peer_choke(PeerConnection& peer, bool choked) override {
        if (!choked || !meta_complete_) return;
        // Les requêtes en vol sont perdues : on les remet dans le pot commun.
        for (const BlockRequest& r : outstanding_of(peer)) {
            picker_.on_request_dropped(r.piece, r.offset, r.length);
        }
    }

    void on_peer_block(PeerConnection& peer, uint32_t piece, uint32_t offset, const uint8_t* data,
                       uint32_t len) override {
        if (!meta_complete_ || state_ == TorrentState::Paused ||
            state_ == TorrentState::Queued) {
            return;
        }
        if (piece >= meta_.piece_count() || len == 0) return;

        std::string err;
        if (!storage_.write_block(piece, offset, data, len, &err)) {
            // Journalisé, pas seulement affiché : c'est l'erreur la plus
            // difficile à reproduire, et sans le contexte exact (pièce, décalage
            // absolu, fichier visé) on ne peut que deviner.
            util::log_fmt("ÉCHEC écriture : %s | pièce %u, décalage %u, %u octets, "
                          "position absolue %llu",
                          err.c_str(), piece, offset, len,
                          static_cast<unsigned long long>(
                              static_cast<uint64_t>(piece) * meta_.piece_length + offset));
            state_ = TorrentState::Failed;
            message_ = err;
            return;
        }
        downloaded_since_sample_ += len;

        if (!picker_.on_block_received(piece, offset, len)) return;

        // Pièce complète. Le hachage et l'écriture partent sur le fil dédié :
        // ils prennent des dizaines de millisecondes, et les passer ici
        // reviendrait à cesser de lire les sockets pendant tout ce temps.
        // La suite du traitement a lieu à la réception du résultat.
        std::vector<uint8_t> whole;
        if (writer_.running() && storage_.take_complete_piece(piece, whole)) {
            writer_.submit(piece, std::move(whole));
            return;
        }

        // Pièce seulement partiellement en mémoire (le reste dort sur la carte,
        // cas d'une reprise) : il faut relire, donc rester synchrone.
        finish_piece(piece, storage_.verify_piece(piece), true, std::string());
    }

    // Suite commune à la fin d'une pièce, qu'elle arrive du fil d'écriture ou
    // du chemin synchrone de reprise.
    void finish_piece(uint32_t piece, bool hash_ok, bool commit_needed,
                      const std::string& write_err) {
        if (!write_err.empty()) {
            util::log_fmt("ÉCHEC écriture définitive de la pièce %u : %s", piece,
                          write_err.c_str());
            state_ = TorrentState::Failed;
            message_ = write_err;
            return;
        }

        picker_.on_piece_verified(piece, hash_ok);
        if (!hash_ok) {
            storage_.discard_piece(piece);
            ++corrupt_pieces_;
            message_ = "pièce corrompue rejetée (" + std::to_string(corrupt_pieces_) + ")";
            return;
        }

        if (commit_needed) {
            std::string err;
            if (!storage_.commit_piece(piece, &err)) {
                util::log_fmt("ÉCHEC écriture définitive de la pièce %u : %s", piece, err.c_str());
                state_ = TorrentState::Failed;
                message_ = err;
                return;
            }
        }

        for (auto& p : peers_) {
            if (p->state() == PeerConnection::State::Ready) p->send_have(piece);
        }

        if (picker_.is_complete()) {
            save_resume();
            state_ = TorrentState::Seeding;
            message_ = "terminé";
            for (auto& p : peers_) p->send_interested(false);
        }
    }

    // Récupère ce que le fil d'écriture a terminé depuis le tour précédent.
    void collect_written_pieces() {
        for (const PieceWriter::Result& r : writer_.take_results()) {
            finish_piece(r.piece, !r.hash_failed, false, r.err);
        }
    }

    void on_peer_metadata_size(PeerConnection& peer, uint32_t total_size) override {
        if (meta_complete_ || total_size == 0) return;
        if (metadata_size_ != 0) return;

        metadata_size_ = total_size;
        metadata_buf_.assign(total_size, '\0');
        const uint32_t pieces =
            (total_size + PeerConnection::kMetadataPieceSize - 1) / PeerConnection::kMetadataPieceSize;
        metadata_have_.assign(pieces, false);
    }

    void on_peer_metadata_piece(PeerConnection& peer, uint32_t index, const uint8_t* data,
                                uint32_t len) override {
        if (meta_complete_ || metadata_size_ == 0) return;
        if (index >= metadata_have_.size() || metadata_have_[index]) return;

        const uint64_t offset = static_cast<uint64_t>(index) * PeerConnection::kMetadataPieceSize;
        if (offset + len > metadata_size_) return;

        std::memcpy(&metadata_buf_[offset], data, len);
        metadata_have_[index] = true;

        if (std::all_of(metadata_have_.begin(), metadata_have_.end(), [](bool b) { return b; })) {
            finish_metadata();
        }
    }

    void on_peer_request(PeerConnection& peer, uint32_t piece, uint32_t offset,
                         uint32_t len) override {
        if (session_.privacy().no_upload) return;
        if (!meta_complete_ || len == 0 || len > 32 * 1024) return;
        if (piece >= meta_.piece_count() || !picker_.have().get(piece)) return;

        std::vector<uint8_t> buf(len);
        if (!storage_.read_block(piece, offset, buf.data(), len)) return;
        peer.send_block(piece, offset, buf.data(), len);
        uploaded_ += len;
        uploaded_since_sample_ += len;
    }

    void on_peer_closed(PeerConnection& peer, const char* reason) override {
        if (meta_complete_ && peer.pieces().size() > 0) picker_.peer_removed(peer.pieces());

        // Les blocs qu'il devait servir repartent immédiatement dans le pot
        // commun. Sans ça ils restent réservés 45 secondes à un pair qui n'existe
        // plus, et la fin de téléchargement traîne interminablement.
        if (meta_complete_) {
            for (const BlockRequest& r : peer.outstanding()) {
                picker_.on_request_dropped(r.piece, r.offset, r.length);
            }
        }

        // La suppression effective a lieu dans tick() : on ne touche pas au
        // vecteur pendant qu'on itère dessus.
        (void)reason;
    }

private:
    std::string resume_path() const { return dir_ + "/." + hash_hex_.substr(0, 16) + ".torfoil"; }

    float metadata_progress() const {
        if (metadata_have_.empty()) return 0.0f;
        const size_t got = static_cast<size_t>(
            std::count(metadata_have_.begin(), metadata_have_.end(), true));
        return static_cast<float>(got) / static_cast<float>(metadata_have_.size());
    }

    std::vector<BlockRequest> outstanding_of(PeerConnection& peer) { return peer.outstanding(); }

    void connect_more(uint64_t now_ms, net::Transport& transport) {
        // Plafond découvert à l'usage : on ne redemande pas indéfiniment des
        // sockets que le système a déjà refusées.
        //
        // Ce plafond doit pouvoir remonter. Une pénurie de sockets est presque
        // toujours passagère (un pic de connexions qui se referment, un tampon
        // système momentanément plein) ; le verrouiller à vie condamne le
        // torrent à traîner à quatre pairs jusqu'au redémarrage de l'appli.
        if (socket_ceiling_ < kMaxPeersPerTorrent && now_ms >= ceiling_retry_ms_) {
            socket_ceiling_ = std::min(kMaxPeersPerTorrent, socket_ceiling_ + 8);
            ceiling_retry_ms_ = now_ms + kCeilingProbeMs;
            util::log_fmt("plafond de sockets relevé à %llu, nouvel essai",
                          static_cast<unsigned long long>(socket_ceiling_));
        }

        const size_t cap = std::min(kMaxPeersPerTorrent, socket_ceiling_);
        if (peers_.size() >= cap) return;
        if (known_peers_.empty()) return;

        size_t attempts = 0;
        while (peers_.size() < cap && attempts < known_peers_.size()) {
            const net::Endpoint ep = known_peers_[next_peer_ % known_peers_.size()];
            ++next_peer_;
            ++attempts;

            const bool busy = std::any_of(peers_.begin(), peers_.end(),
                                          [&](const std::unique_ptr<PeerConnection>& p) {
                                              return p->endpoint() == ep;
                                          });
            if (busy) continue;

            const auto cooldown = failed_peers_.find(ep.ipv4);
            if (cooldown != failed_peers_.end() && now_ms < cooldown->second) continue;

            auto peer = std::make_unique<PeerConnection>(transport, *this, ep, meta_.info_hash,
                                                         session_.peer_id_,
                                                         session_.privacy().enable_pex &&
                                                             !meta_.is_private);

            // errno est global et rémanent : sans cette remise à zéro, on lit
            // ici l'échec d'une opération sans rapport (un envoi UDP du tunnel,
            // par exemple) et on prend une panne de socket pour argent comptant.
            errno = 0;
            if (!peer->start()) {
                // Distinguer « ce pair ne répond pas » de « la console ne peut
                // plus ouvrir de socket ». Mettre le pair en quarantaine deux
                // minutes pour une limite locale reviendrait à brûler tout le
                // carnet d'adresses en quelques secondes.
                if (errno == EMFILE || errno == ENFILE || errno == ENOMEM ||
                    errno == ENOBUFS) {
                    const size_t ceiling = std::max<size_t>(8, peers_.size());
                    if (ceiling < socket_ceiling_) {
                        util::log_fmt("plafond de sockets atteint à %llu connexions (errno %d)",
                                      static_cast<unsigned long long>(ceiling), errno);
                    }
                    socket_ceiling_ = ceiling;
                    ceiling_retry_ms_ = now_ms + kCeilingProbeMs;
                    return;
                }
                failed_peers_[ep.ipv4] = now_ms + kPeerRetryCooldownMs;
                continue;
            }
            peers_.push_back(std::move(peer));
        }
    }

    void drive_metadata(uint64_t now_ms) {
        if (metadata_size_ == 0) return;
        if (now_ms - last_metadata_request_ms_ < kMetadataRetryMs) return;
        last_metadata_request_ms_ = now_ms;

        // On redemande périodiquement toutes les pièces manquantes : c'est peu
        // coûteux (16 Ko) et ça évite de rester bloqué sur un pair muet.
        for (uint32_t i = 0; i < metadata_have_.size(); ++i) {
            if (metadata_have_[i]) continue;
            for (auto& peer : peers_) {
                if (peer->state() != PeerConnection::State::Ready) continue;
                if (!peer->supports_metadata()) continue;
                peer->request_metadata_piece(i);
                break;
            }
        }
    }

    void finish_metadata() {
        MetaInfo parsed;
        std::string err;
        if (!parse_info_dict(metadata_buf_, meta_.info_hash, parsed, &err)) {
            // Un pair a menti : on repart de zéro sur les métadonnées.
            message_ = "métadonnées rejetées : " + err;
            metadata_size_ = 0;
            metadata_buf_.clear();
            metadata_have_.clear();
            return;
        }

        parsed.trackers = meta_.trackers;  // le magnet portait les trackers
        meta_ = parsed;
        meta_complete_ = true;
        name_ = meta_.name;

        if (!prepare_storage(dir_, &err)) return;

        for (auto& peer : peers_) {
            peer->set_piece_count(meta_.piece_count());
            peer->send_interested(true);
        }

        checked_ = false;
        state_ = TorrentState::Checking;
        apply_gate();
    }

    void request_blocks(PeerConnection& peer, uint64_t now_ms) {
        if (picker_.is_complete()) return;
        if (peer.peer_choking()) return;

        peer.send_interested(true);

        const uint32_t in_flight = peer.in_flight();
        if (in_flight >= kMaxRequestsInFlight) return;

        BlockRequest picks[kMaxRequestsInFlight];
        const uint32_t want = kMaxRequestsInFlight - in_flight;
        const uint32_t got = picker_.pick(peer.pieces(), picks, want, now_ms);
        for (uint32_t i = 0; i < got; ++i) peer.send_request(picks[i]);
    }

    void update_rates(uint64_t now_ms) {
        if (last_rate_ms_ == 0) {
            last_rate_ms_ = now_ms;
            return;
        }
        const uint64_t span = now_ms - last_rate_ms_;
        if (span < 1000) return;

        rate_down_ = static_cast<uint32_t>(downloaded_since_sample_ * 1000 / span);
        rate_up_ = static_cast<uint32_t>(uploaded_since_sample_ * 1000 / span);
        downloaded_since_sample_ = 0;
        uploaded_since_sample_ = 0;
        last_rate_ms_ = now_ms;
    }

    bool any_data_on_disk() const {
        const std::string root = meta_.single_file ? dir_ : dir_ + "/" + meta_.name;
        for (const FileEntry& f : meta_.files) {
            struct stat st{};
            if (::stat((root + "/" + f.path).c_str(), &st) == 0 && st.st_size > 0) return true;
        }
        return false;
    }

    // Fichier de reprise : évite de re-hacher 30 Go à chaque lancement.
    void save_resume() {
        if (!meta_complete_) return;
        std::FILE* fp = std::fopen(resume_path().c_str(), "wb");
        if (!fp) return;

        uint8_t header[32];
        util::wr_be32(header, kResumeMagic);
        util::wr_be32(header + 4, meta_.piece_count());
        std::memcpy(header + 8, meta_.info_hash.data(), 20);
        util::wr_be32(header + 28, static_cast<uint32_t>(picker_.have().bytes().size()));

        std::fwrite(header, 1, sizeof(header), fp);
        std::fwrite(picker_.have().bytes().data(), 1, picker_.have().bytes().size(), fp);
        std::fclose(fp);
    }

    bool load_resume() {
        if (!file_exists(resume_path())) return false;
        std::FILE* fp = std::fopen(resume_path().c_str(), "rb");
        if (!fp) return false;

        uint8_t header[32];
        if (std::fread(header, 1, sizeof(header), fp) != sizeof(header)) {
            std::fclose(fp);
            return false;
        }

        const bool sane = util::rd_be32(header) == kResumeMagic &&
                          util::rd_be32(header + 4) == meta_.piece_count() &&
                          std::memcmp(header + 8, meta_.info_hash.data(), 20) == 0;
        if (!sane) {
            std::fclose(fp);
            return false;
        }

        const uint32_t len = util::rd_be32(header + 28);
        std::vector<uint8_t> bits(len);
        const bool read_ok = std::fread(bits.data(), 1, len, fp) == len;
        std::fclose(fp);
        if (!read_ok) return false;

        Bitfield have;
        if (!have.from_bytes(bits.data(), bits.size(), meta_.piece_count())) return false;
        for (uint32_t i = 0; i < meta_.piece_count(); ++i) {
            if (have.get(i)) picker_.set_have(i);
        }
        return true;
    }

    // Recalcule l'état à partir des deux freins — l'utilisateur et la file — et
    // de l'avancement réel. Passer par un seul endroit évite l'incohérence
    // classique : reprendre un torrent en attente le ferait démarrer, alors que
    // la file venait justement de le ranger.
    void apply_gate() {
        if (state_ == TorrentState::Failed) return;

        if (user_paused_) {
            if (state_ != TorrentState::Paused) drop_all_peers();
            state_ = TorrentState::Paused;
            return;
        }
        // Une relecture va au bout. Le frein de la file s'appliquera à sa fin,
        // qui rappelle apply_gate() — et d'ici là on saura si ce torrent a
        // seulement besoin d'un créneau.
        if (state_ == TorrentState::Checking) return;
        if (queued_) {
            if (state_ != TorrentState::Queued) drop_all_peers();
            state_ = TorrentState::Queued;
            return;
        }
        if (state_ != TorrentState::Paused && state_ != TorrentState::Queued) return;

        if (!meta_complete_) state_ = TorrentState::FetchingMetadata;
        else if (!checked_) state_ = TorrentState::Checking;
        else state_ = picker_.is_complete() ? TorrentState::Seeding : TorrentState::Downloading;
    }

    Session& session_;
    MetaInfo meta_;
    bool meta_complete_ = false;
    bool checked_ = false;
    bool user_paused_ = false;
    bool queued_ = false;

    std::string hash_hex_;
    std::string name_;
    std::string dir_;
    std::string primary_file_;
    std::string message_;

    Storage storage_;
    PieceWriter writer_;
    PiecePicker picker_;
    TorrentState state_ = TorrentState::FetchingMetadata;

    std::vector<std::unique_ptr<PeerConnection>> peers_;
    std::vector<net::Endpoint> known_peers_;
    size_t socket_ceiling_ = kMaxPeersPerTorrent;
    uint64_t ceiling_retry_ms_ = 0;
    float check_progress_ = 0.0f;
    std::map<uint32_t, uint64_t> failed_peers_;
    size_t next_peer_ = 0;

    // Assemblage des métadonnées (BEP 9)
    std::string metadata_buf_;
    std::vector<bool> metadata_have_;
    uint32_t metadata_size_ = 0;
    uint64_t last_metadata_request_ms_ = 0;

    uint64_t uploaded_ = 0;
    uint64_t downloaded_since_sample_ = 0;
    uint64_t uploaded_since_sample_ = 0;
    uint64_t last_rate_ms_ = 0;
    uint32_t rate_down_ = 0;
    uint32_t rate_up_ = 0;

    uint32_t seeders_ = 0;
    uint32_t leechers_ = 0;
    uint32_t corrupt_pieces_ = 0;
    uint32_t announce_key_ = 0;
    uint64_t next_announce_ms_ = 0;
    bool announce_started_ = false;
    bool completed_announced_ = false;
    uint64_t last_resume_save_ms_ = 0;

    friend class Session;
};

// ===========================================================================
// Session
// ===========================================================================

Session::Session() {
    peer_id_ = make_peer_id();
}

Session::~Session() {
    stop();
}

bool Session::start(const std::string& download_dir, std::string* err) {
    if (running_.load()) return true;

    download_dir_ = download_dir;
    if (!download_dir_.empty() && download_dir_.back() == '/') download_dir_.pop_back();

    ::mkdir(download_dir_.c_str(), 0777);
    if (!file_exists(download_dir_)) {
        if (err) *err = "dossier de téléchargement inaccessible : " + download_dir_;
        return false;
    }

    if (!transport_) transport_ = net::make_bsd_transport();

    running_.store(true);
    engine_thread_ = std::thread([this] { engine_loop(); });
    announce_thread_ = std::thread([this] { announce_loop(); });

    restore_torrents();
    return true;
}

void Session::stop() {
    if (!running_.exchange(false)) return;

    announce_cv_.notify_all();
    if (engine_thread_.joinable()) engine_thread_.join();
    if (announce_thread_.joinable()) announce_thread_.join();

    // Point de reprise final. Sans lui, tout ce qui a été téléchargé depuis la
    // dernière sauvegarde périodique est oublié : au relancement l'application
    // redemande des pièces qu'elle possède déjà, ou pire, relit toute la carte.
    for (auto& t : torrents_) t->save_resume();

    torrents_.clear();
}

void Session::set_transport(std::shared_ptr<net::Transport> transport) {
    {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        transport_ = std::move(transport);
    }
    std::lock_guard<std::mutex> lock(cmd_mutex_);
    Command cmd;
    cmd.type = Command::Type::SetTransport;
    commands_.push_back(std::move(cmd));
}

std::shared_ptr<net::Transport> Session::transport() const {
    std::lock_guard<std::mutex> lock(transport_mutex_);
    return transport_;
}

std::string Session::transport_name() const {
    const auto t = transport();
    return t ? t->name() : "aucun";
}

void Session::set_privacy(const Privacy& privacy) {
    std::lock_guard<std::mutex> lock(privacy_mutex_);
    privacy_ = privacy;
}

Session::Privacy Session::privacy() const {
    std::lock_guard<std::mutex> lock(privacy_mutex_);
    return privacy_;
}

bool Session::network_ready() const {
    const auto t = transport();
    return t && t->ready();
}

// Où l'on garde les liens des torrents en cours. Un fichier texte par torrent,
// lisible et modifiable à la main — c'est aussi le format le plus simple à
// réparer si quelque chose tourne mal.
std::string Session::magnet_path(const std::string& hash_hex) const {
    return download_dir_ + "/." + hash_hex.substr(0, 16) + ".magnet";
}

void Session::remember_magnet(const std::string& hash_hex, const std::string& uri) const {
    if (std::FILE* fp = std::fopen(magnet_path(hash_hex).c_str(), "wb")) {
        std::fprintf(fp, "%s\n", uri.c_str());
        std::fclose(fp);
    }
}

// Reconstruit un lien à partir de ce qu'on sait du torrent. Suffisant pour le
// reprendre : l'info_hash désigne le contenu, les trackers accélèrent le
// démarrage, et le DHT fournit le reste.
std::string Session::magnet_for(const MetaInfo& meta) {
    std::string uri = "magnet:?xt=urn:btih:" + util::to_hex(meta.info_hash);
    if (!meta.name.empty()) uri += "&dn=" + util::url_encode(meta.name);
    for (const std::string& tracker : meta.trackers) {
        uri += "&tr=" + util::url_encode(tracker);
    }
    return uri;
}

// Un torrent déjà là ne sera pas ajouté deux fois par le moteur — mais il
// répondait quand même « ajouté », ce qui laissait croire à un import réussi
// alors que rien n'avait bougé. On regarde donc les deux endroits où un torrent
// peut exister : l'instantané publié, et la file de commandes pas encore lue.
bool Session::has_torrent(const std::string& hash_hex) const {
    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        // La file est lue à l'envers : c'est le dernier ordre qui décide. Sans
        // ça, retirer un torrent puis le rajouter aussitôt se heurtait à
        // l'instantané, qui le montre encore pendant un quart de seconde — et
        // l'ajout était refusé pour un torrent déjà en train de disparaître.
        for (auto cmd = commands_.rbegin(); cmd != commands_.rend(); ++cmd) {
            if (cmd->hash_hex != hash_hex) continue;
            if (cmd->type == Command::Type::AddTorrent) return true;
            if (cmd->type == Command::Type::Remove) return false;
        }
    }

    std::lock_guard<std::mutex> lock(status_mutex_);
    for (const TorrentStatus& s : cached_status_) {
        if (s.hash_hex == hash_hex) return true;
    }
    return false;
}

bool Session::add_magnet(const std::string& uri, std::string* err) {
    MagnetLink link;
    if (!parse_magnet(uri, link, err)) return false;

    MetaInfo meta;
    meta.info_hash = link.info_hash;
    meta.name = link.display_name.empty() ? util::to_hex(link.info_hash).substr(0, 16)
                                          : util::sanitize_filename(link.display_name);
    meta.trackers = link.trackers;

    const std::string hash_hex = util::to_hex(link.info_hash);
    if (has_torrent(hash_hex)) {
        if (err) *err = "torrent déjà présent";
        return false;
    }
    // Conservé tout de suite : un torrent ajouté puis interrompu avant la fin
    // des métadonnées doit quand même revenir au prochain démarrage.
    remember_magnet(hash_hex, uri);

    Command cmd;
    cmd.type = Command::Type::AddTorrent;
    cmd.meta = std::move(meta);
    cmd.meta_complete = false;
    cmd.hash_hex = hash_hex;

    std::lock_guard<std::mutex> lock(cmd_mutex_);
    commands_.push_back(std::move(cmd));
    return true;
}

// Relance tout ce qui était en cours. Sans ça, fermer l'application abandonne
// silencieusement les téléchargements : les fichiers restent sur la carte, à
// moitié faits, et rien ne les reprend jamais — c'est ce qui laissait un jeu
// éternellement « en-tête illisible » dans la bibliothèque.
int Session::restore_torrents() {
    DIR* dir = ::opendir(download_dir_.c_str());
    if (!dir) return 0;

    std::vector<std::string> links;
    while (dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() < 8) continue;
        if (name.compare(name.size() - 7, 7, ".magnet") != 0) continue;

        std::FILE* fp = std::fopen((download_dir_ + "/" + name).c_str(), "rb");
        if (!fp) continue;
        char buffer[2048];
        if (std::fgets(buffer, sizeof(buffer), fp)) {
            std::string uri(buffer);
            while (!uri.empty() && (uri.back() == '\n' || uri.back() == '\r')) uri.pop_back();
            if (!uri.empty()) links.push_back(uri);
        }
        std::fclose(fp);
    }
    ::closedir(dir);

    int restored = 0;
    for (const std::string& uri : links) {
        if (add_magnet(uri, nullptr)) ++restored;
    }
    if (restored > 0) {
        util::log_fmt("%d torrent(s) repris automatiquement au démarrage", restored);
    }
    return restored;
}

bool Session::add_torrent_file(const std::string& path, std::string* err) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        if (err) *err = "fichier .torrent introuvable";
        return false;
    }

    std::string blob;
    char chunk[8192];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), fp)) > 0) blob.append(chunk, n);
    std::fclose(fp);

    MetaInfo meta;
    if (!parse_torrent(blob, meta, err)) return false;

    if (has_torrent(util::to_hex(meta.info_hash))) {
        if (err) *err = "torrent déjà présent";
        return false;
    }

    Command cmd;
    cmd.type = Command::Type::AddTorrent;
    cmd.meta = std::move(meta);
    cmd.meta_complete = true;
    cmd.hash_hex = util::to_hex(cmd.meta.info_hash);

    // Un torrent ajouté par fichier doit revenir au redémarrage comme les autres.
    // Le .torrent d'origine peut avoir disparu ; on garde donc un lien
    // reconstruit, qui suffit à retrouver le contenu.
    remember_magnet(cmd.hash_hex, magnet_for(cmd.meta));

    std::lock_guard<std::mutex> lock(cmd_mutex_);
    commands_.push_back(std::move(cmd));
    return true;
}

void Session::pause(const std::string& hash_hex) {
    std::lock_guard<std::mutex> lock(cmd_mutex_);
    commands_.push_back(Command{Command::Type::Pause, hash_hex, false, {}, false, nullptr});
}

void Session::skip_check(const std::string& hash_hex) {
    (void)hash_hex;  // une seule relecture tourne à la fois
    skip_check_.store(true);
}

void Session::resume(const std::string& hash_hex) {
    std::lock_guard<std::mutex> lock(cmd_mutex_);
    commands_.push_back(Command{Command::Type::Resume, hash_hex, false, {}, false, nullptr});
}

void Session::remove(const std::string& hash_hex, bool delete_files) {
    std::lock_guard<std::mutex> lock(cmd_mutex_);
    commands_.push_back(
        Command{Command::Type::Remove, hash_hex, delete_files, {}, false, nullptr});
}

std::vector<TorrentStatus> Session::snapshot() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return cached_status_;
}

Torrent* Session::find(const std::string& hash_hex) {
    for (auto& t : torrents_) {
        if (t->hash_hex() == hash_hex) return t.get();
    }
    return nullptr;
}

void Session::process_commands() {
    std::vector<Command> batch;
    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        batch.swap(commands_);
    }

    for (Command& cmd : batch) {
        switch (cmd.type) {
            case Command::Type::AddTorrent: {
                if (find(cmd.hash_hex)) break;  // déjà présent
                auto torrent = std::make_unique<Torrent>(*this, cmd.meta, cmd.meta_complete);
                torrent->dir_ = download_dir_;
                util::random_bytes(reinterpret_cast<uint8_t*>(&torrent->announce_key_), 4);

                if (cmd.meta_complete) {
                    // La vérification elle-même est déclenchée par la boucle
                    // moteur, qui publie d'abord l'état « Vérification ».
                    std::string err;
                    torrent->prepare_storage(download_dir_, &err);
                }
                torrents_.push_back(std::move(torrent));
                break;
            }
            case Command::Type::Pause:
                if (Torrent* t = find(cmd.hash_hex)) t->set_paused(true);
                break;
            case Command::Type::Resume:
                if (Torrent* t = find(cmd.hash_hex)) t->set_paused(false);
                break;
            case Command::Type::Remove:
                for (size_t i = 0; i < torrents_.size(); ++i) {
                    if (torrents_[i]->hash_hex() != cmd.hash_hex) continue;
                    // Arrêter la recherche DHT avec le torrent : sinon elle
                    // continue d'émettre indéfiniment pour un torrent parti.
                    dht_.forget(torrents_[i]->info_hash());
                    // Le lien disparaît avec le torrent, sinon il reviendrait
                    // tout seul au prochain démarrage.
                    std::remove(magnet_path(cmd.hash_hex).c_str());
                    if (cmd.flag) torrents_[i]->delete_files();
                    torrents_.erase(torrents_.begin() + static_cast<long>(i));
                    break;
                }
                break;
            case Command::Type::SetTransport:
                // Toutes les connexions repartent de zéro sur le nouveau chemin.
                for (auto& t : torrents_) t->drop_all_peers();
                break;
        }
    }
}

// Fait tenir le nombre de téléchargements simultanés dans la limite réglée.
//
// L'ordre est celui d'ajout : le premier arrivé est le premier servi, et il le
// reste. Promouvoir dans l'ordre et rétrograder à rebours garantit qu'un
// torrent ne change pas de camp d'un tour à l'autre — un va-et-vient
// coûterait, à chaque bascule, toutes les connexions ouvertes.
void Session::update_queue() {
    const int limit = max_active_.load();
    if (limit <= 0) {
        for (auto& t : torrents_) {
            if (t->waiting_in_queue()) t->set_queued(false);
        }
        return;
    }

    int active = 0;
    for (auto& t : torrents_) {
        if (t->holds_slot()) ++active;
    }

    for (auto& t : torrents_) {
        if (active >= limit) break;
        if (!t->waiting_in_queue() || t->user_paused()) continue;
        t->set_queued(false);
        if (t->holds_slot()) ++active;
    }

    for (auto it = torrents_.rbegin(); it != torrents_.rend() && active > limit; ++it) {
        if (!(*it)->holds_slot()) continue;
        (*it)->set_queued(true);
        --active;
    }
}

void Session::publish_status() {
    std::vector<TorrentStatus> fresh;
    fresh.reserve(torrents_.size());

    uint32_t down = 0;
    uint32_t up = 0;
    for (auto& t : torrents_) {
        TorrentStatus s = t->status();
        down += s.rate_down;
        up += s.rate_up;
        fresh.push_back(std::move(s));
    }

    total_rate_down_.store(down);
    total_rate_up_.store(up);

    std::lock_guard<std::mutex> lock(status_mutex_);
    cached_status_ = std::move(fresh);
}

void Session::queue_announce(AnnounceJob job) {
    {
        std::lock_guard<std::mutex> lock(announce_mutex_);
        announce_queue_.push_back(std::move(job));
    }
    announce_cv_.notify_one();
}

void Session::engine_loop() {
    uint64_t last_publish = 0;

    while (running_.load()) {
        const uint64_t now = util::now_ms();

        process_commands();
        update_queue();

        // Les vérifications de reprise sont longues : on les fait ici, hors du
        // poll, et l'IHM affiche « Vérification » pendant ce temps.
        for (auto& t : torrents_) {
            if (t->state() == TorrentState::Checking) {
                publish_status();
                t->run_check();
            }
        }

        const Privacy rules = privacy();
        auto transport_ref = transport();

        // « Exiger le VPN » : sans tunnel, on ne présente aucun transport au
        // moteur. Il ne s'agit pas de désactiver une option quelque part — il
        // n'y a simplement plus de chemin par lequel un octet pourrait sortir.
        if (rules.require_vpn && transport_ref &&
            std::string(transport_ref->name()).find("direct") != std::string::npos) {
            transport_ref.reset();
        }

        for (auto& t : torrents_) t->tick(now, transport_ref);

        // Le DHT suit le transport : quand le VPN monte ou tombe, ses requêtes
        // doivent emprunter le même chemin que le reste, jamais l'ancien.
        if (transport_ref.get() != dht_transport_) {
            dht_transport_ = transport_ref.get();
            dht_.set_transport(transport_ref);
        }

        if (rules.enable_dht) {
            for (auto& t : torrents_) {
                // BEP 27 : un torrent marqué privé ne doit chercher ses pairs
                // que chez son tracker. Passer par le DHT ou PEX révèle son
                // contenu au reste du réseau et fait bannir le compte, ce qui
                // est bien pire qu'un torrent lent.
                if (t->is_private()) continue;
                if (t->needs_peers()) dht_.find_peers(t->info_hash());
            }
            dht_.tick(now);

            for (const auto& result : dht_.take_results()) {
                const std::string hash_hex = util::to_hex(result.first);
                if (Torrent* t = find(hash_hex)) t->add_peers(result.second);
            }
        }

        // Annonces dues
        for (auto& t : torrents_) {
            if (!t->wants_announce(now)) continue;
            const auto& urls = t->trackers();
            if (urls.empty()) {
                t->mark_announced(now, 300);
                continue;
            }
            const AnnounceEvent event = t->next_event();
            bool sent_any = false;
            for (const std::string& url : urls) {
                // Une annonce en http:// simple transporte l'info_hash en clair.
                // Qui observe la ligne sait exactement ce qui est téléchargé.
                if (rules.https_trackers_only && url.rfind("http://", 0) == 0) continue;

                AnnounceJob job;
                job.hash_hex = t->hash_hex();
                job.url = url;
                job.request = t->make_request(peer_id_, event);
                queue_announce(std::move(job));
                sent_any = true;
            }
            if (sent_any) t->note_event_sent(event);

            // Planification provisoire, volontairement courte : elle ne sert
            // qu'à ne pas harceler un tracker muet. Dès qu'une réponse arrive,
            // c'est l'intervalle qu'il réclame qui s'applique. Poser 30 minutes
            // ici comme avant, c'était punir une annonce ratée d'une demi-heure
            // d'attente alors qu'un tracker peut redevenir joignable en une.
            t->mark_announced(now, 300);
        }

        // Réponses de trackers
        {
            std::vector<AnnounceReply> replies;
            {
                std::lock_guard<std::mutex> lock(announce_mutex_);
                replies.swap(announce_replies_);
            }
            for (const AnnounceReply& reply : replies) {
                if (Torrent* t = find(reply.hash_hex)) t->apply_announce(reply.result, now);
            }
        }

        // Un seul poll pour tous les pairs de tous les torrents.
        std::vector<net::PollItem> items;
        std::vector<PeerConnection*> owners;
        for (auto& t : torrents_) t->gather_poll(items, owners);

        // La socket DHT voyage dans le même poll ; owners reste aligné grâce à
        // l'entrée nulle qu'on ajoute en face.
        const bool poll_dht = dht_.ready();
        if (poll_dht) {
            net::PollItem dht_item;
            dht_.fill_poll(dht_item);
            items.push_back(dht_item);
            owners.push_back(nullptr);
        }

        if (!items.empty() && transport_ref) {
            const int rc = transport_ref->poll(items.data(), items.size(), 50);
            if (rc > 0) {
                for (size_t i = 0; i < items.size(); ++i) {
                    if (owners[i]) owners[i]->on_poll(items[i], now);
                    else dht_.on_poll(items[i], now);
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (now - last_publish >= 250) {
            publish_status();
            last_publish = now;
        }
    }
}

void Session::announce_loop() {
    while (running_.load()) {
        AnnounceJob job;
        {
            std::unique_lock<std::mutex> lock(announce_mutex_);
            announce_cv_.wait_for(lock, std::chrono::milliseconds(500),
                                  [this] { return !announce_queue_.empty() || !running_.load(); });
            if (!running_.load()) return;
            if (announce_queue_.empty()) continue;
            job = std::move(announce_queue_.front());
            announce_queue_.erase(announce_queue_.begin());
        }

        const auto transport_ref = transport();
        if (!transport_ref || !transport_ref->ready()) continue;

        TrackerClient client(*transport_ref);
        AnnounceResult result = client.announce(job.url, job.request);

        std::lock_guard<std::mutex> lock(announce_mutex_);
        announce_replies_.push_back(AnnounceReply{job.hash_hex, job.url, std::move(result)});
    }
}

}  // namespace bt
