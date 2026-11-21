#!/usr/bin/env python3
"""Simulation of the pool of a chunked area (CHUNKED.md) at real sizes: room, garbage
collection and wear. No flash words and no tearing model here, chunked_model.py has them;
only slots, with the allocation and collection rules of proto_c2.py (--policy pass, the
design), of proto_c1.py (--policy ring, the first draft) or of an alternative that was not
kept (--policy any):

- slots are taken in order in the head unit. A unit is free when it holds no chunk of the
  committed map, of the open transaction nor of the operation in progress; full when every
  slot holds a chunk of the committed map; and it holds garbage otherwise;
- pass: the head goes round the pool. It enters the first free unit after it, then the free
  units that follow, stepping over the full ones, up to the first unit that holds garbage:
  that unit, the tail, is evacuated before the head goes further. ring: the head enters the unit right after it, which must be free; the
  tail is the first unit after the free ones, whatever it holds. any: the head enters the
  first free unit after it, wherever it is, and a collection evacuates the unit with the
  fewest chunks (--victim fewest) or the first one that holds garbage (--victim oldest);
- the reserve, when a unit holds several slots: an operation that only writes may enter a
  unit when one more unit it may enter is left, a collection may enter the last one. With
  one slot per unit there is nothing to collect and no reserve (pass and any);
- a collection copies the chunks of the tail to the head and commits. --combine need: an
  update that lacks room evacuates the tail in its own commit when that is enough.
  --combine early: it also does so when fewer than --target units are left to enter;
- an operation that is cut, or a transaction that aborts, burns the slots it started: those in
  the committed head unit stay started, those in units entered since come back at re-entry.

It checks that no operation ever lacks room when the pool is sized by the rule
    pool units >= ceil((chunks + largest operation) / slots per unit) + 2    (several slots)
    pool units >= chunks + largest operation                                 (one slot)
and measures the copies per chunk written, the worst operation, and the erase counts.

Usage: chunked_sim.py --suite            the room check of CHUNKED.md
       chunked_sim.py --compare          the copies and the wear of CHUNKED.md, by policy
       chunked_sim.py --tables           the sizing and cost tables of CHUNKED.md
       chunked_sim.py --large            the sizing and cost tables of LARGE.md
       chunked_sim.py -n 133 -S 16 [-U units] [--kmax 8] [--workload uniform] [--ops 200000]
                      [--pcut 0.05] [--pabort 0.05] [--policy pass] [--victim fewest]
                      [--combine need] [--target 2] [--seed 1]
       (workloads: uniform, hot, same, seq, mixed, whole, frag, shift, burn; the process is
       capped at --mem-mb MB and --cpu-s seconds of CPU, 0 to remove a cap)
"""
import argparse
import random
import sys


class Short(Exception):
    pass


class NoSpace(Exception):
    pass


class Cut(Exception):
    pass


