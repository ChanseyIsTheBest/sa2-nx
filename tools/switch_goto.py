#!/usr/bin/env python3
"""cpu_aggressive (proposed post-codegen step): the generator's jump-table `switch` statements (a guest
`lwzx; mtctr; bctr`) as GCC computed gotos, so that a bytecode interpreter's dispatch is duplicated at the end of each
of its handlers ("threaded" dispatch: one indirect branch per handler, each with its own prediction history).

Today:
    switch (r11.u32) { case 0: goto loc_A; case 1: goto loc_B; ... default: __builtin_trap(); // Switch case out of range }
GCC compiles every such switch to ONE indirect branch, which every bytecode of the game's script interpreters goes
through (sub_82948928, sub_8294EF68, sub_82949790: 4.3 % of the running guest-thread samples of package 21b; the
dispatch line of sub_82948928 alone holds a third of its samples). GCC never duplicates a switch, but it does
duplicate a small computed goto into its predecessors (bb-reorder's duplicate_computed_gotos, at -O2 and up).

Rewrite (cases must be 0..N-1, every one a plain goto; anything else is left alone):
    { static const void* const rex_jt_<site>[N] = { &&loc_A, &&loc_B, ... };
      if (REX_SWITCH_GOTO_IN_RANGE(idx, N)) goto *rex_jt_<site>[idx]; __builtin_trap(); }
Exact: the same targets for every index, the same trap outside 0..N-1. Switch back: REX_SWITCH_GOTO_OFF makes
REX_SWITCH_GOTO_IN_RANGE false... no: the OFF form must keep the switch; so the rewrite writes both forms:
    #if !defined(REX_SWITCH_GOTO_OFF)  <computed goto>  #else  <the original switch>  #endif
(the preprocessor lines inside the function body; the instruction comment that names `bctr` stays in front).

  python switch_goto.py --gen <tree> [--only name1,name2] [--check]
"""
import argparse
import os
import re

FUNC = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
SW = re.compile(r'^(\t+)switch \(([\w.]+)\) \{$')
CASE = re.compile(r'^\t+case (\d+):$')
GOTO = re.compile(r'^\t+goto (loc_[0-9A-F]+);$')
DEFAULT = re.compile(r'^\t+default:$')
TRAP = re.compile(r'^\t+__builtin_trap\(\);.*$')


def rewrite(lines, fname, stats, thread=False):
    out = []
    tables = []
    sites = []          # (index in out of the computed-goto line, table name, idx expr)
    i = 0
    changed = False
    while i < len(lines):
        m = SW.match(lines[i])
        if not m:
            out.append(lines[i])
            i += 1
            continue
        ind, idx = m.group(1), m.group(2)
        j = i + 1
        targets = {}
        ok = True
        while j < len(lines):
            c = CASE.match(lines[j])
            if c and j + 1 < len(lines) and GOTO.match(lines[j + 1]):
                targets[int(c.group(1))] = GOTO.match(lines[j + 1]).group(1)
                j += 2
                continue
            if DEFAULT.match(lines[j]) and j + 2 < len(lines) and TRAP.match(lines[j + 1]) and \
                    lines[j + 2] == ind + '}':
                j += 3
                break
            ok = False
            break
        n = len(targets)
        if not ok or n == 0 or sorted(targets) != list(range(n)):
            stats['left'] += 1
            out.append(lines[i])
            i += 1
            continue
        site = '%s_%d' % (fname, stats['done'])
        table = ', '.join('&&%s' % targets[k] for k in range(n))
        tables.append('	static const void* const rex_jt_%s[%d] = { %s };' % (site, n, table))
        # The guest's own bound check (`cmplwi cr6,rX,N-1; bgt default`) in front of the jump table, with no write
        # of rX after it: the index is then always below N, and the computed goto needs no test of its own (GCC
        # duplicates a computed goto only into predecessors that jump to it unconditionally).
        guarded = False
        reg = idx.split('.')[0]
        for k in range(len(out) - 1, -1, -1):
            t = out[k].strip()
            if re.match(r'^loc_[0-9A-F]+:$', t):
                break
            if t == 'cr6.compare<uint32_t>(%s, %d, xer);' % (idx, n - 1):
                nxt = [x.strip() for x in out[k + 1:] if not x.strip().startswith('//')]
                guarded = bool(nxt) and re.match(r'^if \(cr6\.gt\) goto loc_[0-9A-F]+;$', nxt[0]) is not None
                break
            if re.match(r'^%s\.\w+ = ' % re.escape(reg), t):
                break
        stats['guarded'] += guarded
        out.append('#if !defined(REX_SWITCH_GOTO_OFF)')
        sites.append((len(out), site, idx, n, guarded))
        if guarded:
            out.append('%s{ goto *rex_jt_%s[%s]; }' % (ind, site, idx))
        else:
            out.append('%s{ if (%s < %du) goto *rex_jt_%s[%s]; __builtin_trap(); }' % (ind, idx, n, site, idx))
        out.append('#else')
        out.extend(lines[i:j])
        out.append('#endif')
        stats['done'] += 1
        changed = True
        i = j
    if not changed:
        return None
    if thread:
        out = thread_dispatch(out, sites, stats)
    # the tables at the top of the function (copies of a dispatch may come before the switch in the text)
    k = out.index('	REX_FUNC_PROLOGUE();') + 1 if '	REX_FUNC_PROLOGUE();' in out else 0
    out[k:k] = ['#if !defined(REX_SWITCH_GOTO_OFF)'] + tables + ['#endif']
    return out


