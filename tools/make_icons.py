# -*- coding: utf-8 -*-
"""Engendre source/ui/icons_data.cpp à partir des SVG Material Symbols.

Les icônes viennent de https://github.com/google/material-design-icons, sous
licence Apache 2.0. Elles sont figées dans le dépôt plutôt que téléchargées à la
compilation : un build ne doit pas dépendre du réseau, et six kilo-octets de
texte se relisent dans un diff.

    python tools/make_icons.py assets/icons

Le dossier doit contenir un <nom>.svg par icône listée ci-dessous.
"""
import io
import os
import sys

NAMES = ['folder', 'movie', 'music_note', 'image', 'folder_zip', 'description',
         'terminal', 'sports_esports', 'downloading', 'help', 'pause_circle',
         'check_circle']

HEADER = u'''// Icônes du jeu Material Symbols de Google, licence Apache 2.0.
// https://github.com/google/material-design-icons
//
// FICHIER ENGENDRÉ par tools/make_icons.py — ne pas modifier à la main.
//
// Pourquoi le SVG est en dur ici plutôt que dans un ROMFS : le ROMFS de ce
// projet est vide, délibérément, et c'est précisément lui qui empêchait un
// clone neuf de produire un .nro. Quelques kilo-octets de texte dans le binaire
// coûtent moins cher qu'un système de ressources à faire vivre.
//
// Le SVG plutôt qu'une image toute faite : l'interface dessine la même icône à
// plusieurs tailles, et une image tramée grossie se voit immédiatement.
#include "ui/icons.hpp"

namespace ui {

const IconSource kIconSources[] = {
'''


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else 'assets/icons'
    rows = []
    for name in NAMES:
        path = os.path.join(src, name + '.svg')
        svg = io.open(path, encoding='utf-8').read().strip()
        svg = svg.replace(chr(92), chr(92) * 2).replace('"', chr(92) + '"')
        rows.append(u'    {"%s",\n     "%s"},' % (name, svg))

    body = HEADER + u'\n'.join(rows) + u'''
};

const size_t kIconSourceCount = sizeof(kIconSources) / sizeof(kIconSources[0]);

}  // namespace ui
'''
    io.open('source/ui/icons_data.cpp', 'w', encoding='utf-8', newline='').write(body)
    print('source/ui/icons_data.cpp :', os.path.getsize('source/ui/icons_data.cpp'), 'octets')


if __name__ == '__main__':
    main()
