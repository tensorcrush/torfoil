#include "ui/app.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "util/bytes.hpp"
#include "util/clock.hpp"
#include "util/log.hpp"

namespace ui {

namespace {

constexpr int kTopBarHeight = 78;
constexpr int kTabBarHeight = 56;
constexpr int kHintBarHeight = 52;
constexpr int kContentTop = kTopBarHeight + kTabBarHeight;
constexpr int kContentBottom = Renderer::kHeight - kHintBarHeight;
constexpr int kMargin = 40;
constexpr int kRowHeight = 96;
constexpr int kRowGap = 10;

const char* kDownloadDir = "sdmc:/torfoil/downloads";
const char* kMagnetsFile = "sdmc:/torfoil/magnets.txt";
const char* kSettingsFile = "sdmc:/torfoil/settings.cfg";

const char* tab_label(int index) {
    switch (index) {
        case 0: return "Torrents";
        case 1: return "Bibliothèque";
        case 2: return "VPN";
        case 3: return "Réglages";
    }
    return "?";
}

Color state_color(bt::TorrentState state) {
    switch (state) {
        case bt::TorrentState::Downloading: return palette::kAccent;
        case bt::TorrentState::Seeding:
        case bt::TorrentState::Completed: return palette::kSuccess;
        case bt::TorrentState::Failed: return palette::kError;
        case bt::TorrentState::Paused: return palette::kTextDim;
        default: return palette::kWarn;
    }
}

std::string extension_of(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return {};
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

// Un fichier concatene - la parade de Horizon a la limite de 4 Go de FAT32 -
// est un dossier dont les tranches s'appellent 00, 01... Le systeme le presente
// d'ordinaire comme un fichier unique ; s'il apparait ici tel qu'il est
// vraiment, il faut le recoller plutot que d'entrer dedans.
bool looks_like_split_file(const std::string& dir, uint64_t& total_size) {
    DIR* handle = ::opendir(dir.c_str());
    if (!handle) return false;

    bool only_slices = true;
    bool any = false;
    uint64_t sum = 0;

    while (dirent* entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        any = true;

        if (name.size() != 2 || !std::isdigit(static_cast<unsigned char>(name[0])) ||
            !std::isdigit(static_cast<unsigned char>(name[1]))) {
            only_slices = false;
            break;
        }
        struct stat st{};
        if (::stat((dir + "/" + name).c_str(), &st) == 0) sum += static_cast<uint64_t>(st.st_size);
    }
    ::closedir(handle);

    if (!any || !only_slices) return false;
    total_size = sum;
    return true;
}

// Liste ce que les telechargements ont pose sur la carte. Aucune extension n'est
// privilegiee : Torfoil ne sait pas ce qu'est un paquet, il ne connait que des
// fichiers.
void scan_directory(const std::string& dir, int depth, std::vector<LibraryEntry>& out) {
    if (depth > 3) return;

    DIR* handle = ::opendir(dir.c_str());
    if (!handle) return;

    while (dirent* entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        const std::string full = dir + "/" + name;
        struct stat st{};
        if (::stat(full.c_str(), &st) != 0) continue;

        LibraryEntry item;
        item.path = full;
        item.name = name;
        item.kind = extension_of(name);
        for (char& c : item.kind) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        if (S_ISDIR(st.st_mode)) {
            uint64_t split_size = 0;
            if (!looks_like_split_file(full, split_size)) {
                scan_directory(full, depth + 1, out);
                continue;
            }
            item.size = split_size;
        } else {
            item.size = static_cast<uint64_t>(st.st_size);
        }

        out.push_back(std::move(item));
    }
    ::closedir(handle);
}

}  // namespace

bool text_input(const char* header, const char* initial, std::string& out, size_t max_len) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;

    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetGuideText(&kbd, "magnet:?xt=urn:btih:…");
    if (initial && initial[0] != '\0') swkbdConfigSetInitialText(&kbd, initial);
    swkbdConfigSetStringLenMax(&kbd, static_cast<u32>(max_len));

    std::vector<char> buffer(max_len + 1, '\0');
    const Result rc = swkbdShow(&kbd, buffer.data(), buffer.size());
    swkbdClose(&kbd);

