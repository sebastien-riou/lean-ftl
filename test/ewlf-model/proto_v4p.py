"""EWLF v4, record-in-the-pair variant: the data lives in the log pair, no data units.

A position is four words programmed in this order, each after the previous returned: marker
k (zeros), data d, meta m = (gen, version, cid), confirm c (zeros). An entry is valid when m
reads valid with the generation parity of its unit and d reads the data m names (on the chip
the CRC of the meta field covers the data). Everything else is proto_v4, which EWLF.md
documents: ppu regular positions, nres reserved ones, nx certificate words per log unit, the
decision D = newest valid entry of the live unit, the bury before showing, the switch with
its reserved pair, erase and certificate, the stuck conditions. A bury or a carry copies the
data (one word here; a cut inside a multi-word data field leaves m erased, the same reading).

Geometry: nl,ppu,nres,nx (log units, always 2; regular positions; reserved; certificates).
Negative controls: V4_KNOCKOUT=step1|bury|cert|empty, as for proto_v4.
"""
import os

from ewlf_model import E, V, SOLID, ProgErr, ProtoError

KNOCKOUT = os.environ.get('V4_KNOCKOUT', '')


class Stuck(ProtoError):
    pass


class Geometry:
    def __init__(self, nl, ppu, nres=1, nx=1):
        assert nl == 2
        self.nl, self.ppu, self.nres, self.nx = nl, ppu, nres, nx
        self.nd = self.rpu = 0
        self.addrs = []
        for l in range(nl):
            for q in range(ppu + nres):
                for f in 'kdmc':
                    self.addrs.append(('L', l, q, f))
            for i in range(nx):
                self.addrs.append(('L', l, i, 'x'))
        self.index = {a: i for i, a in enumerate(self.addrs)}
        self.units = {}
        for i, a in enumerate(self.addrs):
            self.units.setdefault((a[0], a[1]), []).append(i)


class Ctx:
    def __init__(self, unit, gen, version, cid):
        self.unit, self.gen, self.version, self.cid = unit, gen, version, cid


def format(geo):
    flash = [(SOLID, None)] * len(geo.addrs)
    for f, val in (('k', 'Z'), ('d', 0), ('m', ('m', 1, 1, 0)), ('c', 'Z')):
        flash[geo.index[('L', 1, 0, f)]] = (SOLID, val)
    return tuple(flash)


class Unit:
    def __init__(self, io, geo, l):
        self.l = l
        self.entries = []        # (q, gen, (version, cid)) valid, newest last
        self.started = -1
        self.proven = False
        self.closed = False
        self.certified = False
        for q in range(geo.ppu + geo.nres):
            r = {f: io.read(('L', l, q, f)) for f in 'kdmc'}
            if any(x[0] != E for x in r.values()):
                self.started = q
                if q >= geo.ppu:
                    self.closed = True
            m = r['m']
            if m[0] == V and m[1][1] % 2 == l and r['d'] == (V, m[1][3]):
                self.entries.append((q, m[1][1], m[1][2:]))
                if r['c'][0] != E:
                    self.proven = True
        for i in range(geo.nx):
            if io.read(('L', l, i, 'x'))[0] != E:
                self.closed = self.certified = True
        self.gen = max((g for _, g, _ in self.entries), default=None)

    def uncertain(self, L):
        return self.started >= 0 and self.gen is None and L.certified

    def confirmed(self, io, q):
        return io.read(('L', self.l, q, 'c'))[0] != E


def live(io, geo):
    units = [Unit(io, geo, l) for l in range(2)]
    cands = [u for u in units if u.gen is not None]
    if not cands:
        raise ProtoError('no valid entry in any log unit')
    L = max(cands, key=lambda u: u.gen)
    return L, units[1 - L.l]


def prog_pair(io, l, q, gen, entry):
    version, cid = entry
    for f, v in (('k', 'Z'), ('d', cid), ('m', ('m', gen, version, cid)), ('c', 'Z')):
        io.prog(('L', l, q, f), v)


def fresh_pair(io, U, lo, hi, gen, entry):
    q = max(lo, U.started + 1)
    while q < hi:
        try:
            prog_pair(io, U.l, q, gen, entry)
            U.started = q
            U.entries.append((q, gen, entry))
            U.gen, U.proven = gen, True
            return q
        except ProgErr:
            U.started = q
            q += 1
    return None


def fresh_cert(io, geo, L):
    if KNOCKOUT == 'cert':
        return
    for i in range(geo.nx):
        if io.read(('L', L.l, i, 'x'))[0] == E:
            try:
                io.prog(('L', L.l, i, 'x'), 'Z')
                L.certified = L.closed = True
                return
            except ProgErr:
                continue
    raise Stuck('certificates exhausted')


def switch(io, geo, L, O, entry):
    gen = L.gen + 1
    if not L.certified and not L.proven and KNOCKOUT != 'step1':
        if fresh_pair(io, L, geo.ppu, geo.ppu + geo.nres, L.gen, L.entries[-1][2]) is None:
            raise Stuck('reserved pairs exhausted')
    if not L.certified or (O.started < 0 and KNOCKOUT != 'empty'):
        io.erase('L', O.l)
        O.started, O.entries, O.gen, O.proven = -1, [], None, False
        fresh_cert(io, geo, L)
    if fresh_pair(io, O, 0, geo.ppu + geo.nres, gen, entry) is None:
        raise Stuck('target unit full of started positions')
    return O


def append(io, geo, L, O, entry):
    if not L.closed and fresh_pair(io, L, 0, geo.ppu, L.gen, entry) is not None:
        return L
    return switch(io, geo, L, O, entry)


def top_solid(io, L, O):
    q = L.entries[-1][0]
    return L.confirmed(io, q) and L.started == q and not O.uncertain(L)


def bury(io, geo, L, O):
    entry = L.entries[-1][2]
    if O.uncertain(L):
        return switch(io, geo, L, O, entry)
    if not L.closed and fresh_pair(io, L, 0, geo.ppu, L.gen, entry) is not None:
        return L
    if fresh_pair(io, L, geo.ppu, geo.ppu + geo.nres, L.gen, entry) is not None:
        return L
    return switch(io, geo, L, O, entry)


def scan(io, geo):
    L, O = live(io, geo)
    q, gen, entry = L.entries[-1]
    if not top_solid(io, L, O) and KNOCKOUT != 'bury':
        U = bury(io, geo, L, O)
        return Ctx(U.l, U.gen, *entry)
    return Ctx(L.l, gen, *entry)


def update(io, geo, ctx, cid):
    L, O = live(io, geo)
    entry = (ctx.version + 1, cid)
    U = append(io, geo, L, O, entry)
    return Ctx(U.l, U.gen, *entry)
