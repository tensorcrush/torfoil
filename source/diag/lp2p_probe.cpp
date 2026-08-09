// Pourquoi le point d'accès est refusé — une réponse par la mesure.
//
// La première tentative de création de réseau a renvoyé 2231-0261 (module 231 =
// lp2p, description 261), une valeur que personne n'a documentée. Deviner
// coûterait un aller-retour par hypothèse, avec une console à l'autre bout du
// fil. On essaie donc toute la matrice d'un coup et on lit les codes : chaque
// ligne isole exactement une variable, et la première qui réussit désigne la
// cause.
//
// Ce que chaque essai départage :
//   * WPA2 standard contre le chiffrement maison de Nintendo — si seul le
//     second passe, le problème est le mode WPA2, pas le droit de créer un
//     réseau ;
//   * lp2p:app contre lp2p:sys — sépare une question de permission d'une
//     question de configuration ;
//   * l'identifiant de communication locale — lp2p le lit dans le NACP du
//     processus. Un homebrew lancé en détournant un jeu hérite du NACP de CE
//     jeu, et un jeu sans mode sans fil local n'en a aucun. Si c'est la cause,
//     seul l'essai qui impose une valeur passera.
#include "diag/lp2p_probe.hpp"

#ifdef __SWITCH__

#include <switch.h>

#include <cstdio>

namespace diag {

namespace {

std::string code(Result rc) {
    char buf[48];
    // Le code affiché par la console est 2000+module — 2231-0261 — parce que
    // c'est celui qu'on retrouve dans un moteur de recherche.
    std::snprintf(buf, sizeof(buf), "0x%08x (2%03u-%04u)", rc, static_cast<unsigned>(rc & 0x1FF),
                  static_cast<unsigned>((rc >> 9) & 0x1FFF) );
    return buf;
}

struct Attempt {
    const char* label;
    Lp2pServiceType service;
    bool wpa2;                    // true = WPA2-PSK standard, false = défaut Nintendo
    s64 local_communication_id;   // 0 = laisser lp2p lire le NACP
    u16 frequency;
    s8 member_count_max;
};

Lp2pProbeStep run_one(const Attempt& attempt) {
    Lp2pProbeStep step;
    step.label = attempt.label;

    Result rc = lp2pInitialize(attempt.service);
    if (R_FAILED(rc)) {
        step.detail = "lp2pInitialize " + code(rc);
        return step;
    }

    Lp2pGroupInfo info;
    lp2pCreateGroupInfo(&info);
    lp2pGroupInfoSetServiceName(&info, "torfoil-test");
    lp2pGroupInfoSetFrequencyChannel(&info, attempt.frequency, 0);
    lp2pGroupInfoSetStealthEnabled(&info, false);
    lp2pGroupInfoSetMemberCountMax(&info, static_cast<size_t>(attempt.member_count_max));
    info.local_communication_id = attempt.local_communication_id;

    if (attempt.wpa2) {
        s8 flags[1] = {0};
        lp2pGroupInfoSetFlags(&info, flags, 1);
        info.security_type = 3;
        rc = lp2pGroupInfoSetPassphrase(&info, "torfoil12345");
        if (R_FAILED(rc)) {
            step.detail = "SetPassphrase " + code(rc);
            lp2pExit();
            return step;
        }
    } else {
        // Le chiffrement d'origine ne veut pas d'une phrase de passe mais d'une
        // clé pré-partagée ; sans elle, la taille reste à zéro et le service
        // refuse la structure avant même de tenter quoi que ce soit.
        const uint8_t key[16] = {'t', 'o', 'r', 'f', 'o', 'i', 'l', 0,
                                 1,   2,   3,   4,   5,   6,   7,   8};
        lp2pGroupInfoSetPresharedKey(&info, key, sizeof(key));
    }

    rc = lp2pCreateGroup(&info);
    if (R_SUCCEEDED(rc)) {
        step.ok = true;
        step.detail = "réseau créé";

        Lp2pIpConfig config{};
        if (R_SUCCEEDED(lp2pGetIpConfig(&config))) {
            const uint8_t* raw = config.ip_addr;
            char addr[32];
            // sockaddr_in : famille et port d'abord, l'adresse à l'octet 4.
            std::snprintf(addr, sizeof(addr), " — %u.%u.%u.%u", raw[4], raw[5], raw[6], raw[7]);
            step.detail += addr;
        }
        lp2pDestroyGroup();
    } else {
        step.detail = "CreateGroup " + code(rc);
    }

    lp2pExit();
    return step;
}

}  // namespace

Lp2pProbeReport probe_lp2p() {
    Lp2pProbeReport report;

    // L'identifiant employé par l'essai « forcé ». Celui de Mario Kart Live,
    // le seul titre connu à utiliser lp2p:app : s'il est accepté alors que zéro
    // ne l'est pas, la validation par le NACP est bien la cause.
    constexpr s64 kKnownId = 0x0100165003504000;

    const Attempt attempts[] = {
        {"WPA2, lp2p:app, NACP",       Lp2pServiceType_App,    true,  0,         24, 4},
        {"WPA2, lp2p:app, id force",   Lp2pServiceType_App,    true,  kKnownId,  24, 4},
        {"WPA2, lp2p:app, 5 GHz",      Lp2pServiceType_App,    true,  0,         50, 4},
        {"WPA2, lp2p:app, 1 membre",   Lp2pServiceType_App,    true,  0,         24, 1},
        {"Nintendo, lp2p:app",         Lp2pServiceType_App,    false, 0,         24, 4},
        {"WPA2, lp2p:sys",             Lp2pServiceType_System, true,  0,         24, 4},
        {"Nintendo, lp2p:sys",         Lp2pServiceType_System, false, 0,         24, 4},
    };

    for (const Attempt& attempt : attempts) report.steps.push_back(run_one(attempt));
    return report;
}

}  // namespace diag

#else

namespace diag {
Lp2pProbeReport probe_lp2p() {
    Lp2pProbeReport report;
    report.steps.push_back({"lp2p", false, "vérifiable uniquement sur console"});
    return report;
}
}  // namespace diag

#endif
