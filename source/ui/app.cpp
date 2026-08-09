#include "ui/app.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "diag/lp2p_probe.hpp"
#include "ui/lang.hpp"
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

std::string tab_label(int index) {
    switch (index) {
        case 0: return tr(Str::TabTorrents);
        case 1: return tr(Str::TabLibrary);
        case 2: return tr(Str::TabPhone);
        case 3: return tr(Str::TabVpn);
        case 4: return tr(Str::TabSettings);
    }
    return "?";
}

// L'état d'un torrent est un texte affiché, donc traduit. Le moteur, lui, garde
// ses propres libellés français : ils partent dans le journal, où ils servent
// aux rapports de bug et pas à l'usage courant.
Str state_key(bt::TorrentState state) {
    switch (state) {
        case bt::TorrentState::FetchingMetadata: return Str::StateMetadata;
        case bt::TorrentState::Checking: return Str::StateChecking;
        case bt::TorrentState::Downloading: return Str::StateDownloading;
        case bt::TorrentState::Seeding: return Str::StateSeeding;
        case bt::TorrentState::Paused: return Str::StatePaused;
        case bt::TorrentState::Completed: return Str::StateCompleted;
        case bt::TorrentState::Failed: return Str::StateFailed;
    }
    return Str::StateMetadata;
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
    // Le fichier absent — ou présent sans ligne « language » — signe un premier
    // lancement. On préselectionne la langue de la console, qui est le meilleur
    // pari, et on laisse le dernier mot à l'utilisateur.
    const bool had_settings = settings_.load(kSettingsFile);
    language_pending_ = !had_settings;
    set_language(settings_.effective_language());
    language_cursor_ = static_cast<int>(language());
    apply_settings();
    refresh_library();
    return true;
}

