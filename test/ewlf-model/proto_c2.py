"""Chunked area, design c2: the EWLF log of proto_v4, unchanged, whose record is a map of
chunk locations, and a pool of erase units that holds the chunks. This module is the model of
CHUNKED.md; the document is the reference, this docstring the summary. proto_c1.py is the
first draft: the same log and the same map, a marker in every slot, and a pool whose head
evacuates every unit in its way, the full ones included.

Pool: np units of spu slots. With several slots per unit a slot is a start marker 's' (zeros)
then the chunk 'd'; with one slot per unit it is the chunk alone. The value of a chunk is
(chunk index, value). The map is (slot of each chunk or None, head unit): the data word of the
EWLF record. The commit of a map is the commit of an EWLF record, by the functions of
proto_v4, imported.

Rules of the pool:
- a unit is free when it holds no chunk of the committed map, of the open transaction nor of
  the operation in progress (knock-outs 'live' and 'txn': corrupt). It is full when every one
  of its slots holds a chunk of the committed map. Otherwise it holds garbage;
- slots are taken in order in the head unit, above its last started slot; a slot whose marker
  the flash refuses counts as started and is stepped over (knock-out 'nostep': corrupt). With
  one slot per unit there is no marker: a unit is written once, right after its erase;
- the head goes round the pool. When the head unit is full the head moves to the first free
  unit after it in ring order, the head itself last, and erases it at that instant, whatever
  it reads (knock-out 'trust': corrupt). It steps over the full units and over those that
  hold a chunk of the open transaction or of the operation in progress. From that first free
  unit on it does not go past the tail, the next unit that holds garbage;
- the reserve, with several slots per unit: an operation that only writes may enter a unit
  when one more unit it may enter is left; a collection may enter the last one, since its
  commit frees the tail (knock-out 'noreserve'). With one slot per unit a unit is full or
  free: there is no tail, nothing to collect, no reserve;
- collection: the chunks of the tail are copied to the head and the map committed. The tail
  is free from then on, and is erased when the head enters it, not before (knock-out 'early':
  corrupt);
- an operation checks the room it needs before it opens the map record, and collects first.
  When a refused marker leaves it short all the same, it abandons its record, collects, and
  starts again.

Operations: a plain write opens the map record, writes its chunks in chunk order, then commits
the map. A transaction writes its chunks first, each in a new slot, collecting when it needs
room, then commits the map; an abort forgets its slots, which stay started.

C2_BACKEND=abs replaces the EWLF log by its contract (proto_c2a.py): a commit that is cut
leaves the old map, or either map at each later boot until a repair returns, or the new one.
C2_BACKEND=v4p puts the map in the pair itself (proto_c2p.py): the record-in-the-pair variant
of the log, proto_v4p, and no map units.
C2_COMBINE=need lets a plain write that lacks room evacuate the tail in its own commit, the
policy of the document; C2_COMBINE=1 lets every plain write and every transaction commit do
so whenever the copies fit; by default a collection always has a commit of its own.

Geometry: chunks, pool units, slots per unit, then the map units, records per map unit,
regular pairs, reserved pairs and certificates of proto_v4, 2 each by default (the map units
and their records are not used by proto_c2p).
"""
import os

from ewlf_model import E, V, SOLID, PowerCut, ProgErr, ProtoError
import proto_v4 as v4
import proto_v4p as v4p

KNOCKOUT = os.environ.get('C2_KNOCKOUT', '')
BACKEND = os.environ.get('C2_BACKEND', 'v4')
COMBINE = os.environ.get('C2_COMBINE', '')

Stuck = v4.Stuck


class NoSpace(ProtoError):
    pass


class Short(Exception):
    """A unit entry would break the reserve: collect first."""


