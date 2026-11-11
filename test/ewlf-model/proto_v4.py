"""EWLF v4 (format v0.4.0): reserved re-commit pairs, erase certificates, bury before erase.

Log unit of generation g is unit g mod 2. Pair = marker k (zeros), entry e = (gen, record,
version, cid), confirm c (zeros), programmed in that order, each after the previous returned.
A log unit has ppu regular pair positions, nres reserved pair positions after them (never
taken by an update's regular append) and nx certificate words (zeros). This module is the
model of EWLF.md; the document is the reference, this docstring the summary.

Reader (every boot, every word of both log units read once):
- a position is started when any of its words reads programmed; it holds a valid entry when
  e reads valid with the generation parity of its unit; an entry is proven when its c reads
  programmed (all zeros on the chip; the model has only Z or E), then e is solid: c is
  programmed only after e returned;
- the live unit L: highest generation among units holding a valid entry, generations being
  adjacent; the decision D is the newest valid entry of L (reserved positions are newer than
  regular ones); D is the newest VALID entry, not the newest proven one: a weak c that read
  programmed at one boot may have shown it;
- L is closed when any word of a reserved position or any certificate of L reads programmed:
  no regular append goes to a closed unit (reserved pairs may still be used);
- L is certified when any certificate of L reads programmed: the other unit's erase returned
  in the session that programmed it, and content of generation g+1 may have entered O since;
  O's own certificates are never read (ghosts of its previous life);
- O is uncertain when L is certified, O has a started pair position and no valid entry: a
  carry attempt, possibly weak, may be there; without a certificate of L, O's started
  positions are a dead life or a ghost of a lower generation and are not consulted;
- the top is solid when D is proven, nothing is started above D in L, and O is not uncertain.

Invariant that carries the design: content enters a unit only after an erase that returned in
the same session and a certificate that session saw return, or above a word that entered
that way. So a weak entry of generation g+1 is never exposed to a torn erase while g is live
(knock-out 'cert' or 'empty': resurrection), and a unit without a certificate reading
programmed is erased before use, safely, since nothing entered it.

Switch from L (g) to O (g+1), by the session that needs room in O, carry = the entry to commit:
1. no certificate of L reads programmed and L has no proven entry: fresh reserved pair of L
   copying D first, so that a solid decision exists outside O (knock-out 'step1': loss);
2. no certificate of L reads programmed, or no pair position of O is started: erase O in this
   session, fresh certificate of L (seen returning), carry at O.0;
3. otherwise: carry at O's first free position above its started ones, regular or reserved;
   no erase, no new certificate (the started word was programmed after one returned).

Boot: show D without writing when the top is solid; else bury D first (knock-out 'bury':
rollback): carry into O when O is uncertain, else a regular pair of L if open, else a
reserved pair of L, else a switch. Update: room for the record above the last started
position of the current record's unit (a refused start marker counts as started); rollover
erases the next unit right before its first record (the defensive bury before it is
unreachable after a scan); record; then append in L if open with regular room, else switch
with the entry as carry. Stuck when the reserved pairs or the certificates of L, or the
positions of O, are exhausted: one word, pair or position per precise cut.
"""
import os

from ewlf_model import E, V, SOLID, ProgErr, ProtoError

# negative controls for the model-check table: V4_KNOCKOUT=step1 (no reserved pair before
# erasing O), bury (no bury at boot), cert (no certificate), empty (no erase of an empty O)
KNOCKOUT = os.environ.get('V4_KNOCKOUT', '')


class Stuck(ProtoError):
    pass


class Geometry:
    def __init__(self, nd, rpu, nl, ppu, nres=1, nx=1):
        assert nl == 2
        self.nd, self.rpu, self.nl, self.ppu, self.nres, self.nx = nd, rpu, nl, ppu, nres, nx
        self.addrs = []
        for u in range(nd):
            for p in range(rpu):
                for f in 'sdm':
                    self.addrs.append(('D', u, p, f))
        for l in range(nl):
            for q in range(ppu + nres):
                for f in 'kec':
                    self.addrs.append(('L', l, q, f))
            for i in range(nx):
                self.addrs.append(('L', l, i, 'x'))
        self.index = {a: i for i, a in enumerate(self.addrs)}
        self.units = {}
        for i, a in enumerate(self.addrs):
            self.units.setdefault((a[0], a[1]), []).append(i)


class Ctx:
    def __init__(self, unit, gen, rec, version, cid):
        self.unit, self.gen, self.rec, self.version, self.cid = unit, gen, rec, version, cid


def format(geo):
    flash = [(SOLID, None)] * len(geo.addrs)
    u = geo.nd - 1
    for f, val in (('s', 'Z'), ('d', 0), ('m', ('m', 1, 0))):
        flash[geo.index[('D', u, 0, f)]] = (SOLID, val)
    for f, val in (('k', 'Z'), ('e', ('e', 1, (u, 0), 1, 0)), ('c', 'Z')):
        flash[geo.index[('L', 1, 0, f)]] = (SOLID, val)
    return tuple(flash)


