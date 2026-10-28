#!/usr/bin/env python3
"""Write-backs of only the registers that changed: one exact transformation of the localized copies (sa2-nx,
2026-10-28; from bejeweled3-nx's tools/hand_native_dirty_sync.py, the Peggle 2 session's, made stricter for SA2's
vector code). tools/localize_native.py applies it to every copy it makes (limit_writebacks below).

A localized copy (app/generated/hot/n_<ADDR>.h) keeps the function's volatile registers in C++ locals l_X and wrote all
of them back to the context at every return and before every fully synced call (SA2_SYNC_OUT), changed or not, and
the smart-synced calls stored every register their callee can reach. The compiler has to keep those stores: the
context is memory the callee reads, and as far as it knows a guest store could alias it. On bejeweled3-nx's console
the copies' hottest instructions were those stores; written this way its copies took 16-42 % less time a call.

Each SA2_SYNC_OUT() becomes the stores of the registers that may differ from the context at that point, and each
smart-synced call keeps only those of its stores; from a may-differ dataflow over the copy's lines:
  - at entry no register differs (every l_X was loaded from ctx.X);
  - a line that may write l_X (or its loop local ll_X, a reference to it when loop locals are off) makes X differ;
  - a fully synced call ({ SA2_SYNC_OUT(); call; SA2_SYNC_IN(); }) leaves none differing (all reloaded); a promoted
    indirect call (both of its branches fully synced) the same;
  - a smart-synced call ({ ctx.X = l_X; ... call; l_X = ctx.X; ... }) leaves the registers it stores and reloads equal
    and the others as they were: the callee cannot reach them (localize_native.py's reach analysis), so neither the
    local nor the context changes;
  - a conditional call or return keeps the state (the union of its two paths: the synced registers are a subset);
  - control flow: labels, gotos, conditional gotos and the return blocks as the copy has them. Anything else (a
    switch, an else other than the promoted call's, a loop keyword) leaves the copy with its full write-backs.
A register that cannot differ holds in ctx exactly the value its store would write, so leaving the store out leaves
the context exactly as the copy left it; every other statement is unchanged. Nothing else writes the context's
localized registers: the copy names them only as l_X (localize_native.py replaced every ctx.X), guest stores go to
guest memory (never the host's PPCContext), and the context is the thread's own.

What counts as a write (stricter than bejeweled3-nx's, which looks for assignments only: SA2's copies write vector
registers through pointers, simde_mm_store_ps(l_v0.f32, ...), and call methods on registers, l_cr6.compare(...)):
every occurrence of l_X / ll_X on a line is a write unless it is one of these reads:
  - a field of a general or float register (l_r3.u32, l_f1.f64) not followed by an assignment, an increment, a call
    or an index, and not after an address-of `&`;
  - an element of a vector register (l_v3.f32[2]) not followed by an assignment;
  - a vector register's field as the argument of a simde load (simde_mm_load_ps(l_v3.f32),
    simde_mm_load_si128((simde__m128i*)l_v3.u8)).
A bare l_X (passed by reference), an address-of and anything unrecognised count as writes: at worst a store is kept.

  python tools/localize_writebacks.py ADDR [ADDR ...]     (reports what the copies in app/generated/hot would get)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOT = os.path.join(ROOT, 'app', 'generated', 'hot')

ASSIGN = r'(?:=(?!=)|\+=|-=|\*=|/=|%=|\|=|&=|\^=|<<=|>>=|\+\+|--)'
VECTOR_LOAD = re.compile(r'simde_mm_load\w*\(\s*(?:\(\s*(?:const\s+)?simde__m\w+\s*\*\s*\)\s*)?$')


class Unsupported(ValueError):
    pass


def line_writes(line, regset):
    """The localized registers a statement line may write (see the docstring: anything not a recognised read)."""
    w = set()
    for m in re.finditer(r'\bl{1,2}_(\w+)\b', line):
        reg = m.group(1)
        if reg not in regset:
            continue
        head, tail = line[:m.start()], line[m.end():]
        # Prefix increment / decrement, address-of (an & after an operator, a bracket or a comma, not a binary and).
        if re.search(r'(?:\+\+|--)\s*$', head):
            w.add(reg)
            continue
        if re.search(r'(?:^|[(,=!<>?:+\-*/%|^~]|\breturn)\s*&\s*$', head) or re.search(r'&&\s*&\s*$', head):
            w.add(reg)
            continue
        fm = re.match(r'\.(\w+)', tail)
        if not fm:
            w.add(reg)  # bare use: passed by reference, assigned whole, unknown
            continue
        after = tail[fm.end():]
        indexed = re.match(r'\s*\[[^\]]*\]', after)
        rest = after[indexed.end():] if indexed else after
        if re.match(r'\s*' + ASSIGN, rest) or re.match(r'\s*[(<]', after):
            w.add(reg)  # assigned (as a whole field or an element), or a method called on it
            continue
        if reg.startswith('v') and not indexed:
            if VECTOR_LOAD.search(head):
                continue  # a simde load's source
            w.add(reg)  # a vector register's array as an argument: may be a store's destination
            continue
        if not reg.startswith('v') and indexed:
            w.add(reg)  # an index into a scalar field: not a form the generated code has
            continue
        # A field read.
    return w


# A copy's inline constant-size memcpy / memset (localize_native.py MEMCALL_LINE): the registers the helper reads
# stored first, the helper tried, the call synced as any other when it declines.
MEMCALL = re.compile(r'\{ ((?:ctx\.\w+ = l_\w+; )*)if \(!SA2_MEM(CPY|SET)_INLINE\((\d+)\)\) (\{.*\}) \}')


def memcall_pre(line):
    m = MEMCALL.fullmatch(line.strip())
    return set(re.findall(r'ctx\.(\w+) = l_\1;', m.group(1)))


def limit_writebacks(addr, text):
    """The copy's text with its write-backs limited; returns (text, write-backs rewritten, register stores kept,
    register stores before). Raises Unsupported for a copy it cannot follow (the caller keeps the copy as it is)."""
    m = re.search(r'#define SA2_SYNC_OUT\(\) do \{ (.*?); \} while \(0\)', text)
    if not m:
        raise Unsupported(f'{addr}: no SA2_SYNC_OUT definition')
    regs = re.findall(r'ctx\.(\w+) = l_\1', m.group(1))
    if not regs:
        raise Unsupported(f'{addr}: no registers in SA2_SYNC_OUT')
    regset = set(regs)
    lines = text.split('\n')
    out = list(lines)
    starts = [i for i, l in enumerate(lines) if l == 'inline void Native(PPCContext& ctx, uint8_t* base) {']
    if len(starts) != 1:
        raise Unsupported(f'{addr}: not one Native body')
    start = starts[0]
    end = next(j for j in range(start + 1, len(lines)) if lines[j] == '}')
    body = list(range(start + 1, end))
    labels = {}
    for i in body:
        lm = re.match(r'^(loc_\w+):\s*$', lines[i])
        if lm:
            labels[lm.group(1)] = i

    def smart_regs(line):
        stored = set(re.findall(r'ctx\.(\w+) = l_\1;', line))
        loaded = set(re.findall(r'\bl_(\w+) = ctx\.\1;', line))
        if stored != loaded or not stored <= regset:
            raise Unsupported(f'{addr}: a call whose stores and reloads differ: {line.strip()[:160]}')
        return stored

    def kind(line):
        s = line.strip()
        if not s or s.startswith('//'):
            return 'plain'
        if s.startswith('#'):
            return 'define'  # the SA2_SYNC_* definitions and their #undef
        if re.fullmatch(r'PPC\w*Register l_(\w+) = ctx\.\1;', s):
            return 'decl'
        if MEMCALL.fullmatch(s):
            return 'memcall'
        n_out, n_in = s.count('SA2_SYNC_OUT()'), s.count('SA2_SYNC_IN()')
        cond = s.startswith('if ')
        if 'else' in s:
            # A promoted indirect call: if (REX_PROMOTED_IS(...)) [[likely]] { REX_PROMOTED_CALL(...); { <sync>
            # __imp__sub_X(ctx, base); <reload> } } else { { SA2_SYNC_OUT(); REX_CALL_INDIRECT...; SA2_SYNC_IN(); } }:
            # each path one call, fully or smart synced.
            if (s.startswith('if (REX_PROMOTED_IS(') and s.count('else') == 1 and s.count('} else {') == 1 and
                    'return' not in s and 'goto' not in s):
                for branch in s.split('} else {'):
                    if branch.count('SA2_SYNC_OUT()') != branch.count('SA2_SYNC_IN()') or branch.count('SA2_SYNC_OUT()') > 1:
                        raise Unsupported(f'{addr}: unhandled promoted call: {s[:160]}')
                    if 'SA2_SYNC_OUT()' not in branch:
                        smart_regs(branch)
                return 'promoted'
            raise Unsupported(f'{addr}: unhandled else: {s[:160]}')
        if re.search(r'\b(?:switch|case|while|for|do|break|continue)\b', s):
            raise Unsupported(f'{addr}: unhandled control flow: {s[:160]}')
        if n_out == 1 and 'return;' in s and n_in == 0:
            return 'cond_return' if cond else 'return'
        if n_out == 1 and n_in == 1:
            return 'cond_sync' if cond else 'sync'
        if s == 'SA2_SYNC_OUT();':
            return 'tail'
        if n_out or n_in:
            raise Unsupported(f'{addr}: unhandled sync form: {s[:160]}')
        if re.search(r'\(ctx, base\)', s) and re.search(r'ctx\.\w+ = l_\w+;', s):
            return 'cond_smart' if cond else 'smart'
        if any(r in regset for r in re.findall(r'\bctx\.(\w+)', s)):
            raise Unsupported(f'{addr}: a localized register named through ctx: {s[:160]}')
        if re.fullmatch(r'goto (loc_\w+);', s):
            return 'goto'
        if re.fullmatch(r'if \(.*\) goto (loc_\w+);', s):
            return 'cond_goto'
        if re.search(r'\bgoto\b|\breturn\b', s):
            raise Unsupported(f'{addr}: unhandled control flow: {s[:160]}')
        return 'plain'

    kinds = {i: kind(lines[i]) for i in body}

    def succ(i):
        k = kinds[i]
        nxt = [i + 1] if i + 1 < end else []
        if k in ('return', 'tail'):
            return []
        target = re.search(r'goto (loc_\w+);', lines[i])
        if k in ('goto', 'cond_goto') and target.group(1) not in labels:
            raise Unsupported(f'{addr}: goto to an unknown label {target.group(1)}')
        if k == 'goto':
            return [labels[target.group(1)]]
        if k == 'cond_goto':
            return nxt + [labels[target.group(1)]]
        return nxt

    def transfer(i, d):
        k = kinds[i]
        line = lines[i]
        if k == 'smart':
            return d - smart_regs(line)
        if k == 'memcall':
            # The registers stored before the inline helper are equal on both paths; the call path (only when the
            # helper declines) leaves at most those differing that the inline path does.
            return d - memcall_pre(line)
        if k == 'sync':
            return frozenset()
        if k == 'promoted':
            # The union of the two paths: a fully synced one leaves nothing differing, a smart one its unsynced part.
            result = frozenset()
            for branch in line.split('} else {'):
                result |= frozenset() if 'SA2_SYNC_OUT()' in branch else (d - smart_regs(branch))
            return result
        if k in ('decl', 'define', 'cond_sync', 'cond_return', 'cond_smart', 'goto', 'cond_goto', 'return', 'tail'):
            return d
        return d | line_writes(line, regset)

    state = {body[0]: frozenset()}
    work = [body[0]]
    while work:
        i = work.pop()
        d_out = frozenset(transfer(i, state[i]))
        for j in succ(i):
            if j >= end:
                continue
            old = state.get(j)
            new = d_out if old is None else old | d_out
            if new != old:
                state[j] = new
                work.append(j)

    changed = kept = before = 0
    for i in body:
        k = kinds[i]
        if i not in state:
            continue  # unreachable from the entry: left as it is
        d = state[i]
        if k == 'memcall':
            m = MEMCALL.fullmatch(lines[i].strip())
            pre = memcall_pre(lines[i])
            pre_kept = [r for r in regs if r in pre and r in d]
            inner_d = d - pre
            inner = m.group(4)
            if 'SA2_SYNC_OUT()' in inner:
                stores = ' '.join(f'ctx.{r} = l_{r};' for r in regs if r in inner_d)
                inner = inner.replace('SA2_SYNC_OUT()', f'do {{ {stores} }} while (0)' if stores else 'do {} while (0)')
                changed += 1
                kept += sum(1 for r in regs if r in inner_d)
                before += len(regs)
            elif re.search(r'ctx\.(\w+) = l_\1;', inner):
                synced = smart_regs(inner)
                for r in synced - inner_d:
                    inner = inner.replace(f'ctx.{r} = l_{r}; ', '', 1)
                changed += 1
                kept += len(synced & inner_d)
                before += len(synced)
            indent = lines[i][:len(lines[i]) - len(lines[i].lstrip())]
            out[i] = (indent + '{ ' + ''.join(f'ctx.{r} = l_{r}; ' for r in pre_kept) +
                      f'if (!SA2_MEM{m.group(2)}_INLINE({m.group(3)})) {inner} }}')
            changed += 1
            kept += len(pre_kept)
            before += len(pre)
            continue
        if k not in ('sync', 'cond_sync', 'return', 'cond_return', 'tail', 'promoted', 'smart', 'cond_smart'):
            continue
        # Every write-back on the line starts from the line's state d (a promoted call's two paths both do).
        stores = ' '.join(f'ctx.{r} = l_{r};' for r in regs if r in d)
        parts = lines[i].split('} else {') if k == 'promoted' else [lines[i]]
        for p, part in enumerate(parts):
            if 'SA2_SYNC_OUT()' in part:
                # A full write-back: only the registers that differ.
                n = part.count('SA2_SYNC_OUT()')
                part = part.replace('SA2_SYNC_OUT()', f'do {{ {stores} }} while (0)' if stores else 'do {} while (0)')
                changed += n
                kept += n * sum(1 for r in regs if r in d)
                before += n * len(regs)
            elif re.search(r'ctx\.(\w+) = l_\1;', part):
                # A smart call's stores: only those of the registers that differ.
                synced = smart_regs(part)
                drop = synced - d
                for r in drop:
                    part = part.replace(f'ctx.{r} = l_{r}; ', '', 1)
                changed += 1
                kept += len(synced) - len(drop)
                before += len(synced)
            parts[p] = part
        out[i] = '} else {'.join(parts)
    return '\n'.join(out), changed, kept, before


def main():
    for a in sys.argv[1:]:
        addr = a.upper().replace('SUB_', '')
        text = open(os.path.join(HOT, f'n_{addr}.h'), encoding='utf-8').read()
        try:
            _, changed, kept, before = limit_writebacks(addr, text)
            print(f'n_{addr}.h: {changed} write-backs, {kept} register stores of {before}')
        except Unsupported as e:
            print(f'n_{addr}.h: left with full write-backs ({e})')


if __name__ == '__main__':
    main()
