#!/usr/bin/env python3
"""Exhaustive model check of chunked-area protocols (CHUNKED.md) against the tearing model of
doc/source/flash.rst.

The flash model is the one of ewlf_model.py, imported: weak words (T2), cut erases (T4), the
STM32U5A5 refusal rule. What differs is what the application does and what is checked. The
data is nchunks chunks. A session is a boot, then up to --app operations, each one of --ops:

- wXY: a plain write of chunks X and Y (one update, atomic);
- tXY: a transaction that writes chunk X, then chunk Y, then commits;
- aXY: a transaction that writes chunk X, then chunk Y, then aborts.

Operation k gives the value k to the chunks it writes. The content a boot shows must be the
content some operation left when it committed (the format's is rank 0); the rank of that
operation is then checked as a version is in ewlf_model.py:

- corrupt: a chunk does not read as a value written to it;
- torn: every chunk reads a value, but no operation left that content (a mix of two states,
  or the content of an aborted transaction);
- rollback, lost-ack, resurrection (--strict), boot-write (--clean-boot): as in ewlf_model.py;
- loss, stuck: as in ewlf_model.py; nospace: an operation finds no room in the pool;
- readback: within a session, the content read after an operation is not what it should be;
- unproven-*: the content read before the repair of the scan (the two-phase scan: phase 1
  reads, phase 2 repairs) is corrupt, torn, or older than what was acknowledged or shown.

A protocol module defines Geometry(*ints), format(geo), phase1(io, geo) -> handle,
phase2(io, geo, handle) -> ctx, content(io, geo, handle or ctx) -> tuple, and
apply(io, geo, ctx, kind, chunks, k) -> ctx.

Usage: chunked_model.py --protocol NAME [--cuts N] [--boots N] [--app N] [--strict]
       [--geometry nchunks,np,spu,...] [--ops w0,w1,w01,t01,a0] [--warmup w01,w0]
       [--ignore stuck,...] [--clean-boot] [--weak-erased] [--mem-mb N]
       (NAME loads proto_NAME.py; --warmup runs operations without any cut before the
       exploration starts, to begin from a pool in use; the process is capped at --mem-mb,
       1500 by default: run two checks at a time at most)
"""
import argparse
import hashlib
import importlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ewlf_model as em
from ewlf_model import PowerCut, ProgErr, ProtoError, Chooser, next_prefix, IO

FAILURES = ('stuck', 'loss', 'nospace')


def parse_kind(s):
    return s[0], tuple(int(c) for c in s[1:])


def is_bad(c):
    return isinstance(c, tuple)


def result_of(cur, kind, chunks, k):
    """The content operation k leaves when it commits, None when it never commits."""
    if kind == 'a':
        return None
    return tuple(k if i in chunks else v for i, v in enumerate(cur))


class Oracle:
    """What the application has seen. states[k] is the content operation k leaves if it
    commits (None for a transaction that aborts); states[0] is the format's."""

    def __init__(self, states, max_shown=0, acked=0, aborted=frozenset(), buried=frozenset()):
        self.states, self.max_shown, self.acked = states, max_shown, acked
        self.aborted, self.buried = aborted, buried

    def key(self, strict):
        if strict:
            return (self.states, self.max_shown, self.acked, self.aborted, self.buried)
        return (self.states, self.max_shown, self.acked)

    def rank(self, content):
        for k, s in enumerate(self.states):
            if s is not None and s == content:
                return k
        return None

    def unknown(self, content):
        return 'corrupt' if any(is_bad(c) for c in content) else 'torn'

    def show(self, content, strict):
        """Returns the violation of showing content, or None, and the next oracle."""
        k = self.rank(content)
        if k is None:
            return self.unknown(content), None
        if k < self.max_shown:
            return 'rollback', None
        if k < self.acked:
            return 'lost-ack', None
        if strict and k in self.buried:
            return 'resurrection', None
        buried = self.buried
        if strict:
            buried = buried | frozenset(w for w in self.aborted if w > k)
        return None, Oracle(self.states, max(self.max_shown, k), self.acked, self.aborted,
                            buried)


class Run:
    """One session, independent of the oracle: the content read before the repair (None if
    phase 1 did not finish), what was shown (None if the scan was cut, a content, or a
    failure kind), the operations started as (k, kind, chunks, outcome, content read after),
    whether the scan wrote, the flash left and the cuts used."""

    def __init__(self, flash, cuts_used, log, unproven, shown, ops, scan_wrote):
        self.flash, self.cuts_used, self.log = flash, cuts_used, log
        self.unproven, self.shown, self.ops, self.scan_wrote = unproven, shown, ops, scan_wrote