class Geometry:
    def __init__(self, nchunks, np_, spu, nd=2, rpu=2, ppu=2, nres=2, nx=2):
        self.nchunks, self.np, self.spu = nchunks, np_, spu
        self.nd, self.rpu, self.nl, self.ppu, self.nres, self.nx = nd, rpu, 2, ppu, nres, nx
        self.fields = 'sd' if spu > 1 else 'd'
        self.addrs = []
        for u in range(np_):
            for p in range(spu):
                for f in self.fields:
                    self.addrs.append(('P', u, p, f))
        if BACKEND == 'v4':
            for u in range(nd):
                for p in range(rpu):
                    for f in 'sdm':
                        self.addrs.append(('D', u, p, f))
        if BACKEND in ('v4', 'v4p'):
            for l in range(2):
                for q in range(ppu + nres):
                    for f in ('kec' if BACKEND == 'v4' else 'kdmc'):
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


class BeV4P:
    """The commit of a map by the record-in-the-pair variant of the EWLF log (proto_v4p):
    the map is the data of the pair, and there are no map units."""

    @staticmethod
    def format(geo, flash, mapval):
        for f, val in (('k', 'Z'), ('d', mapval), ('m', ('m', 1, 1, mapval)), ('c', 'Z')):
            flash[geo.index[('L', 1, 0, f)]] = (SOLID, val)

    @staticmethod
    def phase1(io, geo):
        L, O = v4p.live(io, geo)
        q, gen, entry = L.entries[-1]
        return (L, O, gen, entry)

    @staticmethod
    def mapval(h):
        return h[3][1]

    @staticmethod
    def phase2(io, geo, h):
        L, O, gen, entry = h
        if not v4p.top_solid(io, L, O):
            U = v4p.bury(io, geo, L, O)
            return v4p.Ctx(U.l, U.gen, *entry)
        return v4p.Ctx(L.l, gen, *entry)

    @staticmethod
    def guard(io, geo, b):
        L, O = v4p.live(io, geo)
        if not v4p.top_solid(io, L, O):
            v4p.bury(io, geo, L, O)

    @staticmethod
    def begin(io, geo, b):
        return None

    @staticmethod
    def commit(io, geo, b, token, mapval):
        L, O = v4p.live(io, geo)
        entry = (b.version + 1, mapval)
        U = v4p.append(io, geo, L, O, entry)
        return v4p.Ctx(U.l, U.gen, *entry)


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


BE = {'v4': BeV4, 'v4p': BeV4P, 'abs': BeAbs}[BACKEND]


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


def reserve(geo):
    return 0 if geo.spu == 1 or KNOCKOUT == 'noreserve' else 1


def pos_of(io, geo, ctx):
    """The next slot to try in the head unit. With several slots per unit: the one above its
    last started slot, by the start markers. With one slot per unit a unit is written once,
    right after its erase: the head a boot finds is full."""
    if ctx.head is None:
        return geo.spu
    if ctx.pos is None:
        if geo.spu == 1:
            ctx.pos = 1
        else:
            ctx.pos = max((p for p in range(geo.spu)
                           if io.read(('P', ctx.head, p, 's'))[0] != E), default=-1) + 1
    return ctx.pos


def ring(io, geo, ctx):
    """The units in ring order from the head; the head itself last, once it is full: it is
    then a unit like the others."""
    if ctx.head is None:
        return list(range(geo.np))
    order = [(ctx.head + 1 + i) % geo.np for i in range(geo.np - 1)]
    if pos_of(io, geo, ctx) >= geo.spu:
        order.append(ctx.head)
    return order


