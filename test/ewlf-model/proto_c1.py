"""Chunked area, design c1: the EWLF log of proto_v4, unchanged, whose record is a map of
chunk locations, and a pool of erase units that holds the chunks. This module is the model of
CHUNKED.md; the document is the reference, this docstring the summary.

Pool: np units of spu slots. A slot is a start marker 's' (zeros) then the chunk 'd', whose
value is (chunk index, value). The map is (slot of each chunk or None, head unit): the data
word of the EWLF record. The commit of a map is the commit of an EWLF record, by the functions
of proto_v4, imported.

Rules of the pool:
- slots are taken in order in the head unit, above its last started slot; a slot whose marker
  the flash refuses counts as started and is stepped over (knock-out 'nostep': corrupt);
- units are used as a ring; the unit after the head is entered only when no chunk of the
  committed map, nor of the open transaction, is in it, and it is erased at that instant,
  whatever it reads (knock-out 'trust': corrupt);
- the reserve: after every commit, the unit after the head holds no live chunk. An operation
  that only writes may enter a unit when one more free unit follows it; a collection may enter
  the last one, since its commit frees the unit it evacuates (knock-out 'noreserve');
- collection: the live chunks of the first unit after the free ones are copied to the head,
  and the map committed; that unit is free from then on, and is erased when the head enters
  it, not before (knock-out 'early': corrupt);
- an operation checks the room it needs before it opens the map record, and collects first.
  When a refused marker leaves it short all the same, it abandons its record, collects, and
  starts again.

Operations: a plain write opens the map record, writes its chunks in chunk order, then commits
the map. A transaction writes its chunks first, each in a new slot, collecting when it needs
room, then commits the map; an abort forgets its slots, which stay started.

C1_BACKEND=abs replaces the EWLF log by its contract (proto_c1a.py): a commit that is cut
leaves the old map, or either map at each later boot until a repair returns, or the new one.
C1_COMBINE=1 lets a plain write and a transaction commit evacuate a unit in their own commit.
"""
import os

from ewlf_model import E, V, SOLID, PowerCut, ProgErr, ProtoError
import proto_v4 as v4

KNOCKOUT = os.environ.get('C1_KNOCKOUT', '')
BACKEND = os.environ.get('C1_BACKEND', 'v4')
COMBINE = os.environ.get('C1_COMBINE', '') == '1'

Stuck = v4.Stuck


class NoSpace(ProtoError):
    pass


class Short(Exception):
    """A unit entry would break the reserve: collect first."""


class Geometry:
    def __init__(self, nchunks, np_, spu, nd=2, rpu=2, ppu=2, nres=2, nx=2):
        self.nchunks, self.np, self.spu = nchunks, np_, spu
        self.nd, self.rpu, self.nl, self.ppu, self.nres, self.nx = nd, rpu, 2, ppu, nres, nx
        self.addrs = []
        for u in range(np_):
            for p in range(spu):
                for f in 'sd':
                    self.addrs.append(('P', u, p, f))
        if BACKEND == 'v4':
            for u in range(nd):
                for p in range(rpu):
                    for f in 'sdm':
                        self.addrs.append(('D', u, p, f))
            for l in range(2):
                for q in range(ppu + nres):
                    for f in 'kec':
                        self.addrs.append(('L', l, q, f))
                for i in range(nx):
                    self.addrs.append(('L', l, i, 'x'))
        else:
            self.addrs.append(BeAbs.REG)
        self.index = {a: i for i, a in enumerate(self.addrs)}
        self.units = {}
        for i, a in enumerate(self.addrs):
            self.units.setdefault((a[0], a[1]), []).append(i)