    if (R_FAILED(rc)) return false;
    out.assign(buffer.data());
    return !out.empty();
}

bool App::init(std::string* err) {
    if (!render_.init(err)) return false;

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad_);

    ::mkdir("sdmc:/torfoil", 0777);
    ::mkdir(kDownloadDir, 0777);

    // Le fichier existe dès le premier lancement : on trouve où déposer ses
    // liens sans avoir à deviner le chemin ni lire la doc.
    if (std::FILE* probe = std::fopen(kMagnetsFile, "rb")) {
        std::fclose(probe);
    } else if (std::FILE* seed = std::fopen(kMagnetsFile, "wb")) {
        std::fprintf(seed,
                     "# Un lien magnet par ligne. Dans Torfoil, onglet Torrents, ZL pour importer.\n"
                     "# Les liens acceptés sont retirés d'ici automatiquement.\n");
        std::fclose(seed);
    }

    if (!session_.start(kDownloadDir, err)) return false;

    vpn_.load();
    settings_.load(kSettingsFile);
    apply_settings();
    refresh_library();
    return true;
}

void App::shutdown() {
    if (diag_thread_.joinable()) diag_thread_.join();

    if (sleep_disabled_) appletSetAutoSleepDisabled(false);
    session_.stop();
    render_.shutdown();
}

void App::toast(const std::string& message, bool error) {
    toast_text_ = message;
    toast_error_ = error;
    toast_until_ms_ = util::now_ms() + 4000;
}

int& App::selection() {
    return selection_[static_cast<int>(tab_)];
}

int App::item_count() const {
    switch (tab_) {
        case Tab::Torrents: return static_cast<int>(torrents_.size());
        case Tab::Library: return static_cast<int>(library_.size());
        default: return 0;
    }
}

// Appliqué au moteur ET écrit sur la carte dans le même geste : un réglage de
// confidentialité qui ne survivrait pas au redémarrage serait pire qu'absent.
void App::apply_settings() {
    bt::Session::Privacy privacy;
    privacy.require_vpn = settings_.require_vpn;
    privacy.https_trackers_only = settings_.https_trackers_only;
    privacy.enable_dht = settings_.enable_dht;
    privacy.enable_pex = settings_.enable_pex;
    privacy.no_upload = settings_.no_upload;
    session_.set_privacy(privacy);
    settings_.save(kSettingsFile);
}

void App::refresh_library() {
    library_.clear();
    scan_directory(kDownloadDir, 0, library_);

    // Un fichier en cours de telechargement existe deja sur la carte, souvent a
    // sa taille finale : les pieces arrivent dans le desordre et l'espace est
    // reserve d'avance. Sa taille ne dit donc rien de son etat, et rien ne
    // distingue a l'oeil un fichier complet d'un fichier plein de trous. Seul le
    // moteur le sait - on le lui demande.
    for (const bt::TorrentStatus& t : torrents_) {
        const bool done = t.state == bt::TorrentState::Completed ||
                          t.state == bt::TorrentState::Seeding;
        if (done || t.save_path.empty()) continue;

        for (LibraryEntry& item : library_) {
            if (item.path.compare(0, t.save_path.size(), t.save_path) != 0) continue;
            item.ready = false;
            item.status = "en cours - " + std::to_string(static_cast<int>(t.progress * 100)) + "%";
        }
    }

    std::sort(library_.begin(), library_.end(),
              [](const LibraryEntry& a, const LibraryEntry& b) { return a.name < b.name; });
    last_library_scan_ms_ = util::now_ms();
}

void App::add_magnet_flow() {
    std::string uri;
    if (!text_input("Coller un lien magnet", "", uri)) return;

    std::string err;
    if (session_.add_magnet(uri, &err)) {
        toast("Torrent ajouté");
    } else {
        toast(err, true);
    }
}

