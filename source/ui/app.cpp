#include "ui/app.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "install/installer.hpp"
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
const char* kInboxDir = "sdmc:/torfoil/inbox";
const char* kWatchDir = "sdmc:/torfoil/watch";
const char* kMagnetsFile = "sdmc:/torfoil/magnets.txt";
const char* kSettingsFile = "sdmc:/torfoil/settings.cfg";
constexpr uint16_t kPhonePort = 8080;

// Une carte SD de console contient des dizaines de milliers de fichiers. Au-delà
// de ce nombre par dossier, on écourte et on le dit : personne ne fait défiler
// dix mille lignes à la manette, et tout lire fige l'affichage.
constexpr size_t kBrowserMaxEntries = 400;
constexpr uint64_t kWatchIntervalMs = 5000;

const char* tab_label(int index) {
    switch (index) {
        case 0: return tr(Str::TabTorrents);
        case 1: return tr(Str::TabLibrary);
        case 2: return tr(Str::TabRemote);
        case 3: return tr(Str::TabVpn);
        case 4: return tr(Str::TabSettings);
    }
    return "?";
}

// L'état d'un torrent est produit par le moteur, qui ne connaît aucune langue :
// la traduction se fait ici, au seul endroit qui affiche.
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

        const std::string ext = extension_of(name);
        const bool installable_ext =
            ext == "nsp" || ext == "nsz" || ext == "xci" || ext == "xcz";

        if (S_ISDIR(st.st_mode)) {
            // Sur une carte FAT32, un fichier de plus de 4 Go est stocké en
            // « fichier concaténé » : un dossier découpé en tranches que le
            // système présente normalement comme un fichier unique. Si jamais il
            // apparaît ici comme un dossier, il ne faut surtout pas descendre
            // dedans — c'est le jeu lui-même, pas un répertoire à explorer.
            if (!installable_ext) scan_directory(full, depth + 1, out);
            else {
                LibraryEntry item;
                item.path = full;
                item.name = name;
                // Sa taille est la somme de ses tranches.
                item.size = 0;
                struct stat pst{};
                DIR* inner = ::opendir(full.c_str());
                if (inner) {
                    while (dirent* piece = ::readdir(inner)) {
                        const std::string pname = piece->d_name;
                        if (pname == "." || pname == "..") continue;
                        if (::stat((full + "/" + pname).c_str(), &pst) == 0) {
                            item.size += static_cast<uint64_t>(pst.st_size);
                        }
                    }
                    ::closedir(inner);
                }
                item.kind = ext;
                for (char& c : item.kind) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                out.push_back(std::move(item));
            }
            continue;
        }

        if (!installable_ext) continue;

        LibraryEntry item;
        item.path = full;
        item.name = name;
        item.size = static_cast<uint64_t>(st.st_size);
        item.kind = ext;
        for (char& c : item.kind) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        // On regarde les premiers octets tout de suite. Un torrent télécharge ses
        // pièces dans le désordre : un fichier peut peser sa taille finale sans
        // que son en-tête soit arrivé. Sans cette lecture, la bibliothèque
        // l'annonce installable, et l'installation échoue en disant « ce n'est
        // pas un NSP » — ce qui accuse le fichier au lieu de dire qu'il manque
        // encore des morceaux.
        if (ext == "nsp" || ext == "xci") {
            item.installable = false;
            item.status = tr(Str::HeaderUnreadable);

            if (std::FILE* probe = std::fopen(full.c_str(), "rb")) {
                uint8_t head[0x104] = {0};
                const size_t got = std::fread(head, 1, sizeof(head), probe);
                std::fclose(probe);

                if (ext == "nsp" && got >= 4 && std::memcmp(head, "PFS0", 4) == 0) {
                    item.installable = true;
                    item.status.clear();
                } else if (ext == "xci" && got >= 0x104 &&
                           std::memcmp(head + 0x100, "HEAD", 4) == 0) {
                    item.installable = true;
                    item.status.clear();
                }
            }
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
    ::mkdir("sdmc:/torfoil", 0777);

    // La langue d'abord, avant la moindre chaîne produite. Sans réglage
    // enregistré on suit la console : un homebrew qui démarre en français sur
    // une console réglée en allemand passe pour cassé avant d'avoir servi à
    // quoi que ce soit. Et c'est bien la PREMIÈRE chose à faire — le fichier
    // magnets.txt créé plus bas porte déjà du texte traduit.
    settings_.load(kSettingsFile);
    set_language(settings_.effective_language());
    util::log_fmt("langue : %s (réglage « %s »)", code_of(language()),
                  settings_.language.c_str());

    if (!render_.init(err)) return false;

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad_);

    ::mkdir(kDownloadDir, 0777);
    ::mkdir(kInboxDir, 0777);
    // Créé même si l'accès à distance est coupé : un dossier surveillé qui
    // n'existe pas est un dossier qu'on ne pense jamais à créer.
    ::mkdir(kWatchDir, 0777);

    // Le fichier existe dès le premier lancement : on trouve où déposer ses
    // liens sans avoir à deviner le chemin ni lire la doc.
    if (std::FILE* probe = std::fopen(kMagnetsFile, "rb")) {
        std::fclose(probe);
    } else if (std::FILE* seed = std::fopen(kMagnetsFile, "wb")) {
        std::fprintf(seed, "%s\n%s\n", tr(Str::MagnetsFileHead1), tr(Str::MagnetsFileHead2));
        std::fclose(seed);
    }

    if (!session_.start(kDownloadDir, err)) return false;

    vpn_.load();
    apply_settings();
    refresh_library();
    return true;
}