class BeV4:
    """The commit of a map by the EWLF log of proto_v4."""

    @staticmethod
    def format(geo, flash, mapval):
        u = geo.nd - 1
        for f, val in (('s', 'Z'), ('d', mapval), ('m', ('m', 1, mapval))):
            flash[geo.index[('D', u, 0, f)]] = (SOLID, val)
        for f, val in (('k', 'Z'), ('e', ('e', 1, (u, 0), 1, mapval)), ('c', 'Z')):
            flash[geo.index[('L', 1, 0, f)]] = (SOLID, val)

    @staticmethod
    def phase1(io, geo):
        L, O = v4.live(io, geo)
        q, gen, entry = L.entries[-1]
        if not v4.located(io, entry):
            raise ProtoError('decision names no record')
        return (L, O, gen, entry)

    @staticmethod
    def mapval(h):
        return h[3][2]

    @staticmethod
    def phase2(io, geo, h):
        L, O, gen, entry = h
        if not v4.top_solid(io, L, O):
            U = v4.bury(io, geo, L, O)
            return v4.Ctx(U.l, U.gen, *entry)
        return v4.Ctx(L.l, gen, *entry)

    @staticmethod
    def guard(io, geo, b):
        """Before an erase in the pool: as before the rollover of proto_v4, nothing a later
        boot reads valid may outrank the decision. Unreachable after a scan."""
        L, O = v4.live(io, geo)
        if not v4.top_solid(io, L, O):
            v4.bury(io, geo, L, O)

    @staticmethod
    def begin(io, geo, b):
        """Room for the map record and its start marker: the first lines of v4.update."""
        L, O = v4.live(io, geo)
        u = b.rec[0]
        p = max((q for q in range(geo.rpu) if io.read(('D', u, q, 's'))[0] != E), default=-1) + 1
        while True:
            if p < geo.rpu:
                try:
                    io.prog(('D', u, p, 's'), 'Z')
                    return (u, p)
                except ProgErr:
                    p += 1
                    continue
            if not v4.top_solid(io, L, O):
                U = v4.bury(io, geo, L, O)
                L, O = U, (L if U is O else O)
            u = (u + 1) % geo.nd
            if u == b.rec[0]:
                u = (u + 1) % geo.nd
            io.erase('D', u)
            p = 0

    @staticmethod
    def commit(io, geo, b, token, mapval):
        """The record's data and meta field, then the pair: the last lines of v4.update."""
        u, p = token
        L, O = v4.live(io, geo)
        version = b.version + 1
        io.prog(('D', u, p, 'd'), mapval)
        io.prog(('D', u, p, 'm'), ('m', version, mapval))
        entry = ((u, p), version, mapval)
        U = v4.append(io, geo, L, O, entry)
        return v4.Ctx(U.l, U.gen, *entry)


class BeAbs:
    """The contract of the EWLF log, in one pseudo word (current map, candidate, mode):
    - 'none': the current map is shown at every boot, without a write;
    - 'weak': a commit was cut in its entry: each boot elects the current map or the
      candidate, and repairs before it shows; the repair, cut, leaves the choice open or
      settles it; once it returned the choice is final;
    - 'sure': a commit or a repair was cut in its confirm: the candidate is elected at every
      boot, with or without a repair."""
    REG = ('R', 0, 0, 'r')

    @staticmethod
    def _get(io, geo):
        return io.flash[geo.index[BeAbs.REG]][1]

    @staticmethod
    def _set(io, geo, val):
        io.flash[geo.index[BeAbs.REG]] = (SOLID, val)

    @staticmethod
    def format(geo, flash, mapval):
        flash[geo.index[BeAbs.REG]] = (SOLID, (mapval, None, 'none'))

    @staticmethod
    def phase1(io, geo):
        cur, cand, mode = BeAbs._get(io, geo)
        if mode == 'weak':
            pick = io.ch.choose([0, 1])
            io.log.append(f'the weak entry reads {"valid" if pick else "invalid"}')
            return ((cur, cand)[pick], True)
        if mode == 'sure':
            need = io.ch.choose([False, True])
            io.log.append(f'the confirm reads {"erased" if need else "zeros"}')
            return (cand, need)
        return (cur, False)

    @staticmethod
    def mapval(h):
        return h[0]

    @staticmethod
    def phase2(io, geo, h):
        d, need = h
        cur, cand, mode = BeAbs._get(io, geo)
        if need:
            if io._cut('repair, entry'):
                raise PowerCut()
            if io._cut('repair, confirm'):
                BeAbs._set(io, geo, (cur, d, 'sure'))
                raise PowerCut()
            BeAbs._set(io, geo, (d, None, 'none'))
            io.log.append('prog R: repair')
        return d

    @staticmethod
    def guard(io, geo, b):
        pass

    @staticmethod
    def begin(io, geo, b):
        return None

    @staticmethod
    def commit(io, geo, b, token, mapval):
        if io._cut('commit, record'):
            raise PowerCut()
        if io._cut('commit, entry'):
            BeAbs._set(io, geo, (b, mapval, 'weak'))
            raise PowerCut()
        if io._cut('commit, confirm'):
            BeAbs._set(io, geo, (b, mapval, 'sure'))
            raise PowerCut()
        BeAbs._set(io, geo, (mapval, None, 'none'))
        io.log.append('prog R: commit')
        return mapval


