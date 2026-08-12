#!/bin/bash
#
#   bash tools/emu/run.sh --nro torfoil.nro --script "wait 3000,shot,Bas,A,shot,quit"
#
# Lance un homebrew dans l'emulateur prepare par setup.sh, joue une suite
# d'actions au clavier, enregistre des captures, puis rend la carte SD du guest.
#
# Actions : A B X Y L R ZL ZR + - Haut Bas Gauche Droite · wait <ms> · shot · quit
# Options : --sd <dir>    carte SD de depart, copiee avant le lancement
#           --out <dir>   captures, journaux et carte SD finale (defaut emu-out)
#           --net         autorise le reseau invite
#           --warmup <s>  attente maximale avant la premiere action (defaut 90)
#           --timeout <s> duree maximale totale (defaut 600)
#           --display <n> numero d'ecran X (defaut 99)
set -euo pipefail

EMU_HOME="${EMU_HOME:-$HOME/.torfoil-emu}"
NRO=""
SCRIPT=""
SD=""
OUT="emu-out"
NET=0
WARMUP=90
TIMEOUT=600
DISP_NUM=99

while [ $# -gt 0 ]; do
    case "$1" in
        --nro) NRO="$2"; shift 2 ;;
        --script) SCRIPT="$2"; shift 2 ;;
        --sd) SD="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --net) NET=1; shift ;;
        --warmup) WARMUP="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --display) DISP_NUM="$2"; shift 2 ;;
        --emu-home) EMU_HOME="$2"; shift 2 ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "option inconnue : $1" >&2; exit 2 ;;
    esac
done

[ -n "$NRO" ] || { echo "--nro est obligatoire" >&2; exit 2; }
[ -x "$EMU_HOME/build/Ryujinx" ] || { echo "emulateur absent, lance setup.sh" >&2; exit 3; }
NRO="$(cd "$(dirname "$NRO")" && pwd)/$(basename "$NRO")"

DATA="$EMU_HOME/data"
DISPLAY_ID=":$DISP_NUM"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
rm -f "$OUT"/shot-*.png "$OUT/emulator.log"

if [ -n "$SD" ]; then
    rm -rf "$DATA/sdcard"
    mkdir -p "$DATA/sdcard"
    [ -d "$SD" ] && cp -r "$SD/." "$DATA/sdcard/"
fi
mkdir -p "$DATA/sdcard"

export DISPLAY="$DISPLAY_ID"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/torfoil-xdg}"
export SDL_AUDIODRIVER=dummy
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"

XVFB_PID=""
EMU_PID=""
cleanup() {
    [ -n "$EMU_PID" ] && kill "$EMU_PID" 2>/dev/null || true
    sleep 1
    [ -n "$EMU_PID" ] && kill -9 "$EMU_PID" 2>/dev/null || true
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
}
trap cleanup EXIT

pkill -f "Xvfb $DISPLAY_ID" 2>/dev/null || true
Xvfb "$DISPLAY_ID" -screen 0 1280x720x24 -nolisten tcp >"$OUT/xvfb.log" 2>&1 &
XVFB_PID=$!
for _ in $(seq 1 30); do xdpyinfo >/dev/null 2>&1 && break; sleep 0.3; done

ARGS=(--no-gui --root-data-dir "$DATA" --graphics-backend OpenGl
      --enable-keyboard --ignore-missing-services --disable-shader-cache
      --hide-cursor Always)
[ "$NET" = 1 ] && ARGS+=(--enable-internet-connection)

timeout "$TIMEOUT" "$EMU_HOME/build/Ryujinx" "${ARGS[@]}" "$NRO" >"$OUT/emulator.log" 2>&1 &
EMU_PID=$!

WIN=""
for _ in $(seq 1 "$WARMUP"); do
    WIN="$(xdotool search --name 'Ryujinx .* - ' 2>/dev/null | head -1 || true)"
    [ -n "$WIN" ] && break
    kill -0 "$EMU_PID" 2>/dev/null || { echo "l'emulateur s'est arrete" >&2; tail -5 "$OUT/emulator.log" >&2; exit 4; }
    sleep 1
done
[ -n "$WIN" ] || { echo "aucune fenetre apres ${WARMUP}s" >&2; exit 4; }
xdotool windowfocus --sync "$WIN"

grab() { ffmpeg -loglevel error -f x11grab -video_size 1280x720 -i "$DISPLAY_ID" -frames:v 1 -y "$1"; }

PREV=""
READY=0
for _ in $(seq 1 "$WARMUP"); do
    grab "$OUT/.settle.png" 2>/dev/null || true
    if [ -s "$OUT/.settle.png" ]; then
        DEV="$(convert "$OUT/.settle.png" -format '%[fx:int(standard_deviation*1000)]' info: 2>/dev/null || echo 0)"
        NOW="$(md5sum "$OUT/.settle.png" | cut -d' ' -f1)"
        if [ "${DEV:-0}" -gt 5 ] && [ "$NOW" = "$PREV" ]; then READY=1; break; fi
        PREV="$NOW"
    fi
    sleep 1
done
rm -f "$OUT/.settle.png"
[ "$READY" = 1 ] || { echo "l'image ne s'est jamais stabilisee (${WARMUP}s)" >&2; exit 4; }

HOLD="${EMU_KEY_HOLD:-0.4}"
press() {
    xdotool windowfocus "$WIN" 2>/dev/null || true
    xdotool keydown --clearmodifiers "$1"
    sleep "$HOLD"
    xdotool keyup --clearmodifiers "$1"
    sleep 0.5
}

SHOT=0
IFS=',' read -ra STEPS <<< "$SCRIPT"
for raw in "${STEPS[@]}"; do
    step="$(echo "$raw" | sed 's/^ *//; s/ *$//')"
    [ -z "$step" ] && continue
    case "$step" in
        A) press z ;;
        B) press x ;;
        X) press c ;;
        Y) press v ;;
        L) press e ;;
        R) press u ;;
        ZL) press q ;;
        ZR) press o ;;
        +) press plus ;;
        -) press minus ;;
        Haut|Up) press Up ;;
        Bas|Down) press Down ;;
        Gauche|Left) press Left ;;
        Droite|Right) press Right ;;
        wait*)
            MS="$(echo "$step" | tr -dc 0-9)"
            [ -n "$MS" ] || MS=1000
            sleep "$(awk "BEGIN{print $MS/1000}")"
            ;;
        shot)
            SHOT=$((SHOT + 1))
            grab "$(printf '%s/shot-%02d.png' "$OUT" "$SHOT")"
            echo "capture $(printf 'shot-%02d.png' "$SHOT")"
            ;;
        quit)
            press plus
            sleep 3
            ;;
        *) echo "action inconnue : $step" >&2 ;;
    esac
    kill -0 "$EMU_PID" 2>/dev/null || break
done

if kill -0 "$EMU_PID" 2>/dev/null; then
    kill "$EMU_PID" 2>/dev/null || true
    for _ in $(seq 1 20); do kill -0 "$EMU_PID" 2>/dev/null || break; sleep 0.5; done
fi
wait "$EMU_PID" 2>/dev/null || true
EMU_PID=""

rm -rf "$OUT/sdcard"
cp -r "$DATA/sdcard" "$OUT/sdcard"

echo "captures : $OUT"
echo "carte SD : $OUT/sdcard"
grep -icE 'unhandled exception|fatal error' "$OUT/emulator.log" >/dev/null && true
if grep -qiE 'unhandled exception|fatal error' "$OUT/emulator.log"; then
    echo "l'emulateur a signale une erreur, voir $OUT/emulator.log" >&2
    exit 5
fi