def survey(io, geo, ctx, inflight=()):
    """The units the head may enter, in the order it would, and the tail. The head goes round
    the pool: it enters the first free unit after it, then the free units that follow, up to
    the tail, the first unit that holds garbage next to chunks of the committed map. It steps
    over the units that are full of chunks of the committed map, and over those that hold a
    chunk of the open transaction or of the operation in progress. A unit that holds garbage
    before the first free unit was full when the head came by: its turn comes last."""
    live = {}
    if KNOCKOUT != 'live':
        for s in ctx.map[0]:
            if s is not None:
                live[s[0]] = live.get(s[0], 0) + 1
    held = {u for u, _ in inflight}
    if ctx.txn and KNOCKOUT != 'txn':
        held |= {u for u, _ in ctx.txn.values()}
    order = [u for u in ring(io, geo, ctx) if u not in held]
    first = next((i for i, u in enumerate(order) if live.get(u, 0) == 0), None)
    if first is None:
        return [], None
    run, tail = [], None
    for u in order[first:] + order[:first]:
        n = live.get(u, 0)
        if n == 0:
            run.append(u)
        elif n < geo.spu:
            tail = u
            break
    return run, tail


def alloc(io, geo, ctx, spare, inflight):
    """The next slot, its marker programmed when it has one. spare is 1 for an operation that
    evacuates the tail: its commit frees that unit, so it may enter the last free one."""
    while True:
        p = pos_of(io, geo, ctx)
        while p < geo.spu:
            ctx.pos = p + 1
            if geo.spu == 1:
                return (ctx.head, p)
            try:
                io.prog(('P', ctx.head, p, 's'), 'Z')
                return (ctx.head, p)
            except ProgErr:
                if KNOCKOUT == 'nostep':
                    return (ctx.head, p)
                p += 1
        run, _ = survey(io, geo, ctx, inflight)
        if not run or len(run) - 1 + spare < reserve(geo):
            raise Short()
        u = run[0]
        BE.guard(io, geo, ctx.b)
        if not (KNOCKOUT == 'trust' and all(io.read(('P', u, q, f))[0] == E
                                            for q in range(geo.spu) for f in geo.fields)):
            io.erase('P', u)
        ctx.head, ctx.pos = u, 0


def relocate(io, geo, ctx, slots, i, new):
    s = alloc(io, geo, ctx, 1, new)
    r = io.read(('P', slots[i][0], slots[i][1], 'd'))
    io.prog(('P', s[0], s[1], 'd'), r[1] if r[0] == V else ('junk', i))
    slots[i] = s
    new.append(s)


def collect(io, geo, ctx):
    """Evacuates the tail and commits the map that says so."""
    t = survey(io, geo, ctx)[1]
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
    rem = geo.spu - pos_of(io, geo, ctx)
    run, _ = survey(io, geo, ctx)
    return k <= rem + (len(run) - reserve(geo) + spare) * geo.spu


def tail_chunks(io, geo, ctx, chunks, k):
    """The tail and its chunks to copy, when a commit of k new slots can evacuate it too."""
    t = survey(io, geo, ctx)[1]
    if t is None:
        return None, []
    moved = [i for i, s in enumerate(ctx.map[0])
             if s is not None and s[0] == t and i not in chunks]
    if not fits(io, geo, ctx, k + len(moved), 1):
        return None, []
    return t, moved


def plan(io, geo, ctx, chunks):
    """Makes room for one new slot per chunk, the reserve left after them. Returns the unit
    their commit evacuates with them, None when there is none, and its chunks to copy."""
    k = len(chunks)
    for _ in range(geo.np * geo.spu + 1):
        enough = fits(io, geo, ctx, k, 0)
        if COMBINE == '1' or (COMBINE == 'need' and not enough):
            t, moved = tail_chunks(io, geo, ctx, chunks, k)
            if t is not None:
                return t, moved
        if enough:
            return None, []
        collect(io, geo, ctx)
    raise NoSpace('no garbage to collect')


def write(io, geo, ctx, chunks, k):
    for _ in range(geo.spu * geo.np + 2):
        t, moved = plan(io, geo, ctx, chunks)
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
    for _ in range(geo.spu * geo.np + 2):
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
    for combine in (COMBINE == '1', False):
        t, moved = tail_chunks(io, geo, ctx, chunks, 0) if combine else (None, [])
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