BE = BeV4 if BACKEND == 'v4' else BeAbs


class Ctx:
    def __init__(self, b, mapval):
        self.b = b               # what the commit backend knows
        self.map = mapval        # the committed map: (slot of each chunk or None, head unit)
        self.head = mapval[1]    # the unit being filled, None before the first slot
        self.pos = None          # next slot to try in the head unit, None until derived
        self.txn = None          # the open transaction: {chunk: slot}


def format(geo):
    flash = [(SOLID, None)] * len(geo.addrs)
    BE.format(geo, flash, ((None,) * geo.nchunks, None))
    return tuple(flash)


def phase1(io, geo):
    return BE.phase1(io, geo)


def phase2(io, geo, h):
    mapval = BE.mapval(h)
    return Ctx(BE.phase2(io, geo, h), mapval)


def content(io, geo, x):
    mapval = x.map if isinstance(x, Ctx) else BE.mapval(x)
    out = []
    for i, s in enumerate(mapval[0]):
        if s is None:
            out.append(None)
            continue
        r = io.read(('P', s[0], s[1], 'd'))
        good = r[0] == V and isinstance(r[1], tuple) and r[1][0] == i
        out.append(r[1][1] if good else ('BAD', i))
    return tuple(out)


def refs(ctx, inflight=()):
    """The units that hold a chunk of the committed map, of the open transaction or of the
    operation in progress."""
    slots = [s for s in ctx.map[0] if s is not None]
    if ctx.txn:
        slots += list(ctx.txn.values())
    slots += list(inflight)
    return {u for u, _ in slots}


def clean_ahead(geo, ctx, inflight=()):
    """Units after the head, in ring order, before the first one that holds a chunk."""
    r = refs(ctx, inflight)
    if ctx.head is None:
        return geo.np
    c = 0
    u = (ctx.head + 1) % geo.np
    while c < geo.np - 1 and u not in r:
        c += 1
        u = (u + 1) % geo.np
    return c


def pos_of(io, geo, ctx):
    if ctx.head is None:
        return geo.spu
    if ctx.pos is None:
        ctx.pos = max((p for p in range(geo.spu)
                       if io.read(('P', ctx.head, p, 's'))[0] != E), default=-1) + 1
    return ctx.pos


def alloc(io, geo, ctx, spare, inflight):
    """The next slot, its marker programmed. spare is 1 for an operation that evacuates a
    unit: its commit frees that unit, so it may enter the last free one."""
    while True:
        p = pos_of(io, geo, ctx)
        while p < geo.spu:
            ctx.pos = p + 1
            try:
                io.prog(('P', ctx.head, p, 's'), 'Z')
                return (ctx.head, p)
            except ProgErr:
                if KNOCKOUT == 'nostep':
                    return (ctx.head, p)
                p += 1
        c = clean_ahead(geo, ctx, inflight)
        reserve = 0 if KNOCKOUT == 'noreserve' else 1
        if c < 1 or c - 1 + spare < reserve:
            raise Short()
        u = 0 if ctx.head is None else (ctx.head + 1) % geo.np
        assert u not in refs(ctx, inflight)
        BE.guard(io, geo, ctx.b)
        if not (KNOCKOUT == 'trust' and all(io.read(('P', u, q, f))[0] == E
                                            for q in range(geo.spu) for f in 'sd')):
            io.erase('P', u)
        ctx.head, ctx.pos = u, 0


def tail_of(geo, ctx):
    """The unit a collection evacuates: the first one after the free units. None when every
    other unit is free, or when the open transaction has a chunk in it."""
    c = clean_ahead(geo, ctx)
    if ctx.head is None or c >= geo.np - 1:
        return None
    t = (ctx.head + 1 + c) % geo.np
    if ctx.txn and any(u == t for u, _ in ctx.txn.values()):
        return None
    return t


def relocate(io, geo, ctx, slots, i, new):
    s = alloc(io, geo, ctx, 1, new)
    r = io.read(('P', slots[i][0], slots[i][1], 'd'))
    io.prog(('P', s[0], s[1], 'd'), r[1] if r[0] == V else ('junk', i))
    slots[i] = s
    new.append(s)