// Taper un magnet de 60 caractères au clavier virtuel est une punition. On lit
// donc aussi un simple fichier texte, déposable depuis le PC ou par FTP : une
// ligne = un lien. Les liens acceptés sont retirés du fichier, ceux qui ont
// échoué y restent avec la raison en commentaire.
void App::import_magnets_file() {
    std::FILE* fp = std::fopen(kMagnetsFile, "rb");
    if (!fp) {
        toast("Aucun fichier sdmc:/torfoil/magnets.txt", true);
        return;
    }

    std::vector<std::string> lines;
    char buffer[2048];
    while (std::fgets(buffer, sizeof(buffer), fp)) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        lines.push_back(line);
    }
    std::fclose(fp);

    int added = 0;
    std::vector<std::string> leftovers;
    for (const std::string& line : lines) {
        if (line.empty() || line[0] == '#') continue;
        if (line.compare(0, 7, "magnet:") != 0) {
            leftovers.push_back("# ignoré (pas un magnet) : " + line);
            continue;
        }

        std::string err;
        if (session_.add_magnet(line, &err)) {
            ++added;
        } else {
            leftovers.push_back("# " + err);
            leftovers.push_back(line);
        }
    }

    // Réécriture : ce qui a marché disparaît, le reste est conservé avec sa
    // raison. Relancer l'import ne recrée donc jamais de doublon.
    std::FILE* out = std::fopen(kMagnetsFile, "wb");
    if (out) {
        for (const std::string& line : leftovers) std::fprintf(out, "%s\n", line.c_str());
        std::fclose(out);
    }

    if (added > 0) {
        toast(std::to_string(added) + (added > 1 ? " torrents ajoutés" : " torrent ajouté"));
    } else if (leftovers.empty()) {
        // Cas courant et déroutant : le fichier a déjà été importé. Ce n'est pas
        // une erreur, et le dire autrement évite de chercher un problème absent.
        toast("magnets.txt est vide — tout est déjà importé");
    } else {
        toast("Aucun lien accepté, raisons notées dans magnets.txt", true);
    }
}

