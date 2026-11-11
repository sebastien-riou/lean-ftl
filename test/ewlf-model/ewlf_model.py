#!/usr/bin/env python3
"""Exhaustive model check of EWLF commit protocols against the tearing model of
doc/source/flash.rst.

- T1: a power cut affects only the word being programmed or the unit being erased.
- T2: a word whose program was cut is weak. It may read its value, erased or garbage, drawn
  once per boot, differently from one boot to the next. T3 is never assumed.
- T4: a cut erase leaves weakly erased words, which may read their old value, erased or
  garbage. A word programmed into one may later read garbage.
- A program over a word that is not erased is refused (PROGERR), as on the STM32U5A5, with
  the one exception the STM32U5A5 makes: all zeros ('Z') over a word that reads partially
  programmed is accepted, over a word that reads all zeros it is refused. Zeros programmed
  are solid: every cell gets a full pulse. A valued word programmed into weakly erased cells
  (a torn erase, T4) may later read garbage.
- A protocol module defines format(geo), scan(io, geo) -> ctx with .cid, update(io, geo, ctx,
  cid) -> ctx, and may define its own Geometry(nd, rpu, nl, ppu) with any word layout: the
  checker only needs geo.addrs, geo.index, geo.units[(kind, unit)].

Every session (a boot, the scan, then up to --app updates) is a deterministic function of
its choices: the reading of each weak word it reads, whether each flash operation is cut,
and how many updates the application makes. The explorer enumerates all of them, depth
first over boots, and checks each shown version against the oracle:

- rollback: a version older than one shown at an earlier boot;
- lost-ack: a version older than the last update that returned;
- loss: no version at all;
- stuck: the scan cannot finish, or an update fails without any cut in its session (a
  refused program, or the protocol giving up): the area makes no progress;
- resurrection (--strict): an aborted write shown after a boot showed an older version;
- boot-write (--clean-boot): the scan programs or erases although the previous session
  was not cut (the clean-boot requirement; a weak word from an older cut reading
  differently is the usual cause).

Usage: ewlf_model.py --protocol NAME [--cuts N] [--boots N] [--app N] [--strict]
       [--geometry nd,rpu,nl,ppu] [--ignore stuck,...] [--clean-boot] [--mem-mb N]
       (NAME loads proto_NAME.py; the last line prints the highest acknowledged write;
       the process is capped at --mem-mb, 1500 by default: run one check at a time)
"""
import argparse
import importlib
import os
import sys

E, Z, V, G = 'E', 'Z', 'V', 'G'          # reading classes: erased, zeros, valid, garbage
SOLID, WEAK, WERASED, OVERWEAK = 'S', 'W', 'X', 'T'
WEAKGHOST = 'Y'                          # cut program over a weakly erased word: (new, old)


class PowerCut(Exception):
    pass


class ProgErr(Exception):
    """The flash refused a program: its target does not read erased physically."""


class ProtoError(Exception):
    """The protocol gives up: no version can be shown."""


class Geometry:
    """nd data units of rpu record positions, nl log units of ppu pair positions.
    Position 0 is the top of a unit, used first. A record is three words: start marker
    's', data 'd', meta 'm'. A pair is three words: marker 'k', entry 'e', confirm 'c'.
    Markers and confirms are all-zero words. A protocol may define its own Geometry with
    other words; any word may be programmed with 'Z' (all zeros)."""

    def __init__(self, nd, rpu, nl, ppu):
        self.nd, self.rpu, self.nl, self.ppu = nd, rpu, nl, ppu
        self.addrs = []
        for u in range(nd):
            for p in range(rpu):
                for f in 'sdm':
                    self.addrs.append(('D', u, p, f))
        for l in range(nl):
            for q in range(ppu):
                for f in 'kec':
                    self.addrs.append(('L', l, q, f))
        self.index = {a: i for i, a in enumerate(self.addrs)}
        self.units = {}
        for i, a in enumerate(self.addrs):
            self.units.setdefault((a[0], a[1]), []).append(i)