def failure_kind(e):
    name = type(e).__name__
    if name == 'Stuck' or isinstance(e, ProgErr):
        return 'stuck'
    if name == 'NoSpace':
        return 'nospace'
    return 'loss'


def run_session(proto, geo, flash, cuts_left, ch, app_max, kinds, next_k):
    log = []
    io = IO(geo, flash, ch, cuts_left, log)
    ops = []
    unproven = None

    def done(shown, scan_wrote):
        return Run(tuple(io.flash), cuts_left - io.cuts_left, tuple(log), unproven, shown,
                   tuple(ops), scan_wrote)

    try:
        h = proto.phase1(io, geo)
        unproven = proto.content(io, geo, h)
        log.append(f'UNPROVEN {unproven}')
        ctx = proto.phase2(io, geo, h)
        shown = proto.content(io, geo, ctx)
    except PowerCut:
        return done(None, True)
    except (ProtoError, ProgErr) as e:
        log.append(f'scan: {e!r}')
        return done(failure_kind(e), True)
    scan_wrote = any(l.startswith(('prog ', 'erase ')) for l in log)
    log.append(f'SHOWN {shown}')
    n = ch.choose(list(range(app_max + 1)))
    for j in range(n):
        kind, chunks = kinds[ch.choose(list(range(len(kinds))))] if len(kinds) > 1 else kinds[0]
        k = next_k + j
        log.append(f'app op {k}: {kind}{"".join(str(c) for c in chunks)}')
        try:
            ctx = proto.apply(io, geo, ctx, kind, chunks, k)
            after = proto.content(io, geo, ctx)
        except PowerCut:
            ops.append((k, kind, chunks, 'cut', None))
            return done(shown, scan_wrote)
        except (ProgErr, ProtoError) as e:
            log.append(f'op {k} fails: {e!r}')
            ops.append((k, kind, chunks, failure_kind(e), None))
            return done(shown, scan_wrote)
        log.append(f'op {k} returned, content {after}')
        ops.append((k, kind, chunks, 'returned', after))
    return done(shown, scan_wrote)


def sessions(proto, geo, flash, cuts_left, app_max, kinds, next_k):
    """Every session from this flash state, one per combination of choices. Not cached."""
    prefix = []
    while prefix is not None:
        ch = Chooser(prefix)
        yield run_session(proto, geo, flash, cuts_left, ch, app_max, kinds, next_k)
        prefix = next_prefix(ch.trace)


def judge(run, orc, strict, ignore, prev_clean, clean_boot):
    """Applies the oracle to a run: (violation, next oracle)."""
    if run.unproven is not None:
        k = orc.rank(run.unproven)
        if k is None:
            return 'unproven-' + orc.unknown(run.unproven), orc
        if k < orc.acked or k < orc.max_shown:
            return 'unproven-stale', orc
    if run.shown is None:
        return None, orc
    if run.shown in FAILURES:
        return (None if run.shown in ignore else run.shown), orc
    if clean_boot and prev_clean and run.scan_wrote and 'boot-write' not in ignore:
        return 'boot-write', orc
    violation, orc2 = orc.show(run.shown, strict)
    if violation:
        return violation, orc
    orc = orc2
    cur = run.shown
    for k, kind, chunks, outcome, after in run.ops:
        assert k == len(orc.states)
        new = result_of(cur, kind, chunks, k)
        states = orc.states + (new,)
        aborted = orc.aborted if new is None else orc.aborted | {k}
        orc = Oracle(states, orc.max_shown, orc.acked, aborted, orc.buried)
        if outcome == 'returned':
            if after != (cur if new is None else new):
                return 'readback', orc
            if new is not None:
                orc = Oracle(states, max(orc.max_shown, k), k, orc.aborted - {k}, orc.buried)
                cur = new
        elif outcome != 'cut' and outcome not in ignore:
            # no cut happened in this session (a cut ends it): the area refuses to progress
            return outcome, orc
    return None, orc


class Found(Exception):
    def __init__(self, path, violation):
        self.path, self.violation = path, violation


