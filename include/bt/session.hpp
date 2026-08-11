// Orchestration.
//
// Trois fils d'exécution, et une règle simple pour éviter les courses :
//   * thread moteur   — SEUL propriétaire des torrents et des pairs ;
//   * thread annonces — dialogue avec les trackers (bloquant, donc à part) ;
//   * IHM             — ne lit qu'un instantané recopié, ne touche à rien.
// Toute mutation demandée par l'IHM passe par une file de commandes.
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bt/dht.hpp"
#include "bt/metainfo.hpp"
#include "bt/tracker.hpp"
#include "net/transport.hpp"

namespace bt {

enum class TorrentState {
    FetchingMetadata,  // magnet : on récupère le dictionnaire « info »
    Checking,          // relecture des pièces déjà présentes sur la SD
    Downloading,
    Seeding,
    Paused,
    // En file d'attente : prêt à démarrer, mais le nombre de téléchargements
    // simultanés est atteint. Distinct de Paused, qui est une décision de
    // l'utilisateur et que la file ne doit jamais défaire toute seule.
    Queued,
    Completed,
    Failed,
};

const char* state_label(TorrentState state);

struct TorrentStatus {
    std::string hash_hex;
    std::string name;
    TorrentState state = TorrentState::FetchingMetadata;
    float progress = 0.0f;
    uint64_t total_size = 0;
    uint64_t downloaded = 0;
    uint64_t uploaded = 0;
    uint32_t rate_down = 0;
    uint32_t rate_up = 0;
    uint32_t peers_connected = 0;
    uint32_t peers_known = 0;
    // Pairs connectés qui refusent de nous servir. C'est la mesure qui explique
    // un débit nul alors que tout paraît en ordre.
    uint32_t peers_choking = 0;
    uint32_t blocks_in_flight = 0;
    uint32_t seeders = 0;
    uint32_t leechers = 0;
    uint64_t eta_s = 0;
    std::string message;
    std::string save_path;  // dossier de téléchargement commun
    // Chemin qui appartient VRAIMENT à ce torrent : le fichier lui-même s'il
    // est seul, sinon son dossier. Distinct de save_path, qui est le même pour
    // tout le monde et ne permet donc de reconnaître personne.
    std::string content_root;
    std::string primary_file;  // plus gros fichier du torrent

    // Détail, affiché dans l'écran « Informations ». Renseigné seulement quand
    // les métadonnées sont connues.
    uint32_t pieces_total = 0;
    uint32_t pieces_done = 0;
    uint32_t pieces_rejected = 0;
    uint64_t piece_length = 0;
    uint32_t file_count = 0;
    bool is_private = false;
    std::string magnet;
    std::vector<std::string> trackers;
};

class Torrent;

class Session {
public:
    Session();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool start(const std::string& download_dir, std::string* err = nullptr);
    void stop();

    // Bascule de transport (activation/coupure du VPN). Les connexions en cours
    // sont fermées : rien ne survit à un changement de chemin réseau.
    void set_transport(std::shared_ptr<net::Transport> transport);
    std::string transport_name() const;
    bool network_ready() const;

    // Transport actif, pour l'auto-diagnostic. Passer par lui garantit que la
    // mesure emprunte exactement le même chemin que les téléchargements —
    // tunnel compris.
    std::shared_ptr<net::Transport> transport_for_diagnostics() const { return transport(); }

    // Validés immédiatement (analyse du lien / du fichier), appliqués par le
    // thread moteur.
    bool add_magnet(const std::string& uri, std::string* err = nullptr);
    bool add_torrent_file(const std::string& path, std::string* err = nullptr);

    // Torrent déjà connu ? Sert à répondre « déjà présent » au lieu d'annoncer
    // un ajout qui sera silencieusement ignoré par le moteur.
    bool has_torrent(const std::string& hash_hex) const;

    // Reprend les torrents laissés en cours lors de la session précédente.
    // Renvoie combien ont été relancés.
    int restore_torrents();

    // Réglages de confidentialité. Appliqués à chaud : couper le DHT arrête les
    // requêtes en cours, pas seulement les suivantes.
    struct Privacy {
        bool require_vpn = false;
        bool https_trackers_only = false;
        bool enable_dht = true;
        bool enable_pex = true;
        bool no_upload = false;
    };
    void set_privacy(const Privacy& privacy);
    Privacy privacy() const;

