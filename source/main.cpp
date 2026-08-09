// Torfoil — client BitTorrent pour Nintendo Switch.
#include <switch.h>
#include <sys/stat.h>

#include <cstdio>
#include <string>

#ifndef APP_VERSION_STRING
#define APP_VERSION_STRING "0.2.0"
#endif

#include "ui/app.hpp"
#include "util/log.hpp"

namespace {

// Le pilote socket de la console réserve UN pool de mémoire partagé par toutes
// les connexions ; sa taille se déduit des tailles maximales multipliées par
// sb_efficiency, tandis que chaque socket y prélève d'emblée ses tailles
// initiales.
//
// D'où le réglage, qui n'a rien d'évident : tailles initiales très petites,
// maximales généreuses. Un client BitTorrent veut beaucoup de connexions
// modestes, pas quelques grosses. Avec 32 Ko de réception dès l'ouverture — la
// valeur d'origine ici — le pool était épuisé après une trentaine de sockets,
// et le nombre de pairs plafonnait sans qu'aucune erreur ne remonte.
//
// Un bloc BitTorrent fait 16 Ko : c'est la seule contrainte réelle en réception.
// En émission on n'envoie que des requêtes de 17 octets et des blocs partagés.
//
// Les tailles UDP, en revanche, ne sont pas un détail : VPN allumé, la totalité
// du trafic — tous les pairs, tous les trackers, le DHT — traverse UNE seule
// socket UDP, celle du tunnel WireGuard. Les 0xA500 d'origine (42 Ko) sont la
// valeur usuelle pour un usage occasionnel ; ici ils représentent cinq
// millisecondes de débit. Tout ce qui arrive pendant que la boucle moteur fait
// autre chose est jeté par le pilote, en silence, et chaque perte est vue par
// TCP comme une congestion : la fenêtre s'effondre et n'a jamais le temps de
// repousser. C'est ce qui transformait 8 Mo/s en 70 Ko/s.
const SocketInitConfig kSocketConfig = {
    .tcp_tx_buf_size = 0x4000,
    // 64 Ko. Ce chiffre est le plus important du fichier.
    //
    // Une connexion TCP ne peut pas dépasser « fenêtre de réception / temps
    // d'aller-retour » : le pair envoie une fenêtre, puis attend notre accusé de
    // réception. Vers un pair Internet l'aller-retour tourne autour de 125 ms,
    // donc 16 Ko — la valeur qui était ici, choisie pour faire tenir plus de
    // sockets dans le pool — plafonnaient CHAQUE pair à 130 Ko/s, quelle que
    // soit la bande passante disponible. C'était optimiser le nombre de
    // connexions en étranglant chacune d'elles.
    //
    // 64 Ko est la valeur par défaut de libnx, celle qu'emploient les
    // installeurs réseau qui vont vite. Elle porte le plafond à ~500 Ko/s par
    // pair, ce qui sature une ligne domestique avec une quinzaine de pairs.
    .tcp_rx_buf_size = 0x10000,
    .tcp_tx_buf_max_size = 0x40000,
    .tcp_rx_buf_max_size = 0x80000,

    .udp_tx_buf_size = 0x20000,  // 128 Ko : les ACK du tunnel ne doivent pas être perdus
    .udp_rx_buf_size = 0x80000,  // 512 Ko : ~0,5 s de marge à 8 Mo/s

    .sb_efficiency = 8,

    .num_bsd_sessions = 3,
    .bsd_service_type = BsdServiceType_User,
};

// Repli si le pilote refuse le pool ci-dessus (il est nettement plus gros). Sans
// réseau l'application ne sert à rien : mieux vaut démarrer lent que pas du tout.
const SocketInitConfig kSocketConfigFallback = {
    .tcp_tx_buf_size = 0x2000,
    .tcp_rx_buf_size = 0x8000,
    .tcp_tx_buf_max_size = 0x20000,
    .tcp_rx_buf_max_size = 0x40000,

    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,

    .sb_efficiency = 4,

    .num_bsd_sessions = 3,
    .bsd_service_type = BsdServiceType_User,
};

// Repli en mode texte : si SDL ou la police système refusent de démarrer, on ne
// laisse pas l'utilisateur devant un écran noir.
void fatal_console(const std::string& message) {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    std::printf("\x1b[1;31mTorfoil n'a pas pu démarrer\x1b[0m\n\n%s\n\nAppuyer sur + pour quitter.\n",
                message.c_str());
    consoleUpdate(nullptr);

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
}

}  // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    ::mkdir("sdmc:/torfoil", 0777);
    util::log_open("sdmc:/torfoil/torfoil.log");
    util::log_line("version " APP_VERSION_STRING);

    // Mode applet (lancé depuis l'Album) ou application (R sur un jeu) ? La
    // console n'accorde que 200 à 400 Mo au premier, contre la mémoire complète
    // au second. Tout le dimensionnement qui suit suppose le mode application ;
    // le dire dans le journal évite de chercher longtemps pourquoi c'est lent.
    const AppletType applet_type = appletGetAppletType();
    const bool applet_mode = applet_type != AppletType_Application &&
                             applet_type != AppletType_SystemApplication;
    util::log_fmt("mode %s (type applet %d)%s",
                  applet_mode ? "APPLET" : "application", static_cast<int>(applet_type),
                  applet_mode ? " — mémoire réduite, débit et stabilité dégradés" : "");

    Result rc = socketInitialize(&kSocketConfig);
    if (R_FAILED(rc)) {
        util::log_fmt("socketInitialize a refusé les gros tampons (0x%08x), repli", rc);
        rc = socketInitialize(&kSocketConfigFallback);
        if (R_FAILED(rc)) {
            util::log_fmt("socketInitialize a échoué : 0x%08x", rc);
            util::log_close();
            fatal_console("Pilote réseau indisponible (socketInitialize).");
            return 1;
        }
        util::log_line("réseau : pilote socket initialisé (tampons réduits, débit limité)");
    } else {
        util::log_line("réseau : pilote socket initialisé");
    }

    // pl:u fournit les polices système utilisées par l'interface.
    rc = plInitialize(PlServiceType_User);
    if (R_FAILED(rc)) {
        util::log_fmt("plInitialize a échoué : 0x%08x", rc);
        util::log_close();
        socketExit();
        fatal_console("Service de polices indisponible (pl:u).");
        return 1;
    }
    util::log_line("polices : pl:u initialisé");

    {
        ui::App app;
        std::string err;
        if (app.init(&err)) {
            util::log_line("interface prête, entrée dans la boucle principale");
            app.run();
            util::log_line("sortie de la boucle principale");
            app.shutdown();
        } else {
            util::log_line("échec d'initialisation : " + err);
            app.shutdown();
            util::log_close();
            plExit();
            socketExit();
            fatal_console(err);
            return 1;
        }
    }

    util::log_close();
    plExit();
    socketExit();
    return 0;
}
