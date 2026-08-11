# -*- coding: utf-8 -*-
"""Fabrique des .torrent + leurs donnees pour le bac a sable PC."""
import hashlib
import io
import os
import sys

PIECE = 32 * 1024


def benc(v):
    if isinstance(v, int):
        return b'i' + str(v).encode() + b'e'
    if isinstance(v, bytes):
        return str(len(v)).encode() + b':' + v
    if isinstance(v, list):
        return b'l' + b''.join(benc(x) for x in v) + b'e'
    if isinstance(v, dict):
        out = b'd'
        for k in sorted(v):
            out += benc(k) + benc(v[k])
        return out + b'e'
    raise TypeError(type(v))


def make(root, inbox, name, files, private=False):
    """files : liste de (chemin relatif, taille)."""
    blob = b''
    entries = []
    single = len(files) == 1 and '/' not in files[0][0]

    for rel, size in files:
        data = (rel.encode() * ((size // max(1, len(rel))) + 1))[:size]
        full = os.path.join(root, name, rel) if not single else os.path.join(root, name)
        d = os.path.dirname(full)
        if d and not os.path.isdir(d):
            os.makedirs(d)
        io.open(full, 'wb').write(data)
        blob += data
        entries.append({b'length': size, b'path': [p.encode() for p in rel.split('/')]})

    hashes = b''
    for off in range(0, len(blob), PIECE):
        hashes += hashlib.sha1(blob[off:off + PIECE]).digest()

    info = {b'name': name.encode(), b'piece length': PIECE, b'pieces': hashes}
    if single:
        info[b'length'] = files[0][1]
    else:
        info[b'files'] = entries
    if private:
        info[b'private'] = 1

    meta = {
        b'info': info,
        b'announce': b'https://tracker.exemple.org/announce',
        b'announce-list': [[b'https://tracker.exemple.org/announce'],
                           [b'udp://ouvert.exemple.net:6969/announce']],
    }
    io.open(os.path.join(inbox, name + '.torrent'), 'wb').write(benc(meta))
    print(name, len(blob), 'octets')


def main():
    root = sys.argv[1]
    inbox = sys.argv[2]
    for d in (root, inbox):
        if not os.path.isdir(d):
            os.makedirs(d)

    make(root, inbox, 'Le.Voyage.Fantastique.2019.1080p.BluRay.x264-GROUPE',
         [('Le.Voyage.Fantastique.2019.1080p.mkv', 900 * 1024),
          ('Le.Voyage.Fantastique.2019.1080p.fr.srt', 4 * 1024),
          ('affiche.jpg', 60 * 1024)])

    make(root, inbox, 'Discographie - Groupe Imaginaire (FLAC)',
         [('01 - Ouverture.flac', 300 * 1024),
          ('02 - Second mouvement.flac', 280 * 1024),
          ('03 - Final.flac', 310 * 1024),
          ('pochette.png', 90 * 1024)])

    make(root, inbox, 'archives-photos-2024.zip', [('archives-photos-2024.zip', 700 * 1024)])

    make(root, inbox, 'Manuel-technique.pdf', [('Manuel-technique.pdf', 220 * 1024)])

    make(root, inbox, 'homebrew-collection', [('lanceur.nro', 400 * 1024),
                                              ('LISEZMOI.txt', 2 * 1024)], private=True)


if __name__ == '__main__':
    main()