void App::shutdown() {
    // On ne quitte jamais en laissant une écriture ncm en vol.
    if (install_job_) {
        install_job_->cancel.store(true);
        if (install_job_->worker.joinable()) install_job_->worker.join();
        install_job_.reset();
    }

    if (diag_thread_.joinable()) diag_thread_.join();

    // Avant la session : le serveur lui passe des commandes, il ne doit plus
    // rien avoir en main quand elle s'arrête.
    phone_.stop();

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

int& App::scroll() {
    return scroll_[static_cast<int>(tab_)];
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
    sync_phone_bridge();
    settings_.save(kSettingsFile);
}

// Les choix possibles : « auto », puis les sept langues. « auto » d'abord parce
// que c'est le réglage par défaut et celui vers lequel on revient.
void App::cycle_language(int delta) {
    const int total = 1 + static_cast<int>(Lang::kCount);

    int index = 0;  // 0 = auto
    Lang parsed = Lang::En;
    if (settings_.language != "auto" && lang_from_code(settings_.language, parsed)) {
        index = 1 + static_cast<int>(parsed);
    }

    index = (index + delta % total + total) % total;
    settings_.language = index == 0 ? "auto" : code_of(static_cast<Lang>(index - 1));

    // Appliqué immédiatement : une langue qui n'apparaîtrait qu'au redémarrage
    // laisserait croire que le réglage ne marche pas.
    set_language(settings_.effective_language());
    apply_settings();
}

// Le serveur suit la case à cocher, dans les deux sens et à chaud. Un réglage
// de ce genre qui n'agirait qu'au prochain démarrage donne l'impression de ne
// pas fonctionner.
void App::sync_phone_bridge() {
    if (settings_.phone_import == phone_.running()) return;

    if (!settings_.phone_import) {
        phone_.stop();
        phone_error_.clear();
        return;
    }

    std::string err;
    if (phone_.start(session_, kInboxDir, kPhonePort, &err)) {
        phone_error_.clear();
    } else {
        phone_error_ = err;
        util::log_line("import téléphone indisponible : " + err);
    }
}

void App::refresh_library() {
    library_.clear();
    scan_directory(kDownloadDir, 0, library_);

    // Un fichier en cours de téléchargement existe déjà sur la carte, à sa
    // taille finale ou presque. Sans ce marquage il apparaît comme installable,
    // et l'installation d'un paquet incomplet échoue de façon incompréhensible
    // — ou pire, aboutit sur une tuile morte.
    //
    // La comparaison porte sur content_root, le chemin propre au torrent, et
    // non sur son dossier de téléchargement : celui-ci est le MÊME pour tous,
    // si bien que le moindre torrent inachevé grisait la bibliothèque entière,
    // y compris des paquets complets et parfaitement installables.
    for (const bt::TorrentStatus& t : torrents_) {
        const bool done = t.state == bt::TorrentState::Completed ||
                          t.state == bt::TorrentState::Seeding;
        if (done || t.content_root.empty()) continue;

        for (LibraryEntry& item : library_) {
            const bool mine =
                item.path == t.content_root ||
                (item.path.size() > t.content_root.size() &&
                 item.path.compare(0, t.content_root.size(), t.content_root) == 0 &&
                 item.path[t.content_root.size()] == '/');
            if (!mine) continue;
            item.installable = false;
            item.status = trf(Str::InProgressPct, static_cast<int>(t.progress * 100));
        }
    }

    for (LibraryEntry& item : library_) {
        if (!item.installable) continue;
        if (item.kind == "NSZ" || item.kind == "XCZ") {
            item.installable = false;
            item.status = tr(Str::CompressedFirst);
        }
    }

    std::sort(library_.begin(), library_.end(),
              [](const LibraryEntry& a, const LibraryEntry& b) {
                  // Les installables d'abord : c'est ce qu'on vient chercher.
                  if (a.installable != b.installable) return a.installable;
                  return a.name < b.name;
              });
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
            leftovers.push_back(std::string(tr(Str::MagnetsIgnored)) + line);
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
        toast(added > 1 ? trf(Str::ToastTorrentsAdded, added) : tr(Str::ToastTorrentAdded));
    } else if (leftovers.empty()) {
        // Cas courant et déroutant : le fichier a déjà été importé. Ce n'est pas
        // une erreur, et le dire autrement évite de chercher un problème absent.
        toast(tr(Str::ToastMagnetsEmpty));
    } else {
        toast(tr(Str::ToastMagnetsNoneOk), true);
    }
}

// ---------------------------------------------------------------------------
// Sélecteur de fichiers .torrent
//
// Un .torrent posé sur la carte — récupéré au navigateur de la console, copié
// depuis un PC, envoyé par FTP — n'avait aucun moyen d'entrer : l'application
// ne lisait que des liens magnet. C'est le chaînon qui manquait.
// ---------------------------------------------------------------------------