def collect(io, geo, ctx):
    """Evacuates the oldest unit and commits the map that says so."""
    t = tail_of(geo, ctx)
    if t is None:
        raise NoSpace('nothing to collect')
    token = BE.begin(io, geo, ctx.b)
    slots = list(ctx.map[0])
    new = []
    try:
        for i in range(geo.nchunks):
            if slots[i] is not None and slots[i][0] == t:
                relocate(io, geo, ctx, slots, i, new)
    except Short:
        raise NoSpace('the reserve is gone')
    if KNOCKOUT == 'early':
        io.erase('P', t)
    mapval = (tuple(slots), ctx.head)
    ctx.b = BE.commit(io, geo, ctx.b, token, mapval)
    ctx.map = mapval


def fits(io, geo, ctx, k, spare):
    c = clean_ahead(geo, ctx)
    rem = geo.spu - pos_of(io, geo, ctx)
    reserve = 0 if KNOCKOUT == 'noreserve' else 1
    return k <= rem + (c - reserve + spare) * geo.spu


def ensure(io, geo, ctx, k):
    """Collects until k new slots fit with the reserve left after them."""
    for _ in range(geo.np + 1):
        if fits(io, geo, ctx, k, 0):
            return
        collect(io, geo, ctx)
    raise NoSpace('no garbage to collect')


def victims(geo, ctx, chunks):
    """With C1_COMBINE: the unit this commit evacuates and its chunks, when there is one."""
    if not COMBINE:
        return None, []
    t = tail_of(geo, ctx)
    if t is None:
        return None, []
    return t, [i for i, s in enumerate(ctx.map[0])
               if s is not None and s[0] == t and i not in chunks]


def write(io, geo, ctx, chunks, k):
    for _ in range(geo.spu * geo.np + 2):
        ensure(io, geo, ctx, len(chunks))
        t, moved = victims(geo, ctx, chunks)
        if t is not None and not fits(io, geo, ctx, len(chunks) + len(moved), 1):
            t, moved = None, []
        spare = 0 if t is None else 1
        token = BE.begin(io, geo, ctx.b)
        slots = list(ctx.map[0])
        new = []
        try:
            for i in range(geo.nchunks):       # in chunk order, as the map is streamed
                if i in chunks:
                    s = alloc(io, geo, ctx, spare, new)
                    io.prog(('P', s[0], s[1], 'd'), (i, k))
                    slots[i] = s
                    new.append(s)
                elif i in moved:
                    relocate(io, geo, ctx, slots, i, new)
        except Short:
            continue   # a refused marker left it short: this record is abandoned
        mapval = (tuple(slots), ctx.head)
        ctx.b = BE.commit(io, geo, ctx.b, token, mapval)
        ctx.map = mapval
        return
    raise ProtoError('internal: a write that never fits')


def txn_write(io, geo, ctx, i, k):
    for _ in range(geo.np + 2):
        try:
            s = alloc(io, geo, ctx, 0, ())
        except Short:
            collect(io, geo, ctx)
            continue
        io.prog(('P', s[0], s[1], 'd'), (i, k))
        ctx.txn[i] = s
        return
    raise NoSpace('no garbage to collect')


def txn_commit(io, geo, ctx):
    chunks = tuple(ctx.txn)
    for combine in (True, False):
        t, moved = victims(geo, ctx, chunks) if combine else (None, [])
        if t is not None and not fits(io, geo, ctx, len(moved), 1):
            t, moved = None, []
        token = BE.begin(io, geo, ctx.b)
        slots = list(ctx.map[0])
        new = []
        try:
            for i in range(geo.nchunks):
                if i in ctx.txn:
                    slots[i] = ctx.txn[i]
                elif i in moved:
                    relocate(io, geo, ctx, slots, i, new)
        except Short:
            continue
        mapval = (tuple(slots), ctx.head)
        ctx.b = BE.commit(io, geo, ctx.b, token, mapval)
        ctx.map = mapval
        ctx.txn = None
        return
    raise ProtoError('internal: a commit that never fits')


def apply(io, geo, ctx, kind, chunks, k):
    if kind == 'w':
        write(io, geo, ctx, chunks, k)
        return ctx
    ctx.txn = {}
    try:
        for i in chunks:
            txn_write(io, geo, ctx, i, k)
    except ProtoError:
        ctx.txn = None
        raise
    if kind == 't':
        txn_commit(io, geo, ctx)
    else:
        ctx.txn = None
    return ctx
