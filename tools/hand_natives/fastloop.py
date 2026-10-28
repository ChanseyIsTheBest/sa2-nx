"""Helpers of the hand-native generators (make_*.py): a localized copy's loop wrapped with a fast loop that applies
under conditions it tests, the copy's loop as the fallback, and the verify-mode check of hot/fast_loop_check.h."""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/') + '/'
INCLUDES = ('\n#include <algorithm>\n#include <atomic>\n#include <cmath>\n#include <vector>\n\n#include <rex/logging.h>\n\n'
            '#include "../sa2_enhancements.h"\n#include "../sa2_hot_common.h"\n#include "fast_loop_check.h"\n')


def reg_get(r):
    return 'uint64_t(ea)' if r == 'ea' else f'{r}.u64'


def reg_set(r, v):
    return f'ea = uint32_t({v});' if r == 'ea' else f'{r}.u64 = {v};'


def wrap_loop(src, label, end_line, expect, setup, cond, fast, ranges, regs, compared, what, ptr):
    """Wraps the copy's loop `label:` .. `end_line` (which must read `expect` exactly) in `src`.
    setup: C++ statements before the test; cond: the test; fast: the fast loop, leaving what the copy's loop leaves;
    ranges: (address, length) C++ expressions of the bytes the fast loop writes; regs: the registers either loop writes
    (the first `compared` are compared, the rest are restored only); what: the loop's name in the logs; ptr: the name of
    its check pointer (declared at the top of the function)."""
    start = f'{label}:\n'
    assert src.count(start) == 1, label
    assert src.count(end_line) == 1, end_line
    i = src.index(start)
    j = src.index(end_line) + len(end_line)
    assert src[i:j] == expect, src[i:j]
    ind = '\t\t\t\t'
    save_in = ''.join(f'{ind}{ptr}->in[{k}] = {reg_get(r)};\n' for k, r in enumerate(regs))
    save_out = ''.join(f'\t\t\t{ptr}->out[{k}] = {reg_get(r)};\n' for k, r in enumerate(regs))
    back = ''.join(f'\t\t\t{reg_set(r, f"{ptr}->in[{k}]")}\n' for k, r in enumerate(regs))
    adds = ''.join(f'{ind}{ptr}->Add({a}, {n});\n' for a, n in ranges)
    want = ''.join(f'\t\twant_[{k}] = {reg_get(r)};\n' for k, r in enumerate(regs[:compared]))
    assert len(regs) <= 24 and len(ranges) <= 4
    head = (f'\t{{\n\t\t// sa2-nx (2026-10-28): {what} where it applies (the header\'s conditions), else the copy\'s loop '
            f'below.\n{setup}\t\tif ({cond}) {{\n'
            f'\t\t\tif (::sa2::enh::VerifyMode()) [[unlikely]] {{\n{ind}{ptr} = new ::sa2::hot::fast_loop::Check{{}};\n'
            f'{adds}{ind}::sa2::hot::fast_loop::Begin(*{ptr}, base);\n{save_in}\t\t\t}}\n'
            f'{fast}'
            f'\t\t\tif (!{ptr}) [[likely]] goto {label}_done;\n'
            f'\t\t\t// Verify mode: the fast loop\'s result aside, the inputs back, then the copy\'s loop.\n'
            f'{save_out}\t\t\t::sa2::hot::fast_loop::Swap(*{ptr}, base);\n{back}\t\t}}\n\t}}\n')
    tail = (f'\tif ({ptr}) [[unlikely]] {{\n\t\tuint64_t want_[{max(compared, 1)}];\n{want}'
            f'\t\tstatic std::atomic<uint32_t> checks_{{0}};\n'
            f'\t\t::sa2::hot::fast_loop::Finish({ptr}, base, want_, {compared}, "{what}", checks_);\n\t}}\n'
            f'{label}_done:\n')
    decl = '\tuint32_t ea{};\n'
    assert src.count(decl) == 1
    out = src[:i] + head + src[i:j] + tail + src[j:]
    return out.replace(decl, decl + f'\t::sa2::hot::fast_loop::Check* {ptr} = nullptr;  // verify mode only\n')


def finish(src, addr, ns, header, out_name):
    """Renames the copy's namespace, puts the includes and the header on, writes app/src/cpu/hot/<out_name>."""
    old = f'namespace sa2::hot::n_{addr}'
    assert src.count(old) == 2
    src = src.replace(old + ' {', f'namespace sa2::hot::{ns} {{').replace(f'}}  // {old}', f'}}  // namespace sa2::hot::{ns}')
    start = src.index('#pragma once')
    body = src[start:]
    assert f'n_{addr}' not in body, f'n_{addr} left in the body'
    pragma = '#pragma once\n'
    body = pragma + INCLUDES + body[len(pragma):]
    open(ROOT + 'app/src/cpu/hot/' + out_name, 'w', encoding='utf-8', newline='\n').write(header + body)
    print(out_name, 'written')


def copy_text(addr):
    return open(ROOT + f'app/generated/hot/n_{addr}.h', encoding='utf-8').read()
