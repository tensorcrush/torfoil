#!/bin/bash
#
#   bash tests/webui_live.sh
#
# Éprouve l'accès distant sur la vraie application, compilée pour PC : le
# serveur écoute, refuse ce qui n'a pas la clé, ajoute un lien, met en pause et
# retire. Rien n'est simulé, c'est le même code que sur la console.
set -uo pipefail

cd "$(cd "$(dirname "$0")/.." && pwd)"

SD="sdmc:/torfoil"
PORT=8080
FAIL=0

check() {
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then printf '  \033[32mOK\033[0m    %s\n' "$desc"
    else printf '  \033[31mECHEC\033[0m %s\n' "$desc"; FAIL=$((FAIL + 1)); fi
}
not() { ! "$@"; }

mkdir -p "$SD"
cp "$SD/settings.cfg" "$SD/settings.cfg.bak" 2>/dev/null
printf 'language=fr\nenable_dht=0\nenable_pex=0\nremote_enabled=1\nmax_active=2\n' > "$SD/settings.cfg"

cleanup() {
    [ -n "${APP_PID:-}" ] && kill "$APP_PID" 2>/dev/null
    sleep 1
    [ -n "${APP_PID:-}" ] && kill -9 "$APP_PID" 2>/dev/null
    mv "$SD/settings.cfg.bak" "$SD/settings.cfg" 2>/dev/null
}
trap cleanup EXIT

echo "=== compilation et lancement"
TORFOIL_SHOTS=/tmp/torfoil-webui bash host/run.sh --script 'wait 120000,quit' >/tmp/torfoil-webui.log 2>&1 &
APP_PID=$!

READY=0
for _ in $(seq 1 60); do
    sleep 3
    if curl -s -m 3 -o /dev/null "http://127.0.0.1:$PORT/"; then READY=1; break; fi
    kill -0 "$APP_PID" 2>/dev/null || break
done
[ "$READY" = 1 ] || { echo "le serveur n'a jamais répondu, voir /tmp/torfoil-webui.log" >&2; exit 1; }

KEY=$(grep -oE '\?k=[0-9a-f]+' "$SD/torfoil.log" | tail -1 | cut -d= -f2)
API="http://127.0.0.1:$PORT"
HASH=0123456789abcdef0123456789abcdef01234567
MAGNET="magnet:?xt=urn:btih:$HASH&dn=Essai+WebUI"

code() { curl -s -m 5 -o /dev/null -w '%{http_code}' "$@"; }
state() { curl -s -m 5 "$API/api/state?k=$KEY"; }
act() { curl -s -m 5 -X POST --data "hash=$HASH&action=$1" "$API/api/action?k=$KEY"; }
state_has() { state | grep -q "$1"; }

echo "=== verifications"
check "une clé a été tirée"                    test -n "$KEY"
check "la page est servie sans clé"            test "$(code "$API/")" = 200
check "l'API refuse sans clé"                  test "$(code "$API/api/state")" = 403
check "l'API refuse une clé fausse"            test "$(code "$API/api/state?k=deadbeef")" = 403
check "l'état est du JSON"                     state_has '"torrents"' 

curl -s -m 5 -X POST --data "magnets=$MAGNET" "$API/api/add?k=$KEY" >/dev/null
sleep 2
check "le magnet ajouté apparaît"              state_has "$HASH"

act pause >/dev/null
sleep 2
check "la mise en pause est prise en compte"   state_has "\"hash\":\"$HASH\".*\"paused\":true"

act resume >/dev/null
sleep 2
check "la reprise est prise en compte"         state_has "\"hash\":\"$HASH\".*\"paused\":false"

act remove >/dev/null
sleep 2
check "le retrait est pris en compte"          not state_has "$HASH"
check "une action inconnue est refusée"        bash -c "curl -s -m 5 -X POST --data 'hash=$HASH&action=voler' '$API/api/action?k=$KEY' | grep -q '\"ok\":false'"

echo
if [ "$FAIL" = 0 ]; then echo "tout est vert"; else echo "$FAIL vérification(s) en échec"; fi
exit $((FAIL > 0))
