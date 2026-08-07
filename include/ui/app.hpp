#pragma once

#include <switch.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bt/session.hpp"
#include "diag/selftest.hpp"
#include "ui/phone.hpp"
#include "ui/render.hpp"
#include "ui/settings.hpp"
#include "vpn/manager.hpp"

namespace ui {

// Une installation dure plusieurs minutes : elle tourne sur son propre thread
// pour que l'interface reste vivante et annulable.
struct InstallJob {
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{0};

    std::mutex mutex;
    std::string step;
    std::string result;
    bool finished = false;
    bool ok = false;
    std::string source_name;
    bool verify_only = false;
};

struct LibraryEntry {
    std::string path;
    std::string name;
    uint64_t size = 0;
    std::string kind;  // NSP, NSZ, XCI, XCZ
    bool installable = true;
    std::string status;  // raison affichée quand ce n'est pas installable
};

// Une entrée du sélecteur de fichiers .torrent.
struct BrowserEntry {
    std::string name;
    bool is_dir = false;
    bool is_parent = false;  // « .. »
    uint64_t size = 0;
};

class App {
public:
    bool init(std::string* err);
    void run();
    void shutdown();

private:
    enum class Tab { Torrents, Library, Remote, Vpn, Settings, kCount };

    void handle_input(uint64_t now_ms);
    void draw(uint64_t now_ms);

    void draw_topbar();
    void draw_tabs();
    void draw_hints(const std::vector<std::pair<std::string, std::string>>& hints);
    void draw_torrents();
    void draw_library();
    void draw_remote();
    void draw_vpn();
    void draw_settings();
    // Ascenseur des listes plus longues que l'écran. Sans repère, on ne sait ni
    // qu'il reste des lignes, ni où l'on se trouve dedans.
    void draw_scrollbar(int first_visible, int visible, int total);
    void run_selftest();
    void apply_settings();
    void draw_toast(uint64_t now_ms);
    void draw_empty(const std::string& title, const std::string& hint);
    void draw_confirm();

    void add_magnet_flow();
    void import_magnets_file();

    // Sélecteur de fichiers .torrent sur la carte.
    void open_browser();
    void browse_to(const std::string& dir);
    void handle_browser_input(uint64_t buttons);
    void draw_browser();
    // Ajoute un .torrent depuis la carte ; renvoie false et remplit `err` sinon.
    bool add_torrent_path(const std::string& path, std::string* err);

    // Dossier surveillé : tout .torrent qui y atterrit est ajouté tout seul.
    void poll_watch_folder(uint64_t now_ms);

    void draw_details();
    void refresh_library();
    void install_selected();
    void verify_selected();
    void draw_install_overlay();
    void poll_install_job();
    void toast(const std::string& message, bool error = false);
    void sync_phone_bridge();
    // Fait tourner la langue : « auto », puis les sept, dans les deux sens.
    void cycle_language(int delta);

    int& selection();
    int& scroll();
    int item_count() const;

    Renderer render_;
    bt::Session session_;
    vpn::Manager vpn_;
    PhoneBridge phone_;
    PadState pad_{};

    Tab tab_ = Tab::Torrents;
    int selection_[static_cast<int>(Tab::kCount)] = {};
    // Un défilement par onglet. Un seul compteur partagé faisait perdre la
    // position dès qu'on allait voir ailleurs, ce qui est exactement ce qu'on
    // fait pendant un téléchargement.
    int scroll_[static_cast<int>(Tab::kCount)] = {};

    std::vector<bt::TorrentStatus> torrents_;
    std::vector<LibraryEntry> library_;
    uint64_t last_library_scan_ms_ = 0;
    std::string last_completion_;  // signature des torrents terminés

    std::string toast_text_;
    bool toast_error_ = false;
    uint64_t toast_until_ms_ = 0;

    std::unique_ptr<InstallJob> install_job_;

    // Retirer un torrent était immédiat et irréversible d'une seule gâchette,
    // sans possibilité de supprimer les fichiers au passage. Les deux se
    // règlent ici : on demande, et on propose les deux issues.
    struct Confirm {
        bool active = false;
        std::string hash;
        std::string name;
    };
    Confirm confirm_;

    // Sélecteur de fichiers : la seule façon d'ajouter un .torrent déjà présent
    // sur la carte sans passer par le téléphone ni par un PC.
    struct Browser {
        bool active = false;
        std::string dir;
        std::vector<BrowserEntry> entries;
        int selection = 0;
        int scroll = 0;
        bool truncated = false;  // dossier trop grand, liste écourtée
    };
    Browser browser_;

    // Panneau de détail du torrent sélectionné.
    bool details_ = false;

    uint64_t last_watch_scan_ms_ = 0;
    uint64_t free_space_ = 0;
    uint64_t last_free_space_ms_ = 0;

    Settings settings_;
    int settings_cursor_ = 0;
    std::string phone_error_;  // raison affichée quand le serveur ne monte pas

    std::thread diag_thread_;
    std::atomic<bool> diag_running_{false};
    std::mutex diag_mutex_;
    std::string diag_step_;
    diag::Report diag_report_;

    bool sleep_disabled_ = false;
    bool running_ = true;
};

// Saisie de texte via le clavier système.
bool text_input(const char* header, const char* initial, std::string& out, size_t max_len = 512);

}  // namespace ui
