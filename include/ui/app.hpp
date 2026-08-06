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

class App {
public:
    bool init(std::string* err);
    void run();
    void shutdown();

private:
    enum class Tab { Torrents, Library, Vpn, Settings, kCount };

    void handle_input(uint64_t now_ms);
    void draw(uint64_t now_ms);

    void draw_topbar();
    void draw_tabs();
    void draw_hints(const std::vector<std::pair<std::string, std::string>>& hints);
    void draw_torrents();
    void draw_library();
    void draw_vpn();
    void draw_settings();
    void run_selftest();
    void apply_settings();
    void draw_toast(uint64_t now_ms);
    void draw_empty(const std::string& title, const std::string& hint);

    void add_magnet_flow();
    void import_magnets_file();
    void refresh_library();
    void install_selected();
    void verify_selected();
    void draw_install_overlay();
    void poll_install_job();
    void toast(const std::string& message, bool error = false);

    int& selection();
    int item_count() const;

    Renderer render_;
    bt::Session session_;
    vpn::Manager vpn_;
    PadState pad_{};

    Tab tab_ = Tab::Torrents;
    int selection_[static_cast<int>(Tab::kCount)] = {0, 0, 0, 0};
    int scroll_ = 0;

    std::vector<bt::TorrentStatus> torrents_;
    std::vector<LibraryEntry> library_;
    uint64_t last_library_scan_ms_ = 0;
    std::string last_completion_;  // signature des torrents terminés

    std::string toast_text_;
    bool toast_error_ = false;
    uint64_t toast_until_ms_ = 0;

    std::unique_ptr<InstallJob> install_job_;

    Settings settings_;
    int settings_cursor_ = 0;

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
