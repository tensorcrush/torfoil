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
// Liste dense : de quoi lire un nom, un état et un pourcentage, pas davantage.
// Une ligne de 96 pixels n'en montrait que quatre à la fois, ce qui obligeait à
// faire défiler pour répondre à « où en est ma bibliothèque ? ».
constexpr int kRowHeight = 62;
constexpr int kRowGap = 6;
constexpr int kIconSize = 34;

const char* kDownloadDir = "sdmc:/torfoil/downloads";
const char* kMagnetsFile = "sdmc:/torfoil/magnets.txt";
const char* kSettingsFile = "sdmc:/torfoil/settings.cfg";
// Là où le téléphone dépose ce qu'il envoie, et où l'on peut poser soi-même des
// fichiers .torrent depuis un PC.
const char* kInboxDir = "sdmc:/torfoil/inbox";

std::string tab_label(int index) {
    switch (index) {
        case 0: return tr(Str::TabTorrents);
        case 1: return tr(Str::TabDownloads);
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
        case bt::TorrentState::Queued: return Str::StateQueued;
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
        case bt::TorrentState::Paused:
        case bt::TorrentState::Queued: return palette::kTextDim;
        default: return palette::kWarn;
    }
}

// L'icône dit d'abord ce qui se passe, ensuite ce que c'est. Un torrent en
// pause ou terminé se reconnaît à sa pastille ; sinon on montre le type du plus
// gros fichier, qui est celui pour lequel on a lancé le téléchargement.
// IconSet parle SDL et ignore la palette : c'est ce qui lui permet d'être
// essayé seul, hors de l'application. La conversion tient en une ligne.
SDL_Color tint(Color c) {
    return SDL_Color{c.r, c.g, c.b, c.a};
}

// L'icône dit ce que c'est : le type du plus gros fichier, celui pour lequel on
// a lancé le téléchargement.
//
// L'état ne passe pas par elle. Il est déjà écrit en toutes lettres, coloré
// dans la barre d'avancement et chiffré en pourcentage ; le lui faire dire une
// quatrième fois coûterait la seule information qu'elle porte seule, et une
// bibliothèque terminée redeviendrait une colonne de coches identiques.
IconKind torrent_icon(const bt::TorrentStatus& t) {
    if (!t.primary_file.empty()) {
        const IconKind kind = icon_kind_for(t.primary_file);
        if (kind != IconKind::Unknown) return kind;
    }

    // Type encore inconnu — métadonnées absentes, ou extension qui ne dit rien.
    // L'état est alors ce qu'on a de mieux à montrer.
    switch (t.state) {
        case bt::TorrentState::Paused:
        case bt::TorrentState::Queued: return IconKind::Paused;
        case bt::TorrentState::Seeding:
        case bt::TorrentState::Completed: return IconKind::Done;
        case bt::TorrentState::Failed: return IconKind::Unknown;
        default: return IconKind::Downloading;
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

// Contenu immédiat d'un dossier, sans descendre. La descente est décidée par
// l'utilisateur, un dossier à la fois : parcourir la carte entière pour
// afficher un écran a longtemps été ce qui rendait la bibliothèque lente.
void list_dir(const std::string& dir, std::vector<LibraryEntry>& out) {
    DIR* handle = ::opendir(dir.c_str());
    if (!handle) return;

    while (dirent* entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        // Tout ce qui commence par un point est masqué, et ce n'est pas une
        // convention empruntée pour faire joli : les points de reprise
        // (.torfoil) et les liens mémorisés (.magnet) de Torfoil s'appellent
        // ainsi. Sans ce filtre, chaque torrent ajoutait deux « fichiers sans
        // torrent » à un compteur censé signaler exactement l'inverse.
        if (name.empty() || name[0] == '.') continue;

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
            if (looks_like_split_file(full, split_size)) {
                item.size = split_size;
            } else {
                item.is_dir = true;
            }
        } else {
            item.size = static_cast<uint64_t>(st.st_size);
        }

        out.push_back(std::move(item));
    }
    ::closedir(handle);

    // Dossiers d'abord, puis par nom : c'est l'ordre auquel tout explorateur a
    // habitué, et celui qui met le contenu du torrent avant ses miettes.
    std::sort(out.begin(), out.end(), [](const LibraryEntry& a, const LibraryEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return a.name < b.name;
    });
}

uint64_t dir_size(const std::string& dir, int depth) {
    if (depth > 6) return 0;

    DIR* handle = ::opendir(dir.c_str());
    if (!handle) return 0;

    uint64_t sum = 0;
    while (dirent* entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        const std::string full = dir + "/" + name;
        struct stat st{};
        if (::stat(full.c_str(), &st) != 0) continue;
        sum += S_ISDIR(st.st_mode) ? dir_size(full, depth + 1)
                                   : static_cast<uint64_t>(st.st_size);
    }
    ::closedir(handle);
    return sum;
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
    icons_.attach(render_.raw());

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad_);

    ::mkdir("sdmc:/torfoil", 0777);
    ::mkdir(kDownloadDir, 0777);
    ::mkdir(kInboxDir, 0777);

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
    // Ce qui a été déposé dans l'inbox depuis un PC part tout de suite : ouvrir
    // un menu pour réclamer un fichier qu'on vient de copier là exprès n'a
    // jamais aidé personne.
    import_inbox();
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
    // Les textures avant le rendu qui les a fabriquées : l'inverse les libère
    // au travers d'un renderer déjà détruit.
    icons_.clear();
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
        default: return 0;
    }
}

