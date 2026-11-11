"""Draft v3: write-free clean boots, stuck accepted as the residual.

- Generation g lives in log unit g mod nl, so the switch target is known without reading it.
- Pair: marker k, entry e = (gen, record, version, cid), confirm c. An entry counts only
  when it reads valid and its generation matches its unit.
- A unit is usable when a word at position 1 or below reads programmed. A carry (position 0)
  is only written right after an erase that returned; anything at position >= 1 was
  programmed after a carry confirm returned, or into a usable unit. A unit is erased again
  only when nothing at position >= 1 reads programmed, so the cells of a torn-erase ghost at
  positions >= 1 were never programmed, and appends (always after the last word that reads
  programmed, at position >= 1) never land in weakly erased cells.
- The scan shows a decision without writing only from the newest started position of the
  active unit, at position >= 1, with its confirm reading programmed. Otherwise it appends
  a re-commit.
- Erasing the next unit requires a pair at position >= 1 of the active unit whose confirm
  reads programmed: its entry is then solid and covers everything shown before. A full
  active unit without one is stuck, the accepted residual."""
from ewlf_model import E, V, SOLID, ProgErr, ProtoError


class Stuck(ProtoError):
    pass


class Ctx:
    def __init__(self, unit, gen, rec, version, cid):
        self.unit, self.gen, self.rec, self.version, self.cid = unit, gen, rec, version, cid

    def decision(self):
        return (self.rec, self.version, self.cid)


def format(geo):
    flash = [(SOLID, None)] * len(geo.addrs)
    u, gen = geo.nd - 1, 1
    l = gen % geo.nl
    for f, val in (('s', 'Z'), ('d', 0), ('m', ('m', 1, 0))):
        flash[geo.index[('D', u, 0, f)]] = (SOLID, val)
    for q in (0, 1):
        for f, val in (('k', 'Z'), ('e', ('e', gen, (u, 0), 1, 0)), ('c', 'Z')):
            flash[geo.index[('L', l, q, f)]] = (SOLID, val)
    return tuple(flash)


class Unit:
    def __init__(self, io, geo, l):
        self.l = l
        self.entries = []               # (q, gen, decision), valid and of this unit
        self.started = -1               # last position with a word reading programmed
        self.usable = False
        self.proven = False             # a pair at q >= 1 whose confirm reads programmed
        for q in range(geo.ppu):
            r = {f: io.read(('L', l, q, f)) for f in 'kec'}
            if any(x[0] != E for x in r.values()):
                self.started = q
                if q >= 1:
                    self.usable = True
            e = r['e']
            if e[0] == V and e[1][1] % geo.nl == l:
                self.entries.append((q, e[1][1], e[1][2:]))
                if q >= 1 and r['c'][0] != E:
                    self.proven = True
        self.gen = max((g for _, g, _ in self.entries), default=None)

    def confirmed(self, io, q):
        return io.read(('L', self.l, q, 'c'))[0] != E


def located(io, decision):
    (u, p), version, cid = decision
    return io.read(('D', u, p, 'm')) == (V, ('m', version, cid)) and \
        io.read(('D', u, p, 'd')) == (V, cid)


def switch(io, geo, units, x, gen, carry, decision):
    """Opens generation gen + 1 in the next unit, from active unit x."""
    t = (gen + 1) % geo.nl
    tu = units[t]
    pair0_only = not tu.usable and all(g == gen + 1 for _, g, _ in tu.entries)
    # every other unit holds a newer life than the target's old one: a proven pair in any of
    # them is a solid entry covering everything the target ever showed
    if not pair0_only and not any(u.proven for u in units if u.l != t):
        raise Stuck('no proven pair outside the switch target')
    io.erase('L', t)
    for q, val in ((0, carry), (1, decision)):
        for f, v in (('k', 'Z'), ('e', ('e', gen + 1, *val)), ('c', 'Z')):
            io.prog(('L', t, q, f), v)
    return t, gen + 1


def append(io, geo, units, x, gen, carry, decision):
    q = x.started + 1
    while q < geo.ppu:
        try:
            for f, v in (('k', 'Z'), ('e', ('e', gen, *decision)), ('c', 'Z')):
                io.prog(('L', x.l, q, f), v)
            return x.l, gen
        except ProgErr:
            q += 1
    return switch(io, geo, units, x, gen, carry, decision)


def active(io, geo):
    units = [Unit(io, geo, l) for l in range(geo.nl)]
    gens = sorted((u.gen for u in units if u.gen is not None), reverse=True)
    for g in gens:
        x = units[g % geo.nl]
        if x.usable:
            return units, x, g
    raise ProtoError('no usable log unit')


def scan(io, geo):
    units, x, gen = active(io, geo)
    for q, g, decision in sorted(x.entries, reverse=True):
        if g != gen or not located(io, decision):
            continue
        ctx = Ctx(x.l, gen, *decision)
        if q == x.started and q >= 1 and x.confirmed(io, q):
            return ctx
        if x.gen != gen or units[(gen + 1) % geo.nl].gen == gen + 1:
            # a newer, pair-0-only attempt exists: redo the switch
            ctx.unit, ctx.gen = switch(io, geo, units, x, gen, decision, decision)
        else:
            ctx.unit, ctx.gen = append(io, geo, units, x, gen, decision, decision)
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
    decision = ((u, p), version, cid)
    units = [Unit(io, geo, l) for l in range(geo.nl)]
    x = units[ctx.unit]
    if units[(ctx.gen + 1) % geo.nl].gen == ctx.gen + 1 and (ctx.gen + 1) % geo.nl != ctx.unit:
        l, gen = switch(io, geo, units, x, ctx.gen, ctx.decision(), decision)
    else:
        l, gen = append(io, geo, units, x, ctx.gen, ctx.decision(), decision)
    return Ctx(l, gen, *decision)
