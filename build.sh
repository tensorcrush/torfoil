#!/bin/bash
# Construit torfoil.nro, et torfoil-diag.nro si « diag » est passé en argument.
# Compilation de Torfoil depuis WSL.
#
# Le code source peut vivre côté Windows, mais compiler directement sur /mnt/c
# en WSL1 est très lent : on recopie dans le système de fichiers Linux, on
# compile là, et on rapatrie le .nro.
set -euo pipefail

export DEVKITPRO=/opt/devkitpro
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"

# Emplacement du dépôt. Déduit de celui du script, surchargeable par TORFOIL_SRC
# quand on compile depuis ailleurs.
SRC_WIN="${TORFOIL_SRC:-$(cd "$(dirname "$0")" && pwd)}"
WORK="/root/build/torfoil"

mkdir -p "$WORK"

# Synchronisation (on ignore les artefacts de build)
rsync -a --delete \
      --exclude 'build/' --exclude '*.nro' --exclude '*.elf' --exclude '*.nacp' \
      "$SRC_WIN/" "$WORK/"

cd "$WORK"
mkdir -p source/bt source/net source/util source/install source/ui include romfs
[ -f romfs/.keep ] || echo "torfoil" > romfs/.keep

if [ "${1:-}" = "clean" ]; then
    make clean
    shift || true
fi

make -j"$(nproc)" "$@"

# Le diagnostic en console texte : construit à part, sans SDL ni polices.
make -j"$(nproc)" -f Makefile.diag

# Rapatriement des résultats
for binary in torfoil.nro torfoil-diag.nro; do
    if [ -f "$WORK/$binary" ]; then
        cp "$WORK/$binary" "$SRC_WIN/$binary"
        echo ""
        echo "=== $binary prêt : $(du -h "$WORK/$binary" | cut -f1) ==="
    fi
done