void App::handle_input(uint64_t now_ms) {
    padUpdate(&pad_);
    const u64 down = padGetButtonsDown(&pad_);

    if (down & HidNpadButton_Plus) {
        running_ = false;
        return;
    }

    // Changement d'onglet : gâchettes L/R.
    const int tab_count = static_cast<int>(Tab::kCount);
    if (down & HidNpadButton_L) {
        tab_ = static_cast<Tab>((static_cast<int>(tab_) + tab_count - 1) % tab_count);
        scroll_ = 0;
    }
    if (down & HidNpadButton_R) {
        tab_ = static_cast<Tab>((static_cast<int>(tab_) + 1) % tab_count);
        scroll_ = 0;
    }

    const int count = item_count();
    if (count > 0) {
        if (down & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
            selection() = (selection() + 1) % count;
        }
        if (down & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
            selection() = (selection() + count - 1) % count;
        }
    }
    if (selection() >= count) selection() = count > 0 ? count - 1 : 0;

    switch (tab_) {
        case Tab::Torrents:
            if (down & HidNpadButton_X) add_magnet_flow();
            if (down & HidNpadButton_ZL) import_magnets_file();
            if ((down & HidNpadButton_Y) && !torrents_.empty()) {
                const bt::TorrentStatus& t = torrents_[static_cast<size_t>(selection())];
                if (t.state == bt::TorrentState::Paused) {
                    session_.resume(t.hash_hex);
                    toast("Reprise");
                } else {
                    session_.pause(t.hash_hex);
                    toast("Mis en pause");
                }
            }
            if ((down & HidNpadButton_ZR) && !torrents_.empty()) {
                const bt::TorrentStatus& t = torrents_[static_cast<size_t>(selection())];
                session_.remove(t.hash_hex, /*delete_files=*/false);
                toast("Torrent retiré (fichiers conservés)");
            }
            // Relire 35 Go prend plus d'une heure. Quand elle ne sert à rien —
            // le cas courant — il faut pouvoir la couper sans tuer l'appli.
            if ((down & HidNpadButton_B) && !torrents_.empty()) {
                const bt::TorrentStatus& t = torrents_[static_cast<size_t>(selection())];
                if (t.state == bt::TorrentState::Checking) {
                    session_.skip_check(t.hash_hex);
                    toast("Vérification passée — le reste sera retéléchargé");
                }
            }
            break;

        case Tab::Library:
            if (down & HidNpadButton_X) {
                refresh_library();
                toast("Bibliothèque relue");
            }
            break;

        case Tab::Vpn:
            if (down & HidNpadButton_X) {
                std::string number;
                if (text_input("Numéro de compte Mullvad (16 chiffres)", "", number, 32)) {
                    vpn_.set_account(number);
                    toast("Compte enregistré");
                }
            }
            if (down & HidNpadButton_A) {
                if (vpn_.busy()) {
                    toast("Connexion déjà en cours");
                } else if (vpn_.state() == vpn::State::Connected) {
                    vpn_.disconnect(session_);
                    toast("VPN coupé — trafic en direct");
                } else if (!vpn_.has_account()) {
                    toast("Renseigner le compte Mullvad (X)", true);
                } else {
                    vpn_.connect(session_);
                }
            }
            if (down & HidNpadButton_Y) {
                // Fait tourner le pays préféré parmi ceux vus au dernier
                // rafraîchissement des relais.
                const std::vector<std::string> list = vpn_.countries();
                if (list.empty()) {
                    toast("Se connecter une fois pour obtenir la liste des pays");
                } else {
                    const std::string current = vpn_.preferred_country();
                    size_t index = 0;
                    for (size_t i = 0; i < list.size(); ++i) {
                        if (list[i] == current) {
                            index = (i + 1) % list.size();
                            break;
                        }
                    }
                    vpn_.set_preferred_country(list[index]);
                    toast("Pays : " + list[index]);
                }
            }
            break;

        default: {
            const int count = static_cast<int>(toggles().size());
            if (down & HidNpadButton_Down) settings_cursor_ = (settings_cursor_ + 1) % count;
            if (down & HidNpadButton_Up) {
                settings_cursor_ = (settings_cursor_ + count - 1) % count;
            }

            if (down & HidNpadButton_A) {
                const Toggle& t = toggles()[static_cast<size_t>(settings_cursor_)];
                bool& value = settings_.*(t.field);
                value = !value;
                apply_settings();

                const bool checked = t.inverted ? !value : value;
                toast(std::string(t.label) + (checked ? " : activé" : " : désactivé"));
            }

            if (down & HidNpadButton_Y) {
                if (diag_running_) toast("Diagnostic déjà en cours");
                else run_selftest();
            }
            break;
        }
    }

    (void)now_ms;
}

// ---------------------------------------------------------------------------
// Rendu
// ---------------------------------------------------------------------------

void App::draw_topbar() {
    render_.rect(0, 0, Renderer::kWidth, kTopBarHeight, palette::kSurface);
    render_.text(FontSize::Title, "Torfoil", kMargin, 20, palette::kText);

    // Pastille d'état du VPN.
    const vpn::State vpn_state = vpn_.state();
    const bool up = vpn_state == vpn::State::Connected;
    const std::string vpn_text = up ? "VPN Mullvad"
                                    : (vpn_state == vpn::State::Working ? "VPN…" : "Sans VPN");
    const Color pill = up ? palette::kAccentDim
                          : (vpn_state == vpn::State::Working ? palette::kWarn : palette::kError);
    const int pill_w = render_.text_width(FontSize::Small, vpn_text) + 28;
    render_.rounded_rect(200, 24, pill_w, 30, 15, pill);
    render_.text(FontSize::Small, vpn_text, 214, 28, palette::kText);

    // Débits globaux, à droite.
    char rates[128];
    std::snprintf(rates, sizeof(rates), "↓ %s   ↑ %s",
                  util::human_rate(session_.rate_down()).c_str(),
                  util::human_rate(session_.rate_up()).c_str());
    const int w = render_.text_width(FontSize::Body, rates);
    render_.text(FontSize::Body, rates, Renderer::kWidth - kMargin - w, 24, palette::kTextDim);
}

