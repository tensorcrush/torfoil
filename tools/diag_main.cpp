// Diagnostic autonome, sans interface graphique.
//
// Le .nro principal dépend de SDL et des polices système ; un émulateur peut
// planter là-dedans sans que le moteur soit en cause. Cette variante n'utilise
// que la console texte : elle permet d'exécuter réellement les appels Horizon
// — carte SD au-delà de 4 Go, services d'installation, tunnel — là où le
// programme complet ne démarre pas.
#include <switch.h>
#include <sys/stat.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "bt/session.hpp"
#include "diag/lp2p_probe.hpp"
#include "diag/selftest.hpp"
#include "util/log.hpp"

namespace {

const SocketInitConfig kSocketConfig = {
    .tcp_tx_buf_size = 0x1000,
    .tcp_rx_buf_size = 0x4000,
    .tcp_tx_buf_max_size = 0x20000,
    .tcp_rx_buf_max_size = 0x40000,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 8,
    .num_bsd_sessions = 3,
    .bsd_service_type = BsdServiceType_User,
};

void line(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    consoleUpdate(nullptr);
}

}  // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    ::mkdir("sdmc:/torfoil", 0777);
    util::log_open("sdmc:/torfoil/diagnostic.log");

    line("\x1b[1mTorfoil — diagnostic\x1b[0m");
    line("");

    const Result sock_rc = socketInitialize(&kSocketConfig);
    line("socket        : %s (0x%08x)", R_SUCCEEDED(sock_rc) ? "ok" : "ECHEC", sock_rc);
    util::log_fmt("socket 0x%08x", sock_rc);

    line("");

    // Point d'accès Wi-Fi : la matrice d'essais. Elle passe en premier parce
    // qu'elle est la question ouverte du moment, et parce qu'elle ne dépend ni
    // de la carte ni du réseau.
    line("[1mPoint d'acces (lp2p)[0m");
    for (const diag::Lp2pProbeStep& step : diag::probe_lp2p().steps) {
        line("  %s%-26s[0m %s", step.ok ? "[32m" : "[31m", step.label.c_str(),
             step.detail.c_str());
    }
    line("");

    const std::string dir = "sdmc:/torfoil/downloads";
    ::mkdir(dir.c_str(), 0777);

    // Le moteur est démarré uniquement pour disposer d'un transport : aucun
    // torrent n'est ajouté.
    bt::Session session;
    std::string err;
    const bool session_ok = session.start(dir, &err);
    line("moteur        : %s%s", session_ok ? "demarre" : "ECHEC ", session_ok ? "" : err.c_str());
    line("");

    diag::Report report = diag::run_all(dir, session, [](const std::string& s) {
        std::printf("  ... %s\n", s.c_str());
        consoleUpdate(nullptr);
    });

    line("");
    for (const diag::Check& c : report.checks) {
        const char* tag = !c.ran ? "\x1b[33m[IGNORE]\x1b[0m"
                                 : (c.ok ? "\x1b[32m[ OK   ]\x1b[0m" : "\x1b[31m[ECHEC ]\x1b[0m");
        line("%s %s", tag, c.name.c_str());
        line("         %s", c.detail.c_str());
    }

    line("");
    line(report.all_ok() ? "\x1b[32mTout est passe.\x1b[0m"
                         : "\x1b[31mDes epreuves ont echoue (voir ci-dessus).\x1b[0m");
    line("");
    line("Resultats aussi dans sdmc:/torfoil/diagnostic.log");
    line("Appuyer sur + pour quitter.");

    session.stop();
    util::log_close();

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }

    if (R_SUCCEEDED(sock_rc)) socketExit();
    consoleExit(nullptr);
    return 0;
}
