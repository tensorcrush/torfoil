#!/usr/bin/env python3
"""Génère icon.jpg, l'icône affichée par le menu homebrew.

Le format est imposé par la console : JPEG, 256x256, sans transparence. Le
Makefile la prend automatiquement si le fichier existe à la racine.

Le dessin est fait ici plutôt que déposé en binaire pour une raison simple :
une icône qu'on ne peut pas régénérer devient intouchable, et la palette doit
rester celle de l'application (include/ui/render.hpp). Tout se modifie ci-dessous.

    python tools/make_icon.py
"""

from PIL import Image, ImageDraw

SIZE = 256
SCALE = 4  # dessiné en grand puis réduit : c'est ce qui donne les bords lisses

# Palette de l'application, à l'octet près (ui::palette).
BG = (0x14, 0x17, 0x1C)
SURFACE = (0x1E, 0x22, 0x2A)
ACCENT = (0x3D, 0xD6, 0xC4)
ACCENT_DIM = (0x1F, 0x6B, 0x63)
TEXT = (0xE8, 0xEC, 0xF1)


def build() -> Image.Image:
    w = SIZE * SCALE
    img = Image.new("RGB", (w, w), BG)
    d = ImageDraw.Draw(img)

    # Fond légèrement plus clair au centre : sans lui l'icône est un carré noir
    # parmi d'autres carrés noirs dans le menu.
    for i in range(28, 0, -1):
        t = i / 28.0
        c = tuple(int(BG[k] + (SURFACE[k] - BG[k]) * (1.0 - t)) for k in range(3))
        pad = int(w * 0.5 * t)
        d.ellipse([pad, pad, w - pad, w - pad], fill=c)

    # Le bouclier : c'est le « foil » du nom, et le tunnel qui protège tout le
    # reste. Tracé en contour épais plutôt qu'en aplat pour laisser respirer la
    # flèche qu'il contient.
    cx = w // 2
    top, bottom = int(w * 0.13), int(w * 0.90)
    half = int(w * 0.30)
    shoulder = int(w * 0.55)
    shield = [
        (cx, top),
        (cx + half, top + int(w * 0.09)),
        (cx + half, shoulder),
        (cx, bottom),
        (cx - half, shoulder),
        (cx - half, top + int(w * 0.09)),
    ]
    d.polygon(shield, outline=ACCENT_DIM, width=int(w * 0.030))

    # La flèche de téléchargement, en trois barres : les pièces d'un torrent qui
    # arrivent les unes après les autres. La plus basse est la plus vive, c'est
    # celle qui vient d'atterrir.
    bar_w = int(w * 0.34)
    bar_h = int(w * 0.052)
    gap = int(w * 0.038)
    y0 = int(w * 0.28)
    shades = [ACCENT_DIM, (0x2C, 0xA0, 0x94), ACCENT]
    for i, shade in enumerate(shades):
        y = y0 + i * (bar_h + gap)
        inset = int(i * w * 0.030)
        d.rounded_rectangle(
            [cx - bar_w // 2 + inset, y, cx + bar_w // 2 - inset, y + bar_h],
            radius=bar_h // 2,
            fill=shade,
        )

    # La pointe.
    tip_top = y0 + 3 * (bar_h + gap)
    tip_half = int(w * 0.155)
    d.polygon(
        [(cx - tip_half, tip_top), (cx + tip_half, tip_top), (cx, tip_top + int(w * 0.155))],
        fill=ACCENT,
    )

    # Un liseré clair en bas : le repère qui distingue l'icône du fond du menu.
    d.rounded_rectangle(
        [int(w * 0.34), int(w * 0.925), int(w * 0.66), int(w * 0.945)],
        radius=int(w * 0.010),
        fill=TEXT,
    )

    return img.resize((SIZE, SIZE), Image.LANCZOS)


if __name__ == "__main__":
    import os

    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "icon.jpg")
    build().save(out, "JPEG", quality=95, subsampling=0)
    print(f"écrit : {out} ({os.path.getsize(out)} octets)")