const bt::TorrentStatus* App::focused() const {
    if (!overlay_hash_.empty()) {
        for (const bt::TorrentStatus& t : torrents_) {
            if (t.hash_hex == overlay_hash_) return &t;
        }
        return nullptr;
    }
    const int index = selection_[static_cast<int>(Tab::Torrents)];
    if (index < 0 || index >= static_cast<int>(torrents_.size())) return nullptr;
    return &torrents_[static_cast<size_t>(index)];
}

// Le menu ne propose que ce qui a un sens à cet instant. Une entrée grisée
// oblige à essayer pour comprendre qu'elle est inerte ; une entrée absente ne
// pose pas la question.
void App::open_actions() {
    const bt::TorrentStatus* t = focused();
    if (!t) return;

    overlay_hash_ = t->hash_hex;
    actions_.clear();

    if (!t->content_root.empty()) actions_.push_back(Str::ActOpenLocation);
    actions_.push_back(Str::ActInfo);
    if (t->state == bt::TorrentState::Checking) actions_.push_back(Str::ActSkipCheck);
    actions_.push_back(t->state == bt::TorrentState::Paused ? Str::ActResume : Str::ActPause);
    actions_.push_back(Str::ActRemoveKeep);
    actions_.push_back(Str::ActRemoveDelete);

    action_cursor_ = 0;
    overlay_ = Overlay::Actions;
}

void App::open_files(const std::string& root, const std::string& title) {
    files_title_ = title;

    // Un torrent d'un seul fichier n'a pas de dossier à lui : sa « racine » EST
    // le fichier. Lister un fichier ne donne rien, et l'écran s'ouvrait vide sur
    // exactement le cas le plus courant. On montre alors le dossier qui le
    // contient, curseur posé dessus.
    struct stat st{};
    const bool is_dir = ::stat(root.c_str(), &st) == 0 && S_ISDIR(st.st_mode);

    std::string focus;
    if (!is_dir) {
        const size_t cut = root.rfind('/');
        if (cut == std::string::npos) return;
        focus = root.substr(cut + 1);
        files_root_ = root.substr(0, cut);
    } else {
        files_root_ = root;
    }

    enter_dir(files_root_);

    for (size_t i = 0; i < files_.size(); ++i) {
        if (files_[i].name == focus) {
            files_cursor_ = static_cast<int>(i);
            break;
        }
    }

    overlay_ = Overlay::Files;
}

void App::enter_dir(std::string dir) {
    files_dir_ = std::move(dir);
    files_.clear();
    list_dir(files_dir_, files_);
    files_cursor_ = 0;
    files_scroll_ = 0;
}

void App::run_action() {
    const bt::TorrentStatus* t = focused();
    if (!t || actions_.empty()) {
        overlay_ = Overlay::None;
        return;
    }

    // Recopié : les commandes qui suivent peuvent faire disparaître le torrent
    // du prochain instantané, et avec lui le pointeur.
    const std::string hash = t->hash_hex;
    const std::string root = t->content_root;
    const std::string name = t->name;
    const bool paused = t->state == bt::TorrentState::Paused;

    switch (actions_[static_cast<size_t>(action_cursor_)]) {
        case Str::ActOpenLocation:
            open_files(root, name);
            return;

        case Str::ActInfo:
            overlay_ = Overlay::Info;
            return;

        case Str::ActSkipCheck:
            session_.skip_check(hash);
            toast(tr(Str::ToastCheckSkipped));
            break;

        case Str::ActPause:
        case Str::ActResume:
            if (paused) {
                session_.resume(hash);
                toast(tr(Str::ToastResumed));
            } else {
                session_.pause(hash);
                toast(tr(Str::ToastPaused));
            }
            break;

        case Str::ActRemoveKeep:
            session_.remove(hash, /*delete_files=*/false);
            toast(tr(Str::ToastRemovedKept));
            break;

        case Str::ActRemoveDelete:
            // Effacer des fichiers est le seul geste d'ici qui ne se rattrape
            // pas : on demande, une fois, en nommant ce qui va disparaître.
            overlay_ = Overlay::Confirm;
            return;

        default:
            break;
    }

    overlay_ = Overlay::None;
    overlay_hash_.clear();
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
    session_.set_max_active(settings_.max_active);
    settings_.save(kSettingsFile);
}

