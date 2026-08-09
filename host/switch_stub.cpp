// Le faux Horizon : ce que <switch.h> promet, tenu par SDL et la libc.
//
// Deux modes de fonctionnement, choisis par l'environnement :
//
//   interactif  — une fenêtre SDL, le clavier tient lieu de manette.
//   scripté     — TORFOIL_SCRIPT donne la suite d'actions à jouer, aucune
//                 fenêtre n'est nécessaire (SDL_VIDEODRIVER=dummy), et « shot »
//                 enregistre l'écran en BMP.
//
// Le second est celui qui compte. Il rend l'application vérifiable sans
// personne devant : on décrit un parcours, on lit les images produites. C'est
// ainsi qu'on peut prouver qu'un code QR affiché est vraiment lisible, en le
// décodant depuis le fichier.
#include <switch.h>

#include <SDL2/SDL.h>

#include <sys/stat.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <vector>

namespace {

// --- correspondance clavier → manette ---
//
// Les touches suivent la disposition physique de la console autant qu'un
// clavier le permet : la rangée de droite pour ABXY, les épaules à gauche.
struct KeyMap {
    SDL_Scancode key;
    u64 button;
    const char* name;
};

const KeyMap kKeys[] = {
    {SDL_SCANCODE_L, HidNpadButton_A, "A"},        {SDL_SCANCODE_K, HidNpadButton_B, "B"},
    {SDL_SCANCODE_J, HidNpadButton_X, "X"},        {SDL_SCANCODE_I, HidNpadButton_Y, "Y"},
    {SDL_SCANCODE_Q, HidNpadButton_L, "L"},        {SDL_SCANCODE_E, HidNpadButton_R, "R"},
    {SDL_SCANCODE_A, HidNpadButton_ZL, "ZL"},      {SDL_SCANCODE_D, HidNpadButton_ZR, "ZR"},
    {SDL_SCANCODE_RETURN, HidNpadButton_Plus, "+"},
    {SDL_SCANCODE_BACKSPACE, HidNpadButton_Minus, "-"},
    {SDL_SCANCODE_UP, HidNpadButton_Up, "Haut"},   {SDL_SCANCODE_DOWN, HidNpadButton_Down, "Bas"},
    {SDL_SCANCODE_LEFT, HidNpadButton_Left, "Gauche"},
    {SDL_SCANCODE_RIGHT, HidNpadButton_Right, "Droite"},
};

// --- script ---
struct Action {
    std::string verb;  // nom de bouton, « shot », « wait », « quit »
    long value = 0;
};

std::vector<Action> g_script;
size_t g_script_pos = 0;
uint64_t g_script_ready_at = 0;
int g_shot_index = 0;
bool g_scripted = false;
bool g_running = true;
u64 g_pending_down = 0;

std::string g_shot_dir = "host-shots";
std::string g_typed;  // réponse préparée pour le clavier virtuel

u64 button_by_name(const std::string& name) {
    for (const KeyMap& k : kKeys) {
        if (name == k.name) return k.button;
    }
    return 0;
}

void parse_script(const std::string& text) {
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(',', pos);
        if (end == std::string::npos) end = text.size();
        std::string item = text.substr(pos, end - pos);
        pos = end + 1;

        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (item.empty()) continue;

        Action action;
        const size_t space = item.find(' ');
        if (space == std::string::npos) {
            action.verb = item;
        } else {
            action.verb = item.substr(0, space);
            action.value = std::strtol(item.c_str() + space + 1, nullptr, 10);
        }
        g_script.push_back(action);
    }
}

}  // namespace

namespace {

// La capture d'écran a besoin du moteur de rendu, qui appartient à
// ui::Renderer. Plutôt que d'ajouter un accesseur à une classe qui n'en a pas
// besoin sur console — et un #ifdef avec lui — on le retrouve par SDL : il n'y
// a qu'une fenêtre, et SDL sait rendre le moteur associé à une fenêtre.
SDL_Renderer* find_renderer() {
    for (Uint32 id = 1; id < 8; ++id) {
        if (SDL_Window* window = SDL_GetWindowFromID(id)) {
            if (SDL_Renderer* renderer = SDL_GetRenderer(window)) return renderer;
        }
    }
    return nullptr;
}

// « sdmc:/… » est la racine de la carte SD sur console. Sur PC c'est un dossier
// comme un autre, créé dans le répertoire courant : les chemins du programme
// restent identiques, et ce qu'il écrit reste sous les yeux.
struct SdRoot {
    SdRoot() {
        ::mkdir("sdmc:", 0777);
        ::mkdir("sdmc:/torfoil", 0777);
    }
} g_sd_root;

}  // namespace

void padConfigureInput(u32, u32) {}

void padInitializeDefault(PadState* pad) {
    pad->down = 0;
    pad->held = 0;

    if (const char* script = std::getenv("TORFOIL_SCRIPT")) {
        parse_script(script);
        g_scripted = true;
        if (const char* dir = std::getenv("TORFOIL_SHOTS")) g_shot_dir = dir;
        ::mkdir(g_shot_dir.c_str(), 0777);
        std::cout << "script : " << g_script.size() << " actions, captures dans " << g_shot_dir
                  << "\n";
    } else {
        std::cout << "Touches — A:L  B:K  X:J  Y:I   L:Q  R:E  ZL:A  ZR:D   +:Entrée  -:Retour"
                  << "\n         flèches : navigation. Fermer la fenêtre pour quitter.\n";
    }
    if (const char* typed = std::getenv("TORFOIL_INPUT")) g_typed = typed;
}