def _valid(val):
    return (Z,) if val == 'Z' else (V, val)


def reading_options(ws):
    kind, val = ws
    if kind == SOLID:
        return [(E,)] if val is None else [_valid(val)]
    if kind in (WEAK, WERASED):
        return [_valid(val), (E,)] if val == 'Z' else [_valid(val), (E,), (G,)]
    if kind == WEAKGHOST:
        # the new value, the old one the torn erase left (T4), erased or garbage
        new, old = val
        opts = [_valid(new), _valid(old), (E,), (G,)]
        return [o for i, o in enumerate(opts) if o not in opts[:i]]
    # OVERWEAK: programmed into a weakly erased word (T4)
    return [(Z,)] if val == 'Z' else [_valid(val), (G,)]


def fmt_addr(a):
    return f'{a[0]}{a[1]}.{a[2]}.{a[3]}'


def fmt_read(r):
    return r[0] if r[0] != V else f'V{r[1]}'


class Chooser:
    """Replays a prefix of choices, takes the first option past it, records the trace."""

    def __init__(self, prefix):
        self.prefix = prefix
        self.trace = []

    def choose(self, options):
        i = len(self.trace)
        c = self.prefix[i] if i < len(self.prefix) else 0
        self.trace.append((c, len(options)))
        return options[c]


def next_prefix(trace):
    for j in range(len(trace) - 1, -1, -1):
        c, n = trace[j]
        if c + 1 < n:
            return [t[0] for t in trace[:j]] + [c + 1]
    return None


class IO:
    def __init__(self, geo, flash, chooser, cuts_left, log):
        self.geo = geo
        self.flash = list(flash)
        self.ch = chooser
        self.cuts_left = cuts_left
        self.cache = {}
        self.log = log

    def read(self, addr):
        i = self.geo.index[addr]
        if i in self.cache:
            return self.cache[i]
        opts = reading_options(self.flash[i])
        if len(opts) == 1:
            r = opts[0]
        else:
            r = self.ch.choose(opts)
            self.log.append(f'{fmt_addr(addr)} reads {fmt_read(r)}')
        self.cache[i] = r
        return r

    def _cut(self, what):
        if self.cuts_left > 0 and self.ch.choose([False, True]):
            self.cuts_left -= 1
            self.log.append(f'CUT in {what}')
            return True
        return False

    def prog(self, addr, val):
        i = self.geo.index[addr]
        kind, old = self.flash[i]
        zero = val == 'Z'
        erased = kind == SOLID and old is None
        if kind == SOLID:
            # STM32U5A5: all zeros over a partially programmed word is accepted, anything
            # else over a non-erased word, and zeros over zeros, is refused
            ok = erased or (zero and old != 'Z')
        else:
            # a weak or weakly erased word may pass the flash's check or not, whatever is
            # programmed: it may read erased, partially programmed or all zeros right now
            ok = self.ch.choose([True, False])
        if not ok:
            self.log.append(f'PROGERR {fmt_addr(addr)}')
            raise ProgErr(addr)
        if self._cut(f'prog {fmt_addr(addr)}'):
            if kind == WERASED:
                self.flash[i] = (WEAKGHOST, (val, old))
            elif kind == WEAKGHOST:
                self.flash[i] = (WEAKGHOST, (val, old[1]))
            else:
                self.flash[i] = (WEAK, val)
            raise PowerCut()
        # all zeros gives every cell a full pulse: solid zeros, whatever the word held
        self.flash[i] = (SOLID, val) if erased or zero else (OVERWEAK, val)
        self.cache[i] = (Z,) if zero else (V, val)
        self.log.append(f'prog {fmt_addr(addr)}={val}')

    def erase(self, kind, unit):
        idxs = self.geo.units[(kind, unit)]
        if self._cut(f'erase {kind}{unit}'):
            for i in idxs:
                k, v = self.flash[i]
                if not (k == SOLID and v is None):
                    self.flash[i] = (WERASED, v)
            raise PowerCut()
        for i in idxs:
            self.flash[i] = (SOLID, None)
            self.cache[i] = (E,)
        self.log.append(f'erase {kind}{unit}')