    // Nombre de torrents autorisés à télécharger en même temps. Au-delà, les
    // suivants attendent leur tour. Zéro lève la limite.
    //
    // Ce n'est pas un confort d'affichage : trente torrents lancés ensemble se
    // partagent la même liaison et les mêmes entrées/sorties de carte SD, et
    // aucun ne finit. Deux à la fois terminent l'un après l'autre, ce qui est
    // toujours plus utile que trente à moitié.
    void set_max_active(int n) { max_active_.store(n < 0 ? 0 : n); }
    int max_active() const { return max_active_.load(); }

    // Lien magnet reconstruit pour un torrent connu — affiché dans l'interface
    // et réutilisable ailleurs.
    static std::string magnet_for(const MetaInfo& meta);

    std::vector<TorrentStatus> snapshot() const;

    void pause(const std::string& hash_hex);
    // Interrompt la relecture de la carte. Ce qui est vérifié reste acquis.
    void skip_check(const std::string& hash_hex);
    void resume(const std::string& hash_hex);
    void remove(const std::string& hash_hex, bool delete_files);

    uint32_t rate_down() const { return total_rate_down_.load(); }
    uint32_t rate_up() const { return total_rate_up_.load(); }
    const std::string& download_dir() const { return download_dir_; }

private:
    friend class Torrent;

    struct Command {
        enum class Type { AddTorrent, Pause, Resume, Remove, SetTransport } type;
        std::string hash_hex;
        bool flag = false;  // Remove : supprimer aussi les fichiers
        MetaInfo meta;      // AddTorrent : complet, ou seulement info_hash+trackers
        bool meta_complete = false;
        std::shared_ptr<net::Transport> transport;
    };

    struct AnnounceJob {
        std::string hash_hex;
        std::string url;
        AnnounceRequest request;
    };

    struct AnnounceReply {
        std::string hash_hex;
        std::string url;
        AnnounceResult result;
    };

    void engine_loop();
    void announce_loop();
    void process_commands();
    void publish_status();
    void update_queue();

    void queue_announce(AnnounceJob job);
    std::shared_ptr<net::Transport> transport() const;

    Torrent* find(const std::string& hash_hex);

    std::string magnet_path(const std::string& hash_hex) const;
    void remember_magnet(const std::string& hash_hex, const std::string& uri) const;

    std::string download_dir_;
    std::array<uint8_t, 20> peer_id_{};

    // Propriété exclusive du thread moteur.
    std::vector<std::unique_ptr<Torrent>> torrents_;

    mutable std::mutex transport_mutex_;
    std::shared_ptr<net::Transport> transport_;

    mutable std::mutex privacy_mutex_;
    Privacy privacy_;

    // Propriété du thread moteur, comme les torrents.
    Dht dht_;
    const net::Transport* dht_transport_ = nullptr;

    mutable std::mutex cmd_mutex_;
    std::vector<Command> commands_;

    mutable std::mutex status_mutex_;
    std::vector<TorrentStatus> cached_status_;

    std::mutex announce_mutex_;
    std::condition_variable announce_cv_;
    std::vector<AnnounceJob> announce_queue_;
    std::vector<AnnounceReply> announce_replies_;

    std::thread engine_thread_;
    std::thread announce_thread_;
    std::atomic<bool> running_{false};
    // Demande d'interruption de la relecture en cours.
    //
    // Volontairement HORS de la file de commandes : celle-ci n'est vidée
    // qu'entre deux tours de boucle, et la relecture ne rend la main qu'une
    // fois terminée. Un ordre déposé dans la file attendrait donc la fin de ce
    // qu'il est censé interrompre. Un atomique traverse, lui, la frontière des
    // fils sans rien attendre. Une seule relecture tourne à la fois, ce drapeau
    // unique suffit.
    std::atomic<bool> skip_check_{false};

    std::atomic<int> max_active_{2};

    std::atomic<uint32_t> total_rate_down_{0};
    std::atomic<uint32_t> total_rate_up_{0};
};

}  // namespace bt
