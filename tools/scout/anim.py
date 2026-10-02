#!/usr/bin/env python3
"""anim.py - what a POSE is, checked against the player's own capture.

An independent implementation of everything `src\\rrformats\\pose.h` does, written from the
disassembly rather than from that header, so the two can be compared.

Nothing here ships any game data: the part matrices come out of a guest RAM image under `work\\`,
the sine table out of the player's own `SLUS_010.53` and the attachment programs out of `RASHCDG.BIN`.

Commands
    anim.py slots [--ram R] [--geo G]     the live 22 part slots: which sub-mesh each one belongs
                                          to (by its DPD3 pointer) and the Euler triple that
                                          reproduces its 3x3
    anim.py rot   [--ram R] [--exe E]     the RotMatrix check on its own: matrices compared,
                                          matching, differing, worst element
    anim.py world [--ram R] ...           the composed world rotations of both objects
    anim.py info | live | verify [--mutate] [--only a,b] [--keep]
                                          the rider animation machine, RASHCDG 0x8005BD74..0x8005E848
                                          (anim_machine.py)

Addresses, all with provenance in docs\\formats\\rmd3.md section 11:
    0x801BDF1C  the player object's 5 part slots   (obj 0x801B65D4, +0x04)
    0x801BDF9C  the rider object's 17 part slots   (obj 0x801BB2EC, +0x04)
    0x8005624C  the game's 4096-entry (sin, cos) table, 4096 = 1.0 and 4096 = one turn
"""
import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rmd3  # noqa: E402

RAM_BASE = 0x80000000
EXE_LOAD = 0x80010000
EXE_HEADER = 0x800
SINE_TABLE = 0x8005624C
BIKE_SLOTS = (0x801BDF1C, 5, 100)
RIDER_SLOTS = (0x801BDF9C, 17, 150)
PLAYER_OBJECT = 0x801B65D4
RIDER_OBJECT = 0x801BB2EC

DEFAULT_RAM = os.path.join('work', 'oracle', 'state', 'rr-race', 'ram.bin')
DEFAULT_EXE = os.path.join('work', 'disc_us', 'SLUS_010.53')
DEFAULT_GEO = os.path.join('work', 'disc_us', 'DATA', 'BBLEVEL1.GEO')
DEFAULT_OVERLAY = os.path.join('work', 'disc_us', 'RASHCDG.BIN')


def load(path, what):
    if not os.path.exists(path):
        sys.exit('%s not found: %s' % (what, path))
    with open(path, 'rb') as handle:
        return handle.read()


class Sine(object):
    """The game's own quarter-free sine table: 4096 entries of (s16 sin, s16 cos)."""

    def __init__(self, exe):
        if exe[:8] != b'PS-X EXE':
            sys.exit('not a PS-X EXE')
        load_address = struct.unpack_from('<I', exe, 0x18)[0]
        at = EXE_HEADER + (SINE_TABLE - load_address)
        self.sin = []
        self.cos = []
        for i in range(4096):
            s, c = struct.unpack_from('<hh', exe, at + 4 * i)
            self.sin.append(s)
            self.cos.append(c)
        if (self.sin[0], self.cos[0]) != (0, 4096) or (self.sin[1024], self.cos[1024]) != (4096, 0):
            sys.exit('the table at %08x does not read as (sin, cos)' % SINE_TABLE)


def mul(a, b):
    out = [0] * 9
    for r in range(3):
        for c in range(3):
            out[r * 3 + c] = (a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] +
                              a[r * 3 + 2] * b[2 * 3 + c]) >> 12
    return out


def transpose(a):
    return [a[c * 3 + r] for r in range(3) for c in range(3)]


