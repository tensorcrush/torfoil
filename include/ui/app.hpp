#pragma once

#include <switch.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bt/search.hpp"
#include "bt/session.hpp"
#include "diag/selftest.hpp"
#include "ui/icons.hpp"
#include "ui/phone.hpp"
#include "ui/remote.hpp"
#include "util/archive.hpp"
#include "ui/render.hpp"
#include "ui/settings.hpp"
#include "vpn/manager.hpp"

namespace ui {

// Un fichier posé sur la carte par un téléchargement. Torfoil n'en fait rien
// d'autre que le montrer : ce qu'on installe et comment ne le regarde pas.
struct LibraryEntry {
    std::string path;
    std::string name;
    uint64_t size = 0;
    std::string kind;    // extension en majuscules, vide si le nom n'en a pas
    bool ready = true;   // faux tant que le torrent qui l'écrit n'a pas fini
    bool is_dir = false;
    std::string status;  // « en cours — 42% » le cas échéant
};

class App {
public:
    bool init(std::string* err);
    void run();
    void shutdown();

private:
    enum class Tab { Torrents, Downloads, Phone, Vpn, Settings, kCount };

    // Ce qui recouvre l'écran. Un seul à la fois : empiler des panneaux rendrait
    // le bouton B ambigu, et c'est le seul chemin de retour.
    enum class Overlay { None, Actions, Info, Files, Confirm, Extract, Search };

    void handle_input(uint64_t now_ms);
    void draw(uint64_t now_ms);

    void draw_topbar();
    void draw_tabs();
    void draw_hints(const std::vector<std::pair<std::string, std::string>>& hints);
    void draw_torrents();
    void draw_downloads();
    void draw_actions();
    void draw_info();
    void draw_files();
    void draw_confirm();
    void draw_extract();
    void draw_search();
    void draw_phone();
    // Écran de premier lancement : la seule chose affichée tant que la langue
    // n'a pas été choisie.
    void draw_language_picker();
    // Écran d'accueil, montré une fois, juste après le choix de la langue.
    void draw_welcome();
    bool language_pending() const { return language_pending_; }
    void draw_vpn();
    void draw_settings();
    void run_selftest();
    void apply_settings();
    void draw_toast(uint64_t now_ms);
    void draw_empty(const std::string& title, const std::string& hint);

    void add_magnet_flow();
    void import_magnets_file();
    int import_inbox();
    void refresh_library();
    void toast(const std::string& message, bool error = false);

    int& selection();
    int item_count() const;

    // Torrent sous le curseur, ou celui que l'incrustation regarde. Renvoie
    // nullptr plutôt qu'une référence sur un torrent disparu entre deux images :
    // le moteur peut en retirer un pendant qu'une incrustation le décrit.
    const bt::TorrentStatus* focused() const;

    void open_actions();
    void open_files(const std::string& root, const std::string& title);
    // Par valeur, et ce n'est pas un oubli : on l'appelle avec le chemin d'une
    // entrée de files_, que la première ligne du corps efface. Une référence
    // pendrait dans le vide et le dossier s'ouvrait vide.
    void enter_dir(std::string dir);
    void run_action();
    // Lance l'extraction dans un fil : une archive de plusieurs gigaoctets
    // gèlerait l'affichage pendant des minutes.
    void start_extract(const std::string& archive, const std::string& dest);
    void poll_extract();
    void start_search();
    void poll_search();

    Renderer render_;
    IconSet icons_;
    bt::Session session_;
    vpn::Manager vpn_;
    PadState pad_{};

    Tab tab_ = Tab::Torrents;
    int selection_[static_cast<int>(Tab::kCount)] = {0, 0, 0, 0, 0};
    int scroll_ = 0;

    std::vector<bt::TorrentStatus> torrents_;
    uint64_t last_library_scan_ms_ = 0;
    std::string last_completion_;  // signature des torrents terminés

    // Ce que les téléchargements ont laissé sur la carte sans qu'aucun torrent
    // ne le revendique : un torrent retiré « en gardant les fichiers » laisse
    // parfois des dizaines de gigaoctets dont plus rien ne parle.
    uint32_t orphan_count_ = 0;
    uint64_t orphan_bytes_ = 0;

    Overlay overlay_ = Overlay::None;
    std::string overlay_hash_;  // torrent décrit par l'incrustation
    int action_cursor_ = 0;
    std::vector<Str> actions_;

    std::string files_root_;  // racine dont on ne remonte pas
    std::string files_dir_;
    std::string files_title_;
    std::vector<LibraryEntry> files_;
    int files_cursor_ = 0;
    int files_scroll_ = 0;

    std::string toast_text_;
    bool toast_error_ = false;
    uint64_t toast_until_ms_ = 0;

    Phone phone_;
    Remote remote_;

    util::ExtractProgress extract_;
    std::thread extract_thread_;
    std::string extract_name_;

    std::vector<bt::SearchResult> results_;
    std::vector<bt::SearchProvider> providers_;
    std::thread search_thread_;
    std::atomic<bool> searching_{false};
    std::string search_query_;
    std::string search_error_;
    int results_cursor_ = 0;
    int results_scroll_ = 0;

    Settings settings_;
    int settings_cursor_ = 0;

    // Vrai tant que settings.cfg ne portait aucune langue, c'est-à-dire au tout
    // premier lancement. On demande alors, plutôt que de deviner : suivre la
    // console est un bon défaut, mais une console prêtée ou revendue n'est pas
    // réglée dans la langue de celui qui la tient.
    bool language_pending_ = false;
    bool welcome_pending_ = false;
    int language_cursor_ = 0;

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
