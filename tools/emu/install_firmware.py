# -*- coding: utf-8 -*-
"""Installe un firmware Switch dans le dossier de donnees de l'emulateur.

L'installateur de l'emulateur n'existe que dans son interface graphique. Ici on
refait exactement ce qu'il fait (ContentManager.InstallFromZip) : chaque .nca de
l'archive devient un dossier <ncaId>.nca contenant un fichier « 00 ».

    python3 install_firmware.py <firmware.zip|dossier> <root-data-dir>
"""
import os
import shutil
import sys
import zipfile


def nca_id(name):
    parts = name.replace('.cnmt', '').split('/')
    last = parts[-1]
    if last == '00' and len(parts) >= 2:
        last = parts[-2]
    return last if last.endswith('.nca') else None


def install(source, registered):
    if os.path.isdir(registered):
        shutil.rmtree(registered)
    tmp = registered + '.tmp'
    if os.path.isdir(tmp):
        shutil.rmtree(tmp)
    os.makedirs(tmp)

    count = 0
    if os.path.isdir(source):
        for name in sorted(os.listdir(source)):
            ident = nca_id(name)
            if not ident:
                continue
            os.makedirs(os.path.join(tmp, ident))
            shutil.copyfile(os.path.join(source, name), os.path.join(tmp, ident, '00'))
            count += 1
    else:
        with zipfile.ZipFile(source) as archive:
            for entry in archive.namelist():
                if not (entry.endswith('.nca') or entry.endswith('.nca/00')):
                    continue
                ident = nca_id(entry)
                if not ident:
                    continue
                os.makedirs(os.path.join(tmp, ident), exist_ok=True)
                with archive.open(entry) as src, \
                        open(os.path.join(tmp, ident, '00'), 'wb') as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
                count += 1

    os.makedirs(os.path.dirname(registered), exist_ok=True)
    os.rename(tmp, registered)
    return count


def main():
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    source, root = sys.argv[1], sys.argv[2]
    registered = os.path.join(root, 'bis', 'system', 'Contents', 'registered')
    count = install(source, registered)
    print('%d NCA installes dans %s' % (count, registered))
    return 0 if count else 1


if __name__ == '__main__':
    sys.exit(main())
