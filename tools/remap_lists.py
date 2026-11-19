#!/usr/bin/env python3
"""banjo-nx (2026-10-29, the title update): the performance lists re-keyed to another executable of the game (the base
XEX's lists to title update 3's), with tools/remap_addresses.py's Mapper.

Every function in a list is a generated symbol (sub_XXXXXXXX or a name from config/renut_funcs.toml) or an address. It
becomes the new executable's symbol for the function the map pairs it with: its name there if the new list names it,
else sub_<new address>; an address stays an address. An entry whose function (or call site) has no pair is dropped
and reported: every list here is a performance choice (the localized copies, the hot layout, the locals opt-outs, the
indirect promotions guarded by their target compare, the choice files of the proven rewrites), so a dropped entry only
loses its speed-up. hand.txt is the exception that is reported, not dropped silently: a hand-written native must stay on
its function (tools/fdiff on both listings); its namespace n_<id> moves with it (banjo_hand_natives.h).

  python tools/remap_lists.py --maps <dir> --old-gen <dir> --new-gen <dir> [--dry-run]
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from remap_addresses import Mapper  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')
HEX8 = re.compile(r'^(?:sub_|0x)?([0-9A-Fa-f]{8})$')


class Symbols:
    def __init__(self, m):
        self.m = m
        self.old_addr = dict(m.old.names)                       # symbol -> address (base)
        self.new_name = {a: n for n, a in m.new.names.items()}  # address -> symbol (new)
        self.dropped = []

    def addr_of(self, tok):
        h = HEX8.match(tok)
        if h:
            return int(h.group(1), 16)
        a = self.old_addr.get(tok)
        return int(a, 16) if a else None

    def func(self, tok, where, keep_form=False):
        """The new symbol (or address, for an address token) of a function token; None when it has no pair."""
        a = self.addr_of(tok)
        if a is None:
            self.dropped.append((where, tok, 'not a function of the old executable'))
            return None
        got, kind, note = self.m.map(a)
        if got is None or kind not in ('start', 'manual'):
            self.dropped.append((where, tok, 'no pair (%s)' % note))
            return None
        if HEX8.match(tok) and not tok.startswith('sub_') and not keep_form:
            return '%08X' % got
        return self.new_name.get('%08X' % got, 'sub_%08X' % got)

    def site(self, tok, where):
        a = self.addr_of(tok)
        got, kind, note = self.m.map(a)
        if got is None:
            self.dropped.append((where, tok, 'site: %s' % note))
            return None
        return '%08X' % got


def lines_file(path, fn, dry):
    out, n_in, n_out = [], 0, 0
    for line in open(path, encoding='utf-8'):
        if not line.strip() or line.lstrip().startswith('#'):
            out.append(line)
            continue
        n_in += 1
        new = fn(line.rstrip('\n'))
        if new is not None:
            out.append(new + '\n')
            n_out += 1
    if not dry:
        open(path, 'w', encoding='utf-8', newline='\n').writelines(out)
    print('%-40s %4d -> %4d entries' % (os.path.relpath(path, ROOT), n_in, n_out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--maps', required=True)
    ap.add_argument('--old-gen', required=True)
    ap.add_argument('--new-gen', required=True)
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    m = Mapper(a.maps, a.old_gen, a.new_gen)
    S = Symbols(m)
    m.prepare([])
    dry = a.dry_run

    # hand.txt: <function> [id=..] [exact=..] # comment. The id defaults to the token; it moves with the function.
    hand_ids = {}

    def hand(line):
        body, _, comment = line.partition('#')
        parts = body.split()
        new = S.func(parts[0], 'hand.txt')
        if new is None:
            print('  HAND NATIVE WITHOUT A PAIR: %s (its native is not used: localize_native.py finds no entry)' % parts[0])
            return None
        if not any(p.startswith('id=') for p in parts[1:]):
            hand_ids[parts[0]] = new
        rest = ' '.join(parts[1:])
        return ('%s %s' % (new, rest)).rstrip() + ('   #' + comment if comment else '')
    lines_file(os.path.join(APP, 'src', 'cpu', 'hot', 'hand.txt'), hand, dry)
    hdr = os.path.join(APP, 'src', 'cpu', 'hot', 'banjo_hand_natives.h')
    t = open(hdr, encoding='utf-8').read()
    for old, new in hand_ids.items():
        t = re.sub(r'\bn_%s\b' % re.escape(old), 'n_' + new, t)
    if not dry:
        open(hdr, 'w', encoding='utf-8', newline='').write(t)

    # localized.txt / localized_bulk.txt: <function> [key=..] description.
    def first_token(where):
        def fn(line):
            parts = line.split(None, 1)
            new = S.func(parts[0], where)
            return None if new is None else new + (' ' + parts[1] if len(parts) > 1 else '')
        return fn
    lines_file(os.path.join(APP, 'src', 'cpu', 'hot', 'localized.txt'), first_token('localized.txt'), dry)
    lines_file(os.path.join(APP, 'src', 'cpu', 'hot', 'localized_bulk.txt'), first_token('localized_bulk.txt'), dry)
    lines_file(os.path.join(APP, 'config', 'hot_functions.txt'), first_token('hot_functions.txt'), dry)

    def optout(line):
        tool, name = line.split()[:2]
        new = S.func(name, 'locals_optout.txt')
        return None if new is None else '%s %s' % (tool, new)
    lines_file(os.path.join(APP, 'config', 'locals_optout.txt'), optout, dry)

    def edge(line):
        a_, b_ = line.split()[:2]
        x, y = S.func(a_, 'hot_edges.txt'), S.func(b_, 'hot_edges.txt')
        return None if x is None or y is None else '%s %s' % (x, y)
    lines_file(os.path.join(APP, 'config', 'hot_edges.txt'), edge, dry)

    # indirect_promotions.txt: <caller function> <its n-th bctrl> <target> [<target> ...]  # comment. The caller and the
    # targets are function starts; n stays (the guard compares the target, so a reordered call is only slower).
    # tools/indirect_calls.py refuses a list whose caller has fewer bctrl than n + 1: run it, or count them first.
    def promo(line):
        body, _, comment = line.partition('#')
        parts = body.split()
        caller = S.func(parts[0], 'indirect_promotions.txt')
        if caller is None:
            return None
        targets = [x for x in (S.func(p, 'indirect_promotions.txt (target)') for p in parts[2:])
                   if x is not None]
        if not targets:
            return None
        caller = caller[4:] if caller.startswith('sub_') else caller
        targets = [x[4:] if x.startswith('sub_') else x for x in targets]
        return ' '.join([caller, parts[1]] + targets) + ('   #' + comment if comment else '')
    lines_file(os.path.join(APP, 'indirect_promotions.txt'), promo, dry)

    for name in ('lr_write_same_choice.json', 'r1_local_choice.json'):
        p = os.path.join(APP, 'config', name)
        old = json.load(open(p))
        new = [x for x in (S.func(n, name) for n in old) if x is not None]
        if not dry:
            json.dump(sorted(set(new)), open(p, 'w'))
        print('%-40s %4d -> %4d entries' % (os.path.relpath(p, ROOT), len(old), len(set(new))))

    rep = os.path.join(a.maps, 'remap_lists_dropped.tsv')
    with open(rep, 'w', encoding='utf-8') as f:
        for w, tok, why in S.dropped:
            f.write('%s\t%s\t%s\n' % (w, tok, why))
    print('dropped %d entries (%s)' % (len(S.dropped), rep))


if __name__ == '__main__':
    main()
