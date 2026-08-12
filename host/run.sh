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
# miniz est du C : le passer a g++ le compilerait en C++, ou ses definitions
# provisoires deviennent des redefinitions.
MINIZ_O="${TMPDIR:-/tmp}/torfoil-miniz.o"
cc -O2 -std=gnu11 -w -c third_party/miniz/miniz.c -o "$MINIZ_O"

g++ -std=gnu++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -I host/include -I include -I third_party/lwip/src/include -I third_party/nanosvg \
    -I third_party/miniz \
    host/switch_stub.cpp \
    source/main.cpp \
    source/ui/*.cpp \
    source/bt/*.cpp \
    source/net/*.cpp source/net/wg/*.cpp source/net/wg/*.c \
    source/vpn/*.cpp \
    source/diag/*.cpp \
    source/util/*.cpp \
    third_party/lwip/src/core/*.c third_party/lwip/src/core/ipv4/*.c \
    "$MINIZ_O" \
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