// « sdmc:/ » se termine déjà par une barre, contrairement à tous les autres
// dossiers. Concaténer sans y prendre garde donne « sdmc://Nintendo ».
static std::string join_path(const std::string& dir, const std::string& name) {
    if (!dir.empty() && dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

void App::browse_to(const std::string& dir) {
    browser_.dir = dir;
    browser_.entries.clear();
    browser_.selection = 0;
    browser_.scroll = 0;
    browser_.truncated = false;

    // « .. » sauf à la racine du périphérique.
    if (dir.size() > 6) {
        BrowserEntry up;
        up.name = "..";
        up.is_dir = true;
        up.is_parent = true;
        browser_.entries.push_back(std::move(up));
    }

    DIR* handle = ::opendir(dir.c_str());
    if (!handle) return;

    std::vector<BrowserEntry> dirs;
    std::vector<BrowserEntry> files;
    while (dirent* entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        // Les fichiers cachés du système et nos propres marqueurs de reprise
        // n'ont rien à faire dans une liste où l'on cherche un .torrent.
        if (!name.empty() && name[0] == '.') continue;

        if (dirs.size() + files.size() >= kBrowserMaxEntries) {
            browser_.truncated = true;
            break;
        }

        const std::string full = join_path(dir, name);
        struct stat st{};
        if (::stat(full.c_str(), &st) != 0) continue;

        BrowserEntry item;
        item.name = name;
        if (S_ISDIR(st.st_mode)) {
            item.is_dir = true;
            dirs.push_back(std::move(item));
        } else {
            // Seuls les .torrent sont proposés : afficher toute la carte
            // reviendrait à noyer ce qu'on cherche.
            if (extension_of(name) != "torrent") continue;
            item.size = static_cast<uint64_t>(st.st_size);
            files.push_back(std::move(item));
        }
    }
    ::closedir(handle);

    auto by_name = [](const BrowserEntry& a, const BrowserEntry& b) { return a.name < b.name; };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);

    browser_.entries.insert(browser_.entries.end(), dirs.begin(), dirs.end());
    browser_.entries.insert(browser_.entries.end(), files.begin(), files.end());
}

void App::open_browser() {
    // On ouvre là où les .torrent arrivent le plus souvent, et on retombe sur la
    // racine de la carte si ce dossier n'existe pas encore.
    std::string start = kInboxDir;
    struct stat st{};
    if (::stat(start.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) start = "sdmc:/";

    browser_.active = true;
    browse_to(start);
}

bool App::add_torrent_path(const std::string& path, std::string* err) {
    if (!session_.add_torrent_file(path, err)) return false;

    // Copie conservée au même endroit que ce qui arrive du téléphone : on
    // retrouve d'un seul coup d'œil tout ce qu'on a fait entrer.
    const size_t slash = path.find_last_of('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::string target = std::string(kInboxDir) + "/" + util::sanitize_filename(name);
    if (target != path) {
        if (std::FILE* in = std::fopen(path.c_str(), "rb")) {
            if (std::FILE* out = std::fopen(target.c_str(), "wb")) {
                char chunk[8192];
                size_t n;
                while ((n = std::fread(chunk, 1, sizeof(chunk), in)) > 0) {
                    std::fwrite(chunk, 1, n, out);
                }
                std::fclose(out);
            }
            std::fclose(in);
        }
    }
    return true;
}

void App::handle_browser_input(uint64_t buttons) {
    const int count = static_cast<int>(browser_.entries.size());

    if (buttons & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
        if (count > 0) browser_.selection = (browser_.selection + 1) % count;
    }
    if (buttons & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
        if (count > 0) browser_.selection = (browser_.selection + count - 1) % count;
    }

    if (buttons & HidNpadButton_B) {
        // B remonte d'un cran, et ferme seulement une fois en haut : c'est le
        // geste attendu, et ça évite de tout refermer par erreur au troisième
        // niveau de dossiers.
        if (browser_.dir.size() > 6) {
            const std::string current = browser_.dir;
            const size_t slash = current.find_last_of('/');
            browse_to(slash > 5 ? current.substr(0, slash) : "sdmc:/");
        } else {
            browser_.active = false;
        }
        return;
    }

    if (buttons & HidNpadButton_X) {
        browser_.active = false;
        return;
    }

    // Y : tout ajouter d'un coup. Un dossier de .torrent préparé sur un PC part
    // ainsi en une pression au lieu d'une par fichier.
    if (buttons & HidNpadButton_Y) {
        int added = 0;
        std::string last_error;
        for (const BrowserEntry& item : browser_.entries) {
            if (item.is_dir) continue;
            std::string err;
            if (add_torrent_path(join_path(browser_.dir, item.name), &err)) ++added;
            else last_error = err;
        }
        if (added > 0) {
            toast(added > 1 ? trf(Str::ToastTorrentsAdded, added) : tr(Str::ToastTorrentAdded));
            browser_.active = false;
        } else {
            toast(last_error.empty() ? tr(Str::NoTorrentInFolder) : last_error, true);
        }
        return;
    }

    if ((buttons & HidNpadButton_A) && count > 0) {
        const BrowserEntry& item = browser_.entries[static_cast<size_t>(browser_.selection)];
        if (item.is_dir) {
            if (item.is_parent) {
                const size_t slash = browser_.dir.find_last_of('/');
                browse_to(slash > 5 ? browser_.dir.substr(0, slash) : "sdmc:/");
            } else {
                browse_to(join_path(browser_.dir, item.name));
            }
            return;
        }

        std::string err;
        if (add_torrent_path(join_path(browser_.dir, item.name), &err)) {
            toast(trf(Str::ToastTorrentAddedName, item.name.c_str()));
            browser_.active = false;
        } else {
            toast(err, true);
        }
    }
}

void App::draw_browser() {
    if (!browser_.active) return;

    const int w = 980;
    const int h = 560;
    const int x = (Renderer::kWidth - w) / 2;
    const int y = (Renderer::kHeight - h) / 2;

    render_.rect(0, 0, Renderer::kWidth, Renderer::kHeight, Color{0x00, 0x00, 0x00, 0xc0});
    render_.rounded_rect(x, y, w, h, 16, palette::kSurface);

    render_.text(FontSize::Title, tr(Str::BrowserTitle), x + 28, y + 20, palette::kText);
    render_.text_clipped(FontSize::Small, browser_.dir, x + 28, y + 62, w - 56, palette::kAccent);

    const int list_top = y + 96;
    const int row_h = 40;
    const int visible = (h - 96 - 60) / row_h;

    if (browser_.selection < browser_.scroll) browser_.scroll = browser_.selection;
    if (browser_.selection >= browser_.scroll + visible) {
        browser_.scroll = browser_.selection - visible + 1;
    }

    if (browser_.entries.empty()) {
        render_.text(FontSize::Body, tr(Str::BrowserEmpty), x + 28, list_top + 10,
                     palette::kTextDim);
    }

    const int total = static_cast<int>(browser_.entries.size());
    for (int i = browser_.scroll; i < total && i < browser_.scroll + visible; ++i) {
        const BrowserEntry& item = browser_.entries[static_cast<size_t>(i)];
        const int row_y = list_top + (i - browser_.scroll) * row_h;

        if (i == browser_.selection) {
            render_.rounded_rect(x + 16, row_y - 4, w - 32, row_h - 2, 8, palette::kSelected);
        }

        const char* glyph = item.is_dir ? "[+]" : " · ";
        render_.text(FontSize::Body, glyph, x + 28, row_y, palette::kTextDim);
        render_.text_clipped(FontSize::Body, item.name, x + 76, row_y, w - 250,
                             item.is_dir ? palette::kTextDim : palette::kText);

        if (!item.is_dir) {
            const std::string size = util::human_size(item.size);
            render_.text(FontSize::Small, size,
                         x + w - 28 - render_.text_width(FontSize::Small, size), row_y + 4,
                         palette::kTextDim);
        }
    }

    const char* footer = tr(browser_.truncated ? Str::BrowserTruncated : Str::BrowserFooter);
    render_.text_clipped(FontSize::Small, footer, x + 28, y + h - 40, w - 56,
                         browser_.truncated ? palette::kWarn : palette::kTextDim);
}

// Dossier surveillé : déposer un .torrent dedans suffit, il part tout seul.
// C'est ce qui rend l'application utilisable avec sys-ftpd sans jamais y
// toucher, et ce qui permet d'enchaîner sans reprendre la manette.
void App::poll_watch_folder(uint64_t now_ms) {
    if (now_ms - last_watch_scan_ms_ < kWatchIntervalMs) return;
    last_watch_scan_ms_ = now_ms;

    DIR* handle = ::opendir(kWatchDir);
    if (!handle) return;

    std::vector<std::string> found;
    while (dirent* entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (extension_of(name) != "torrent") continue;
        found.push_back(name);
    }
    ::closedir(handle);

    int added = 0;
    std::string last_name;
    for (const std::string& name : found) {
        const std::string path = join_path(kWatchDir, name);

        std::string err;
        if (session_.add_torrent_file(path, &err)) {
            // Déplacé, pas copié : laisser le fichier sur place le ferait
            // reproposer toutes les cinq secondes pour l'éternité.
            const std::string target = std::string(kInboxDir) + "/" + util::sanitize_filename(name);
            std::remove(target.c_str());
            if (std::rename(path.c_str(), target.c_str()) != 0) std::remove(path.c_str());
            ++added;
            last_name = name;
        } else {
            // Un fichier refusé est marqué, sinon il serait réessayé sans fin et
            // le journal se remplirait de la même erreur toutes les 5 secondes.
            const std::string rejected = path + ".refuse";
            std::remove(rejected.c_str());
            std::rename(path.c_str(), rejected.c_str());
            util::log_line("dossier surveillé : " + name + " refusé — " + err);
            toast(name + " : " + err, true);
        }
    }

    if (added == 1) toast(trf(Str::ToastWatchAdded, last_name.c_str()));
    else if (added > 1) toast(trf(Str::ToastWatchAddedMany, added));
}

// Rejoue toute la chaîne d'installation sans écrire dans la mémoire système, et
// recalcule l'empreinte SHA-256 de chaque contenu. Sert à répondre à la seule
// question qui compte quand une installation rate : est-ce le paquet, ou est-ce
// l'installateur ?
void App::verify_selected() {
    if (library_.empty()) return;
    if (install_job_ && install_job_->running.load()) {
        toast(tr(Str::ToastOperationBusy), true);
        return;
    }

    const LibraryEntry& item = library_[static_cast<size_t>(selection())];
    if (item.kind == "NSZ" || item.kind == "XCZ") {
        toast(tr(Str::NothingToVerify), true);
        return;
    }

    std::string keys_err;
    if (!install::keys_available(&keys_err)) {
        toast(keys_err, true);
        return;
    }

    install_job_ = std::make_unique<InstallJob>();
    install_job_->running.store(true);
    install_job_->source_name = item.name;
    install_job_->verify_only = true;
    {
        std::lock_guard<std::mutex> lock(install_job_->mutex);
        install_job_->step = tr(Str::OverlayReading);
    }

    const std::string path = item.path;
    InstallJob* job = install_job_.get();

    job->worker = std::thread([job, path] {
        const install::Outcome outcome =
            install::verify_package(path, /*deep=*/true, [job](const install::Progress& p) {
                job->done.store(p.done);
                job->total.store(p.total);
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->step = p.step;
                }
                return !job->cancel.load();
            });

        util::log_line(std::string("vérification ") + (outcome.ok ? "OK" : "ÉCHEC") + " : " +
                       outcome.message);

        std::lock_guard<std::mutex> lock(job->mutex);
        job->result = outcome.message;
        job->ok = outcome.ok;
        job->finished = true;
        job->running.store(false);
    });
}

void App::install_selected() {
    if (library_.empty()) return;
    if (install_job_ && install_job_->running.load()) {
        toast(tr(Str::ToastInstallBusy), true);
        return;
    }

    const LibraryEntry& item = library_[static_cast<size_t>(selection())];

    if (!item.installable) {
        toast(item.status.empty() ? tr(Str::NotInstallableFile) : item.status, true);
        return;
    }

    std::string keys_err;
    if (!install::keys_available(&keys_err)) {
        toast(keys_err, true);
        return;
    }

    install_job_ = std::make_unique<InstallJob>();
    install_job_->running.store(true);
    install_job_->source_name = item.name;
    {
        std::lock_guard<std::mutex> lock(install_job_->mutex);
        install_job_->step = tr(Str::OverlayReading);
    }

    const std::string path = item.path;
    InstallJob* job = install_job_.get();

    job->worker = std::thread([job, path] {
        const install::Outcome outcome = install::install_package(
            path, install::Target::Sd, [job](const install::Progress& p) {
                job->done.store(p.done);
                job->total.store(p.total);
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->step = p.step;
                }
                return !job->cancel.load();
            });

        util::log_line(std::string("installation ") + (outcome.ok ? "réussie" : "ÉCHOUÉE") +
                       " : " + outcome.message);

        std::lock_guard<std::mutex> lock(job->mutex);
        job->result = outcome.message;
        job->ok = outcome.ok;
        job->finished = true;
        job->running.store(false);
    });
}

void App::poll_install_job() {
    if (!install_job_) return;
    if (install_job_->running.load()) return;

    bool finished = false;
    bool ok = false;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(install_job_->mutex);
        finished = install_job_->finished;
        ok = install_job_->ok;
        message = install_job_->result;
    }
    if (!finished) return;

    if (install_job_->worker.joinable()) install_job_->worker.join();
    install_job_.reset();

    toast(message.empty() ? tr(Str::ToastInstallDone) : message, !ok);
}

