// Faux <switch.h> pour la compilation sur PC.
//
// Le but n'est pas d'émuler la console mais de faire tourner LE VRAI programme
// — même moteur, même interface, mêmes fichiers source — sur une machine de
// développement, sans émulateur. Aucun émulateur ne le permet ici : yuzu plante
// sur un « bonjour » de douze lignes avant qu'une seule instruction invitée
// s'exécute.
//
// La bonne nouvelle, découverte en inventoriant les appels : la surface Horizon
// du programme est minuscule. Les sockets sont du POSIX ordinaire, l'affichage
// est du SDL2 ordinaire. Ne restent que la manette, le clavier virtuel, la
// police système, quelques services de cycle de vie, et les fichiers concaténés
// de FAT32. Tout cela tient dans ce fichier.
//
// Il est placé AVANT les chemins d'inclusion habituels par Makefile.host : le
// code applicatif écrit `#include <switch.h>` sans savoir qu'il ment, et pas
// une ligne de logique n'a besoin d'un #ifdef.
//
// Ce qui n'existe pas ici, et qu'il ne faut donc pas croire testé sur PC : le
// point d'accès lp2p, les fichiers concaténés (émulés par des fichiers
// ordinaires, la limite de 4 Go de FAT32 n'existant pas sur ext4), et le
// dimensionnement du pilote socket de la console.
#pragma once

#include <SDL2/SDL.h>

#include <cstdint>
#include <cstdio>
#include <string>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint32_t Result;

#define R_SUCCEEDED(res) ((res) == 0)
#define R_FAILED(res) ((res) != 0)

// --- manette ---
//
// Les boutons gardent leurs valeurs réelles : le code de l'application les
// manipule par masques, et des valeurs inventées rendraient une combinaison
// testée sur PC fausse sur console.
enum {
    HidNpadButton_A = 1u << 0,
    HidNpadButton_B = 1u << 1,
    HidNpadButton_X = 1u << 2,
    HidNpadButton_Y = 1u << 3,
    HidNpadButton_L = 1u << 6,
    HidNpadButton_R = 1u << 7,
    HidNpadButton_ZL = 1u << 8,
    HidNpadButton_ZR = 1u << 9,
    HidNpadButton_Plus = 1u << 10,
    HidNpadButton_Minus = 1u << 11,
    HidNpadButton_Left = 1u << 12,
    HidNpadButton_Up = 1u << 13,
    HidNpadButton_Right = 1u << 14,
    HidNpadButton_Down = 1u << 15,
    HidNpadButton_StickLLeft = 1u << 16,
    HidNpadButton_StickLUp = 1u << 17,
    HidNpadButton_StickLRight = 1u << 18,
    HidNpadButton_StickLDown = 1u << 19,
};

enum { HidNpadStyleSet_NpadStandard = 0 };

typedef struct {
    u64 down;
    u64 held;
} PadState;

void padConfigureInput(u32 players, u32 style_set);
void padInitializeDefault(PadState* pad);
void padUpdate(PadState* pad);
u64 padGetButtonsDown(PadState* pad);
u64 padGetButtons(PadState* pad);

// --- cycle de vie ---
typedef enum {
    AppletType_None = -2,
    AppletType_Default = -1,
    AppletType_Application = 0,
    AppletType_SystemApplet = 1,
    AppletType_LibraryApplet = 2,
    AppletType_SystemApplication = 3,
} AppletType;

bool appletMainLoop(void);
AppletType appletGetAppletType(void);
void appletSetAutoSleepDisabled(bool disable);

// --- clavier système ---
//
// Sur PC il est remplacé par une saisie au terminal : la fenêtre SDL affiche
// l'application, la ligne se tape à côté. C'est moins joli et strictement aussi
// utile pour éprouver ce qui suit la saisie.
typedef struct {
    std::string header;
    std::string guide;
    std::string initial;
    size_t max_len;
} SwkbdConfig;

Result swkbdCreate(SwkbdConfig* kbd, int unk);
void swkbdClose(SwkbdConfig* kbd);
void swkbdConfigMakePresetDefault(SwkbdConfig* kbd);
void swkbdConfigSetHeaderText(SwkbdConfig* kbd, const char* text);
void swkbdConfigSetGuideText(SwkbdConfig* kbd, const char* text);
void swkbdConfigSetInitialText(SwkbdConfig* kbd, const char* text);
void swkbdConfigSetStringLenMax(SwkbdConfig* kbd, u32 max);
Result swkbdShow(SwkbdConfig* kbd, char* out, size_t out_size);

// --- police système ---
typedef enum { PlServiceType_User = 0 } PlServiceType;
typedef enum {
    PlSharedFontType_Standard = 0,
    PlSharedFontType_ChineseSimplified = 1,
} PlSharedFontType;

typedef struct {
    void* address;
    size_t size;
} PlFontData;

Result plInitialize(PlServiceType type);
void plExit(void);
Result plGetSharedFontByType(PlFontData* font, PlSharedFontType type);

// --- divers ---
void randomGet(void* out, size_t len);

// Fichier concaténé : sans intérêt hors FAT32. Sur PC on crée un fichier
// ordinaire, ce qui suffit à éprouver tout le code d'écriture autour.
enum { FsCreateOption_BigFile = 1 };
Result fsdevCreateFile(const char* path, size_t size, u32 flags);
Result fsdevDeleteDirectoryRecursively(const char* path);

// --- console texte (utilisée par le repli d'erreur fatale) ---
void consoleInit(void* unused);
void consoleUpdate(void* unused);
void consoleExit(void* unused);

// --- pilote réseau ---
//
// Il n'y a rien à initialiser sur PC : les sockets sont déjà là. La structure
// existe pour que main.cpp compile sans retouche, et ses champs sont ignorés.
typedef enum { BsdServiceType_User = 0, BsdServiceType_System = 1 } BsdServiceType;

typedef struct {
    u32 tcp_tx_buf_size;
    u32 tcp_rx_buf_size;
    u32 tcp_tx_buf_max_size;
    u32 tcp_rx_buf_max_size;
    u32 udp_tx_buf_size;
    u32 udp_rx_buf_size;
    u32 sb_efficiency;
    u32 num_bsd_sessions;
    BsdServiceType bsd_service_type;
} SocketInitConfig;

Result socketInitialize(const SocketInitConfig* config);
void socketExit(void);