void App::draw_tabs() {
    render_.rect(0, kTopBarHeight, Renderer::kWidth, kTabBarHeight, palette::kBackground);

    int x = kMargin;
    for (int i = 0; i < static_cast<int>(Tab::kCount); ++i) {
        const std::string label = tab_label(i);
        const int w = render_.text_width(FontSize::Body, label) + 36;
        const bool active = i == static_cast<int>(tab_);

        if (active) render_.rounded_rect(x, kTopBarHeight + 8, w, 40, 20, palette::kSurfaceAlt);
        render_.text(FontSize::Body, label, x + 18, kTopBarHeight + 14,
                     active ? palette::kAccent : palette::kTextDim);
        x += w + 8;
    }

    render_.line(kMargin, kContentTop - 1, Renderer::kWidth - kMargin, kContentTop - 1,
                 palette::kSurfaceAlt);
}

void App::draw_hints(const std::vector<std::pair<std::string, std::string>>& hints) {
    const int y = kContentBottom;
    render_.rect(0, y, Renderer::kWidth, kHintBarHeight, palette::kSurface);

    int x = kMargin;
    for (const auto& [button, action] : hints) {
        render_.text(FontSize::Small, button, x, y + 15, palette::kAccent);
        x += render_.text_width(FontSize::Small, button) + 8;
        render_.text(FontSize::Small, action, x, y + 15, palette::kTextDim);
        x += render_.text_width(FontSize::Small, action) + 26;
    }
}

void App::draw_empty(const std::string& title, const std::string& hint) {
    const int cx = Renderer::kWidth / 2;
    render_.text_centered(FontSize::Title, title, cx, kContentTop + 160, palette::kTextDim);
    render_.text_centered(FontSize::Body, hint, cx, kContentTop + 210, palette::kTextDim);
}

void App::draw_torrents() {
    if (torrents_.empty()) {
        draw_empty("Aucun torrent",
                   "X pour coller un lien  ·  ZL pour importer sdmc:/torfoil/magnets.txt");
        return;
    }

    const int visible = (kContentBottom - kContentTop - 20) / (kRowHeight + kRowGap);
    if (selection() < scroll_) scroll_ = selection();
    if (selection() >= scroll_ + visible) scroll_ = selection() - visible + 1;

    int y = kContentTop + 14;
    for (int i = scroll_; i < static_cast<int>(torrents_.size()) && i < scroll_ + visible; ++i) {
        const bt::TorrentStatus& t = torrents_[static_cast<size_t>(i)];
        const bool active = i == selection();

        render_.rounded_rect(kMargin, y, Renderer::kWidth - 2 * kMargin, kRowHeight, 12,
                             active ? palette::kSelected : palette::kSurface);

        const int text_w = Renderer::kWidth - 2 * kMargin - 260;
        render_.text_clipped(FontSize::Body, t.name, kMargin + 20, y + 12, text_w,
                             palette::kText);

        // Le nombre de pairs qui nous étranglent est LA donnée qui explique un
        // débit décevant : un pair connecté qui étrangle ne sert rien. Sans elle
        // on ne peut que constater la lenteur sans savoir d'où elle vient.
        char peers[64];
        if (t.peers_choking > 0 && t.peers_connected > 0) {
            std::snprintf(peers, sizeof(peers), "%u pairs (%u refusent)", t.peers_connected,
                          t.peers_choking);
        } else {
            std::snprintf(peers, sizeof(peers), "%u pairs", t.peers_connected);
        }

        char detail[256];
        std::snprintf(detail, sizeof(detail), "%s · %s · %s · ↓ %s",
                      bt::state_label(t.state),
                      t.total_size ? util::human_size(t.total_size).c_str() : "taille inconnue",
                      peers, util::human_rate(t.rate_down).c_str());
        render_.text_clipped(FontSize::Small, detail, kMargin + 20, y + 46, text_w,
                             palette::kTextDim);

        render_.progress_bar(kMargin + 20, y + 74, text_w, 8, t.progress, state_color(t.state));

        // Colonne de droite : pourcentage et temps restant.
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%.0f %%", t.progress * 100.0f);
        const int right = Renderer::kWidth - kMargin - 24;
        render_.text(FontSize::Title, pct, right - render_.text_width(FontSize::Title, pct),
                     y + 18, state_color(t.state));

        if (t.eta_s > 0 && t.state == bt::TorrentState::Downloading) {
            const std::string eta = util::human_duration(t.eta_s);
            render_.text(FontSize::Small, eta, right - render_.text_width(FontSize::Small, eta),
                         y + 60, palette::kTextDim);
        }

        y += kRowHeight + kRowGap;
    }

    if (!torrents_.empty() && !torrents_[static_cast<size_t>(selection())].message.empty()) {
        render_.text_clipped(FontSize::Small,
                             torrents_[static_cast<size_t>(selection())].message, kMargin,
                             kContentBottom - 26, Renderer::kWidth - 2 * kMargin,
                             palette::kWarn);
    }
}