// Bandeau discret en bas de l'écran plutôt qu'une fenêtre modale : on doit
// pouvoir consulter ses torrents ou le VPN pendant qu'un jeu s'installe, comme
// on le ferait pendant un téléchargement.
void App::draw_install_overlay() {
    if (!install_job_ || !install_job_->running.load()) return;

    const int h = 74;
    const int y = kContentBottom - h - 8;
    const int x = kMargin;
    const int w = Renderer::kWidth - 2 * kMargin;

    render_.rounded_rect(x, y, w, h, 12, palette::kSurfaceAlt);

    std::string step;
    {
        std::lock_guard<std::mutex> lock(install_job_->mutex);
        step = install_job_->step;
    }

    const uint64_t done = install_job_->done.load();
    const uint64_t total = install_job_->total.load();
    const float ratio = total > 0 ? static_cast<float>(static_cast<double>(done) /
                                                       static_cast<double>(total))
                                  : 0.0f;

    const std::string title =
        std::string(tr(install_job_->verify_only ? Str::OverlayVerifying : Str::OverlayInstalling)) +
        install_job_->source_name;
    render_.text_clipped(FontSize::Small, title, x + 18, y + 10, w - 200, palette::kText);

    char detail[224];
    std::snprintf(detail, sizeof(detail), "%s  —  %s / %s  (%.0f %%)   ·   %s", step.c_str(),
                  util::human_size(done).c_str(), util::human_size(total).c_str(),
                  ratio * 100.0f, tr(Str::OverlayCancel));
    render_.text_clipped(FontSize::Small, detail, x + 18, y + 34, w - 36, palette::kTextDim);

    render_.progress_bar(x + 18, y + 58, w - 36, 8, ratio, palette::kAccent);
}