void App::shutdown() {
    // Avant tout le reste : tant que le point d'accès est levé, la console n'a
    // pas d'accès à Internet. Le laisser derrière soi couperait le Wi-Fi du
    // salon jusqu'au redémarrage suivant.
    phone_.stop();

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
    //
    // La comparaison porte sur content_root, pas sur save_path : save_path est
    // le dossier de téléchargement, le même pour tout le monde. Un seul torrent
    // inachevé faisait donc passer TOUTE la carte pour « en cours ».
    for (const bt::TorrentStatus& t : torrents_) {
        const bool done = t.state == bt::TorrentState::Completed ||
                          t.state == bt::TorrentState::Seeding;
        if (done || t.content_root.empty()) continue;

        for (LibraryEntry& item : library_) {
            if (item.path.compare(0, t.content_root.size(), t.content_root) != 0) continue;
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
    if (!text_input(tr(Str::KeyboardMagnet), "", uri)) return;

    std::string err;
    if (session_.add_magnet(uri, &err)) {
        toast(tr(Str::ToastTorrentAdded));
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
        toast(tr(Str::ToastNoMagnetsFile), true);
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
        toast(tr(Str::ToastMagnetsNoneOk), true);
    }
}

void App::handle_input(uint64_t now_ms) {
    padUpdate(&pad_);
    const u64 down = padGetButtonsDown(&pad_);

    // Tant que la langue n'est pas choisie, rien d'autre n'est atteignable :
    // un menu qu'on ne sait pas lire ne se navigue pas.
    if (language_pending_) {
        const int count = static_cast<int>(Lang::kCount);
        if (down & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
            language_cursor_ = (language_cursor_ + 1) % count;
            set_language(static_cast<Lang>(language_cursor_));
            render_.use_font_for(language());
        }
        if (down & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
            language_cursor_ = (language_cursor_ + count - 1) % count;
            set_language(static_cast<Lang>(language_cursor_));
            render_.use_font_for(language());
        }
        if (down & HidNpadButton_A) {
            settings_.language = code_of(static_cast<Lang>(language_cursor_));
            settings_.save(kSettingsFile);
            language_pending_ = false;
        }
        return;
    }

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
                    toast(tr(Str::ToastResumed));
                } else {
                    session_.pause(t.hash_hex);
                    toast(tr(Str::ToastPaused));
                }
            }
            if ((down & HidNpadButton_ZR) && !torrents_.empty()) {
                const bt::TorrentStatus& t = torrents_[static_cast<size_t>(selection())];
                session_.remove(t.hash_hex, /*delete_files=*/false);
                toast(tr(Str::ToastRemovedKept));
            }
            // Relire 35 Go prend plus d'une heure. Quand elle ne sert à rien —
            // le cas courant — il faut pouvoir la couper sans tuer l'appli.
            if ((down & HidNpadButton_B) && !torrents_.empty()) {
                const bt::TorrentStatus& t = torrents_[static_cast<size_t>(selection())];
                if (t.state == bt::TorrentState::Checking) {
                    session_.skip_check(t.hash_hex);
                    toast(tr(Str::ToastCheckSkipped));
                }
            }
            break;

        case Tab::Library:
            if (down & HidNpadButton_X) {
                refresh_library();
                toast(tr(Str::LibraryRescanned));
            }
            break;

        case Tab::Phone:
            if (down & HidNpadButton_A) {
                if (phone_.step() == Phone::Step::Off || phone_.step() == Phone::Step::Failed) {
                    std::string err;
                    if (phone_.start(session_, &err)) {
                        toast(tr(Str::PhoneOpened));
                    } else {
                        // Un échec ici ne se rejoue pas : personne ne va
                        // relancer un autre programme pour comprendre. La
                        // sonde part donc immédiatement, à l'endroit et au
                        // moment de la panne, et pose ses dix lignes dans le
                        // journal. Elle prend quelques secondes ; c'est le prix
                        // d'un diagnostic qu'on n'aura pas à redemander.
                        toast(err + " — diagnostic en cours…", true);
                        draw(util::now_ms());
                        util::log_line("point d'accès refusé, sonde lp2p :");
                        for (const diag::Lp2pProbeStep& step : diag::probe_lp2p().steps) {
                            util::log_fmt("  lp2p %-26s %s %s", step.label.c_str(),
                                          step.ok ? "OK" : "ECHEC", step.detail.c_str());
                        }
                        toast(err + " — détail dans torfoil.log", true);
                    }
                } else {
                    phone_.stop();
                    toast(tr(Str::PhoneClosed));
                }
            }
            if ((down & HidNpadButton_Y) && phone_.step() == Phone::Step::JoinWifi) {
                phone_.confirm_joined();
            }
            break;

        case Tab::Vpn:
            if (down & HidNpadButton_X) {
                std::string number;
                if (text_input(tr(Str::VpnAccountPrompt), "", number, 32)) {
                    vpn_.set_account(number);
                    toast(tr(Str::VpnAccountSaved));
                }
            }
            if (down & HidNpadButton_A) {
                if (vpn_.busy()) {
                    toast(tr(Str::VpnBusy));
                } else if (vpn_.state() == vpn::State::Connected) {
                    vpn_.disconnect(session_);
                    toast(tr(Str::VpnCut));
                } else if (!vpn_.has_account()) {
                    toast(tr(Str::VpnNeedAccount), true);
                } else {
                    vpn_.connect(session_);
                }
            }
            if (down & HidNpadButton_Y) {
                // Fait tourner le pays préféré parmi ceux vus au dernier
                // rafraîchissement des relais.
                const std::vector<std::string> list = vpn_.countries();
                if (list.empty()) {
                    toast(tr(Str::VpnConnectForCountries));
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
                    toast(trf(Str::VpnCountrySet, list[index].c_str()));
                }
            }
            break;

        default: {
            // Ligne 0 : la langue. Les bascules suivent, décalées d'un cran.
            const int count = static_cast<int>(toggles().size()) + 1;
            if (down & HidNpadButton_Down) settings_cursor_ = (settings_cursor_ + 1) % count;
            if (down & HidNpadButton_Up) {
                settings_cursor_ = (settings_cursor_ + count - 1) % count;
            }

            if (settings_cursor_ == 0) {
                // ←→ autant que A : sur une liste de sept valeurs, obliger à
                // faire le tour complet pour revenir en arrière est une punition.
                const int langs = static_cast<int>(Lang::kCount);
                int step = 0;
                if (down & (HidNpadButton_A | HidNpadButton_Right)) step = 1;
                if (down & HidNpadButton_Left) step = langs - 1;
                if (step != 0) {
                    const Lang next = static_cast<Lang>((static_cast<int>(language()) + step) %
                                                        langs);
                    set_language(next);
                    render_.use_font_for(next);
                    settings_.language = code_of(next);
                    apply_settings();
                }
            } else if (down & HidNpadButton_A) {
                const Toggle& t = toggles()[static_cast<size_t>(settings_cursor_ - 1)];
                bool& value = settings_.*(t.field);
                value = !value;
                apply_settings();

                const bool checked = t.inverted ? !value : value;
                toast(std::string(tr(t.label)) + " : " +
                      tr(checked ? Str::SettingOn : Str::SettingOff));
            }

            if (down & HidNpadButton_Y) {
                if (diag_running_) toast(tr(Str::DiagBusy));
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
    const std::string vpn_text = up ? tr(Str::VpnOn)
                                    : (vpn_state == vpn::State::Working ? "VPN…" : tr(Str::VpnNone));
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
        // Une aide qui déborde de l'écran est tronquée par le bord sans qu'on
        // sache qu'il en manquait : on s'arrête proprement avant.
        const int width = render_.text_width(FontSize::Small, button) + 8 +
                          render_.text_width(FontSize::Small, action);
        if (x + width > Renderer::kWidth - kMargin) break;

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
        draw_empty(tr(Str::NoTorrents),
                   std::string(tr(Str::HowMagnet)) + "  ·  " + tr(Str::HowMagnetsFile));
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
                      tr(state_key(t.state)),
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
        draw_empty(tr(Str::LibraryEmpty),
                   tr(Str::LibraryEmptyHintFiles));
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

// Deux etapes, deux codes QR, comme l'Album de la console : le premier fait
// rejoindre le reseau, le second ouvre la page. Les afficher tous les deux en
// meme temps serait plus court a coder et impossible a suivre - l'appareil
// photo ne saurait pas lequel viser.
// Sept lignes, chacune écrite dans sa propre langue. C'est la seule mise en
// forme qui marche ici : on ne peut pas demander « quelle langue parlez-vous ? »
// dans une langue que l'on ne parle pas, mais tout le monde reconnaît le nom de
// la sienne.
void App::draw_language_picker() {
    const int cx = Renderer::kWidth / 2;

    render_.text_centered(FontSize::Huge, "Torfoil", cx, kContentTop + 10, palette::kAccent);
    render_.text_centered(FontSize::Title, tr(Str::LangPickTitle), cx, kContentTop + 70,
                          palette::kText);

    const int row_h = 52;
    int y = kContentTop + 130;
    for (int i = 0; i < static_cast<int>(Lang::kCount); ++i) {
        const bool focused = i == language_cursor_;
        const int w = 420;
        render_.rounded_rect(cx - w / 2, y, w, row_h - 8, 10,
                             focused ? palette::kSelected : palette::kSurface);
        render_.text_centered(FontSize::Body, endonym(static_cast<Lang>(i)), cx, y + 8,
                              focused ? palette::kText : palette::kTextDim);
        y += row_h;
    }

    render_.text_centered(FontSize::Small, tr(Str::LangPickHint), cx, y + 16, palette::kTextDim);
}

void App::draw_phone() {
    const int cx = Renderer::kWidth / 2;

    if (phone_.step() == Phone::Step::Off) {
        render_.text_centered(FontSize::Title, tr(Str::PhoneTitle), cx,
                              kContentTop + 60, palette::kText);
        render_.text_centered(FontSize::Body,
                              tr(Str::PhoneIntro1),
                              cx, kContentTop + 120, palette::kTextDim);
        render_.text_centered(FontSize::Body,
                              tr(Str::PhoneIntro2),
                              cx, kContentTop + 154, palette::kTextDim);
        render_.text_centered(FontSize::Body,
                              tr(Str::PhoneWarn1),
                              cx, kContentTop + 210, palette::kWarn);
        render_.text_centered(FontSize::Body,
                              tr(Str::PhoneWarn2),
                              cx, kContentTop + 244, palette::kWarn);
        render_.text_centered(FontSize::Body, tr(Str::PhoneOpenHint), cx, kContentTop + 300,
                              palette::kAccent);
        return;
    }

    if (phone_.step() == Phone::Step::Failed) {
        render_.text_centered(FontSize::Title, tr(Str::PhoneFailed), cx,
                              kContentTop + 100, palette::kError);
        render_.text_clipped(FontSize::Body, phone_.error(), kMargin, kContentTop + 160,
                             Renderer::kWidth - 2 * kMargin, palette::kTextDim);
        return;
    }

    const bool joining = phone_.step() == Phone::Step::JoinWifi;
    render_.text_centered(FontSize::Title,
                          joining ? tr(Str::PhoneStep1)
                                  : tr(Str::PhoneStep2),
                          cx, kContentTop + 20, palette::kText);
    render_.text_centered(FontSize::Small,
                          joining ? tr(Str::PhoneAim1)
                                  : tr(Str::PhoneAim2),
                          cx, kContentTop + 62, palette::kTextDim);

    // La zone de silence autour du motif est comprise dans le tracé : sans
    // cette marge claire, beaucoup de lecteurs ne trouvent tout simplement pas
    // les repères.
    const util::QrCode& qr = phone_.qr();
    if (qr.size > 0) {
        const int available = kContentBottom - (kContentTop + 100) - 90;
        int scale = available / (qr.size + 2 * Renderer::kQrQuietZone);
        if (scale < 2) scale = 2;
        const int side = Renderer::qr_extent(qr, scale);
        const int ox = cx - side / 2;
        const int oy = kContentTop + 100;
        render_.qr_code(qr, ox, oy, scale);

        // Le texte sous le code n'est pas un doublon : un appareil photo qui
        // refuse de lire laisse sinon l'utilisateur sans recours.
        const int ty = oy + side + 14;
        if (joining) {
            render_.text_centered(FontSize::Body, "Réseau : " + phone_.ap().ssid(), cx, ty,
                                  palette::kText);
            render_.text_centered(FontSize::Body, "Mot de passe : " + phone_.ap().passphrase(), cx,
                                  ty + 30, palette::kText);
            render_.text_centered(FontSize::Small,
                                  tr(Str::PhoneThenY), cx, ty + 64,
                                  palette::kAccent);
        } else {
            render_.text_centered(FontSize::Body, phone_.url(), cx, ty, palette::kText);
            const uint32_t got = phone_.imported();
            render_.text_centered(FontSize::Small,
                                  got == 0 ? tr(Str::PhoneWaiting)
                                           : std::to_string(got) + " torrent(s) reçu(s)",
                                  cx, ty + 32, got == 0 ? palette::kTextDim : palette::kSuccess);
        }
    }
}

void App::draw_vpn() {
    const int x = kMargin;
    int y = kContentTop + 24;

    render_.text(FontSize::Title, tr(Str::VpnStatus), x, y, palette::kText);
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

    render_.text(FontSize::Body, tr(Str::VpnStatus), x, y, palette::kTextDim);
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

    row(tr(Str::VpnAccount), vpn_.account_masked());
    const std::string country = vpn_.preferred_country();
    row("Pays", country.empty() ? "automatique" : country);

    if (state == vpn::State::Connected) {
        row(tr(Str::VpnRelay), vpn_.relay_label());
        row(tr(Str::VpnTunnel), "↑ " + util::human_size(vpn_.bytes_sent()) + "   ↓ " +
                          util::human_size(vpn_.bytes_received()));
    }

    y += 20;
    render_.text_clipped(
        FontSize::Small,
        std::string(tr(Str::VpnNoteLine1)) + " " + tr(Str::VpnNoteLine2),
        x, y, Renderer::kWidth - 2 * kMargin, palette::kTextDim);
}

// Deux colonnes, et c'est le correctif autant que la mise en forme. En une
// seule colonne, le titre, l'état, les cinq bascules et l'auto-diagnostic
// faisaient 620 pixels dans une zone qui en offre 534 : la section
// d'auto-diagnostic était dessinée sous le bas de l'écran, donc invisible, et
// on ne pouvait pas « descendre » puisque rien ne défilait.
void App::draw_settings() {
    const int gutter = 40;
    const int left = kMargin;
    const int left_w = (Renderer::kWidth - 2 * kMargin - gutter) * 62 / 100;
    const int right = left + left_w + gutter;
    const int right_w = Renderer::kWidth - kMargin - right;

    // ---- colonne de gauche : confidentialité ----
    int y = kContentTop + 12;
    render_.section_header(tr(Str::SecPrivacy), left, y, palette::kAccent);
    y += 34;

    // Chaque bascule annonce son coût. Une option de sécurité qui ne dit pas ce
    // qu'elle enlève finit toujours par être activée sans le vouloir, puis
    // accusée de « ralentir l'application ».
    const std::vector<Toggle>& list = toggles();
    constexpr int row_h = 66;
    constexpr int switch_w = 54;
    constexpr int switch_h = 28;

    // La langue en premier : c'est le réglage qu'on vient chercher quand on ne
    // comprend pas le reste de l'écran.
    {
        const bool focused = settings_cursor_ == 0;
        render_.rounded_rect(left, y, left_w, row_h - 8, 10,
                             focused ? palette::kSelected : palette::kSurface);
        render_.text(FontSize::Body, tr(Str::FieldLanguage), left + 18, y + 6, palette::kText);
        render_.text(FontSize::Small, tr(Str::HintChangeValue), left + 18, y + 32,
                     palette::kTextDim);
        const std::string value = endonym(language());
        render_.text(FontSize::Body, value,
                     left + left_w - 18 - render_.text_width(FontSize::Body, value), y + 16,
                     focused ? palette::kAccent : palette::kText);
        y += row_h;
    }

    for (size_t i = 0; i < list.size(); ++i) {
        const Toggle& t = list[i];
        const bool raw = settings_.*(t.field);
        const bool checked = t.inverted ? !raw : raw;
        const bool focused = static_cast<int>(i) + 1 == settings_cursor_;

        // Seul le fond change avec le focus : rien ne bouge, donc l'écran n'a
        // pas à être relu après chaque appui.
        render_.rounded_rect(left, y, left_w, row_h - 8, 10,
                             focused ? palette::kSelected : palette::kSurface);

        render_.text_clipped(FontSize::Body, tr(t.label), left + 18, y + 6,
                             left_w - 36 - switch_w - 18,
                             checked ? palette::kText : palette::kTextDim);
        render_.text_clipped(FontSize::Small, tr(t.effect), left + 18, y + 32,
                             left_w - 36 - switch_w - 18, palette::kTextDim);
        render_.toggle_switch(left + left_w - 18 - switch_w, y + 14, switch_w, switch_h, checked,
                              focused);
        y += row_h;
    }

    // ---- colonne de droite : état, puis auto-diagnostic ----
    int ry = kContentTop + 12;
    render_.section_header(tr(Str::SecStatus), right, ry, palette::kTextDim);
    ry += 34;

    auto info = [&](const std::string& key, const std::string& value) {
        render_.text(FontSize::Small, key, right, ry, palette::kTextDim);
        render_.text_clipped(FontSize::Small, value, right + 180, ry, right_w - 180,
                             palette::kText);
        ry += 28;
    };
    info(tr(Str::FieldTransport), session_.transport_name());
    info(tr(Str::FieldActiveTorrents), std::to_string(torrents_.size()));
    info("Point d\'accès", phone_.step() == Phone::Step::Off ? "fermé" : phone_.ap().ssid());

    ry += 20;
    render_.section_header(tr(Str::SecSelfTest), right, ry, palette::kAccent);
    render_.key_badge(FontSize::Small, "Y", right + right_w - 34, ry, palette::kAccent);
    ry += 36;

    if (diag_running_) {
        render_.text_clipped(FontSize::Small, diag_step_, right, ry, right_w, palette::kAccent);
        return;
    }

    if (diag_report_.checks.empty()) {
        int hy = ry;
        for (Str hint : {Str::SelfTestHint1, Str::SelfTestHint2, Str::SelfTestHint3}) {
            render_.text_clipped(FontSize::Small, tr(hint), right, hy, right_w, palette::kTextDim);
            hy += 24;
        }
        return;
    }

    for (const diag::Check& c : diag_report_.checks) {
        // Plutôt tronquer que déborder : ce qui dépasse le bas de l'écran est
        // dessiné dans le vide, et emportait les résultats avec lui.
        if (ry + 56 > kContentBottom) break;

        // Une épreuve non exécutée n'est pas un échec : la confondre enverrait
        // chercher un bug là où il n'y en a pas.
        const Color color = !c.ran ? palette::kWarn : (c.ok ? palette::kSuccess : palette::kError);
        const std::string tag =
            tr(!c.ran ? Str::CheckSkipped : (c.ok ? Str::CheckOk : Str::CheckFailed));

        const int tag_w = render_.text_width(FontSize::Small, tag) + 16;
        render_.rounded_rect(right, ry - 2, tag_w, 24, 6, palette::kSurfaceAlt);
        render_.text(FontSize::Small, tag, right + 8, ry, color);
        render_.text_clipped(FontSize::Small, c.name, right + tag_w + 12, ry,
                             right_w - tag_w - 12, palette::kText);
        ry += 26;
        render_.text_clipped(FontSize::Small, c.detail, right + 12, ry, right_w - 12,
                             palette::kTextDim);
        ry += 32;
    }
}

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

    // Le sélecteur de langue occupe l'écran seul. Laisser la barre du haut et
    // les onglets derrière lui donnerait à croire qu'on peut aller ailleurs.
    if (language_pending_) {
        draw_language_picker();
        render_.end_frame();
        return;
    }

    draw_topbar();
    draw_tabs();

    switch (tab_) {
        case Tab::Torrents:
            draw_torrents();
            draw_hints({{"X", tr(Str::HintPasteMagnet)},
                        {"ZL", tr(Str::HintMagnetsFile)},
                        {"Y", tr(Str::HintPause)},
                        {"B", tr(Str::HintSkipCheck)},
                        {"ZR", tr(Str::HintRemove)},
                        {"L/R", tr(Str::HintTab)},
                        {"+", tr(Str::HintQuit)}});
            break;
        case Tab::Library:
            draw_library();
            draw_hints({{"X", tr(Str::HintRescan)},
                        {"L/R", tr(Str::HintTab)},
                        {"+", tr(Str::HintQuit)}});
            break;
        case Tab::Phone:
            draw_phone();
            draw_hints({{"A", phone_.step() == Phone::Step::Off ||
                                      phone_.step() == Phone::Step::Failed
                                  ? "Ouvrir"
                                  : "Fermer"},
                        {"Y", tr(Str::HintPhoneJoined)},
                        {"L/R", tr(Str::HintTab)},
                        {"+", tr(Str::HintQuit)}});
            break;
        case Tab::Vpn:
            draw_vpn();
            draw_hints({{"A", tr(Str::HintConnect)},
                        {"X", tr(Str::HintAccount)},
                        {"Y", tr(Str::HintCountry)},
                        {"L/R", tr(Str::HintTab)},
                        {"+", tr(Str::HintQuit)}});
            break;
        default:
            draw_settings();
            draw_hints({{"A", tr(Str::HintToggle)},
                        {"↑↓", tr(Str::HintChoose)},
                        {"Y", tr(Str::HintSelfTest)},
                        {"L/R", tr(Str::HintTab)},
                        {"+", tr(Str::HintQuit)}});
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

        for (const std::string& event : phone_.take_events()) toast(event);
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