def rule_units(n, S, kmax):
    """Smallest pool, in units, the sizing rule allows."""
    if S == 1:
        return n + kmax
    return -(-(n + kmax) // S) + 2


class Pool:
    def __init__(self, n, U, S, policy='pass', victim='fewest', combine='need', target=2):
        self.n, self.U, self.S = n, U, S
        self.policy, self.victim_rule, self.combine, self.target = policy, victim, combine, target
        self.reserve = 1 if policy == 'ring' or S > 1 else 0
        self.map = [None] * n        # committed: chunk -> (unit, slot)
        self.started = [0] * U       # slots started in each unit since its last erase
        self.erases = [0] * U
        self.chead = None            # head unit of the committed map
        self.head = None             # head unit of the session
        self.txn = None
        self.live = [0] * U          # committed chunks per unit
        self.stat = dict(user=0, copies=0, commits=0, gc_commits=0, burnt=0, worst_copies=0,
                         worst_erases=0, worst_slots=0, cuts=0, aborts=0)
        self.op_copies = self.op_erases = self.op_slots = 0

    # --- what a boot derives
    def boot(self):
        self.head = self.chead
        self.txn = None

    def held(self, inflight):
        h = {u for u, _ in self.txn.values()} if self.txn else set()
        return h | {u for u, _ in inflight}

    def order(self):
        """The units in ring order from the head. pass and any: the head itself comes last,
        once it is full: it is then a unit like the others. ring: never the head."""
        if self.head is None:
            return list(range(self.U))
        o = [(self.head + 1 + i) % self.U for i in range(self.U - 1)]
        if self.policy != 'ring' and self.started[self.head] >= self.S:
            o.append(self.head)
        return o

    def survey(self, inflight=()):
        """The units the head may enter, in the order it would, and the tail: the unit a
        collection evacuates, None when there is none."""
        held = self.held(inflight)
        run, tail = [], None
        if self.policy == 'pass':
            order = [u for u in self.order() if u not in held]
            first = next((i for i, u in enumerate(order) if self.live[u] == 0), None)
            if first is None:
                return [], None
            # from the first free unit on, round the pool: the units before it come last
            for u in order[first:] + order[:first]:
                if self.live[u] == 0:
                    run.append(u)
                elif self.live[u] < self.S:
                    tail = u            # it holds garbage: the head does not go past it
                    break
            return run, tail
        if self.policy == 'ring':
            for u in self.order():
                if self.live[u] or u in held:
                    tail = None if u in held else u
                    break
                run.append(u)
            return run, tail
        order = self.order()
        run = [u for u in order if not self.live[u] and u not in held]
        cands = [u for u in order if 0 < self.live[u] < self.S and u not in held]
        if cands:
            tail = cands[0] if self.victim_rule == 'oldest' else \
                min(cands, key=lambda u: self.live[u])
        return run, tail

    def ahead(self, inflight=()):
        return self.survey(inflight)[0]

    def rem(self):
        return 0 if self.head is None else self.S - self.started[self.head]

    # --- allocation
    def alloc(self, spare, inflight):
        while True:
            if self.head is not None and self.started[self.head] < self.S:
                p = self.started[self.head]
                self.started[self.head] += 1
                self.op_slots += 1
                return (self.head, p)
            a = self.ahead(inflight)
            if not a or len(a) - 1 + spare < self.reserve:
                raise Short()
            u = a[0]
            self.erases[u] += 1
            self.op_erases += 1
            self.started[u] = 0
            self.head = u

    def commit(self, slots, gc):
        for s in self.map:
            if s is not None:
                self.live[s[0]] -= 1
        self.map = slots
        for s in self.map:
            if s is not None:
                self.live[s[0]] += 1
        self.chead = self.head
        self.stat['commits'] += 1
        if gc:
            self.stat['gc_commits'] += 1

    def victim(self):
        return self.survey()[1]

    def collect(self, cut_after=None):
        t = self.victim()
        if t is None:
            raise NoSpace('nothing to collect')
        slots = list(self.map)
        new = []
        try:
            for i in range(self.n):
                if slots[i] is not None and slots[i][0] == t:
                    if cut_after is not None and len(new) >= cut_after:
                        raise Cut()
                    s = self.alloc(1, new)
                    slots[i] = s
                    new.append(s)
                    self.op_copies += 1
        except Short:
            raise NoSpace('the reserve is gone')
        self.commit(slots, True)

    def fits(self, k, spare):
        return k <= self.rem() + (len(self.ahead()) - self.reserve + spare) * self.S

    def tail_chunks(self, chunks):
        t = self.victim()
        if t is None:
            return None, []
        return t, [i for i, s in enumerate(self.map) if s is not None and s[0] == t
                   and i not in chunks]

    def plan(self, chunks):
        """Makes room for the chunks; returns the unit their commit evacuates with them, if
        any, and the chunks to copy."""
        k = len(chunks)
        for _ in range(self.U * self.S + 1):
            enough = self.fits(k, 0)
            if not enough or (self.combine == 'early' and len(self.ahead()) < self.target):
                if self.combine != 'no':
                    t, moved = self.tail_chunks(chunks)
                    if t is not None and self.fits(k + len(moved), 1):
                        return t, moved
            if enough:
                return None, []
            self.collect()
        raise NoSpace('no garbage to collect')

    # --- operations
    def write(self, chunks, cut_after=None):
        """A plain write of the given chunks; cut_after slots, it is cut before its commit."""
        t, moved = self.plan(chunks)
        spare = 0 if t is None else 1
        moved = set(moved)
        slots = list(self.map)
        new = []
        for i in sorted(set(chunks) | moved):
            if cut_after is not None and len(new) >= cut_after:
                raise Cut()
            s = self.alloc(spare, new)
            slots[i] = s
            new.append(s)
            if i in moved:
                self.op_copies += 1
        if cut_after is not None:
            raise Cut()
        self.commit(slots, False)
        self.stat['user'] += len(chunks)

    def transaction(self, chunks, end, cut_after=None):
        """A transaction that writes the chunks one by one; end is 'commit', 'abort' or
        'cut' (after cut_after slots)."""
        self.txn = {}
        for j, i in enumerate(chunks):
            if end == 'cut' and j >= cut_after:
                raise Cut()
            for _ in range(self.U * self.S + 2):
                try:
                    s = self.alloc(0, ())
                    break
                except Short:
                    self.collect()
            else:
                raise NoSpace('no garbage to collect')
            self.txn[i] = s
        if end == 'cut':
            raise Cut()
        if end == 'abort':
            self.stat['burnt'] += len(self.txn)
            self.stat['aborts'] += 1
            self.txn = None
            return
        slots = list(self.map)
        for i, s in self.txn.items():
            slots[i] = s
        self.txn = None
        self.commit(slots, False)
        self.stat['user'] += len(chunks)

    def op(self, f, *args, **kw):
        """Runs one application operation and keeps its worst figures."""
        self.op_copies = self.op_erases = self.op_slots = 0
        try:
            f(*args, **kw)
        except Cut:
            self.stat['cuts'] += 1
            self.stat['burnt'] += self.op_slots
            self.boot()
        finally:
            st = self.stat
            st['copies'] += self.op_copies
            st['worst_copies'] = max(st['worst_copies'], self.op_copies)
            st['worst_erases'] = max(st['worst_erases'], self.op_erases)
            st['worst_slots'] = max(st['worst_slots'], self.op_slots)

    def check(self):
        seen = set()
        for i, s in enumerate(self.map):
            if s is None:
                continue
            assert s not in seen and s[1] < self.started[s[0]], (i, s)
            seen.add(s)
        assert self.chead is None or self.reserve == 0 or len(self.ahead()) >= 1 \
            or self.head != self.chead or self.txn


def chunk_sets(rng, n, kmax, workload):
    """An endless stream of the chunk sets the application writes."""
    seq = 0
    hot = max(1, n // 10)
    while True:
        if workload == 'same':
            yield [0]
            continue
        if workload == 'seq':
            yield [seq % n]
            seq += 1
            continue
        if workload == 'whole':
            yield list(range(n))[:kmax]
            continue
        if workload == 'frag':
            # the pool is fragmented by single chunks, then the largest operation comes
            seq += 1
            if seq % 50 == 0:
                first = rng.randrange(n)
                yield [(first + j) % n for j in range(kmax)]
            else:
                yield [rng.randrange(n)]
            continue
        if workload == 'shift':
            # every chunk for a while, which scatters them over the pool, then one chunk only
            seq += 1
            yield [rng.randrange(n)] if seq < 20000 else [0]
            continue
        if workload == 'mixed':
            k = 1
            while k < kmax and rng.random() < 0.6:
                k *= 2
            k = min(k, kmax)
        elif workload == 'burn':
            k = 1
        else:
            k = rng.randint(1, kmax) if kmax > 1 and rng.random() < 0.1 else 1
        if workload == 'hot' and rng.random() < 0.9:
            first = rng.randrange(hot)
        else:
            first = rng.randrange(n)
        yield [(first + j) % n for j in range(k)]


def run(n, S, U, kmax, workload, ops, pcut, pabort, combine, target, seed, policy='pass',
        victim='fewest'):
    rng = random.Random(seed)
    pool = Pool(n, U, S, policy, victim, combine, target)
    pool.boot()
    sets = chunk_sets(rng, n, kmax, workload)
    nospace = 0
    try:
        # first, the whole data once, in operations of kmax chunks
        for first in range(0, n, kmax):
            pool.op(pool.write, list(range(first, min(n, first + kmax))))
    except NoSpace:
        nospace += 1
        pool.boot()
    for count in range(ops):
        chunks = next(sets)
        try:
            if workload == 'burn' and count % 2:
                # the attack on the room: an operation of the largest size, cut before its commit
                big = [(chunks[0] + j) % n for j in range(kmax)]
                pool.op(pool.transaction, big, 'cut', cut_after=kmax)
                continue
            r = rng.random()
            if r < pcut:
                cut_after = rng.randint(0, len(chunks))
                if rng.random() < 0.5:
                    pool.op(pool.write, chunks, cut_after=cut_after)
                else:
                    pool.op(pool.transaction, chunks, 'cut', cut_after=cut_after)
            elif r < pcut + pabort:
                pool.op(pool.transaction, chunks, 'abort')
            elif rng.random() < 0.5:
                pool.op(pool.write, chunks)
            else:
                pool.op(pool.transaction, chunks, 'commit')
        except NoSpace:
            nospace += 1
            pool.boot()
        if count % 1000 == 0:
            pool.check()
    st = pool.stat
    e = pool.erases
    return dict(st, nospace=nospace, erase_min=min(e), erase_max=max(e),
                per_chunk=st['copies'] / max(1, st['user']),
                slots_per_chunk=sum(e) * S / max(1, st['user']),
                # chunks written per erase of the most erased unit: the endurance the pool
                # gives is this figure times the endurance of a unit
                user_per_erase=st['user'] / max(1, max(e)))


GEOMETRIES = (
    (8, 1, '64 KB, 8 KB chunks'),
    (2, 1, '16 KB, 8 KB chunks'),
    (16, 1, '128 KB, 8 KB chunks'),
    (33, 4, '64 KB, 2 KB chunks'),
    (133, 16, '64 KB, 496 B chunks'),
    (69, 32, '16 KB, 240 B chunks'),
)
WORKLOADS = ('uniform', 'hot', 'same', 'mixed', 'whole', 'frag', 'shift', 'burn')


def suite(ops, seed, less):
    """Room: every geometry at the smallest pool the rule allows (--less units fewer, to see
    how tight the rule is)."""
    rows = []
    for n, S, label in GEOMETRIES:
        for kmax in sorted({1, min(8, n), n}):
            U = rule_units(n, S, kmax) - less
            for workload in WORKLOADS:
                for pcut in (0.0, 0.05):
                    for combine in (('no', 'need') if S > 1 else ('no',)):
                        r = run(n, S, U, kmax, workload, ops, pcut, pcut, combine, 2, seed)
                        rows.append((label, n, S, U, kmax, workload, pcut, combine, r))
    print('data | n | S | U | kmax | workload | pcut | combine | no room | copies/chunk | '
          'worst copies | worst erases | slots/chunk | erases min..max | collections')
    for label, n, S, U, kmax, workload, pcut, combine, r in rows:
        print(f'{label} | {n} | {S} | {U} | {kmax} | {workload} | {pcut} | '
              f'{combine} | {r["nospace"]} | {r["per_chunk"]:.2f} | '
              f'{r["worst_copies"]} | {r["worst_erases"]} | {r["slots_per_chunk"]:.2f} | '
              f'{r["erase_min"]}..{r["erase_max"]} | {r["gc_commits"]}')
    bad = [x for x in rows if x[-1]['nospace']]
    print(f'{len(rows)} runs of {ops} operations, {len(bad)} with an operation that lacked room'
          f'{f", pool {less} unit(s) under the rule" if less else ""}')
    return 1 if bad else 0


def compare(ops, seed):
    """Copies and wear by policy, same sequences: the first draft (ring), the design (pass)
    and the alternative that was not kept (any free unit, the fewest chunks evacuated)."""
    cases = (
        (133, 16, 20, 8, '64 KB, 496 B chunks, 20 units, updates of 1 to 8 chunks'),
        (133, 16, 20, 133, '64 KB, 496 B chunks, 20 units, updates up to the whole data'),
        (33, 4, 20, 33, '64 KB, 2 KB chunks, 20 units, updates up to the whole data'),
        (8, 1, 18, 8, '64 KB, 8 KB chunks, 18 units, updates up to the whole data'),
    )
    policies = (('ring', '', 'early'), ('pass', '', 'need'), ('pass', '', 'no'),
                ('any', 'fewest', 'need'))
    print('case | workload | policy | no room | copies/chunk | worst copies | collections | '
          'erases min..max | chunks written per erase of the most erased unit | pool slots '
          'less chunks')
    bad = 0
    for n, S, U, kmax, label in cases:
        for workload in ('uniform', 'hot', 'same', 'mixed', 'whole', 'frag', 'shift'):
            for policy, victim, combine in policies:
                if S == 1 and combine == 'no':
                    continue
                for pcut in (0.0, 0.05):
                    r = run(n, S, U, kmax, workload, ops, pcut, pcut, combine, 2, seed, policy,
                            victim or 'fewest')
                    bad += 1 if r['nospace'] else 0
                    name = f'{policy}{" " + victim if victim else ""}, combine {combine}'
                    print(f'{label} | {workload}{" cuts" if pcut else ""} | {name} | '
                          f'{r["nospace"]} | {r["per_chunk"]:.2f} | {r["worst_copies"]} | '
                          f'{r["gc_commits"]} | {r["erase_min"]}..{r["erase_max"]} | '
                          f'{r["user_per_erase"]:.1f} | {U * S - n}')
    return 1 if bad else 0


# --- the sizing and cost tables of CHUNKED.md

def ceil_div(a, b):
    return -(-a // b)


def round_up(a, b):
    return ceil_div(a, b) * b


class Target:
    """An NVM and its timings. Defaults: the STM32U5A5 of EWLF.md, "Key figures"."""

    def __init__(self, page=8192, wu=16, prog_us=128.0, erase_ms=1.56, crc64_us=0.65,
                 crc32_us=0.3, endurance=10000, certs=32, reserved=8):
        self.page, self.wu, self.prog_us, self.erase_ms = page, wu, prog_us, erase_ms
        self.crc64_us, self.crc32_us, self.endurance = crc64_us, crc32_us, endurance
        self.certs, self.reserved = certs, reserved


class Layout:
    """A chunked area of data bytes with S chunks per erase unit."""

    def __init__(self, t, data, S):
        self.t, self.data, self.S = t, data, S
        self.slot = t.page // S
        self.marker = t.wu if S > 1 else 0
        self.chunk = self.slot - self.marker
        self.n = ceil_div(data, self.chunk)
        self.map_bytes = round_up(2 * self.n + 2, t.wu)
        self.record = t.wu + self.map_bytes + round_up(12, t.wu)
        self.records_per_unit = t.page // self.record
        self.pair = 2 * t.wu + round_up(16, t.wu)
        self.pairs = (t.page - t.certs * t.wu) // self.pair - t.reserved
        self.tracker = ceil_div(ceil_div(data, t.wu), 8) + 2 * self.n

    def words(self, nbytes):
        return nbytes // self.t.wu

    def commit_ms(self):
        """The map record and its pair, with the checksum of the map and the share of the
        erases of the map units and of the log units."""
        t = self.t
        w = self.words(self.record) + self.words(self.pair)
        return (w * t.prog_us + self.map_bytes * t.crc64_us) / 1000 \
            + t.erase_ms / self.records_per_unit + t.erase_ms / self.pairs

    def chunks_ms(self, k):
        """k chunks written, with the share of the erases of the pool units."""
        t = self.t
        return k * (self.words(self.slot) * t.prog_us / 1000 + t.erase_ms / self.S)

    def update_ms(self, k):
        return self.chunks_ms(k) + self.commit_ms()

    def chunks_of(self, nbytes):
        """Chunks an update of nbytes touches, on average, at any offset."""
        if nbytes >= self.data:
            return self.n
        return min(self.n, nbytes / self.chunk + 1 - 1 / self.chunk * self.t.wu)

    def pool_units(self, kmax, writes, k, static):
        """Pool units for `writes` updates of k chunks each: the room rule and the wear rule,
        `static` chunks never rewritten."""
        room = rule_units(self.n, self.S, kmax)
        wear = ceil_div(int(static + writes * k / self.t.endurance + 0.999999), self.S)
        return max(room, wear)

    def map_units(self, commits):
        return max(2, ceil_div(commits, self.t.endurance * self.records_per_unit))

    def area_units(self, kmax, writes, k, static):
        return self.pool_units(kmax, writes, k, static) + self.map_units(writes) + 2


def regular(t, data, updates):
    """A regular area: pages per slot, slots, total pages, update time in ms."""
    item = max(t.wu, 4)
    np_ = ceil_div(data + 3 * item, t.page)
    ns = max(2, ceil_div(updates, t.endurance))
    words = ceil_div(data, t.wu) + 3
    ms = np_ * t.erase_ms + (words * t.prog_us + data * t.crc32_us) / 1000
    return np_, ns, np_ * ns, ms


def kb(units, t):
    b = units * t.page
    return f'{b / 1048576:.2f} MB' if b >= 1000 * 1024 else f'{b // 1024} KB'


def tables(writes):
    t = Target()
    item = max(t.wu, 4)
    print(f'target: page {t.page} B, write unit {t.wu} B, {t.prog_us:.0f} us per write unit, '
          f'{t.erase_ms} ms per erase, CRC-64 {t.crc64_us} us per byte, CRC-32 '
          f'{t.crc32_us} us per byte (estimates), {t.endurance} erase cycles')
    print(f'data sizes: whole pages less {3 * item} bytes, the best case of a regular area '
          f'(its three meta-data items fit in the last page)')
    sizes = [(k * t.page - 3 * item, f'{k * t.page // 1024} KB') for k in (2, 8, 16)]
    print('\nLayout and costs')
    print('| Data | Chunks per page | Chunk | `n` | Map record | Update of one chunk | '
          'One unit evacuated, at most | Update of the whole data | Regular area, any update '
          '| Tracker |')
    print('|---|---|---|---|---|---|---|---|---|---|')
    for data, label in sizes:
        for S in (1, 4, 16, 32):
            L = Layout(t, data, S)
            if L.records_per_unit < 2:
                continue
            np_, ns, pages, ms = regular(t, data, 2 * t.endurance)
            evac = f'+ {L.chunks_ms(S - 1):.0f} ms' if S > 1 else 'never'
            print(f'| {label} | {S} | {L.chunk} B | {L.n} | {L.record} B | '
                  f'{L.update_ms(1):.1f} ms | {evac} | {L.update_ms(L.n):.0f} ms | '
                  f'{ms:.0f} ms | {L.tracker} B |')
    print('\nFlash an update of the whole data uses')
    print('| Data | Regular area | 1 chunk per page | 4 | 16 | 32 |')
    print('|---|---|---|---|---|---|')
    for k in (1, 2, 8, 16):
        for data, label in ((k * t.page - 3 * item, f'{k * t.page // 1024} KB less '
                             f'{3 * item} B'), (k * t.page, f'{k * t.page // 1024} KB')):
            np_, ns, pages, ms = regular(t, data, 1)
            cells = []
            for S in (1, 4, 16, 32):
                L = Layout(t, data, S)
                cells.append(f'{L.n * L.slot / t.page:.2f}')
            print(f'| {label} | {np_} pages | ' + ' | '.join(cells) + ' |')
    print(f'\nArea for {writes} writes, a transaction of the whole data allowed')
    print('| Data | Chunks per page | 1 chunk | 2 KB | 8 KB | the whole data | Regular area |')
    print('|---|---|---|---|---|---|---|')
    for data, label in sizes:
        for S in (1, 4, 16, 32):
            L = Layout(t, data, S)
            if L.records_per_unit < 2:
                continue
            cells = []
            for size in (0, 2048, 8192, data):
                if size == 0:
                    k = 1
                elif size >= data:
                    k = L.n
                else:
                    k = L.chunks_of(size)
                static = 0 if size >= data else L.n
                cells.append(kb(L.area_units(L.n, writes, k, static), t))
            np_, ns, pages, ms = regular(t, data, writes)
            print(f'| {label} | {S} | ' + ' | '.join(cells) + f' | {kb(pages, t)} |')
    print('\nSmallest area, a transaction of the whole data allowed')
    print('| Data | Chunks per page | Pool | Area | Regular area, 2 slots |')
    print('|---|---|---|---|---|')
    for k in (1, 2, 8, 16):
        data, label = k * t.page - 3 * item, f'{k * t.page // 1024} KB'
        for S in (1, 16):
            L = Layout(t, data, S)
            if L.records_per_unit < 2:
                continue
            pool = rule_units(L.n, S, L.n)
            np_, ns, pages, ms = regular(t, data, 1)
            print(f'| {label} | {S} | {pool} units | {kb(pool + L.map_units(1) + 2, t)} | '
                  f'{kb(pages, t)} |')
    return 0


# --- large areas: one chunk per erase unit, the map in the pair, a CRC-64 of the data

class Large:
    """A large area of data bytes: the pool, then two log units whose pairs hold the map
    (2 bytes per chunk, the head unit, the CRC-64 of the data)."""

    def __init__(self, t, data):
        self.t, self.data = t, data
        self.n = ceil_div(data, t.page)
        self.map = 2 * self.n + 2 + 8
        self.pair_words = ceil_div(self.map, t.wu) + 3
        self.pairs = (t.page // t.wu - t.certs) // self.pair_words - t.reserved
        self.commits = 2 * t.endurance * self.pairs
        self.tracker = ceil_div(ceil_div(data, t.wu), 8) + 2 * self.n

    def commit_ms(self):
        t = self.t
        return (self.pair_words * t.prog_us + (self.map + 6) * t.crc64_us) / 1000 \
            + t.erase_ms / self.pairs

    def update_ms(self, k):
        """k chunks written: their units erased and programmed, the commit, and the
        checksum of the data: afresh for the whole data, else the old and the new content
        of the chunks."""
        t = self.t
        crc = self.data if k >= self.n else 2 * k * t.page
        return k * (t.page // t.wu * t.prog_us / 1000 + t.erase_ms) + self.commit_ms() \
            + crc * t.crc64_us / 1000

    def area_units(self, kmax, writes, k, static):
        wear = int(static + writes * k / self.t.endurance + 0.999999)
        return max(self.n + kmax, wear) + 2


def large_tables(writes):
    t = Target()
    def size(pages):
        b = pages * t.page
        return f'{b // 1024} KB' if b < 1 << 20 else f'{b / (1 << 20):g} MB'
    def commits(c):
        return f'{c / 1e6:.2f} M' if c >= 1e6 else f'{c // 1000} k'
    def secs(ms):
        return f'{ms:.0f} ms' if ms < 1000 else f'{ms / 1000:.2f} s'
    print(f'target: page {t.page} B, write unit {t.wu} B, {t.prog_us:.0f} us per write unit, '
          f'{t.erase_ms} ms per erase, CRC-64 {t.crc64_us} us per byte, CRC-32 '
          f'{t.crc32_us} us per byte (estimates), {t.endurance} erase cycles, {t.certs} '
          f'certificates, {t.reserved} reserved pairs')
    print('data sizes: whole pages less 48 bytes, the best case of a regular area')
    print('\nLayout, time and wear of the log')
    print('| Data | Chunks | Map | Pair | Regular pairs per log unit | Commits the log endures | '
          'Update of one chunk | Update of the whole data | Regular area, any update | '
          'Check of the data | Tracker |')
    print('|---|---|---|---|---|---|---|---|---|---|---|')
    for pages in (1, 2, 4, 8, 16, 32, 64, 83, 128, 256):
        data = pages * t.page - 48
        L = Large(t, data)
        np_, ns, tot, ms = regular(t, data, 1)
        print(f'| {size(pages)} | {L.n} | {L.map} B | {L.pair_words * t.wu} B | {L.pairs} | '
              f'{commits(L.commits)} | {L.update_ms(1):.1f} ms | {secs(L.update_ms(L.n))} | '
              f'{secs(ms)} | {secs(data * t.crc64_us / 1000)} | {L.tracker} B |')
    print(f'\nArea, a transaction of the whole data allowed; {writes} writes')
    print('| Data | Smallest area | Regular area, 2 slots | Writes of one chunk | of two chunks | '
          'of the whole data | Regular area, any write |')
    print('|---|---|---|---|---|---|---|')
    for pages in (1, 2, 4, 8, 16, 32, 64):
        data = pages * t.page - 48
        L = Large(t, data)
        np_, ns, tot, ms = regular(t, data, writes)
        cells = [kb(L.area_units(L.n, writes, k, 0 if k >= L.n else L.n), t)
                 for k in (1, min(2, L.n), L.n)]
        print(f'| {size(pages)} | {kb(L.area_units(L.n, 1, 1, 0), t)} | {kb(2 * np_, t)} | '
              + ' | '.join(cells) + f' | {kb(tot, t)} |')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--suite', action='store_true')
    ap.add_argument('--compare', action='store_true')
    ap.add_argument('--tables', action='store_true')
    ap.add_argument('--large', action='store_true', help='the tables of LARGE.md')
    ap.add_argument('--less', type=int, default=0, help='--suite: units under the rule')
    ap.add_argument('--writes', type=int, default=500000, help='--tables: writes over life')
    ap.add_argument('-n', type=int, default=133, help='chunks')
    ap.add_argument('-S', type=int, default=16, help='slots per pool unit')
    ap.add_argument('-U', type=int, default=0, help='pool units (0: the sizing rule)')
    ap.add_argument('--kmax', type=int, default=1, help='largest operation, in chunks')
    ap.add_argument('--workload', default='uniform')
    ap.add_argument('--ops', type=int, default=200000)
    ap.add_argument('--pcut', type=float, default=0.0)
    ap.add_argument('--pabort', type=float, default=0.0)
    ap.add_argument('--policy', default='pass', choices=('pass', 'ring', 'any'))
    ap.add_argument('--victim', default='fewest', choices=('fewest', 'oldest'),
                    help='--policy any only')
    ap.add_argument('--combine', default='need', choices=('no', 'need', 'early'))
    ap.add_argument('--target', type=int, default=2)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--mem-mb', type=int, default=500, help='address-space cap, 0: none')
    ap.add_argument('--cpu-s', type=int, default=1800, help='CPU time cap, 0: none')
    a = ap.parse_args()
    import resource
    if a.mem_mb:
        resource.setrlimit(resource.RLIMIT_AS, (a.mem_mb << 20, a.mem_mb << 20))
    if a.cpu_s:
        resource.setrlimit(resource.RLIMIT_CPU, (a.cpu_s, a.cpu_s))
    if a.large:
        return large_tables(a.writes)
    if a.tables:
        return tables(a.writes)
    if a.suite:
        return suite(a.ops, a.seed, a.less)
    if a.compare:
        return compare(a.ops, a.seed)
    U = a.U or rule_units(a.n, a.S, a.kmax)
    r = run(a.n, a.S, U, a.kmax, a.workload, a.ops, a.pcut, a.pabort, a.combine, a.target,
            a.seed, a.policy, a.victim)
    print(f'n={a.n} S={a.S} U={U} kmax={a.kmax} {a.workload} ops={a.ops} pcut={a.pcut} '
          f'pabort={a.pabort} policy={a.policy} victim={a.victim} combine={a.combine} '
          f'target={a.target}')
    for k, v in r.items():
        print(f'  {k}: {v:.3f}' if isinstance(v, float) else f'  {k}: {v}')
    return 1 if r['nospace'] else 0


if __name__ == '__main__':
    sys.exit(main())