LABEL = re.compile(r'^(loc_[0-9A-F]+):$')
UNSAFE_COPY = re.compile(r'\(ctx, base|REX_CALL|ppc_trap|return|switch|REX_LOOP_IN|REX_LOOP_OUT|\{|\}|#')


def thread_dispatch(out, sites, stats):
    """Copy each dispatch block (from its label to the computed goto) into the gotos that jump to that label."""
    copies = {}
    for pos, site, idx, n, guarded in sites:
        k = pos - 2                      # the line before '#if !defined(REX_SWITCH_GOTO_OFF)'
        body = []
        while k >= 0 and not LABEL.match(out[k]):
            t = out[k]
            if not t.strip().startswith('//'):
                body.append(t.strip())
            k -= 1
        if k < 0 or len(body) > 24 or any(UNSAFE_COPY.search(t) for t in body):
            stats['thread_skip'] += 1
            continue
        body.reverse()
        body = [re.sub(r'\bgoto (loc_[0-9A-F]+);', r'REX_TGOTO(\1);', t) for t in body]
        body.append(('goto *rex_jt_%s[%s];' % (site, idx)) if guarded else
                    ('if (%s < %du) goto *rex_jt_%s[%s]; __builtin_trap();' % (idx, n, site, idx)))
        copies[LABEL.match(out[k]).group(1)] = ' '.join(body)
    if not copies:
        return out
    res = []
    for t in out:
        m = re.match(r'^(	+)(if \(.*\) )?goto (loc_[0-9A-F]+);$', t)
        if m and m.group(3) in copies:
            ind, cond, lab = m.group(1), m.group(2) or '', m.group(3)
            res.append('#if !defined(REX_SWITCH_GOTO_OFF) && !defined(REX_SWITCH_THREAD_OFF)')
            res.append('%s%s{ %s }' % (ind, cond, copies[lab]))
            res.append('#else')
            res.append(t)
            res.append('#endif')
            stats['threaded'] += 1
        else:
            res.append(t)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--only')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--thread', action='store_true', help='also copy each dispatch into the gotos to it')
    ap.add_argument('--restore', action='store_true', help='take an earlier run back out (the original statements)')
    a = ap.parse_args()
    if a.restore:
        n = 0
        for f in sorted(os.listdir(a.gen)):
            if not re.match(r'\w+_recomp\.\d+\.cpp$', f):
                continue
            path = os.path.join(a.gen, f)
            lines = open(path, encoding='utf-8').read().split('\n')
            if not any(l.startswith('#if !defined(REX_SWITCH_GOTO_OFF)') for l in lines):
                continue
            out = []
            i = 0
            while i < len(lines):
                l = lines[i]
                if l in ('#pragma GCC push_options', '#pragma GCC optimize ("no-crossjumping")', '#pragma GCC pop_options'):
                    i += 1
                    continue
                if l.startswith('#if !defined(REX_SWITCH_GOTO_OFF)'):
                    j = i + 1
                    while lines[j] not in ('#else', '#endif'):
                        j += 1
                    if lines[j] == '#else':
                        k = j + 1
                        while lines[k] != '#endif':
                            k += 1
                        out.extend(lines[j + 1:k])
                        i = k + 1
                    else:
                        i = j + 1
                    continue
                out.append(l)
                i += 1
            open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
            n += 1
        print('switch_goto: restored %d files' % n)
        return
    only = set(a.only.split(',')) if a.only else None
    stats = {'done': 0, 'left': 0, 'files': 0, 'threaded': 0, 'thread_skip': 0, 'guarded': 0}
    for f in sorted(os.listdir(a.gen)):
        if not re.match(r'\w+_recomp\.\d+\.cpp$', f):
            continue
        path = os.path.join(a.gen, f)
        lines = open(path, encoding='utf-8').read().split('\n')
        out = []
        i = 0
        changed = False
        while i < len(lines):
            m = FUNC.match(lines[i])
            if not m:
                out.append(lines[i])
                i += 1
                continue
            j = i + 1
            while j < len(lines) and lines[j] != '}':
                j += 1
            body = lines[i + 1:j]
            name = m.group(1)
            before_t = stats['threaded']
            r = rewrite(body, name, stats, a.thread) if (only is None or name in only) and 'switch (' in '\n'.join(body) else None
            if r is not None and a.thread and stats['threaded'] > before_t:
                # GCC's cross-jumping merges the copies back into one dispatch: off for this function only
                out.append('#pragma GCC push_options')
                out.append('#pragma GCC optimize ("no-crossjumping")')
                out.append(lines[i])
                out.extend(r)
                out.append(lines[j])
                out.append('#pragma GCC pop_options')
                changed = True
                i = j + 1
                continue
            out.append(lines[i])
            out.extend(r if r is not None else body)
            changed |= r is not None
            i = j
        if changed:
            stats['files'] += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    pch = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if pch and not a.check and stats['done']:
        pp = os.path.join(a.gen, pch[0])
        t = open(pp, encoding='utf-8').read()
        mark = '// cpu_aggressive: switch dispatch as computed gotos (switch_goto.py)'
        if mark not in t:
            t += '\n' + mark + '\n#ifndef REX_TGOTO\n#define REX_TGOTO(label) goto label\n#endif\n'
            open(pp, 'w', encoding='utf-8', newline='\n').write(t)
    print('switch_goto: %s' % stats)


if __name__ == '__main__':
    main()