// Compte ce que plus aucun torrent ne revendique.
//
// C'est la seule chose que la liste de torrents ne peut pas dire d'elle-même :
// retirer un torrent en gardant ses fichiers laisse parfois des dizaines de
// gigaoctets dont plus rien, dans l'application, ne parle. Une ligne discrète
// en pied d'écran vaut mieux qu'un onglet entier, mais l'absence totale de
// mention vaut la carte pleine sans explication.
void App::refresh_library() {
    std::vector<LibraryEntry> top;
    list_dir(kDownloadDir, top);

    orphan_count_ = 0;
    orphan_bytes_ = 0;

    // Un torrent dont on ignore encore le nom ne revendique rien : tout ce qu'il
    // a déjà écrit passerait pour orphelin. Le temps que les métadonnées
    // arrivent, on se tait plutôt que d'annoncer des gigaoctets à la dérive qui
    // appartiennent en réalité au téléchargement en cours.
    for (const bt::TorrentStatus& t : torrents_) {
        if (t.content_root.empty()) return;
    }

    for (const LibraryEntry& item : top) {
        bool claimed = false;
        for (const bt::TorrentStatus& t : torrents_) {
            if (!t.content_root.empty() && t.content_root == item.path) {
                claimed = true;
                break;
            }
        }
        if (claimed) continue;

        ++orphan_count_;
        orphan_bytes_ += item.is_dir ? dir_size(item.path, 0) : item.size;
    }

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
// Les fichiers .torrent posés dans l'inbox, avalés puis retirés. C'est le même
// dossier que celui où le téléphone dépose ses envois : un fichier copié depuis
// un PC et un fichier reçu par Wi-Fi n'ont aucune raison de suivre deux chemins
// différents.
int App::import_inbox() {
    std::vector<LibraryEntry> entries;
    list_dir(kInboxDir, entries);

    int added = 0;
    for (const LibraryEntry& item : entries) {
        if (item.is_dir || item.kind != "TORRENT") continue;
        std::string err;
        if (session_.add_torrent_file(item.path, &err)) {
            ++added;
            std::remove(item.path.c_str());
        }
    }
    return added;
}

void App::import_magnets_file() {
    const int from_inbox = import_inbox();

    std::FILE* fp = std::fopen(kMagnetsFile, "rb");
    if (!fp) {
        if (from_inbox > 0) {
            toast(std::to_string(from_inbox) +
                  (from_inbox > 1 ? " torrents ajoutés" : " torrent ajouté"));
        } else {
            toast(tr(Str::ToastNoMagnetsFile), true);
        }
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

    int added = from_inbox;
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

    // Une incrustation prend tout : tant qu'elle est ouverte, les gâchettes ne
    // changent pas d'onglet et le curseur de la liste ne bouge pas sous elle.
    if (overlay_ != Overlay::None) {
        const bool next = (down & (HidNpadButton_Down | HidNpadButton_StickLDown)) != 0;
        const bool prev = (down & (HidNpadButton_Up | HidNpadButton_StickLUp)) != 0;

        switch (overlay_) {
            case Overlay::Actions: {
                const int count = static_cast<int>(actions_.size());
                if (count > 0 && next) action_cursor_ = (action_cursor_ + 1) % count;
                if (count > 0 && prev) action_cursor_ = (action_cursor_ + count - 1) % count;
                if (down & HidNpadButton_A) run_action();
                if (down & HidNpadButton_B) {
                    overlay_ = Overlay::None;
                    overlay_hash_.clear();
                }
                break;
            }

            case Overlay::Info:
                if (down & (HidNpadButton_B | HidNpadButton_A)) {
                    overlay_ = Overlay::None;
                    overlay_hash_.clear();
                }
                break;

            case Overlay::Files: {
                const bool has_up = files_dir_ != files_root_;
                const int count = static_cast<int>(files_.size()) + (has_up ? 1 : 0);
                if (count > 0 && next) files_cursor_ = (files_cursor_ + 1) % count;
                if (count > 0 && prev) files_cursor_ = (files_cursor_ + count - 1) % count;

                if (down & HidNpadButton_A) {
                    if (has_up && files_cursor_ == 0) {
                        const size_t cut = files_dir_.rfind('/');
                        if (cut != std::string::npos) enter_dir(files_dir_.substr(0, cut));
                    } else {
                        const int index = files_cursor_ - (has_up ? 1 : 0);
                        if (index >= 0 && index < static_cast<int>(files_.size()) &&
                            files_[static_cast<size_t>(index)].is_dir) {
                            enter_dir(files_[static_cast<size_t>(index)].path);
                        }
                    }
                }
                if (down & HidNpadButton_B) {
                    // B remonte tant qu'il y a où remonter, et ne referme qu'à
                    // la racine : sortir d'un coup depuis trois niveaux plus bas
                    // oblige à tout reparcourir pour revenir.
                    if (has_up) {
                        const size_t cut = files_dir_.rfind('/');
                        if (cut != std::string::npos) enter_dir(files_dir_.substr(0, cut));
                    } else {
                        overlay_ = Overlay::None;
                        overlay_hash_.clear();
                    }
                }
                break;
            }

            case Overlay::Confirm:
                if (down & HidNpadButton_A) {
                    if (const bt::TorrentStatus* t = focused()) {
                        session_.remove(t->hash_hex, /*delete_files=*/true);
                        toast(tr(Str::ToastRemovedDeleted));
                    }
                    overlay_ = Overlay::None;
                    overlay_hash_.clear();
                }
                if (down & HidNpadButton_B) overlay_ = Overlay::Actions;
                break;

            default:
                break;
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
            // Tout ce qui agit sur un torrent passe par le menu : la liste ne
            // sert qu'à montrer, et aucune touche ne supprime quoi que ce soit
            // au premier appui.
            if ((down & HidNpadButton_A) && !torrents_.empty()) open_actions();
            if ((down & HidNpadButton_ZR) && orphan_count_ > 0) {
                refresh_library();
                open_files(kDownloadDir, tr(Str::OrphanTitle));
            }
            break;

        case Tab::Downloads:
            if (down & HidNpadButton_X) add_magnet_flow();
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
            // Ligne 0 : la langue, ligne 1 : le nombre de téléchargements
            // simultanés. Les bascules suivent, décalées de deux crans.
            const int count = static_cast<int>(toggles().size()) + 2;
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
            } else if (settings_cursor_ == 1) {
                // 0 à 8, et 0 signifie « sans limite ». Le tour complet est
                // court, ←→ et A font donc la même chose sans gêner.
                int step = 0;
                if (down & (HidNpadButton_A | HidNpadButton_Right)) step = 1;
                if (down & HidNpadButton_Left) step = 8;
                if (step != 0) {
                    settings_.max_active = (settings_.max_active + step) % 9;
                    apply_settings();
                }
            } else if (down & HidNpadButton_A) {
                const Toggle& t = toggles()[static_cast<size_t>(settings_cursor_ - 2)];
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

// Une seule liste, et elle ne montre que des torrents.
//
// Les fichiers ont disparu d'ici volontairement : mélangés aux torrents, ils
// doublaient chaque entrée et donnaient à croire qu'il y avait deux fois plus
// de choses qu'en réalité. Ce que la carte contient se regarde depuis le
// torrent qui l'a écrit, par le menu d'actions.
void App::draw_torrents() {
    const bool has_orphans = orphan_count_ > 0;
    const int footer = has_orphans ? 30 : 0;

    if (torrents_.empty()) {
        draw_empty(tr(Str::NoTorrents),
                   std::string(tr(Str::HowMagnet)) + "  ·  " + tr(Str::HowMagnetsFile));
    } else {
        const int room = kContentBottom - kContentTop - 20 - footer;
        const int visible = room / (kRowHeight + kRowGap);
        if (selection() < scroll_) scroll_ = selection();
        if (selection() >= scroll_ + visible) scroll_ = selection() - visible + 1;

        const int row_w = Renderer::kWidth - 2 * kMargin;
        int y = kContentTop + 12;

        for (int i = scroll_; i < static_cast<int>(torrents_.size()) && i < scroll_ + visible;
             ++i) {
            const bt::TorrentStatus& t = torrents_[static_cast<size_t>(i)];
            const bool active = i == selection();
            const Color accent = state_color(t.state);

            render_.rounded_rect(kMargin, y, row_w, kRowHeight, 10,
                                 active ? palette::kSelected : palette::kSurface);

            icons_.draw(torrent_icon(t), kMargin + 14, y + (kRowHeight - kIconSize) / 2,
                        kIconSize, tint(active ? accent : palette::kTextDim));

            // Réservé à droite : le pourcentage, qui est la seule colonne que
            // l'œil balaie verticalement. Le nom s'arrête avant lui plutôt que
            // de passer dessous.
            const int pct_w = 96;
            const int text_x = kMargin + 14 + kIconSize + 16;
            const int text_w = row_w - (text_x - kMargin) - pct_w - 20;

            render_.text_clipped(FontSize::Body, t.name, text_x, y + 8, text_w, palette::kText);

            char detail[256];
            if (t.state == bt::TorrentState::Downloading) {
                std::snprintf(detail, sizeof(detail), "%s · %s · ↓ %s · %u %s",
                              tr(state_key(t.state)),
                              t.total_size ? util::human_size(t.total_size).c_str() : "?",
                              util::human_rate(t.rate_down).c_str(), t.peers_connected,
                              tr(Str::InfoPeers));
            } else {
                std::snprintf(detail, sizeof(detail), "%s · %s", tr(state_key(t.state)),
                              t.total_size ? util::human_size(t.total_size).c_str() : "?");
            }
            render_.text_clipped(FontSize::Small, detail, text_x, y + 34, text_w,
                                 palette::kTextDim);

            char pct[16];
            std::snprintf(pct, sizeof(pct), "%.0f %%", t.progress * 100.0f);
            const int right = kMargin + row_w - 18;
            render_.text(FontSize::Body, pct, right - render_.text_width(FontSize::Body, pct),
                         y + 8, accent);
            render_.progress_bar(right - pct_w + 8, y + 40, pct_w - 8, 6, t.progress, accent);

            y += kRowHeight + kRowGap;
        }
    }

    // Rien sous une incrustation : le pied d'écran affleurait sous le coin
    // arrondi du panneau, à moitié lisible.
    if (has_orphans && overlay_ == Overlay::None) {
        // Discret, et seulement quand il y a lieu. Sans cette ligne, retirer un
        // torrent « en gardant les fichiers » fait disparaître toute mention de
        // vingt gigaoctets qui restent bel et bien sur la carte.
        render_.text_clipped(FontSize::Small,
                             trf(Str::OrphanFooter, orphan_count_,
                                 util::human_size(orphan_bytes_).c_str()),
                             kMargin, kContentBottom - 26, Renderer::kWidth - 2 * kMargin,
                             palette::kTextDim);
    }
}

// L'onglet qui répond à « ça avance ? ». Le torrent en cours occupe le haut,
// en grand ; ceux qui attendent leur tour sont listés dessous, dans l'ordre où
// ils partiront.
void App::draw_downloads() {
    std::vector<const bt::TorrentStatus*> active;
    std::vector<const bt::TorrentStatus*> queued;
    for (const bt::TorrentStatus& t : torrents_) {
        if (t.state == bt::TorrentState::Downloading ||
            t.state == bt::TorrentState::FetchingMetadata ||
            t.state == bt::TorrentState::Checking) {
            active.push_back(&t);
        } else if (t.state == bt::TorrentState::Queued) {
            queued.push_back(&t);
        }
    }

    if (active.empty() && queued.empty()) {
        draw_empty(tr(Str::DlNothing), tr(Str::DlNothingHint));
        return;
    }

    const int width = Renderer::kWidth - 2 * kMargin;
    int y = kContentTop + 10;

    render_.section_header(tr(Str::DlActive), kMargin, y, palette::kAccent);
    y += 30;

    if (active.empty()) {
        render_.text(FontSize::Small, tr(Str::DlNothing), kMargin + 4, y, palette::kTextDim);
        y += 30;
    }

    // Deux cartes au plus : au-delà, la mise en avant ne veut plus rien dire et
    // la file en dessous devient illisible.
    for (size_t i = 0; i < active.size() && i < 2; ++i) {
        const bt::TorrentStatus& t = *active[i];
        constexpr int card_h = 118;
        render_.rounded_rect(kMargin, y, width, card_h, 12, palette::kSurface);

        icons_.draw(torrent_icon(t), kMargin + 18, y + 16, 44, tint(palette::kAccent));

        const int text_x = kMargin + 18 + 44 + 18;
        render_.text_clipped(FontSize::Body, t.name, text_x, y + 14, width - (text_x - kMargin) -
                                                                          160,
                             palette::kText);

        char pct[16];
        std::snprintf(pct, sizeof(pct), "%.0f %%", t.progress * 100.0f);
        render_.text(FontSize::Title, pct,
                     kMargin + width - 18 - render_.text_width(FontSize::Title, pct), y + 12,
                     palette::kAccent);

        render_.progress_bar(text_x, y + 52, width - (text_x - kMargin) - 18, 10, t.progress,
                             state_color(t.state));

        char line[320];
        std::snprintf(line, sizeof(line), "%s · ↓ %s · ↑ %s · %u %s · %u %s",
                      tr(state_key(t.state)), util::human_rate(t.rate_down).c_str(),
                      util::human_rate(t.rate_up).c_str(), t.peers_connected,
                      tr(Str::InfoPeers), t.blocks_in_flight, tr(Str::InfoBlocksInFlight));
        render_.text_clipped(FontSize::Small, line, text_x, y + 72, width - (text_x - kMargin) - 18,
                             palette::kTextDim);

        std::string tail = t.total_size ? util::human_size(t.downloaded) + " / " +
                                              util::human_size(t.total_size)
                                        : std::string();
        if (t.eta_s > 0 && t.state == bt::TorrentState::Downloading) {
            tail += "   ·   " + std::string(tr(Str::InfoEta)) + " " +
                    util::human_duration(t.eta_s);
        }
        render_.text_clipped(FontSize::Small, tail, text_x, y + 94,
                             width - (text_x - kMargin) - 18, palette::kTextDim);

        y += card_h + 10;
    }

    y += 6;
    render_.section_header(tr(Str::DlQueue), kMargin, y, palette::kTextDim);
    y += 30;

    if (queued.empty()) {
        render_.text(FontSize::Small, tr(Str::DlQueueEmpty), kMargin + 4, y, palette::kTextDim);
        return;
    }

    constexpr int q_h = 44;
    for (size_t i = 0; i < queued.size(); ++i) {
        if (y + q_h > kContentBottom - 4) break;
        const bt::TorrentStatus& t = *queued[i];

        render_.rounded_rect(kMargin, y, width, q_h - 4, 8, palette::kSurface);

        // Le rang plutôt qu'une icône : la question posée à cet écran est
        // « dans combien de temps mon tour ? », pas « quel type de fichier ».
        const std::string rank = std::to_string(i + 1);
        render_.text(FontSize::Small, rank, kMargin + 16, y + 10, palette::kAccent);
        render_.text_clipped(FontSize::Body, t.name, kMargin + 46, y + 6, width - 46 - 140,
                             palette::kTextDim);

        const std::string size = t.total_size ? util::human_size(t.total_size) : std::string("?");
        render_.text(FontSize::Small, size,
                     kMargin + width - 18 - render_.text_width(FontSize::Small, size), y + 10,
                     palette::kTextDim);
        y += q_h;
    }
}

// ---------------------------------------------------------------------------
// Incrustations
// ---------------------------------------------------------------------------

namespace {

// Voile sombre : ce qui est derrière reste visible mais cesse d'être lisible,
// donc plus personne n'essaie d'y agir.
void dim_background(Renderer& r) {
    r.rect(0, 0, Renderer::kWidth, Renderer::kHeight, Color{0, 0, 0, 170});
}

}  // namespace

void App::draw_actions() {
    dim_background(render_);

    const bt::TorrentStatus* t = focused();
    if (!t) return;

    const int w = 620;
    const int row_h = 52;
    const int h = 96 + static_cast<int>(actions_.size()) * row_h + 16;
    const int x = (Renderer::kWidth - w) / 2;
    const int y = (Renderer::kHeight - h) / 2;

    render_.rounded_rect(x, y, w, h, 16, palette::kSurfaceAlt);

    icons_.draw(torrent_icon(*t), x + 24, y + 20, 32, tint(palette::kAccent));
    render_.text_clipped(FontSize::Body, t->name, x + 24 + 32 + 16, y + 22, w - 96,
                         palette::kText);
    render_.line(x + 20, y + 68, x + w - 20, y + 68, palette::kSurface);

    int ry = y + 84;
    for (size_t i = 0; i < actions_.size(); ++i) {
        const bool focus = static_cast<int>(i) == action_cursor_;
        const bool destructive = actions_[i] == Str::ActRemoveDelete;

        if (focus) render_.rounded_rect(x + 16, ry, w - 32, row_h - 6, 8, palette::kSelected);
        render_.text(FontSize::Body, tr(actions_[i]), x + 34, ry + 8,
                     destructive ? palette::kError : (focus ? palette::kText : palette::kTextDim));
        ry += row_h;
    }
}

void App::draw_info() {
    dim_background(render_);

    const bt::TorrentStatus* t = focused();
    if (!t) return;

    const int w = 900;
    const int h = 500;
    const int x = (Renderer::kWidth - w) / 2;
    const int y = (Renderer::kHeight - h) / 2;

    render_.rounded_rect(x, y, w, h, 16, palette::kSurfaceAlt);

    icons_.draw(torrent_icon(*t), x + 24, y + 20, 32, tint(palette::kAccent));
    render_.text_clipped(FontSize::Body, t->name, x + 72, y + 22, w - 96, palette::kText);
    render_.line(x + 20, y + 66, x + w - 20, y + 66, palette::kSurface);

    const int key_x = x + 28;
    const int val_x = x + 260;
    const int val_w = w - (val_x - x) - 28;
    int ry = y + 82;

    auto row = [&](const char* key, const std::string& value, Color color = palette::kText) {
        if (ry + 26 > y + h - 12) return;
        render_.text(FontSize::Small, key, key_x, ry, palette::kTextDim);
        render_.text_clipped(FontSize::Small, value, val_x, ry, val_w, color);
        ry += 26;
    };

    char buf[256];

    row(tr(Str::InfoState), tr(state_key(t->state)), state_color(t->state));

    std::snprintf(buf, sizeof(buf), "%.1f %%  (%s / %s)", t->progress * 100.0f,
                  util::human_size(t->downloaded).c_str(),
                  util::human_size(t->total_size).c_str());
    row(tr(Str::InfoProgress), buf);

    row(tr(Str::InfoSize), t->total_size ? util::human_size(t->total_size) : std::string("?"));

    if (t->pieces_total > 0) {
        std::snprintf(buf, sizeof(buf), "%u / %u", t->pieces_done, t->pieces_total);
        row(tr(Str::InfoPieces), buf);
        row(tr(Str::InfoPieceLen), util::human_size(t->piece_length));
    }
    if (t->file_count > 0) row(tr(Str::InfoFiles), std::to_string(t->file_count));

    // Une pièce rejetée n'est pas anodine : c'est du volume téléchargé deux
    // fois. Zéro se tait, le reste se dit.
    if (t->pieces_rejected > 0) {
        row(tr(Str::InfoRejected), std::to_string(t->pieces_rejected), palette::kWarn);
    }

    // Le nombre de pairs qui nous étranglent est LA donnée qui explique un
    // débit décevant : un pair connecté qui refuse de servir ne sert à rien.
    if (t->peers_choking > 0) {
        std::snprintf(buf, sizeof(buf), "%u / %u  (%u ↛)", t->peers_connected, t->peers_known,
                      t->peers_choking);
    } else {
        std::snprintf(buf, sizeof(buf), "%u / %u", t->peers_connected, t->peers_known);
    }
    row(tr(Str::InfoPeers), buf);

    std::snprintf(buf, sizeof(buf), "↓ %s   ↑ %s", util::human_rate(t->rate_down).c_str(),
                  util::human_rate(t->rate_up).c_str());
    row(tr(Str::InfoRates), buf);

    if (t->blocks_in_flight > 0) {
        row(tr(Str::InfoBlocksInFlight), std::to_string(t->blocks_in_flight));
    }
    if (t->eta_s > 0) row(tr(Str::InfoEta), util::human_duration(t->eta_s));

    row(tr(Str::InfoPrivate), tr(t->is_private ? Str::InfoYes : Str::InfoNo));

    if (!t->trackers.empty()) {
        row(tr(Str::InfoTrackers), t->trackers.front());
        for (size_t i = 1; i < t->trackers.size() && i < 3; ++i) row("", t->trackers[i]);
    }

    row(tr(Str::InfoLocation), t->content_root.empty() ? t->save_path : t->content_root);
    row(tr(Str::InfoHash), t->hash_hex);

    if (!t->message.empty()) row("", t->message, palette::kWarn);
}

void App::draw_files() {
    dim_background(render_);

    const int w = Renderer::kWidth - 2 * kMargin;
    const int h = Renderer::kHeight - 120;
    const int x = kMargin;
    const int y = 60;

    render_.rounded_rect(x, y, w, h, 16, palette::kSurfaceAlt);
    render_.text_clipped(FontSize::Body, files_title_, x + 24, y + 18, w - 48, palette::kText);
    render_.text_clipped(FontSize::Small, files_dir_, x + 24, y + 48, w - 48, palette::kTextDim);
    render_.line(x + 20, y + 76, x + w - 20, y + 76, palette::kSurface);

    const bool has_up = files_dir_ != files_root_;
    const int count = static_cast<int>(files_.size()) + (has_up ? 1 : 0);
    if (count == 0) {
        render_.text_centered(FontSize::Body, tr(Str::FilesEmpty), Renderer::kWidth / 2,
                              y + h / 2, palette::kTextDim);
        return;
    }

    constexpr int row_h = 48;
    const int list_top = y + 88;
    const int visible = (y + h - 12 - list_top) / row_h;
    if (files_cursor_ < files_scroll_) files_scroll_ = files_cursor_;
    if (files_cursor_ >= files_scroll_ + visible) files_scroll_ = files_cursor_ - visible + 1;

    int ry = list_top;
    for (int i = files_scroll_; i < count && i < files_scroll_ + visible; ++i) {
        const bool focus = i == files_cursor_;
        if (focus) render_.rounded_rect(x + 16, ry, w - 32, row_h - 4, 8, palette::kSelected);

        if (has_up && i == 0) {
            icons_.draw(IconKind::Folder, x + 28, ry + 7, 28, tint(palette::kTextDim));
            render_.text(FontSize::Body, tr(Str::FilesUp), x + 70, ry + 6, palette::kTextDim);
            ry += row_h;
            continue;
        }

        const LibraryEntry& item = files_[static_cast<size_t>(i - (has_up ? 1 : 0))];
        icons_.draw(item.is_dir ? IconKind::Folder : icon_kind_for(item.name), x + 28, ry + 7, 28,
                    tint(focus ? palette::kAccent : palette::kTextDim));
        render_.text_clipped(FontSize::Body, item.name, x + 70, ry + 6, w - 70 - 180,
                             focus ? palette::kText : palette::kTextDim);

        if (!item.is_dir) {
            const std::string size = util::human_size(item.size);
            render_.text(FontSize::Small, size,
                         x + w - 24 - render_.text_width(FontSize::Small, size), ry + 10,
                         palette::kTextDim);
        }
        ry += row_h;
    }
}

void App::draw_confirm() {
    dim_background(render_);

    const bt::TorrentStatus* t = focused();
    const int w = 760;
    const int h = 230;
    const int x = (Renderer::kWidth - w) / 2;
    const int y = (Renderer::kHeight - h) / 2;

    render_.rounded_rect(x, y, w, h, 16, palette::kSurfaceAlt);
    render_.text_centered(FontSize::Title, tr(Str::ConfirmDeleteTitle), Renderer::kWidth / 2,
                          y + 30, palette::kError);
    render_.text_clipped(FontSize::Body,
                         trf(Str::ConfirmDeleteBody, t ? t->name.c_str() : ""), x + 30, y + 96,
                         w - 60, palette::kText);
    render_.text_centered(FontSize::Small,
                          std::string("A · ") + tr(Str::HintConfirm) + "        B · " +
                              tr(Str::HintCancel),
                          Renderer::kWidth / 2, y + h - 46, palette::kTextDim);
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

    // Le nombre de téléchargements simultanés juste après : c'est le réglage
    // qui explique pourquoi un torrent ajouté reste « en attente ».
    {
        const bool focused = settings_cursor_ == 1;
        render_.rounded_rect(left, y, left_w, row_h - 8, 10,
                             focused ? palette::kSelected : palette::kSurface);
        render_.text(FontSize::Body, tr(Str::FieldMaxActive), left + 18, y + 6, palette::kText);
        render_.text_clipped(FontSize::Small, tr(Str::MaxActiveEffect), left + 18, y + 32,
                             left_w - 36 - 90, palette::kTextDim);
        const std::string value = settings_.max_active == 0
                                      ? std::string(tr(Str::MaxActiveUnlimited))
                                      : std::to_string(settings_.max_active);
        render_.text(FontSize::Body, value,
                     left + left_w - 18 - render_.text_width(FontSize::Body, value), y + 16,
                     focused ? palette::kAccent : palette::kText);
        y += row_h;
    }

    for (size_t i = 0; i < list.size(); ++i) {
        const Toggle& t = list[i];
        const bool raw = settings_.*(t.field);
        const bool checked = t.inverted ? !raw : raw;
        const bool focused = static_cast<int>(i) + 2 == settings_cursor_;

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
    info(tr(Str::FieldAccessPoint),
         phone_.step() == Phone::Step::Off ? tr(Str::AccessPointOff) : phone_.ap().ssid());

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
        case Tab::Torrents: {
            draw_torrents();
            std::vector<std::pair<std::string, std::string>> hints = {
                {"A", tr(Str::HintActions)},
                {"X", tr(Str::HintPasteMagnet)},
                {"ZL", tr(Str::HintMagnetsFile)}};
            if (orphan_count_ > 0) hints.push_back({"ZR", tr(Str::HintOrphans)});
            hints.push_back({"L/R", tr(Str::HintTab)});
            hints.push_back({"+", tr(Str::HintQuit)});
            draw_hints(hints);
            break;
        }
        case Tab::Downloads:
            draw_downloads();
            draw_hints({{"X", tr(Str::HintPasteMagnet)},
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

    // Les incrustations passent après les onglets et avant le message éphémère :
    // un toast déclenché depuis le menu doit rester lisible.
    switch (overlay_) {
        case Overlay::Actions:
            draw_actions();
            draw_hints({{"A", tr(Str::HintChoose)}, {"B", tr(Str::HintBack)}});
            break;
        case Overlay::Info:
            draw_info();
            draw_hints({{"B", tr(Str::HintBack)}});
            break;
        case Overlay::Files:
            draw_files();
            draw_hints({{"A", tr(Str::HintOpen)}, {"B", tr(Str::HintBack)}});
            break;
        case Overlay::Confirm:
            draw_confirm();
            break;
        default:
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