def rot_zyx(table, ax, ay, az):
    """RotMatrix in the psyq order: M = Rz(az) * Ry(ay) * Rx(ax)."""
    sx, cx = table.sin[ax & 0xFFF], table.cos[ax & 0xFFF]
    sy, cy = table.sin[ay & 0xFFF], table.cos[ay & 0xFFF]
    sz, cz = table.sin[az & 0xFFF], table.cos[az & 0xFFF]
    rx = [4096, 0, 0, 0, cx, -sx, 0, sx, cx]
    ry = [cy, 0, sy, 0, 4096, 0, -sy, 0, cy]
    rz = [cz, -sz, 0, sz, cz, 0, 0, 0, 4096]
    return mul(mul(rz, ry), rx)


def worst(a, b):
    return max(abs(a[i] - b[i]) for i in range(9))


def decompose(table, m):
    """The closed-form inverse of rot_zyx, refined over a +-3 neighbourhood."""
    f = [v / 4096.0 for v in m]
    y = math.asin(max(-1.0, min(1.0, -f[6])))
    if abs(math.cos(y)) > 1e-6:
        x = math.atan2(f[7], f[8])
        z = math.atan2(f[3], f[0])
    else:
        x, z = 0.0, math.atan2(-f[1], f[4])
    turn = 4096.0 / (2.0 * math.pi)
    seed = [int(round(a * turn)) & 0xFFF for a in (x, y, z)]
    best = None
    for dx in range(-3, 4):
        for dy in range(-3, 4):
            for dz in range(-3, 4):
                trial = ((seed[0] + dx) & 0xFFF, (seed[1] + dy) & 0xFFF, (seed[2] + dz) & 0xFFF)
                e = worst(rot_zyx(table, *trial), m)
                if best is None or e < best[1]:
                    best = (trial, e)
    return best


def read_slots(ram, address, count):
    out = []
    at = address - RAM_BASE
    for i in range(count):
        record = at + 24 * i
        pointer = struct.unpack_from('<I', ram, record)[0]
        matrix = list(struct.unpack_from('<9h', ram, record + 4))
        out.append((pointer, matrix))
    return out


def signed(a):
    return a - 4096 if a > 2048 else a


def geo_group0(geo_path, model_id):
    model = rmd3.Model(geo_path)
    for obj in model.objects:
        if obj.id == model_id:
            return obj, obj.groups[0]
    sys.exit('model %d is not in %s' % (model_id, geo_path))


def cmd_slots(args):
    ram = load(args.ram, 'RAM image')
    exe = load(args.exe, 'SLUS_010.53')
    table = Sine(exe)
    total = matching = 0
    worst_element = 0
    named = 0
    for address, count, model_id in (BIKE_SLOTS, RIDER_SLOTS):
        obj, group = geo_group0(args.geo, model_id)
        slots = read_slots(ram, address, count)
        # Where the model itself sits in RAM: the object's +0x00 is its group-0 DOD3, and a DOD3
        # is at RMD3 + 0x10, so the RMD3 base follows. Sub-mesh i's DPD3 is then at the same
        # distance from that base as it is from the RMD3 tag in the file.
        object_address = PLAYER_OBJECT if model_id == 100 else RIDER_OBJECT
        dod3 = struct.unpack_from('<I', ram, object_address - RAM_BASE)[0]
        base = dod3 - 0x10
        print('== model %d, %d slots at %08x (RMD3 in RAM at %08x)' % (model_id, count, address, base))
        for i, (pointer, matrix) in enumerate(slots):
            expected = base + (group.subs[i].off - obj.off) if i < len(group.subs) else 0
            names = pointer == expected
            named += 1 if names else 0
            angles, error = decompose(table, matrix)
            total += 1
            matching += 1 if error <= 3 else 0
            worst_element = max(worst_element, error)
            print('  slot %2d dpd3 %08x  %s sub-mesh %2d  angles (%5d,%5d,%5d)  worst element %d' % (
                i, pointer, 'IS' if names else 'is NOT', i,
                signed(angles[0]), signed(angles[1]), signed(angles[2]), error))
    print('\nslots naming their own sub-mesh: %d of %d' % (named, total))
    print('matrices compared %d, matching (<= 3 of 4096) %d, differing %d, worst element %d' % (
        total, matching, total - matching, worst_element))


