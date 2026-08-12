#!/bin/bash
#
#   bash tools/emu/e2e.sh [--out <dir>]
#
# Test de bout en bout sur emulateur : carte SD neuve, cinq torrents deposes
# dans l'inbox avec leurs donnees, puis verification de ce que le guest a
# reellement ecrit sur la carte. Ne demande aucun reseau.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/emu-out"
[ "${1:-}" = "--out" ] && OUT="$2"

SEED="$(mktemp -d)"
trap 'rm -rf "$SEED"' EXIT
mkdir -p "$SEED/torfoil/downloads" "$SEED/torfoil/inbox"
python3 "$ROOT/host/mkfixtures.py" "$SEED/torfoil/downloads" "$SEED/torfoil/inbox" >/dev/null

FAIL=0
check() {
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then printf '  \033[32mOK\033[0m    %s\n' "$desc"
    else printf '  \033[31mECHEC\033[0m %s\n' "$desc"; FAIL=$((FAIL + 1)); fi
}
not() { ! "$@"; }
atleast() { [ "$(eval "$2")" -ge "$1" ]; }

echo "=== lancement"
bash "$ROOT/tools/emu/run.sh" --nro "$ROOT/torfoil.nro" --sd "$SEED" --out "$OUT" \
    --script 'shot,Bas,Bas,A,wait 10000,shot,R,wait 3000,shot,L,wait 1500,A,wait 2000,shot,B,wait 1000,quit'

SD="$OUT/sdcard/torfoil"
LOG="$SD/torfoil.log"

READY=$(grep -c 'stockage prêt' "$LOG" 2>/dev/null || true)
RESUME=$(find "$SD/downloads" -maxdepth 1 -name '.*.torfoil' 2>/dev/null | wc -l)
SHOTS=$(ls "$OUT"/shot-*.png 2>/dev/null | wc -l)
BLANK=0
for png in "$OUT"/shot-*.png; do
    DEV=$(convert "$png" -format '%[fx:int(standard_deviation*1000)]' info: 2>/dev/null || echo 0)
    [ "$DEV" -lt 5 ] && BLANK=$((BLANK + 1))
done

echo "=== verifications"
check "le guest a ecrit torfoil.log"                       test -f "$LOG"
check "la langue choisie a l'ecran est enregistree (fr)"   grep -q '^language=fr' "$SD/settings.cfg"
check "les 5 torrents de l'inbox sont montes ($READY)"     test "$READY" -ge 5
check "aucun echec d'ecriture SD"                          not grep -q 'ÉCHEC' "$LOG"
check "l'inbox a ete videe apres import"                   test -z "$(ls -A "$SD/inbox" 2>/dev/null)"
check "les points de reprise sont ecrits ($RESUME)"        test "$RESUME" -ge 5
check "les captures sont produites ($SHOTS)"               test "$SHOTS" -ge 4
check "aucune capture vide ($BLANK)"                       test "$BLANK" = 0
check "l'emulateur n'a signale aucune exception"           not grep -qiE 'unhandled exception|fatal error' "$OUT/emulator.log"

echo
if [ "$FAIL" = 0 ]; then
    echo "tout est vert — captures dans $OUT"
else
    echo "$FAIL verification(s) en echec"
fi
exit $((FAIL > 0))
