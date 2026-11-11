"""EWLF.md as first written, plus a marker word at the start of each log pair (the fix
for the program over a weak entry that reads erased). Exposes holes 1 and 2."""
import proto_v1
from proto_v1 import Ctx, format as _format, entries, locate, scan as _scan, update as _update
from ewlf_model import E, ProgErr, SOLID


def format(geo):
    flash = list(_format(geo))
    flash[geo.index[('L', geo.nl - 1, 0, 'k')]] = (SOLID, 'Z')
    return tuple(flash)


def log_room(io, geo):
    ents = entries(io, geo)
    l = max(ents)[3] if ents else geo.nl - 1
    for q in range(geo.ppu):
        if io.read(('L', l, q, 'k'))[0] == E:
            return l, q
    other = (l - 1) % geo.nl
    io.erase('L', other)
    return other, 0


def append(io, geo, where, val):
    l, q = where
    while True:
        if q == geo.ppu:                # full: the other log unit is erased
            l = (l - 1) % geo.nl
            io.erase('L', l)
            q = 0
        try:
            io.prog(('L', l, q, 'k'), 'Z')
            break
        except ProgErr:
            q += 1                      # started after all: step over
    io.prog(('L', l, q, 'e'), val)
    io.prog(('L', l, q, 'c'), 'Z')


proto_v1.log_room = log_room
proto_v1.append = append
scan = _scan
update = _update
