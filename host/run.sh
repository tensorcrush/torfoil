#!/bin/bash
# Compile et lance Torfoil sur PC — le vrai programme, pas une maquette.
#
#   bash host/run.sh                     interactif (demande un serveur X)
#   bash host/run.sh --script "R,R,shot" scripté, sans affichage, captures BMP
#
# Le mode scripté est celui qui rend l'application vérifiable sans console :
# aucune fenêtre n'est ouverte (pilote vidéo « dummy »), les actions sont jouées
# dans l'ordre, et « shot » enregistre l'écran. Les images atterrissent dans
# host-shots/ et se regardent depuis Windows.
#
# Actions reconnues : A B X Y L R ZL ZR + - Haut Bas Gauche Droite,
#                     « wait <ms> », « shot », « quit ».
# TORFOIL_INPUT fournit d'avance ce que le clavier virtuel doit renvoyer.
#
# Dépendances : libsdl2-dev libsdl2-ttf-dev libmbedtls-dev fonts-dejavu-core
set -eo pipefail

cd "$(cd "$(dirname "$0")/.." && pwd)"

SCRIPT=""
if [ "${1:-}" = "--script" ]; then
    SCRIPT="$2"
    shift 2
fi

BIN="${TMPDIR:-/tmp}/torfoil-host"

# host/include vient EN PREMIER : c'est ce qui fait que #include <switch.h>
# trouve le faux, et que pas une ligne du programme n'a besoin d'être adaptée.
g++ -std=gnu++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -I host/include -I include -I third_party/lwip/src/include \
    host/switch_stub.cpp \
    source/main.cpp \
    source/ui/app.cpp source/ui/render.cpp source/ui/settings.cpp source/ui/phone.cpp \
    source/bt/*.cpp \
    source/net/*.cpp source/net/wg/*.cpp source/net/wg/*.c \
    source/vpn/*.cpp \
    source/diag/*.cpp \
    source/util/*.cpp \
    third_party/lwip/src/core/*.c third_party/lwip/src/core/ipv4/*.c \
    $(pkg-config --cflags --libs sdl2 SDL2_ttf) \
    -lmbedtls -lmbedx509 -lmbedcrypto -lz -lpthread \
    -o "$BIN"

echo "=== compilé : $BIN ==="

if [ -n "$SCRIPT" ]; then
    export SDL_VIDEODRIVER=dummy
    export TORFOIL_SCRIPT="$SCRIPT"
    export TORFOIL_SHOTS="${TORFOIL_SHOTS:-$PWD/host-shots}"
fi

exec "$BIN" "$@"
