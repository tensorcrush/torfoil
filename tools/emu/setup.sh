#!/bin/bash
#
#   bash tools/emu/setup.sh --firmware <firmware.zip> --keys <dossier des .keys>
#
# Prepare un emulateur Switch capable de lancer un homebrew sans GPU ni GUI :
# telecharge les sources de Kenji-NX, applique le patch headless, compile, puis
# installe les cles et le firmware fournis par l'utilisateur.
#
# Options : --emu-home <dir> (defaut ~/.torfoil-emu) · --ref <branche|tag>
#           --skip-deps · --rebuild
set -euo pipefail

EMU_HOME="${EMU_HOME:-$HOME/.torfoil-emu}"
REPO="https://git.ryujinx.app/projects/Kenji-NX.git"
REF="master"
FIRMWARE=""
KEYS=""
SKIP_DEPS=0
REBUILD=0
HERE="$(cd "$(dirname "$0")" && pwd)"

while [ $# -gt 0 ]; do
    case "$1" in
        --firmware) FIRMWARE="$2"; shift 2 ;;
        --keys) KEYS="$2"; shift 2 ;;
        --emu-home) EMU_HOME="$2"; shift 2 ;;
        --ref) REF="$2"; shift 2 ;;
        --skip-deps) SKIP_DEPS=1; shift ;;
        --rebuild) REBUILD=1; shift ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "option inconnue : $1" >&2; exit 2 ;;
    esac
done

say() { printf '\n=== %s\n' "$1"; }

if [ "$SKIP_DEPS" = 0 ]; then
    say "dependances systeme"
    if [ "$(id -u)" != 0 ]; then
        echo "non root : installe manuellement xvfb xdotool ffmpeg mesa-utils libgl1-mesa-dri python3"
    else
        export DEBIAN_FRONTEND=noninteractive
        for unit in systemd systemd-timesyncd systemd-resolved udev; do
            post="/var/lib/dpkg/info/$unit.postinst"
            if [ -f "$post" ] && ! grep -q 'torfoil-emu-stub' "$post"; then
                if ! dpkg -s "$unit" 2>/dev/null | grep -q '^Status: install ok installed'; then
                    printf '#!/bin/sh\n# torfoil-emu-stub\nexit 0\n' > "$post"
                    chmod +x "$post"
                fi
            fi
        done
        apt-get update -qq
        apt-get install -y -qq xvfb xdotool ffmpeg libgl1-mesa-dri mesa-vulkan-drivers \
            python3 curl git ca-certificates libicu-dev >/dev/null
    fi
fi

say "SDK .NET"
DOTNET="$EMU_HOME/dotnet/dotnet"
if [ ! -x "$DOTNET" ]; then
    mkdir -p "$EMU_HOME"
    curl -sSL https://dot.net/v1/dotnet-install.sh -o "$EMU_HOME/dotnet-install.sh"
    bash "$EMU_HOME/dotnet-install.sh" --channel 10.0 --install-dir "$EMU_HOME/dotnet" --no-path >/dev/null
fi
"$DOTNET" --version

say "sources de l'emulateur"
SRC="$EMU_HOME/src"
if [ ! -d "$SRC/.git" ]; then
    git clone --depth 1 --branch "$REF" "$REPO" "$SRC"
else
    git -C "$SRC" fetch --depth 1 origin "$REF" && git -C "$SRC" checkout -f FETCH_HEAD
fi

say "patch headless"
git -C "$SRC" checkout -- src/Ryujinx/Headless/OpenGL/OpenGLWindow.cs 2>/dev/null || true
if git -C "$SRC" apply --check "$HERE/patches/0001-headless-swap-interval.patch" 2>/dev/null; then
    git -C "$SRC" apply "$HERE/patches/0001-headless-swap-interval.patch"
    echo "applique"
else
    grep -q 'SetSwapInterval unsupported' "$SRC/src/Ryujinx/Headless/OpenGL/OpenGLWindow.cs" \
        && echo "deja present" || { echo "le patch ne s'applique pas sur $REF" >&2; exit 1; }
fi

say "compilation"
if [ "$REBUILD" = 1 ] || [ ! -x "$EMU_HOME/build/Ryujinx" ]; then
    DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 "$DOTNET" publish \
        "$SRC/src/Ryujinx/Ryujinx.csproj" -c Release -r linux-x64 \
        --self-contained true -p:PublishTrimmed=false -o "$EMU_HOME/build" 2>&1 | tail -2
else
    echo "binaire deja compile ($EMU_HOME/build/Ryujinx)"
fi
chmod +x "$EMU_HOME/build/Ryujinx"

DATA="$EMU_HOME/data"
mkdir -p "$DATA/system" "$DATA/sdcard"

if [ -n "$KEYS" ]; then
    say "cles"
    for k in prod.keys title.keys; do
        [ -f "$KEYS/$k" ] && cp "$KEYS/$k" "$DATA/system/$k" && echo "$k"
    done
fi

if [ -n "$FIRMWARE" ]; then
    say "firmware"
    python3 "$HERE/install_firmware.py" "$FIRMWARE" "$DATA"
fi

say "pret"
echo "emulateur : $EMU_HOME/build/Ryujinx"
echo "donnees   : $DATA"
echo "lancer    : bash tools/emu/run.sh --nro torfoil.nro --script 'wait 2000,shot,quit'"