class Oracle:
    """What the application has seen. cids number the application's writes in time order;
    the format wrote cid 0."""

    def __init__(self, max_shown=0, acked=0, next_cid=1, aborted=frozenset(),
                 buried=frozenset()):
        self.max_shown, self.acked, self.next_cid = max_shown, acked, next_cid
        self.aborted, self.buried = aborted, buried

    def key(self, strict):
        if strict:
            return (self.max_shown, self.acked, self.next_cid, self.aborted, self.buried)
        return (self.max_shown, self.acked, self.next_cid)

    def show(self, cid, strict):
        """Returns the violation of showing cid, or None, and the next oracle."""
        if cid < self.max_shown:
            return 'rollback', None
        if cid < self.acked:
            return 'lost-ack', None
        if strict and cid in self.buried:
            return 'resurrection', None
        buried = self.buried
        if strict:
            buried = buried | frozenset(w for w in self.aborted if w > cid)
        return None, Oracle(max(self.max_shown, cid), self.acked, self.next_cid,
                            self.aborted, buried)


class Run:
    """One session of the protocol, independent of the oracle: what it showed (None if the
    scan was cut, else a cid or a failure kind), the writes it started and whether each
    returned, whether its scan wrote, the flash it left and the cuts it used."""

    def __init__(self, flash, cuts_used, log, shown, writes, scan_wrote):
        self.flash, self.cuts_used, self.log = flash, cuts_used, log
        self.shown, self.writes, self.scan_wrote = shown, writes, scan_wrote


def run_session(proto, geo, flash, cuts_left, ch, app_max, next_cid):
    """One session driven by the chooser ch: the scan, then up to app_max updates numbered
    from next_cid. The oracle is applied afterwards (judge), so a run can be cached."""
    log = []
    io = IO(geo, flash, ch, cuts_left, log)
    writes = []

    def done(shown, scan_wrote):
        return Run(tuple(io.flash), cuts_left - io.cuts_left, tuple(log), shown, tuple(writes),
                   scan_wrote)

    try:
        ctx = proto.scan(io, geo)
    except PowerCut:
        return done(None, True)
    except ProtoError as e:
        log.append(f'scan: {e}')
        return done('stuck' if type(e).__name__ == 'Stuck' else 'loss', True)
    except ProgErr:
        return done('stuck', True)
    scan_wrote = any(l.startswith(('prog ', 'erase ')) for l in log)
    shown = ctx.cid
    log.append(f'SHOWN w{shown}')
    n = ch.choose(list(range(app_max + 1)))
    for k in range(n):
        cid = next_cid + k
        log.append(f'app writes w{cid}')
        try:
            ctx = proto.update(io, geo, ctx, cid)
        except PowerCut:
            writes.append((cid, 'cut'))
            return done(shown, scan_wrote)
        except (ProgErr, ProtoError) as e:
            log.append(f'update w{cid} fails: {e!r}')
            writes.append((cid, 'failed'))
            return done(shown, scan_wrote)
        log.append(f'w{cid} returned')
        writes.append((cid, 'returned'))
    return done(shown, scan_wrote)


def sessions(proto, geo, flash, cuts_left, app_max, next_cid):
    """Every session from this flash state, one per combination of choices. Not cached:
    a cache of runs is what a 3-cut run spends gigabytes on."""
    prefix = []
    while prefix is not None:
        ch = Chooser(prefix)
        yield run_session(proto, geo, flash, cuts_left, ch, app_max, next_cid)
        prefix = next_prefix(ch.trace)


