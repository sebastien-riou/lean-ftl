"""EWLF.md as first written: entries ordered by seq, computed from the entries that read
valid; the active log unit is the one holding the highest valid seq; a full log unit makes
the update erase the other one."""
from ewlf_model import ProtoError, SOLID, V, E


class Ctx:
    def __init__(self, rec, version, cid):
        self.rec, self.version, self.cid = rec, version, cid


def format(geo):
    flash = [(SOLID, None)] * len(geo.addrs)
    u = geo.nd - 1
    for f, val in (('s', 'Z'), ('d', 0), ('m', ('m', 1, 0))):
        flash[geo.index[('D', u, 0, f)]] = (SOLID, val)
    l = geo.nl - 1
    flash[geo.index[('L', l, 0, 'e')]] = (SOLID, ('e', 1, 1, 0))
    flash[geo.index[('L', l, 0, 'c')]] = (SOLID, 'Z')
    return tuple(flash)


def entries(io, geo):
    """Valid entries as (seq, version, cid, l, q), in scan order."""
    out = []
    for l in range(geo.nl):
        for q in range(geo.ppu):
            r = io.read(('L', l, q, 'e'))
            if r[0] == V:
                _, seq, version, cid = r[1]
                out.append((seq, version, cid, l, q))
    return out


def locate(io, geo, version, cid):
    for u in range(geo.nd):
        for p in range(geo.rpu):
            if io.read(('D', u, p, 'm')) == (V, ('m', version, cid)) and \
                    io.read(('D', u, p, 'd')) == (V, cid):
                return (u, p)
    return None


def log_room(io, geo):
    """Free pair position in the active log unit, erasing the other one when it is full."""
    ents = entries(io, geo)
    l = max(ents)[3] if ents else geo.nl - 1
    for q in range(geo.ppu):
        if io.read(('L', l, q, 'e'))[0] == E and io.read(('L', l, q, 'c'))[0] == E:
            return l, q
    other = (l - 1) % geo.nl
    io.erase('L', other)
    return other, 0


def append(io, geo, where, val):
    l, q = where
    io.prog(('L', l, q, 'e'), val)
    io.prog(('L', l, q, 'c'), 'Z')


def scan(io, geo):
    ents = sorted(entries(io, geo), key=lambda e: -e[0])
    for seq, version, cid, l, q in ents:
        rec = locate(io, geo, version, cid)
        if rec is None:
            continue
        if io.read(('L', l, q, 'c'))[0] != E:
            return Ctx(rec, version, cid)
        top = max(e[0] for e in ents)
        append(io, geo, log_room(io, geo), ('e', top + 1, version, cid))
        return Ctx(rec, version, cid)
    raise ProtoError('no valid version')


def update(io, geo, ctx, cid):
    u = ctx.rec[0]
    where = None
    for p in range(geo.rpu):
        if all(io.read(('D', u, p, f))[0] == E for f in 'sdm'):
            where = (u, p)
            break
    pair = log_room(io, geo)
    if where is None:
        target = (u - 1) % geo.nd
        io.erase('D', target)
        where = (target, 0)
    u, p = where
    version = ctx.version + 1
    io.prog(('D', u, p, 's'), 'Z')
    io.prog(('D', u, p, 'd'), cid)
    io.prog(('D', u, p, 'm'), ('m', version, cid))
    top = max((e[0] for e in entries(io, geo)), default=0)
    append(io, geo, pair, ('e', top + 1, version, cid))
    return Ctx(where, version, cid)