class Unit:
    def __init__(self, io, geo, l):
        self.l = l
        self.entries = []        # (q, gen, (rec, version, cid)) valid, newest last
        self.started = -1        # last position, regular or reserved, with a programmed word
        self.proven = False      # some valid entry's confirm reads programmed
        self.closed = False
        self.certified = False   # a certificate of THIS unit: the other unit's erase returned
        for q in range(geo.ppu + geo.nres):
            r = {f: io.read(('L', l, q, f)) for f in 'kec'}
            if any(x[0] != E for x in r.values()):
                self.started = q
                if q >= geo.ppu:
                    self.closed = True
            e = r['e']
            if e[0] == V and e[1][1] % 2 == l:
                self.entries.append((q, e[1][1], e[1][2:]))
                if r['c'][0] != E:
                    self.proven = True
        for i in range(geo.nx):
            if io.read(('L', l, i, 'x'))[0] != E:
                self.closed = self.certified = True
        self.gen = max((g for _, g, _ in self.entries), default=None)

    def uncertain(self, L):
        """Started, no valid entry, and L certified: a carry attempt of generation L.gen + 1
        may be there, weak. Without a certificate of L nothing was written into this unit
        since its erase: its words are a dead life or a ghost, of a lower generation."""
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
    for f, v in (('k', 'Z'), ('e', ('e', gen, *entry)), ('c', 'Z')):
        io.prog(('L', l, q, f), v)


def fresh_pair(io, U, lo, hi, gen, entry):
    """Programs a pair at the first position in [lo, hi) above the started ones, stepping
    over refused positions. Returns the position, or None when the range is exhausted."""
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
    """Opens generation L.gen + 1 in O with entry as its first entry; returns O."""
    gen = L.gen + 1
    if not L.certified and not L.proven and KNOCKOUT != 'step1':
        if fresh_pair(io, L, geo.ppu, geo.ppu + geo.nres, L.gen, L.entries[-1][2]) is None:
            raise Stuck('reserved pairs exhausted')
    if not L.certified or (O.started < 0 and KNOCKOUT != 'empty'):
        # O holds nothing this session can rely on: not certified, nothing entered it since
        # its erase; no started pair position, a certificate that reads programmed may be
        # weak (its attempt died in the carry's marker). Writing the carry on that reading
        # would put a generation g+1 entry under a certificate that may read erased at the
        # next boot, which would then erase O again, possibly torn: the entry could come back
        # above the live content. Erase O here and certify with a program this session sees
        # return. Once O holds a started pair position a solid certificate exists: that word
        # was programmed after one returned, by the session that programmed both.
        io.erase('L', O.l)
        O.started, O.entries, O.gen, O.proven = -1, [], None, False
        fresh_cert(io, geo, L)
    if fresh_pair(io, O, 0, geo.ppu + geo.nres, gen, entry) is None:
        raise Stuck('target unit full of started positions')
    return O


def append(io, geo, L, O, entry):
    """Commits entry above everything started: in L when open with regular room, else by a
    switch into O. Returns the unit holding it."""
    if not L.closed and fresh_pair(io, L, 0, geo.ppu, L.gen, entry) is not None:
        return L
    return switch(io, geo, L, O, entry)


def top_solid(io, L, O):
    q = L.entries[-1][0]
    return L.confirmed(io, q) and L.started == q and not O.uncertain(L)


def bury(io, geo, L, O):
    """Re-commits D above every started position of both units; returns the unit holding
    the new copy of D."""
    entry = L.entries[-1][2]
    if O.uncertain(L):
        return switch(io, geo, L, O, entry)
    if not L.closed and fresh_pair(io, L, 0, geo.ppu, L.gen, entry) is not None:
        return L
    if fresh_pair(io, L, geo.ppu, geo.ppu + geo.nres, L.gen, entry) is not None:
        return L
    return switch(io, geo, L, O, entry)


def located(io, entry):
    (u, p), version, cid = entry
    return io.read(('D', u, p, 'm')) == (V, ('m', version, cid)) and \
        io.read(('D', u, p, 'd')) == (V, cid)


def scan(io, geo):
    L, O = live(io, geo)
    q, gen, entry = L.entries[-1]
    if not located(io, entry):
        raise ProtoError('decision names no record')
    if not top_solid(io, L, O) and KNOCKOUT != 'bury':
        U = bury(io, geo, L, O)
        return Ctx(U.l, U.gen, *entry)
    return Ctx(L.l, gen, *entry)


def update(io, geo, ctx, cid):
    L, O = live(io, geo)
    u = ctx.rec[0]
    # room: the first position of the current record's unit above the last started one (the
    # newest record is the lowest started position); a refused program (a weak start marker
    # reading erased this instant) counts as started
    p = max((q for q in range(geo.rpu) if io.read(('D', u, q, 's'))[0] != E), default=-1) + 1
    while True:
        if p < geo.rpu:
            try:
                io.prog(('D', u, p, 's'), 'Z')
                break
            except ProgErr:
                p += 1
                continue
        # rollover: nothing a later boot reads valid may outrank D once its record is gone
        if not top_solid(io, L, O):
            U = bury(io, geo, L, O)
            L, O = U, (L if U is O else O)
        u = (u + 1) % geo.nd
        if u == ctx.rec[0]:
            u = (u + 1) % geo.nd
        io.erase('D', u)
        p = 0
    version = ctx.version + 1
    io.prog(('D', u, p, 'd'), cid)
    io.prog(('D', u, p, 'm'), ('m', version, cid))
    entry = ((u, p), version, cid)
    U = append(io, geo, L, O, entry)
    return Ctx(U.l, U.gen, *entry)