void App::draw_library() {
    if (library_.empty()) {
        draw_empty("Bibliothèque vide",
                   "Les fichiers téléchargés apparaîtront ici — X pour relire");
        return;
    }

    const int row_h = 68;
    const int visible = (kContentBottom - kContentTop - 20) / (row_h + 8);
    if (selection() < scroll_) scroll_ = selection();
    if (selection() >= scroll_ + visible) scroll_ = selection() - visible + 1;

    int y = kContentTop + 14;
    for (int i = scroll_; i < static_cast<int>(library_.size()) && i < scroll_ + visible; ++i) {
        const LibraryEntry& item = library_[static_cast<size_t>(i)];
        const bool active = i == selection();

        render_.rounded_rect(kMargin, y, Renderer::kWidth - 2 * kMargin, row_h, 12,
                             active ? palette::kSelected : palette::kSurface);

        // Étiquette de type à gauche. Un fichier sans extension n'en reçoit pas.
        int badge_w = 0;
        if (!item.kind.empty()) {
            badge_w = render_.text_width(FontSize::Small, item.kind) + 22;
            render_.rounded_rect(kMargin + 16, y + 20, badge_w, 28, 8, palette::kAccentDim);
            render_.text(FontSize::Small, item.kind, kMargin + 27, y + 23, palette::kText);
            badge_w += 16;
        }

        // Un fichier encore en cours se voit au premier coup d'œil : le nom est
        // grisé et l'avancement remplace la taille, qui ne veut rien dire tant
        // que le téléchargement n'est pas fini.
        render_.text_clipped(FontSize::Body, item.name, kMargin + 16 + badge_w, y + 8,
                             Renderer::kWidth - 2 * kMargin - badge_w - 220,
                             item.ready ? palette::kText : palette::kTextDim);

        const std::string detail = item.ready ? util::human_size(item.size) : item.status;
        render_.text(FontSize::Small, detail, kMargin + 16 + badge_w, y + 38,
                     palette::kTextDim);

        y += row_h + 8;
    }
}

void App::draw_vpn() {
    const int x = kMargin;
    int y = kContentTop + 24;

    render_.text(FontSize::Title, "Mullvad", x, y, palette::kText);
    y += 50;

    const vpn::State state = vpn_.state();
    Color color = palette::kTextDim;
    std::string label = "déconnecté";
    switch (state) {
        case vpn::State::Connected:
            color = palette::kSuccess;
            label = "connecté";
            break;
        case vpn::State::Working:
            color = palette::kWarn;
            label = "connexion…";
            break;
        case vpn::State::Failed:
            color = palette::kError;
            label = "échec";
            break;
        default:
            break;
    }

    render_.text(FontSize::Body, "Statut", x, y, palette::kTextDim);
    render_.text(FontSize::Body, label, x + 260, y, color);
    y += 38;

    const std::string message = vpn_.status();
    if (!message.empty()) {
        render_.text_clipped(FontSize::Small, message, x + 260, y,
                             Renderer::kWidth - 2 * kMargin - 260, palette::kTextDim);
        y += 34;
    }

    auto row = [&](const char* key, const std::string& value) {
        render_.text(FontSize::Body, key, x, y, palette::kTextDim);
        render_.text_clipped(FontSize::Body, value, x + 260, y,
                             Renderer::kWidth - 2 * kMargin - 260, palette::kText);
        y += 38;
    };

    row("Compte", vpn_.account_masked());
    const std::string country = vpn_.preferred_country();
    row("Pays", country.empty() ? "automatique" : country);

    if (state == vpn::State::Connected) {
        row("Relais", vpn_.relay_label());
        row("Tunnel", "↑ " + util::human_size(vpn_.bytes_sent()) + "   ↓ " +
                          util::human_size(vpn_.bytes_received()));
    }

    y += 20;
    render_.text_clipped(
        FontSize::Small,
        "Le moteur BitTorrent ne peut pas ouvrir de socket lui-même : il en demande au "
        "transport actif. Tunnel absent = aucune connexion possible.",
        x, y, Renderer::kWidth - 2 * kMargin, palette::kTextDim);
}