def cmd_rot(args):
    ram = load(args.ram, 'RAM image')
    table = Sine(load(args.exe, 'SLUS_010.53'))
    total = matching = 0
    worst_element = 0
    moved = moved_matching = 0
    for address, count, _ in (BIKE_SLOTS, RIDER_SLOTS):
        for _, matrix in read_slots(ram, address, count):
            angles, _ = decompose(table, matrix)
            built = rot_zyx(table, *angles)
            control = built
            if args.mutate:
                rx = rot_zyx(table, angles[0], 0, 0)
                ry = rot_zyx(table, 0, angles[1], 0)
                rz = rot_zyx(table, 0, 0, angles[2])
                control = mul(mul(rx, ry), rz)
            error = worst(control, matrix)
            total += 1
            worst_element = max(worst_element, error)
            ok = error <= 3
            matching += 1 if ok else 0
            if args.mutate and worst(control, built) > 3:
                moved += 1
                moved_matching += 1 if ok else 0
    print('matrices compared %d, matching %d, differing %d, worst element %d of 4096' % (
        total, matching, total - matching, worst_element))
    if args.mutate:
        print('matrices the mutation did not move at all %d, of the rest still matching %d' % (
            total - moved, moved_matching))
        print('verdict %s' % ('PASS' if moved_matching == 0 else 'FAIL'))
    else:
        print('verdict %s' % ('PASS' if matching == total else 'FAIL'))


def cmd_world(args):
    ram = load(args.ram, 'RAM image')
    skeleton = rmd3.Skeleton(args.overlay)
    for address, count, model_id in (BIKE_SLOTS, RIDER_SLOTS):
        obj, group = geo_group0(args.geo, model_id)
        slots = read_slots(ram, address, count)
        origins, program, error = rmd3.group_origins(group, skeleton)
        links = skeleton.progs[program]
        words = skeleton.words[skeleton.index[program][0]:]
        # The matrix a link loads is named by bits 23..27 of its own word, NOT by the child index.
        matrix_part = []
        for w in words:
            if w == 0:
                break
            if (w & 3) == 3:
                continue
            matrix_part.append((w >> 23) & 0x1F)
            if len(matrix_part) >= len(links):
                break
        world = {0: slots[0][1]}
        print('== model %d, program %d (box error %s)' % (model_id, program, error))
        for child in range(1, count):
            parent, _ = links[child - 1]
            slot = matrix_part[child - 1]
            world[child] = mul(world[parent], slots[slot][1])
            print('  part %2d <- parent %2d, matrix from slot %2d  world [%6d %6d %6d | %6d %6d %6d '
                  '| %6d %6d %6d]' % ((child, parent, slot) + tuple(world[child])))


def main():
    if len(sys.argv) > 1 and sys.argv[1] in ('info', 'live', 'verify'):
        # the rider animation machine: skeleton, data, live models, facts
        import anim_machine
        sys.exit(anim_machine.main(sys.argv[1:]))
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command')
    for name in ('slots', 'rot', 'world'):
        p = sub.add_parser(name)
        p.add_argument('--ram', default=DEFAULT_RAM)
        p.add_argument('--exe', default=DEFAULT_EXE)
        p.add_argument('--geo', default=DEFAULT_GEO)
        p.add_argument('--overlay', default=DEFAULT_OVERLAY)
        p.add_argument('--mutate', action='store_true',
                       help='negative control: build the triple in XYZ order instead of ZYX')
    args = parser.parse_args()
    if args.command == 'slots':
        cmd_slots(args)
    elif args.command == 'rot':
        cmd_rot(args)
    elif args.command == 'world':
        cmd_world(args)
    else:
        parser.print_help()


if __name__ == '__main__':
    main()
