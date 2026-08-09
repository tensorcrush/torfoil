#pragma once

#include <switch.h>

#include <atomic>
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

// Un fichier posé sur la carte par un téléchargement. Torfoil n'en fait rien
// d'autre que le montrer : ce qu'on installe et comment ne le regarde pas.
struct LibraryEntry {
    std::string path;
    std::string name;
    uint64_t size = 0;
    std::string kind;    // extension en majuscules, vide si le nom n'en a pas
    bool ready = true;   // faux tant que le torrent qui l'écrit n'a pas fini
    std::string status;  // « en cours — 42% » le cas échéant
};

class App {
public:
    bool init(std::string* err);
    void run();
    void shutdown();

private:
    enum class Tab { Torrents, Library, Phone, Vpn, Settings, kCount };

    void handle_input(uint64_t now_ms);
    void draw(uint64_t now_ms);

    void draw_topbar();
    void draw_tabs();
    void draw_hints(const std::vector<std::pair<std::string, std::string>>& hints);
    void draw_torrents();
    void draw_library();
    void draw_phone();
    // Écran de premier lancement : la seule chose affichée tant que la langue
    // n'a pas été choisie.
    void draw_language_picker();
    bool language_pending() const { return language_pending_; }
    void draw_vpn();
    void draw_settings();
    void run_selftest();
    void apply_settings();
    void draw_toast(uint64_t now_ms);
    void draw_empty(const std::string& title, const std::string& hint);

    void add_magnet_flow();
    void import_magnets_file();
    void refresh_library();
    void toast(const std::string& message, bool error = false);

    int& selection();
    int item_count() const;

    Renderer render_;
    bt::Session session_;
    vpn::Manager vpn_;
    PadState pad_{};

    Tab tab_ = Tab::Torrents;
    int selection_[static_cast<int>(Tab::kCount)] = {0, 0, 0, 0, 0};
    int scroll_ = 0;

    std::vector<bt::TorrentStatus> torrents_;
    std::vector<LibraryEntry> library_;
    uint64_t last_library_scan_ms_ = 0;
    std::string last_completion_;  // signature des torrents terminés

    std::string toast_text_;
    bool toast_error_ = false;
    uint64_t toast_until_ms_ = 0;

    Phone phone_;

    Settings settings_;
    int settings_cursor_ = 0;

    // Vrai tant que settings.cfg ne portait aucune langue, c'est-à-dire au tout
    // premier lancement. On demande alors, plutôt que de deviner : suivre la
    // console est un bon défaut, mais une console prêtée ou revendue n'est pas
    // réglée dans la langue de celui qui la tient.
    bool language_pending_ = false;
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