void App::draw_settings() {
    const int x = kMargin;
    int y = kContentTop + 30;

    render_.text(FontSize::Title, "Réglages", x, y, palette::kText);
    y += 56;

    auto row = [&](const std::string& key, const std::string& value) {
        render_.text(FontSize::Body, key, x, y, palette::kTextDim);
        render_.text(FontSize::Body, value, x + 320, y, palette::kText);
        y += 40;
    };

    row("Transport réseau", session_.transport_name());
    row("Torrents actifs", std::to_string(torrents_.size()));

    y += 16;
    render_.text(FontSize::Body, "Confidentialité", x, y, palette::kText);
    render_.text(FontSize::Small, "A pour cocher · ↑↓ pour choisir", x + 320, y + 4,
                 palette::kTextDim);
    y += 40;

    // Chaque case annonce son coût. Une option de sécurité qui ne dit pas ce
    // qu'elle enlève finit toujours par être activée sans le vouloir, puis
    // accusée de « ralentir l'application ».
    const std::vector<Toggle>& list = toggles();
    for (size_t i = 0; i < list.size(); ++i) {
        const Toggle& t = list[i];
        const bool raw = settings_.*(t.field);
        const bool checked = t.inverted ? !raw : raw;
        const bool active = static_cast<int>(i) == settings_cursor_;

        if (active) {
            render_.rounded_rect(x - 10, y - 6, Renderer::kWidth - 2 * kMargin + 20, 52, 10,
                                 palette::kSelected);
        }

        render_.text(FontSize::Body, checked ? "[x]" : "[ ]", x, y,
                     checked ? palette::kAccent : palette::kTextDim);
        render_.text(FontSize::Body, t.label, x + 56, y,
                     checked ? palette::kText : palette::kTextDim);
        render_.text_clipped(FontSize::Small, t.effect, x + 56, y + 24,
                             Renderer::kWidth - 2 * kMargin - 56, palette::kTextDim);
        y += 52;
    }

    y += 12;
    render_.text(FontSize::Body, "Auto-diagnostic", x, y, palette::kText);
    y += 40;

    if (diag_running_) {
        render_.text(FontSize::Body, diag_step_, x, y, palette::kAccent);
        return;
    }

    if (diag_report_.checks.empty()) {
        render_.text_clipped(FontSize::Small,
                             "Y — éprouve la carte au-delà de 4 Go et la sortie réelle du "
                             "trafic. Rien n'est conservé.",
                             x, y, Renderer::kWidth - 2 * kMargin, palette::kTextDim);
        return;
    }

    for (const diag::Check& c : diag_report_.checks) {
        // Une épreuve non exécutée n'est pas un échec : la confondre enverrait
        // chercher un bug là où il n'y en a pas.
        const Color color = !c.ran ? palette::kWarn : (c.ok ? palette::kSuccess : palette::kError);
        const char* label = !c.ran ? "IGNORÉ" : (c.ok ? "OK" : "ÉCHEC");
        render_.text(FontSize::Body, label, x, y, color);
        render_.text_clipped(FontSize::Body, c.name, x + 90, y,
                             Renderer::kWidth - 2 * kMargin - 90, palette::kText);
        y += 32;
        render_.text_clipped(FontSize::Small, c.detail, x + 90, y,
                             Renderer::kWidth - 2 * kMargin - 90, palette::kTextDim);
        y += 38;
    }
}