void padUpdate(PadState* pad) {
    pad->down = 0;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) g_running = false;
        if (event.type == SDL_KEYDOWN && !event.key.repeat) {
            for (const KeyMap& k : kKeys) {
                if (event.key.keysym.scancode == k.key) {
                    pad->down |= k.button;
                    pad->held |= k.button;
                }
            }
        }
        if (event.type == SDL_KEYUP) {
            for (const KeyMap& k : kKeys) {
                if (event.key.keysym.scancode == k.key) pad->held &= ~k.button;
            }
        }
    }

    if (!g_scripted) return;

    // Une action par image au plus : deux pressions dans la même image seraient
    // vues comme une seule par l'application, exactement comme sur console.
    if (g_pending_down != 0) {
        pad->down |= g_pending_down;
        g_pending_down = 0;
        return;
    }

    const uint64_t now = SDL_GetTicks64();
    if (now < g_script_ready_at) return;

    if (g_script_pos >= g_script.size()) {
        g_running = false;
        return;
    }

    const Action& action = g_script[g_script_pos++];
    if (action.verb == "wait") {
        g_script_ready_at = now + static_cast<uint64_t>(action.value);
        std::cout << "  attente " << action.value << " ms\n";
    } else if (action.verb == "shot") {
        char name[256];
        std::snprintf(name, sizeof(name), "%s/%02d.bmp", g_shot_dir.c_str(), g_shot_index++);
        if (SDL_Renderer* renderer = find_renderer()) {
            SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, 1280, 720, 32,
                                                               SDL_PIXELFORMAT_ARGB8888);
            if (shot) {
                SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, shot->pixels,
                                     shot->pitch);
                SDL_SaveBMP(shot, name);
                SDL_FreeSurface(shot);
                std::cout << "  capture " << name << "\n";
            }
        }
    } else if (action.verb == "quit") {
        g_running = false;
    } else {
        const u64 button = button_by_name(action.verb);
        if (button == 0) {
            std::cout << "  action inconnue : " << action.verb << "\n";
        } else {
            std::cout << "  bouton " << action.verb << "\n";
            g_pending_down = button;
        }
    }
}

u64 padGetButtonsDown(PadState* pad) {
    return pad->down;
}

u64 padGetButtons(PadState* pad) {
    return pad->held;
}

bool appletMainLoop(void) {
    return g_running;
}

AppletType appletGetAppletType(void) {
    // On se déclare en mode application : c'est ainsi que l'utilisateur lance
    // le programme sur sa console, et c'est le cas que le dimensionnement
    // mémoire suppose.
    return AppletType_Application;
}

void appletSetAutoSleepDisabled(bool) {}

Result swkbdCreate(SwkbdConfig* kbd, int) {
    kbd->max_len = 512;
    return 0;
}

void swkbdClose(SwkbdConfig*) {}
void swkbdConfigMakePresetDefault(SwkbdConfig*) {}

void swkbdConfigSetHeaderText(SwkbdConfig* kbd, const char* text) {
    kbd->header = text ? text : "";
}

void swkbdConfigSetGuideText(SwkbdConfig* kbd, const char* text) {
    kbd->guide = text ? text : "";
}

void swkbdConfigSetInitialText(SwkbdConfig* kbd, const char* text) {
    kbd->initial = text ? text : "";
}

void swkbdConfigSetStringLenMax(SwkbdConfig* kbd, u32 max) {
    kbd->max_len = max;
}

Result swkbdShow(SwkbdConfig* kbd, char* out, size_t out_size) {
    // En mode scripté il n'y a personne pour taper : TORFOIL_INPUT fournit la
    // réponse d'avance. Sans elle, on annule — ce que fait un utilisateur qui
    // ferme le clavier, donc un chemin qui mérite d'être éprouvé aussi.
    if (g_scripted) {
        if (g_typed.empty()) return 1;
        std::snprintf(out, out_size, "%s", g_typed.c_str());
        std::cout << "  clavier : " << g_typed << "\n";
        return 0;
    }

    std::cout << "\n" << kbd->header << " (" << kbd->guide << ")\n> " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line) || line.empty()) return 1;
    std::snprintf(out, out_size, "%s", line.c_str());
    return 0;
}

Result plInitialize(PlServiceType) {
    return 0;
}

void plExit(void) {}

namespace {
std::vector<uint8_t> g_font;
}

Result plGetSharedFontByType(PlFontData* font, PlSharedFontType) {
    if (g_font.empty()) {
        const char* path = std::getenv("TORFOIL_FONT");
        const char* candidates[] = {path, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                                    "C:/Windows/Fonts/segoeui.ttf"};
        for (const char* candidate : candidates) {
            if (!candidate) continue;
            std::ifstream file(candidate, std::ios::binary);
            if (!file) continue;
            g_font.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            break;
        }
        if (g_font.empty()) return 1;
    }
    font->address = g_font.data();
    font->size = g_font.size();
    return 0;
}

void randomGet(void* out, size_t len) {
    static std::random_device device;
    uint8_t* bytes = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < len; ++i) bytes[i] = static_cast<uint8_t>(device() & 0xff);
}

Result fsdevCreateFile(const char* path, size_t size, u32) {
    // Un fichier ordinaire : sur ext4 comme sur NTFS, la limite de 4 Go qui
    // justifie les fichiers concaténés n'existe pas.
    std::FILE* fp = std::fopen(path, "wb");
    if (!fp) return 1;
    if (size > 0) {
        std::fseek(fp, static_cast<long>(size) - 1, SEEK_SET);
        std::fputc(0, fp);
    }
    std::fclose(fp);
    return 0;
}

Result fsdevDeleteDirectoryRecursively(const char* path) {
    return std::remove(path) == 0 ? 0 : 1;
}

void consoleInit(void*) {}
void consoleUpdate(void*) {}
void consoleExit(void*) {}

Result socketInitialize(const SocketInitConfig*) {
    return 0;
}

void socketExit(void) {}