def warm_up(proto, geo, flash, ops):
    """Runs operations from a flash state without any cut: the state the exploration starts
    from, and the states the oracle knows. An operation that fails here is a violation too."""
    states = [(None,) * geo.nchunks]
    path = []
    for kind, chunks in ops:
        log = []
        path.append(log)
        io = IO(geo, flash, Chooser([]), 0, log)
        k = len(states)
        log.append(f'warm-up op {k}: {kind}{"".join(str(c) for c in chunks)}')
        try:
            ctx = proto.phase2(io, geo, proto.phase1(io, geo))
            cur = proto.content(io, geo, ctx)
            ctx = proto.apply(io, geo, ctx, kind, chunks, k)
            after = proto.content(io, geo, ctx)
        except (ProgErr, ProtoError) as e:
            log.append(f'op {k} fails: {e!r}')
            raise Found(path, failure_kind(e))
        new = result_of(cur, kind, chunks, k)
        log.append(f'op {k} returned, content {after}')
        if after != (cur if new is None else new):
            raise Found(path, 'readback')
        states.append(new)
        flash = tuple(io.flash)
    acked = max(k for k, s in enumerate(states) if s is not None)
    return flash, Oracle(tuple(states), acked, acked)


def explore(proto, geo, max_cuts, max_boots, app_max, kinds, strict, stats, ignore=(),
            clean_boot=False, warmup=()):
    flash0, orc0 = warm_up(proto, geo, proto.format(geo), warmup)
    visited = {}

    def dfs(flash, orc, cuts, boots, path, prev_clean):
        # a 128-bit digest of the state, not the state: a tenth of the memory
        key = hashlib.blake2b(repr((flash, orc.key(strict), cuts, prev_clean)).encode(),
                              digest_size=16).digest()
        remaining = max_boots - boots
        if visited.get(key, -1) >= remaining:
            return
        visited[key] = remaining
        if remaining == 0:
            return
        for r in sessions(proto, geo, flash, max_cuts - cuts, app_max, kinds, len(orc.states)):
            stats['sessions'] += 1
            violation, orc2 = judge(r, orc, strict, ignore, prev_clean, clean_boot)
            if violation:
                raise Found(path + [list(r.log)], violation)
            stats['acked'] = max(stats['acked'], orc2.acked)
            dfs(r.flash, orc2, cuts + r.cuts_used, boots + 1, path + [list(r.log)],
                r.cuts_used == 0)

    dfs(flash0, orc0, 0, 0, [], True)
    stats['states'] = len(visited)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--protocol', default='c2')
    ap.add_argument('--cuts', type=int, default=2)
    ap.add_argument('--boots', type=int, default=5)
    ap.add_argument('--app', type=int, default=1, help='operations per session, at most')
    ap.add_argument('--strict', action='store_true')
    ap.add_argument('--ignore', default='', help='violations to ignore, comma separated')
    ap.add_argument('--geometry', default='2,4,2',
                    help='chunks, pool units, slots per unit, then the protocol\'s own')
    ap.add_argument('--ops', default='w0,w1,w01,t01,a0',
                    help='the operations the application chooses from')
    ap.add_argument('--warmup', default='', help='operations run without cuts first')
    ap.add_argument('--clean-boot', action='store_true',
                    help='a scan after an uncut session must not program nor erase')
    ap.add_argument('--weak-erased', action='store_true',
                    help='a cut erase leaves the erased words weak too (off: they stay erased)')
    ap.add_argument('--mem-mb', type=int, default=1500,
                    help='address-space limit of this process in MB (0: none)')
    a = ap.parse_args()
    em.WEAK_ERASED_CELLS = a.weak_erased
    if a.mem_mb:
        import resource
        resource.setrlimit(resource.RLIMIT_AS, (a.mem_mb << 20, a.mem_mb << 20))
    proto = importlib.import_module('proto_' + a.protocol)
    geo = proto.Geometry(*[int(x) for x in a.geometry.split(',')])
    kinds = [parse_kind(x) for x in a.ops.split(',') if x]
    warmup = [parse_kind(x) for x in a.warmup.split(',') if x]
    stats = {'sessions': 0, 'states': 0, 'acked': 0}
    print(f'protocol {a.protocol}: cuts<={a.cuts} boots<={a.boots} app<={a.app} '
          f'geometry {a.geometry} ops {a.ops}{" warmup " + a.warmup if a.warmup else ""}'
          f'{" strict" if a.strict else ""}{" clean-boot" if a.clean_boot else ""}'
          f'{" weak-erased" if a.weak_erased else ""}'
          f'{" ignore " + a.ignore if a.ignore else ""}', flush=True)
    try:
        explore(proto, geo, a.cuts, a.boots, a.app, kinds, a.strict, stats,
                tuple(x for x in a.ignore.split(',') if x), a.clean_boot, warmup)
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
          f'highest acknowledged operation {stats["acked"]})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