// Lance l'auto-diagnostic sur son propre thread : la vérification VPN interroge
// le réseau et bloquerait l'affichage plusieurs secondes.
void App::run_selftest() {
    if (diag_running_) return;
    if (diag_thread_.joinable()) diag_thread_.join();

    diag_running_ = true;
    diag_report_.checks.clear();
    diag_step_ = "démarrage…";

    diag_thread_ = std::thread([this] {
        diag::Report report = diag::run_all(kDownloadDir, session_, [this](const std::string& s) {
            std::lock_guard<std::mutex> lock(diag_mutex_);
            diag_step_ = s;
        });
        {
            std::lock_guard<std::mutex> lock(diag_mutex_);
            diag_report_ = std::move(report);
        }
        diag_running_ = false;
    });
}

void App::draw_toast(uint64_t now_ms) {
    if (now_ms >= toast_until_ms_ || toast_text_.empty()) return;

    const int w = render_.text_width(FontSize::Body, toast_text_) + 48;
    const int x = (Renderer::kWidth - w) / 2;
    const int y = kContentBottom - 80;

    render_.rounded_rect(x, y, w, 52, 14, toast_error_ ? palette::kError : palette::kAccentDim);
    render_.text_centered(FontSize::Body, toast_text_, Renderer::kWidth / 2, y + 12,
                          palette::kText);
}

void App::draw(uint64_t now_ms) {
    render_.begin_frame();
    render_.fill(palette::kBackground);

    draw_topbar();
    draw_tabs();

    switch (tab_) {
        case Tab::Torrents:
            draw_torrents();
            draw_hints({{"X", "Coller un magnet"},
                        {"ZL", "Importer magnets.txt"},
                        {"Y", "Pause / Reprise"},
                        {"B", "Passer la vérification"},
                        {"ZR", "Retirer"},
                        {"L/R", "Onglet"},
                        {"+", "Quitter"}});
            break;
        case Tab::Library:
            draw_library();
            draw_hints({{"X", "Relire"},
                        {"L/R", "Onglet"},
                        {"+", "Quitter"}});
            break;
        case Tab::Vpn:
            draw_vpn();
            draw_hints({{"A", "Connecter / Couper"},
                        {"X", "Compte Mullvad"},
                        {"Y", "Pays"},
                        {"L/R", "Onglet"},
                        {"+", "Quitter"}});
            break;
        default:
            draw_settings();
            draw_hints({{"A", "Cocher"},
                        {"↑↓", "Choisir"},
                        {"Y", "Auto-diagnostic"},
                        {"L/R", "Onglet"},
                        {"+", "Quitter"}});
            break;
    }

    draw_toast(now_ms);
    render_.end_frame();
}

void App::run() {
    while (running_ && appletMainLoop()) {
        const uint64_t now = util::now_ms();

        torrents_ = session_.snapshot();

        // Empêche la mise en veille tant qu'un transfert est actif : la console
        // endormie coupe le réseau et le téléchargement s'arrête net.
        const bool busy = std::any_of(torrents_.begin(), torrents_.end(),
                                      [](const bt::TorrentStatus& t) {
                                          return t.state == bt::TorrentState::Downloading ||
                                                 t.state == bt::TorrentState::FetchingMetadata ||
                                                 t.state == bt::TorrentState::Checking;
                                      });
        if (busy != sleep_disabled_) {
            appletSetAutoSleepDisabled(busy);
            sleep_disabled_ = busy;
        }

        vpn_.update(session_);

        // Relire la bibliothèque parcourt récursivement la carte : c'est lent,
        // ça bloque l'affichage et ça vole des entrées/sorties au moteur. On ne
        // le fait donc que quand quelque chose a pu changer — un torrent qui
        // s'achève — avec un filet de sécurité très espacé.
        std::string completion;
        for (const bt::TorrentStatus& t : torrents_) {
            if (t.state == bt::TorrentState::Completed || t.state == bt::TorrentState::Seeding) {
                completion += t.hash_hex;
            }
        }
        if (completion != last_completion_ || now - last_library_scan_ms_ > 60000) {
            last_completion_ = completion;
            refresh_library();
        }

        handle_input(now);
        draw(now);
    }
}

}  // namespace ui
