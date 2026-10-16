#!/usr/bin/env python3
"""Builds the Project Zomboid build 41 data for the Switch port from your own copy of the game.

    python3 setup-sd.py <projectzomboid folder> <output folder>

<projectzomboid folder> is the LINUX build 41.78 of the game (the folder with media/, stdlib.lbc and libPZPopMan64.so);
see README.txt for how to download it. <output folder> gets a pz41/ folder: copy it to the root of the SD card, next to
the pz41/ folder from the zip (they merge). Needs about 7 GB free.
"""
import os, shutil, sys, tarfile

PART = 3900 * 1024 * 1024   # FAT32-safe part size; every part but the last must be exactly this big
NATIVES = ['libfmodintegration64.so', 'libjassimp64.so', 'libLighting64.so', 'libPZBullet64.so', 'libPZBulletNoOpenGL64.so',
           'libPZClipper64.so', 'libPZPopMan64.so', 'libRakNet64.so', 'libZNetNoSteam64.so']


class SplitWriter:
    """File-like sink that writes game.tar.00, .01, ... of PART bytes each."""
    def __init__(self, folder):
        self.folder, self.n, self.left, self.f = folder, 0, 0, None

    def write(self, b):
        n, b = len(b), memoryview(b)
        while len(b):
            if not self.left:
                if self.f: self.f.close()
                name = os.path.join(self.folder, 'game.tar.%02d' % self.n)
                print('  writing', name)
                self.f, self.n, self.left = open(name, 'wb'), self.n + 1, PART
            k = min(self.left, len(b))
            self.f.write(b[:k]); b = b[k:]; self.left -= k
        return n

    def close(self):
        if self.f: self.f.close()


def main():
    if len(sys.argv) != 3: sys.exit(__doc__)
    game, out = sys.argv[1], os.path.join(sys.argv[2], 'pz41')
    for f in ['media', 'stdlib.lbc', 'libPZPopMan64.so', 'linux64/libfmod.so.13.6']:
        if not os.path.exists(os.path.join(game, f)):
            sys.exit('%s is missing %s: this needs the Linux build 41.78 game folder (see README.txt)' % (game, f))
    xfer, nat = os.path.join(out, 'xfer'), os.path.join(out, 'game', 'natives')
    os.makedirs(xfer, exist_ok=True); os.makedirs(nat, exist_ok=True)

    print('game data -> %s (6.6 GB, takes a while)' % xfer)
    for f in os.listdir(xfer):
        if f.startswith('game.tar.') or f == 'game.idx': os.remove(os.path.join(xfer, f))
    sink = SplitWriter(xfer)
    with tarfile.open(fileobj=sink, mode='w|', format=tarfile.GNU_FORMAT) as t:
        for f in ['media', 'stdlib.lbc', 'stdlib.lua', 'serialize.lua']:
            t.add(os.path.join(game, f), arcname=f)
    sink.close()

    print('native libraries ->', nat)
    for f in NATIVES: shutil.copy(os.path.join(game, f), nat)
    for lib in ['fmod', 'fmodstudio']:
        src = os.path.join(game, 'linux64', 'lib%s.so.13.6' % lib)
        for name in ['lib%s.so.13' % lib, 'lib%s.so' % lib]: shutil.copy(src, os.path.join(nat, name))
    print('done: copy %s to the root of the SD card' % out)


if __name__ == '__main__':
    main()
