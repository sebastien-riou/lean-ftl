"""Draft v2.

- Every position starts with a zero marker, programmed first; a position is free only when
  its marker reads erased, and only positions below the newest started one are used.
- Entries are ordered by (log-unit generation, position): nothing computed from readings
  orders anything.
- A log unit's life opens with a carry pair at position 0, naming the decision current at
  the switch, and a second pair is programmed at once. The unit is permanent when a pair at
  position 1 or below has its entry or confirm programmed: its marker then returned, so the
  life is real (its last erase returned) and stays live.
- Only a permanent unit is shown from, appended to at a later boot, or the base of a switch,
  which erases the other unit. A unit that is not permanent never showed anything and is
  ignored until the next switch erases it again.
- The scan shows a decision only from the newest started position of the active unit,
  confirmed, and never from a carry; otherwise it appends a re-commit first."""
from ewlf_model import E, V, SOLID, ProgErr, ProtoError


class Ctx:
    def __init__(self, unit, gen, rec, version, cid):
        self.unit, self.gen, self.rec, self.version, self.cid = unit, gen, rec, version, cid

    def decision(self):
        return (self.rec, self.version, self.cid)


def format(geo):
    flash = [(SOLID, None)] * len(geo.addrs)
    u, l = geo.nd - 1, geo.nl - 1
    for f, val in (('s', 'Z'), ('d', 0), ('m', ('m', 1, 0))):
        flash[geo.index[('D', u, 0, f)]] = (SOLID, val)
    for q in (0, 1):
        for f, val in (('k', 'Z'), ('e', ('e', 1, (u, 0), 1, 0)), ('c', 'Z')):
            flash[geo.index[('L', l, q, f)]] = (SOLID, val)
    return tuple(flash)


def permanent_gen(io, geo, l):
    """The generation of a permanent log unit, else None."""
    if io.read(('L', l, 0, 'k'))[0] == E:
        return None
    e = io.read(('L', l, 0, 'e'))
    if e[0] != V:
        return None
    for q in range(1, geo.ppu):
        if io.read(('L', l, q, 'e'))[0] != E or io.read(('L', l, q, 'c'))[0] != E:
            return e[1][1]
    return None


def newest_started(io, geo, l):
    return max((q for q in range(geo.ppu) if io.read(('L', l, q, 'k'))[0] != E), default=-1)


def located(io, rec, version, cid):
    u, p = rec
    return io.read(('D', u, p, 'm')) == (V, ('m', version, cid)) and \
        io.read(('D', u, p, 'd')) == (V, cid)


def program_pair(io, l, q, val):
    io.prog(('L', l, q, 'k'), 'Z')
    io.prog(('L', l, q, 'e'), val)
    io.prog(('L', l, q, 'c'), 'Z')


def append(io, geo, ctx, decision):
    """Programs decision below the newest started position of ctx's unit, which is
    permanent, or switches to the other unit with a carry of ctx's decision. Returns the
    unit and generation the decision ended up in."""
    l, gen = ctx.unit, ctx.gen
    q = newest_started(io, geo, l) + 1
    while q < geo.ppu:
        try:
            program_pair(io, l, q, ('e', gen, *decision))
            return l, gen
        except ProgErr:
            q += 1
    l, gen = (l - 1) % geo.nl, gen + 1
    io.erase('L', l)
    program_pair(io, l, 0, ('e', gen, *ctx.decision()))
    program_pair(io, l, 1, ('e', gen, *decision))
    return l, gen


def scan(io, geo):
    best = None
    for l in range(geo.nl):
        g = permanent_gen(io, geo, l)
        if g is not None and (best is None or g > best[1]):
            best = (l, g)
    if best is None:
        raise ProtoError('no permanent log unit')
    l, gen = best
    newest = newest_started(io, geo, l)
    for q in range(newest, -1, -1):
        e = io.read(('L', l, q, 'e'))
        if e[0] != V or e[1][1] != gen:
            continue
        _, _, rec, version, cid = e[1]
        if not located(io, rec, version, cid):
            continue
        ctx = Ctx(l, gen, rec, version, cid)
        if q == newest and q > 0 and io.read(('L', l, q, 'c'))[0] != E:
            return ctx
        ctx.unit, ctx.gen = append(io, geo, ctx, ctx.decision())
        return ctx
    raise ProtoError('no decision in the active unit')


def update(io, geo, ctx, cid):
    u = ctx.rec[0]
    p = 0
    while True:
        while p < geo.rpu and io.read(('D', u, p, 's'))[0] != E:
            p += 1
        if p == geo.rpu:
            u = next(x for x in ((u - i) % geo.nd for i in range(1, geo.nd))
                     if x != ctx.rec[0])
            io.erase('D', u)
            p = 0
        try:
            io.prog(('D', u, p, 's'), 'Z')
            break
        except ProgErr:
            p += 1
    version = ctx.version + 1
    io.prog(('D', u, p, 'd'), cid)
    io.prog(('D', u, p, 'm'), ('m', version, cid))
    l, gen = append(io, geo, ctx, ((u, p), version, cid))
    return Ctx(l, gen, (u, p), version, cid)