void App::handle_input(uint64_t now_ms) {
    padUpdate(&pad_);
    const u64 down = padGetButtonsDown(&pad_);

    // Les panneaux modaux captent tout, dans l'ordre où ils se recouvrent.
    if (browser_.active) {
        handle_browser_input(down);
        return;
    }

    if (details_) {
        if (down & (HidNpadButton_B | HidNpadButton_Minus | HidNpadButton_A)) details_ = false;
        return;
    }

    // Une confirmation capte tout : tant qu'on n'a pas répondu, aucune autre
    // touche ne doit produire d'effet dans le dos de la question posée.
    if (confirm_.active) {
        if (down & HidNpadButton_A) {
            session_.remove(confirm_.hash, /*delete_files=*/false);
            toast(tr(Str::ToastRemovedKept));
            confirm_ = Confirm{};
        } else if (down & HidNpadButton_Y) {
            session_.remove(confirm_.hash, /*delete_files=*/true);
            toast(tr(Str::ToastRemovedDeleted));
            confirm_ = Confirm{};
        } else if (down & HidNpadButton_B) {
            confirm_ = Confirm{};
        }
        return;
    }

    // L'installation tourne en fond, comme un téléchargement : rien ne justifie
    // d'immobiliser l'application pendant plusieurs minutes. On garde seulement
    // deux garde-fous — pas de seconde installation en parallèle (contrôlé à
    // l'entrée), et pas de sortie qui laisserait une écriture ncm en vol.
    const bool installing = install_job_ && install_job_->running.load();

    if (installing && (down & HidNpadButton_B)) {
        install_job_->cancel.store(true);
        toast(tr(Str::ToastCancelling));
        return;
    }

    if (installing && (down & HidNpadButton_Plus)) {
        toast(tr(Str::ToastQuitBlocked), true);
        return;
    }

    if (down & HidNpadButton_Plus) {
        running_ = false;
        return;
    }

    // Changement d'onglet : gâchettes L/R. Le défilement de chaque onglet lui
    // appartient et n'est plus remis à zéro au passage.
    const int tab_count = static_cast<int>(Tab::kCount);
    if (down & HidNpadButton_L) {
        tab_ = static_cast<Tab>((static_cast<int>(tab_) + tab_count - 1) % tab_count);
    }
    if (down & HidNpadButton_R) {
        tab_ = static_cast<Tab>((static_cast<int>(tab_) + 1) % tab_count);
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
            if (down & HidNpadButton_A) open_browser();
            if (down & HidNpadButton_X) add_magnet_flow();
            if (down & HidNpadButton_ZL) import_magnets_file();
            if ((down & HidNpadButton_Minus) && !torrents_.empty()) details_ = true;
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
                confirm_.active = true;
                confirm_.hash = t.hash_hex;
                confirm_.name = t.name;
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
            if (down & HidNpadButton_A) install_selected();
            if (down & HidNpadButton_Y) verify_selected();
            if (down & HidNpadButton_X) {
                refresh_library();
                toast(tr(Str::LibraryRescanned));
            }
            break;

        case Tab::Remote:
            if (down & HidNpadButton_A) {
                settings_.phone_import = !settings_.phone_import;
                apply_settings();
                if (!settings_.phone_import) {
                    toast(tr(Str::RemoteServerStopped));
                } else if (phone_.running()) {
                    toast(trf(Str::RemotePageOpen, phone_.url().c_str()));
                } else {
                    toast(phone_error_.empty() ? tr(Str::RemoteUnavailable) : phone_error_, true);
                }
            }
            // Le Wi-Fi peut être revenu depuis le lancement. Plutôt que de
            // laisser l'onglet mort jusqu'au prochain démarrage, on redemande.
            if ((down & HidNpadButton_Y) && settings_.phone_import) {
                phone_.stop();
                sync_phone_bridge();
                toast(phone_.running()
                          ? tr(Str::RemoteRestarted)
                          : (phone_error_.empty() ? tr(Str::RemoteStillNoNetwork) : phone_error_),
                      !phone_.running());
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
            // Ligne 0 : la langue. Les suivantes : les cases. La langue est en
            // tête parce que c'est la seule qu'on doit pouvoir atteindre quand
            // on ne comprend pas encore ce qui est écrit en dessous.
            const int count = 1 + static_cast<int>(toggles().size());
            if (down & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
                settings_cursor_ = (settings_cursor_ + 1) % count;
            }
            if (down & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
                settings_cursor_ = (settings_cursor_ + count - 1) % count;
            }

            const bool next = (down & (HidNpadButton_A | HidNpadButton_Right |
                                       HidNpadButton_StickLRight)) != 0;
            const bool previous = (down & (HidNpadButton_Left | HidNpadButton_StickLLeft)) != 0;

            if (settings_cursor_ == 0) {
                if (next || previous) cycle_language(next ? 1 : -1);
            } else if (next || previous) {
                // Une case n'a que deux états : les deux directions la
                // basculent, ce qui évite de chercher quelle touche l'active.
                const Toggle& t = toggles()[static_cast<size_t>(settings_cursor_ - 1)];
                bool& value = settings_.*(t.field);
                value = !value;
                apply_settings();

                const bool checked = t.inverted ? !value : value;
                toast(trf(checked ? Str::SettingOn : Str::SettingOff, tr(t.label)));
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

    int x = kMargin + render_.text_width(FontSize::Title, "Torfoil") + 30;

    auto pill = [&](const std::string& text, const Color& color) {
        const int w = render_.text_width(FontSize::Small, text) + 28;
        render_.rounded_rect(x, 24, w, 30, 15, color);
        render_.text(FontSize::Small, text, x + 14, 28, palette::kText);
        x += w + 10;
    };

    // Pastille d'état du VPN.
    const vpn::State vpn_state = vpn_.state();
    const bool up = vpn_state == vpn::State::Connected;
    pill(tr(up ? Str::VpnOn : (vpn_state == vpn::State::Working ? Str::VpnWorking : Str::VpnNone)),
         up ? palette::kAccentDim
            : (vpn_state == vpn::State::Working ? palette::kWarn : palette::kError));

    // Le serveur téléphone n'apparaît que s'il tourne : une pastille « éteint »
    // en permanence n'apprendrait rien et volerait la place au reste. On y met
    // l'adresse sans le « http:// » ni la barre finale — c'est exactement ce
    // qu'on tape dans un navigateur, et ça tient dans la barre.
    if (phone_.running()) {
        std::string host = phone_.url();
        if (host.compare(0, 7, "http://") == 0) host.erase(0, 7);
        if (!host.empty() && host.back() == '/') host.pop_back();
        pill(std::string(tr(Str::TabRemote)) + "  " + host, palette::kSurfaceAlt);
    }

    // Débits globaux, à droite, et l'espace libre juste en dessous.
    //
    // Cette dernière ligne mérite sa place : un jeu Switch pèse plusieurs
    // dizaines de gigaoctets, et découvrir que la carte était pleine se faisait
    // jusqu'ici en lisant un message d'erreur — parfois après trois heures de
    // téléchargement.
    char rates[128];
    std::snprintf(rates, sizeof(rates), "↓ %s   ↑ %s",
                  util::human_rate(session_.rate_down()).c_str(),
                  util::human_rate(session_.rate_up()).c_str());
    const int w = render_.text_width(FontSize::Body, rates);
    render_.text(FontSize::Body, rates, Renderer::kWidth - kMargin - w, 16, palette::kTextDim);

    if (free_space_ > 0) {
        // Rouge sous 5 Go : en dessous, aucun jeu récent ne rentre.
        const std::string text = trf(Str::FreeSpace, util::human_size(free_space_).c_str());
        const Color color = free_space_ < 5ull * 1024 * 1024 * 1024 ? palette::kWarn
                                                                   : palette::kTextDim;
        const int fw = render_.text_width(FontSize::Small, text);
        render_.text(FontSize::Small, text, Renderer::kWidth - kMargin - fw, 46, color);
    }
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
        const int badge = render_.key_badge_width(FontSize::Small, button);
        const int width = badge + 10 + render_.text_width(FontSize::Small, action);
        // Une aide qui déborde de l'écran est tronquée par le bord, sans qu'on
        // sache qu'il en manquait : on s'arrête proprement avant.
        if (x + width > Renderer::kWidth - kMargin) break;

        render_.key_badge(FontSize::Small, button, x, y + 15, palette::kText);
        x += badge + 10;
        render_.text(FontSize::Small, action, x, y + 15, palette::kTextDim);
        x += render_.text_width(FontSize::Small, action) + 24;
    }
}

void App::draw_scrollbar(int first_visible, int visible, int total) {
    if (total <= visible || visible <= 0) return;

    const int track_x = Renderer::kWidth - kMargin + 12;
    const int track_y = kContentTop + 14;
    const int track_h = kContentBottom - track_y - 14;
    render_.rounded_rect(track_x, track_y, 6, track_h, 3, palette::kSurfaceAlt);

    const int thumb_h = std::max(24, track_h * visible / total);
    const int span = track_h - thumb_h;
    const int max_first = total - visible;
    const int thumb_y = track_y + (max_first > 0 ? span * first_visible / max_first : 0);
    render_.rounded_rect(track_x, thumb_y, 6, thumb_h, 3, palette::kAccentDim);
}

void App::draw_empty(const std::string& title, const std::string& hint) {
    const int cx = Renderer::kWidth / 2;
    render_.text_centered(FontSize::Title, title, cx, kContentTop + 160, palette::kTextDim);
    render_.text_centered(FontSize::Body, hint, cx, kContentTop + 210, palette::kTextDim);
}

void App::draw_torrents() {
    if (torrents_.empty()) {
        const int cx = Renderer::kWidth / 2;
        render_.text_centered(FontSize::Title, tr(Str::NoTorrents), cx, kContentTop + 120,
                              palette::kTextDim);
        // Les quatre entrées possibles, dans l'ordre où elles sont commodes.
        // L'onglet À distance vient en premier parce que c'est le seul chemin
        // qui ne demande ni clavier virtuel ni PC.
        render_.text_centered(FontSize::Body, tr(Str::HowRemote), cx, kContentTop + 180,
                              palette::kAccent);
        render_.text_centered(FontSize::Body, tr(Str::HowBrowse), cx, kContentTop + 218,
                              palette::kTextDim);
        render_.text_centered(FontSize::Body, tr(Str::HowMagnet), cx, kContentTop + 252,
                              palette::kTextDim);
        render_.text_centered(FontSize::Body, tr(Str::HowMagnetsFile), cx, kContentTop + 286,
                              palette::kTextDim);
        render_.text_centered(FontSize::Small, tr(Str::HowWatchFolder), cx, kContentTop + 330,
                              palette::kTextDim);
        return;
    }

    const int total = static_cast<int>(torrents_.size());
    const int visible = (kContentBottom - kContentTop - 20) / (kRowHeight + kRowGap);
    int& first = scroll();
    if (selection() < first) first = selection();
    if (selection() >= first + visible) first = selection() - visible + 1;
    if (first > std::max(0, total - visible)) first = std::max(0, total - visible);

    int y = kContentTop + 14;
    for (int i = first; i < total && i < first + visible; ++i) {
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
        const std::string peers =
            t.peers_choking > 0 && t.peers_connected > 0
                ? trf(Str::PeersBlocked, t.peers_connected, t.peers_choking)
                : trf(Str::PeersPlain, t.peers_connected);

        const std::string detail =
            std::string(tr(state_key(t.state))) + " · " +
            (t.total_size ? util::human_size(t.total_size) : std::string(tr(Str::SizeUnknown))) +
            " · " + peers + " · ↓ " + util::human_rate(t.rate_down);
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

    draw_scrollbar(first, visible, total);

    if (!torrents_.empty() && !torrents_[static_cast<size_t>(selection())].message.empty()) {
        render_.text_clipped(FontSize::Small,
                             torrents_[static_cast<size_t>(selection())].message, kMargin,
                             kContentBottom - 26, Renderer::kWidth - 2 * kMargin,
                             palette::kWarn);
    }
}

void App::draw_library() {
    if (library_.empty()) {
        draw_empty(tr(Str::LibraryEmpty), tr(Str::LibraryEmptyHint));
        return;
    }

    const int row_h = 68;
    const int total = static_cast<int>(library_.size());
    const int visible = (kContentBottom - kContentTop - 20) / (row_h + 8);
    int& first = scroll();
    if (selection() < first) first = selection();
    if (selection() >= first + visible) first = selection() - visible + 1;
    if (first > std::max(0, total - visible)) first = std::max(0, total - visible);

    int y = kContentTop + 14;
    for (int i = first; i < total && i < first + visible; ++i) {
        const LibraryEntry& item = library_[static_cast<size_t>(i)];
        const bool active = i == selection();

        render_.rounded_rect(kMargin, y, Renderer::kWidth - 2 * kMargin, row_h, 12,
                             active ? palette::kSelected : palette::kSurface);

        // Étiquette de type à gauche.
        const int badge_w = render_.text_width(FontSize::Small, item.kind) + 22;
        render_.rounded_rect(kMargin + 16, y + 20, badge_w, 28, 8, palette::kAccentDim);
        render_.text(FontSize::Small, item.kind, kMargin + 27, y + 23, palette::kText);

        // Un fichier non installable se voit au premier coup d'œil : le nom est
        // grisé et la raison remplace la taille, qui ne veut rien dire tant que
        // le téléchargement n'est pas fini.
        render_.text_clipped(FontSize::Body, item.name, kMargin + 16 + badge_w + 16, y + 8,
                             Renderer::kWidth - 2 * kMargin - badge_w - 220,
                             item.installable ? palette::kText : palette::kTextDim);

        const std::string detail =
            item.installable ? util::human_size(item.size)
                             : (item.status.empty() ? tr(Str::NotInstallable) : item.status);
        render_.text(FontSize::Small, detail, kMargin + 16 + badge_w + 16, y + 38,
                     palette::kTextDim);

        y += row_h + 8;
    }

    draw_scrollbar(first, visible, total);
}

// L'onglet qui répond à « comment je fais entrer un .torrent là-dedans ». Tout
// tient sur un écran : le code à viser, l'adresse pour ceux qui préfèrent la
// taper, et ce que la console a reçu jusqu'ici.
void App::draw_remote() {
    const int x = kMargin;
    int y = kContentTop + 24;

    if (!settings_.phone_import) {
        draw_empty(tr(Str::RemoteDisabled), tr(Str::RemoteDisabledHint));
        return;
    }

    if (!phone_.running()) {
        draw_empty(tr(Str::RemoteUnavailable),
                   phone_error_.empty() ? std::string(tr(Str::RemoteCheckWifi))
                                        : phone_error_ + tr(Str::RemoteRetrySuffix));
        return;
    }

    // Le code QR, aussi grand que la hauteur disponible le permet : il est visé
    // à un mètre ou deux, depuis un canapé.
    const util::QrCode& code = phone_.qr();
    const int available = kContentBottom - kContentTop - 60;
    const int scale = code.size > 0
                          ? std::max(2, available / (code.size + 2 * Renderer::kQrQuietZone))
                          : 0;
    const int extent = Renderer::qr_extent(code, scale);
    if (extent > 0) render_.qr_code(code, x, y, scale);

    const int text_x = x + extent + 46;
    const int text_w = Renderer::kWidth - kMargin - text_x;

    render_.text_clipped(FontSize::Title, tr(Str::RemoteScanTitle), text_x, y, text_w,
                         palette::kText);
    y += 48;
    // Lignes courtes plutôt qu'un paragraphe : le rendu tronque à l'ellipse au
    // lieu de passer à la ligne, et une phrase trop longue perdrait sa fin.
    render_.text_clipped(FontSize::Small, tr(Str::RemoteScanLine1), text_x, y, text_w,
                         palette::kTextDim);
    y += 24;
    render_.text_clipped(FontSize::Small, tr(Str::RemoteScanLine2), text_x, y, text_w,
                         palette::kTextDim);
    y += 40;

    render_.text_clipped(FontSize::Small, tr(Str::RemoteTypeAddress), text_x, y, text_w,
                         palette::kTextDim);
    y += 26;
    render_.text_clipped(FontSize::Title, phone_.url(), text_x, y, text_w, palette::kAccent);
    y += 54;

    render_.line(text_x, y, text_x + text_w, y, palette::kSurfaceAlt);
    y += 22;

    auto row = [&](const char* key, const std::string& value) {
        render_.text(FontSize::Body, key, text_x, y, palette::kTextDim);
        render_.text_clipped(FontSize::Body, value, text_x + 240, y, text_w - 240, palette::kText);
        y += 34;
    };
    row(tr(Str::RemoteReceived), std::to_string(phone_.added()));
    row(tr(Str::RemoteRequests), std::to_string(phone_.requests()));
    row(tr(Str::RemoteCopies), std::string(kInboxDir));

    const std::string error = phone_.last_error();
    if (!error.empty()) {
        y += 6;
        render_.text_clipped(FontSize::Small, error, text_x, y, text_w, palette::kWarn);
        y += 30;
    }

    y += 10;
    render_.text_clipped(FontSize::Small, tr(Str::RemotePrivacy1), text_x, y, text_w,
                         palette::kTextDim);
    y += 24;
    render_.text_clipped(FontSize::Small, tr(Str::RemotePrivacy2), text_x, y, text_w,
                         palette::kTextDim);
    y += 24;
    render_.text_clipped(FontSize::Small, tr(Str::RemotePrivacy3), text_x, y, text_w,
                         palette::kTextDim);
}

void App::draw_vpn() {
    const int x = kMargin;
    int y = kContentTop + 24;

    render_.text(FontSize::Title, "Mullvad", x, y, palette::kText);
    y += 50;

    const vpn::State state = vpn_.state();
    Color color = palette::kTextDim;
    std::string label = tr(Str::VpnDisconnected);
    switch (state) {
        case vpn::State::Connected:
            color = palette::kSuccess;
            label = tr(Str::VpnConnected);
            break;
        case vpn::State::Working:
            color = palette::kWarn;
            label = tr(Str::VpnConnecting);
            break;
        case vpn::State::Failed:
            color = palette::kError;
            label = tr(Str::VpnFailed);
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
    row(tr(Str::VpnCountry), country.empty() ? tr(Str::VpnCountryAuto) : country);

    if (state == vpn::State::Connected) {
        row(tr(Str::VpnRelay), vpn_.relay_label());
        row(tr(Str::VpnTunnel), "↑ " + util::human_size(vpn_.bytes_sent()) + "   ↓ " +
                                    util::human_size(vpn_.bytes_received()));
    }

    y += 20;
    render_.text_clipped(FontSize::Small, tr(Str::VpnNoteLine1), x, y,
                         Renderer::kWidth - 2 * kMargin, palette::kTextDim);
    y += 26;
    render_.text_clipped(FontSize::Small, tr(Str::VpnNoteLine2), x, y,
                         Renderer::kWidth - 2 * kMargin, palette::kTextDim);
}

// Deux colonnes, et ce n'est pas de l'esthétique : empilé sur toute la largeur,
// le contenu de cet onglet dépassait le bas de l'écran dès la cinquième case, et
// les résultats de l'auto-diagnostic — ce qu'on vient précisément regarder —
// tombaient hors champ sans laisser deviner qu'ils existaient.
void App::draw_settings() {
    constexpr int kLeftWidth = 690;
    constexpr int kRowHeight = 60;
    constexpr int kSwitchW = 58;
    constexpr int kSwitchH = 30;

    const int left = kMargin;
    const int right = kMargin + kLeftWidth + 46;
    const int right_w = Renderer::kWidth - kMargin - right;

    // Chaque ligne est une carte : le fond change avec le focus, jamais la
    // position. Une liste dont les éléments se décalent quand on la parcourt
    // oblige à relire l'écran à chaque pression.
    auto row_card = [&](int y, bool focused) {
        render_.rounded_rect(left, y, kLeftWidth, kRowHeight - 6, 10,
                             focused ? palette::kSelected : palette::kSurface);
        if (focused) render_.rounded_rect(left, y + 8, 3, kRowHeight - 22, 2, palette::kAccent);
    };

    int y = kContentTop + 12;
    render_.section_header(tr(Str::SecGeneral), left, y, palette::kAccent);
    y += 30;

    // ---- la langue, en tête ----
    {
        const bool focused = settings_cursor_ == 0;
        row_card(y, focused);

        const Lang effective = settings_.effective_language();
        const std::string value = settings_.language == "auto"
                                      ? trf(Str::LanguageAuto, endonym(effective))
                                      : endonym(effective);

        render_.text(FontSize::Body, tr(Str::FieldLanguage), left + 20, y + 14, palette::kText);

        // Les chevrons ne s'allument que sur la ligne active : ailleurs, ils
        // annonceraient une action que la touche ne ferait pas.
        const Color arrow = focused ? palette::kAccent : palette::kSurfaceAlt;
        const int value_w = render_.text_width(FontSize::Body, value);
        const int value_x = left + kLeftWidth - 40 - value_w;
        render_.text(FontSize::Body, "‹", value_x - 30, y + 14, arrow);
        render_.text(FontSize::Body, value, value_x, y + 14, palette::kAccent);
        render_.text(FontSize::Body, "›", left + kLeftWidth - 26, y + 14, arrow);
        y += kRowHeight;
    }

    y += 14;
    render_.section_header(tr(Str::SecPrivacy), left, y, palette::kWarn);
    y += 30;

    // Chaque case annonce son coût. Une option de sécurité qui ne dit pas ce
    // qu'elle enlève finit toujours par être activée sans le vouloir, puis
    // accusée de « ralentir l'application ».
    const std::vector<Toggle>& list = toggles();
    for (size_t i = 0; i < list.size(); ++i) {
        const Toggle& t = list[i];
        const bool raw = settings_.*(t.field);
        const bool checked = t.inverted ? !raw : raw;
        const bool focused = static_cast<int>(i) + 1 == settings_cursor_;

        row_card(y, focused);
        render_.text(FontSize::Body, tr(t.label), left + 20, y + 6,
                     checked ? palette::kText : palette::kTextDim);
        render_.text_clipped(FontSize::Small, tr(t.effect), left + 20, y + 30,
                             kLeftWidth - 40 - kSwitchW - 20, palette::kTextDim);
        render_.toggle_switch(left + kLeftWidth - 20 - kSwitchW, y + 12, kSwitchW, kSwitchH,
                              checked, focused);
        y += kRowHeight;
    }

    // ---- colonne de droite : état de la session et auto-diagnostic ----
    int ry = kContentTop + 12;
    render_.section_header(tr(Str::SecStatus), right, ry, palette::kTextDim);
    ry += 32;

    auto info = [&](const std::string& key, const std::string& value) {
        render_.text(FontSize::Small, key, right, ry, palette::kTextDim);
        render_.text_clipped(FontSize::Small, value, right + 200, ry, right_w - 200,
                             palette::kText);
        ry += 28;
    };
    info(tr(Str::FieldTransport), session_.transport_name());
    info(tr(Str::FieldActiveTorrents), std::to_string(torrents_.size()));
    info(tr(Str::FieldRemoteAccess), phone_.running() ? phone_.url() : tr(Str::RemoteStopped));

    ry += 22;
    render_.section_header(tr(Str::SecSelfTest), right, ry, palette::kAccent);
    render_.key_badge(FontSize::Small, "Y", right + right_w - 34, ry, palette::kAccent);
    ry += 34;

    if (diag_running_) {
        render_.text_clipped(FontSize::Small, diag_step_, right, ry, right_w, palette::kAccent);
        return;
    }

    if (diag_report_.checks.empty()) {
        for (Str line : {Str::SelfTestHint1, Str::SelfTestHint2, Str::SelfTestHint3}) {
            render_.text_clipped(FontSize::Small, tr(line), right, ry, right_w, palette::kTextDim);
            ry += 24;
        }
        return;
    }

    for (const diag::Check& c : diag_report_.checks) {
        if (ry + 56 > kContentBottom) break;  // plutôt tronquer que déborder

        // Une épreuve non exécutée n'est pas un échec : la confondre enverrait
        // chercher un bug là où il n'y en a pas.
        const Color color = !c.ran ? palette::kWarn : (c.ok ? palette::kSuccess : palette::kError);
        const Str label = !c.ran ? Str::CheckSkipped : (c.ok ? Str::CheckOk : Str::CheckFailed);

        const std::string tag = tr(label);
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

// Lance l'auto-diagnostic sur son propre thread : la vérification VPN interroge
// le réseau et bloquerait l'affichage plusieurs secondes.
void App::run_selftest() {
    if (diag_running_) return;
    if (diag_thread_.joinable()) diag_thread_.join();

    diag_running_ = true;
    diag_report_.checks.clear();
    diag_step_ = tr(Str::DiagStarting);

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

    // Les messages d'erreur des trackers ou du stockage sont parfois très
    // longs : sans borne, le bandeau dépassait des deux côtés de l'écran et le
    // texte centré partait dans le vide. C'est justement dans ces cas-là qu'on
    // a besoin de le lire.
    const int max_w = Renderer::kWidth - 2 * kMargin;
    const int text_w = std::min(render_.text_width(FontSize::Body, toast_text_), max_w - 48);
    const int w = text_w + 48;
    const int x = (Renderer::kWidth - w) / 2;
    const int y = kContentBottom - 80;

    render_.rounded_rect(x, y, w, 52, 14, toast_error_ ? palette::kError : palette::kAccentDim);
    render_.text_clipped(FontSize::Body, toast_text_, x + 24, y + 12, text_w, palette::kText);
}

// Tout ce que la ligne d'un torrent ne peut pas montrer. Rassemblé ici parce
// que ces chiffres ne servent qu'à une question — « pourquoi ça ne descend
// pas ? » — et que la réponse est presque toujours dans le compte des pairs qui
// étranglent ou dans la liste des trackers.
void App::draw_details() {
    if (!details_ || torrents_.empty()) return;

    const size_t index = static_cast<size_t>(selection());
    if (index >= torrents_.size()) return;
    const bt::TorrentStatus& t = torrents_[index];

    const int w = 1060;
    const int h = 600;
    const int x = (Renderer::kWidth - w) / 2;
    const int y = (Renderer::kHeight - h) / 2;

    render_.rect(0, 0, Renderer::kWidth, Renderer::kHeight, Color{0x00, 0x00, 0x00, 0xc0});
    render_.rounded_rect(x, y, w, h, 16, palette::kSurface);

    render_.text_clipped(FontSize::Title, t.name, x + 28, y + 20, w - 56, palette::kText);

    const int col_a = x + 28;
    const int col_b = x + 540;
    int ya = y + 76;
    int yb = y + 76;

    auto row = [&](int cx, int& cy, const char* key, const std::string& value, const Color& color) {
        render_.text(FontSize::Small, key, cx, cy, palette::kTextDim);
        render_.text_clipped(FontSize::Small, value, cx + 200, cy, 300, color);
        cy += 28;
    };

    char pct[32];
    std::snprintf(pct, sizeof(pct), "%.1f %%", t.progress * 100.0f);

    row(col_a, ya, tr(Str::DetState), tr(state_key(t.state)), state_color(t.state));
    row(col_a, ya, tr(Str::DetProgress), pct, palette::kText);
    row(col_a, ya, tr(Str::DetSize),
        t.total_size ? util::human_size(t.total_size) : tr(Str::SizeUnknown), palette::kText);
    row(col_a, ya, tr(Str::DetReceived), util::human_size(t.downloaded), palette::kText);
    row(col_a, ya, tr(Str::DetSent), util::human_size(t.uploaded), palette::kText);
    if (t.pieces_total > 0) {
        row(col_a, ya, tr(Str::DetPieces),
            std::to_string(t.pieces_done) + " / " + std::to_string(t.pieces_total),
            palette::kText);
    }
    if (t.files_count > 0) {
        row(col_a, ya, tr(Str::DetFiles), std::to_string(t.files_count), palette::kText);
    }
    // Une pièce corrompue arrive ; une centaine désigne un pair ou une carte.
    row(col_a, ya, tr(Str::DetRejected), std::to_string(t.corrupt_pieces),
        t.corrupt_pieces > 0 ? palette::kWarn : palette::kTextDim);

    row(col_b, yb, tr(Str::DetRate),
        "↓ " + util::human_rate(t.rate_down) + "   ↑ " + util::human_rate(t.rate_up),
        palette::kText);
    row(col_b, yb, tr(Str::DetPeersConnected), std::to_string(t.peers_connected), palette::kText);
    // La donnée qui explique le plus souvent un débit décevant.
    row(col_b, yb, tr(Str::DetPeersBlocking), std::to_string(t.peers_choking),
        t.peers_choking > 0 && t.peers_choking == t.peers_connected ? palette::kWarn
                                                                    : palette::kTextDim);
    row(col_b, yb, tr(Str::DetPeersKnown), std::to_string(t.peers_known), palette::kText);
    row(col_b, yb, tr(Str::DetBlocksInFlight), std::to_string(t.blocks_in_flight),
        palette::kText);
    row(col_b, yb, tr(Str::DetSeedsPeers),
        std::to_string(t.seeders) + " / " + std::to_string(t.leechers), palette::kText);
    if (t.eta_s > 0) {
        row(col_b, yb, tr(Str::DetEta), util::human_duration(t.eta_s), palette::kText);
    }
    if (t.is_private) {
        row(col_b, yb, tr(Str::DetPrivate), tr(Str::DetPrivateValue), palette::kWarn);
    }

    int y_low = std::max(ya, yb) + 12;
    render_.line(col_a, y_low, x + w - 28, y_low, palette::kSurfaceAlt);
    y_low += 18;

    render_.text(FontSize::Small, tr(Str::DetHash), col_a, y_low, palette::kTextDim);
    render_.text_clipped(FontSize::Small, t.hash_hex, col_a + 200, y_low, w - 240,
                         palette::kAccent);
    y_low += 28;

    if (!t.content_root.empty()) {
        render_.text(FontSize::Small, tr(Str::DetOnCard), col_a, y_low, palette::kTextDim);
        render_.text_clipped(FontSize::Small, t.content_root, col_a + 200, y_low, w - 240,
                             palette::kText);
        y_low += 28;
    }

    render_.text(FontSize::Small,
                 trf(Str::DetTrackers, static_cast<unsigned>(t.trackers.size())), col_a, y_low,
                 palette::kTextDim);
    if (t.trackers.empty()) {
        render_.text(FontSize::Small, tr(Str::DetNoTrackers), col_a + 200, y_low,
                     palette::kTextDim);
        y_low += 28;
    } else {
        for (const std::string& url : t.trackers) {
            if (y_low + 24 > y + h - 46) break;  // plutôt tronquer que déborder
            render_.text_clipped(FontSize::Small, url, col_a + 200, y_low, w - 240,
                                 palette::kText);
            y_low += 24;
        }
    }

    render_.text(FontSize::Small, tr(Str::DetailsClose), col_a, y + h - 36, palette::kTextDim);
}

// Panneau modal de confirmation. Volontairement au centre et opaque : c'est le
// seul endroit de l'application où une touche détruit quelque chose.
void App::draw_confirm() {
    if (!confirm_.active) return;

    const int w = 800;
    const int h = 250;
    const int x = (Renderer::kWidth - w) / 2;
    const int y = (Renderer::kHeight - h) / 2;

    render_.rect(0, 0, Renderer::kWidth, Renderer::kHeight, Color{0x00, 0x00, 0x00, 0xb0});
    render_.rounded_rect(x, y, w, h, 16, palette::kSurface);

    render_.text_clipped(FontSize::Title, tr(Str::ConfirmRemoveTitle), x + 32, y + 26, w - 64,
                         palette::kText);
    render_.text_clipped(FontSize::Body, confirm_.name, x + 32, y + 78, w - 64, palette::kAccent);

    int ly = y + 130;
    auto choice = [&](const char* button, const char* text, const Color& color) {
        render_.text(FontSize::Body, button, x + 32, ly, color);
        render_.text(FontSize::Body, text, x + 90, ly, palette::kTextDim);
        ly += 34;
    };
    choice("A", tr(Str::ConfirmKeep), palette::kAccent);
    choice("Y", tr(Str::ConfirmDelete), palette::kError);
    choice("B", tr(Str::ConfirmCancel), palette::kTextDim);
}

void App::draw(uint64_t now_ms) {
    render_.begin_frame();
    render_.fill(palette::kBackground);

    draw_topbar();
    draw_tabs();

    switch (tab_) {
        case Tab::Torrents:
            draw_torrents();
            draw_hints({{"A", tr(Str::HintOpenTorrent)},
                        {"X", tr(Str::HintPasteMagnet)},
                        {"ZL", tr(Str::HintMagnetsFile)},
                        {"Y", tr(Str::HintPause)},
                        {"−", tr(Str::HintDetails)},
                        {"B", tr(Str::HintSkipCheck)},
                        {"ZR", tr(Str::HintRemove)},
                        {"L/R", tr(Str::HintTab)},
                        {"+", tr(Str::HintQuit)}});
            break;
        case Tab::Library:
            draw_library();
            draw_hints({{"A", tr(Str::HintInstall)},
                        {"Y", tr(Str::HintVerify)},
                        {"X", tr(Str::HintRescan)},
                        {"L/R", tr(Str::HintTab)},
                        {"+", tr(Str::HintQuit)}});
            break;
        case Tab::Remote:
            draw_remote();
            draw_hints({{"A", tr(settings_.phone_import ? Str::HintStop : Str::HintStart)},
                        {"Y", tr(Str::HintRestart)},
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
                        {"←→", tr(Str::HintChangeValue)},
                        {"Y", tr(Str::HintSelfTest)},
                        {"L/R", tr(Str::HintTab)},
                        {"+", tr(Str::HintQuit)}});
            break;
    }

    draw_install_overlay();
    draw_toast(now_ms);
    draw_confirm();
    draw_details();
    draw_browser();
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

        poll_install_job();
        poll_watch_folder(now);
        vpn_.update(session_);

        // statvfs interroge la carte : une fois toutes les cinq secondes suffit
        // largement pour un chiffre qui bouge à la vitesse d'un téléchargement.
        if (now - last_free_space_ms_ > 5000) {
            free_space_ = util::disk_free(kDownloadDir);
            last_free_space_ms_ = now;
        }

        // Ce qui arrive du téléphone doit se voir sur l'écran de la console :
        // sinon on ne sait pas si l'envoi est passé sans aller vérifier dans la
        // liste, et on renvoie le même lien trois fois.
        {
            std::string message;
            bool error = false;
            if (phone_.take_notice(message, error)) toast(message, error);
        }

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