def judge(run, orc, strict, ignore, prev_clean, clean_boot):
    """Applies the oracle to a run: (violation, next oracle)."""
    if run.shown is None:
        return None, orc
    if run.shown in ('stuck', 'loss'):
        return (None if run.shown in ignore else run.shown), orc
    if clean_boot and prev_clean and run.scan_wrote and 'boot-write' not in ignore:
        return 'boot-write', orc
    violation, orc = orc.show(run.shown, strict)
    if violation:
        return violation, orc
    for cid, outcome in run.writes:
        orc = Oracle(orc.max_shown, orc.acked, cid + 1, orc.aborted | {cid}, orc.buried)
        if outcome == 'returned':
            orc = Oracle(max(orc.max_shown, cid), cid, orc.next_cid, orc.aborted - {cid},
                         orc.buried)
        elif outcome == 'failed' and 'stuck' not in ignore:
            # no cut happened in this session (a cut ends it): the area refuses to progress
            return 'stuck', orc
    return None, orc


class Found(Exception):
    def __init__(self, path, violation):
        self.path, self.violation = path, violation


def explore(proto, geo, max_cuts, max_boots, app_max, strict, stats, ignore=(),
            clean_boot=False):
    flash0 = proto.format(geo)
    visited = {}

    def dfs(flash, orc, cuts, boots, path, prev_clean):
        key = (flash, orc.key(strict), cuts, prev_clean)
        remaining = max_boots - boots
        if visited.get(key, -1) >= remaining:
            return
        visited[key] = remaining
        if remaining == 0:
            return
        for r in sessions(proto, geo, flash, max_cuts - cuts, app_max, orc.next_cid):
            stats['sessions'] += 1
            violation, orc2 = judge(r, orc, strict, ignore, prev_clean, clean_boot)
            if violation:
                raise Found(path + [list(r.log)], violation)
            stats['acked'] = max(stats['acked'], orc2.acked)
            dfs(r.flash, orc2, cuts + r.cuts_used, boots + 1, path + [list(r.log)],
                r.cuts_used == 0)

    dfs(flash0, Oracle(), 0, 0, [], True)
    stats['states'] = len(visited)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--protocol', default='v2')
    ap.add_argument('--cuts', type=int, default=4)
    ap.add_argument('--boots', type=int, default=6)
    ap.add_argument('--app', type=int, default=1, help='updates per session, at most')
    ap.add_argument('--strict', action='store_true')
    ap.add_argument('--ignore', default='', help='violations to ignore, comma separated')
    ap.add_argument('--geometry', default='2,2,2,2',
                    help='data units, records per unit, log units, pairs per unit')
    ap.add_argument('--clean-boot', action='store_true',
                    help='a scan after an uncut session must not program nor erase')
    ap.add_argument('--mem-mb', type=int, default=1500,
                    help='address-space limit of this process in MB (0: none)')
    a = ap.parse_args()
    if a.mem_mb:
        import resource
        resource.setrlimit(resource.RLIMIT_AS, (a.mem_mb << 20, a.mem_mb << 20))
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    # the protocols import ewlf_model: make it this module, not a second copy
    sys.modules.setdefault('ewlf_model', sys.modules[__name__])
    proto = importlib.import_module('proto_' + a.protocol)
    geo = getattr(proto, 'Geometry', Geometry)(*[int(x) for x in a.geometry.split(',')])
    stats = {'sessions': 0, 'states': 0, 'acked': 0}
    print(f'protocol {a.protocol}: cuts<={a.cuts} boots<={a.boots} app<={a.app} '
          f'geometry {a.geometry}{" strict" if a.strict else ""}'
          f'{" clean-boot" if a.clean_boot else ""}'
          f'{" ignore " + a.ignore if a.ignore else ""}')
    try:
        explore(proto, geo, a.cuts, a.boots, a.app, a.strict, stats,
                tuple(x for x in a.ignore.split(',') if x), a.clean_boot)
    except Found as f:
        print(f'VIOLATION: {f.violation}')
        for b, log in enumerate(f.path, 1):
            print(f'  boot {b}:')
            for line in log:
                print(f'    {line}')
        return 1
    except MemoryError:
        print(f'OUT OF MEMORY at {stats["states"]} states ({a.mem_mb} MB limit): '
              f'no verdict, lower the bounds')
        return 2
    print(f'no violation ({stats["states"]} states, {stats["sessions"]} sessions, '
          f'highest acknowledged write w{stats["acked"]})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
