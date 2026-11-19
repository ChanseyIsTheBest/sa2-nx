#!/usr/bin/env python3
"""banjo-nx: every enhancement key with its group, code default, shipped value, defining file and help text, read from
the sources (the data behind docs/enhancements-reference.md, and its audit).

  python tools/enhancement_keys.py [--json FILE] [--audit DOC.md ...] [--audit-codegen DOC.md ...]

The keys are the rows of LogEnhancements() in app/src/cpu/banjo_enhancements.cpp (the "[enhancements]" log line) and
the banjo_cpu_native_* keys of the localized copies (app/src/cpu/hot/localized.txt). --audit checks a page against
them: every key the page names in backquotes must exist, every table row's default must match the shipped
dist/switch/banjo-nx/renut.toml (or the code default when the toml does not set it), and every key in the table must
appear on the page; for docs/enhancements-reference.md also the premade configs (dist/switch/configs) and the A/B
target table (banjo_ab_target's own description). --audit-codegen checks docs/codegen-enhancements.md against
app/renut_config.toml, app/CMakeLists.txt, tools/ and the step order of tools/post_codegen.py.
"""
import argparse
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE = os.path.join(ROOT, 'app', 'src', 'cpu', 'banjo_enhancements.cpp')
TOML = os.path.join(ROOT, 'dist', 'switch', 'banjo-nx', 'renut.toml')
DEFINE = re.compile(r'REXCVAR_DEFINE_(BOOL|INT32|INT64|STRING|DOUBLE|FLOAT|UINT32)\(\s*(\w+)\s*,\s*([^,]+?)\s*,\s*"([^"]*)"\s*,'
                    r'\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)', re.S)


def definitions():
    out = {}
    roots = [os.path.join(ROOT, 'app', 'src'), os.path.join(ROOT, 'sdk', 'src')]
    for root in roots:
        for d, _, files in os.walk(root):
            for f in files:
                if not f.endswith(('.cpp', '.h', '.inc')):
                    continue
                path = os.path.join(d, f)
                text = open(path, encoding='utf-8', errors='ignore').read()
                for m in DEFINE.finditer(text):
                    kind, name, default, category, help_text = m.groups()
                    help_text = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', help_text))
                    out[name] = dict(type=kind.lower(), default=default.strip(), category=category,
                                     help=help_text.replace('\\"', '"'),
                                     file=os.path.relpath(path, ROOT).replace(os.sep, '/'))
    return out


def table_rows():
    text = open(TABLE, encoding='utf-8').read()
    start = text.index('void LogEnhancements()')
    # A row's name can carry the value it counts as on ("masseffect_native_upload_memory=1"), and its third field can
    # start on the next line.
    rows = re.findall(r'\{"(\w+)(?:=[^"]*)?", EnhancementGroup::k(Cpu|NonCpu),\s', text[start:])
    return [(name, 'CPU' if group == 'Cpu' else 'non-CPU') for name, group in rows]


HOT_LIST = os.path.join(ROOT, 'app', 'generated', 'hot', 'banjo_hot_list.inc')
HOT_HOOK = re.compile(r'^BANJO_HOT_HOOK\((\w+), (\w+), (\w+), (true|false), \d+, "((?:[^"\\]|\\.)*)"\)', re.M)


def localized():
    """key -> (default, help) of the localized copies' keys (tools/localize_native.py writes the list)."""
    if not os.path.exists(HOT_LIST):
        return {}
    text = open(HOT_LIST, encoding='utf-8').read()
    return {m.group(3): (m.group(4), m.group(5)) for m in HOT_HOOK.finditer(text)}


def localized_rows():
    return [(key, 'CPU') for key in localized()]


def toml_values():
    out = {}
    section = ''  # 2026-10-21: [input_p1] a = ... is the cvar input_p1_a
    for line in open(TOML, encoding='utf-8'):
        s = re.match(r'^\s*\[(\w+)\]', line)
        if s:
            section = s.group(1) + '_'
            continue
        m = re.match(r'^\s*(\w+)\s*=\s*([^#\n]+?)\s*(#.*)?$', line)
        if m:
            out[section + m.group(1)] = m.group(2).strip().strip('"')
    return out


def keys():
    defs = definitions()
    for key, (default, help_text) in localized().items():
        defs[key] = dict(type='bool', default=default, category='Banjo/Hot', help=help_text,
                         file='app/generated/hot/banjo_hot_list.inc')
    toml = toml_values()
    out = []
    for name, group in table_rows() + localized_rows():
        d = defs.get(name, {})
        out.append(dict(key=name, group=group, code_default=d.get('default'), shipped=toml.get(name, d.get('default')),
                        set_in_toml=name in toml, file=d.get('file'), help=d.get('help')))
    return out, defs, toml


def audit(doc_paths, rows, defs, toml):
    problems = 0
    by_key = {r['key']: r for r in rows}
    for doc in doc_paths:
        text = open(doc, encoding='utf-8').read()
        named = set(re.findall(r'`((?:banjo|masseffect|switch|kernel|thread|fp|BANJO|REX)_\w+)`', text))
        generator = generator_options()
        for name in sorted(named):
            if name in NOT_KEYS:
                continue
            if name.startswith(('BANJO_', 'REX_')):
                continue  # build options and macros: the codegen page's own audit checks them
            if name in generator:
                continue  # a code generator option (app/renut_config.toml), not a run-time key
            if name not in defs and name not in by_key:
                print('%s: names `%s`, which no source defines' % (doc, name))
                problems += 1
        for m in re.finditer(r'^\| `(\w+)` \| (CPU|non-CPU) \| `([^`]*)` \|', text, re.M):
            name, group, value = m.groups()
            row = by_key.get(name)
            if not row and name in defs:
                # A key outside the "[enhancements]" line (a part of another, a parameter): its value still counts.
                row = dict(group=group, shipped=toml.get(name, defs[name]['default']))
            if not row:
                print('%s: table row `%s` is not a key' % (doc, name))
                problems += 1
                continue
            if row['group'] != group:
                print('%s: `%s` is in the %s group, the page says %s' % (doc, name, row['group'], group))
                problems += 1
            if str(row['shipped']).lower() != value.lower():
                print('%s: `%s` ships as %s, the page says %s' % (doc, name, row['shipped'], value))
                problems += 1
        if 'enhancements-reference' in doc:
            for row in rows:
                if '`%s`' % row['key'] not in text:
                    print('%s: key `%s` is missing from the page' % (doc, row['key']))
                    problems += 1
            problems += audit_configs(doc, text)
            problems += audit_ab_targets(doc, text, defs)
    print('audit: %d problems' % problems)
    return problems


# Backquoted names with a key's prefix that are not keys.
NOT_KEYS = {'thread_local', 'banjo_clear_own_surface',  # a C++ keyword; a field of the draw's EDRAM plan
            # keys removed with the ring prover (2026-10-16), named on the page where it says so
            'banjo_ring_prover', 'banjo_ring_prover_loads', 'banjo_ring_prover_fine', 'banjo_ring_prover_core'}
SDK_CODEGEN_CONFIG = os.path.join(ROOT, 'sdk', 'src', 'codegen', 'config.cpp')


def generator_options():
    """Every option the SDK's code generator reads from renut_config.toml (set or not)."""
    names = set(re.findall(r'^\s*(\w+)\s*=', open(CODEGEN_CONFIG, encoding='utf-8').read(), re.M))
    if os.path.exists(SDK_CODEGEN_CONFIG):
        names |= set(re.findall(r'toml\["(\w+)"\]', open(SDK_CODEGEN_CONFIG, encoding='utf-8').read()))
    return names


CONFIGS = os.path.join(ROOT, 'dist', 'switch', 'configs')


def audit_configs(doc, text):
    """Every premade config the page names exists, and every one that exists is in the page's table."""
    problems = 0
    named = set(re.findall(r'`(banjo_\w+\.toml)`', text))
    present = {f for f in os.listdir(CONFIGS) if f.endswith('.toml')} if os.path.isdir(CONFIGS) else set()
    for name in sorted(named - present):
        print('%s: names the config `%s`, which dist/switch/configs does not have' % (doc, name))
        problems += 1
    listed = set(re.findall(r'^\| `(banjo_\w+\.toml)` \|', text, re.M))
    for name in sorted(present - listed):
        print('%s: the config `%s` is missing from the configs table' % (doc, name))
        problems += 1
    return problems


def audit_ab_targets(doc, text, defs):
    """The page's banjo_ab_target table against the key's own description ("N = <key or words>")."""
    problems = 0
    described = dict(re.findall(r'(\d+) = (banjo_\w+|masseffect_\w+)', defs['banjo_ab_target']['help']))
    on_page = {}
    for m in re.finditer(r'^\| (\d+) \| (.*) \|$', text, re.M):
        keys_in_row = re.findall(r'`((?:banjo|masseffect)_\w+)`', m.group(2))
        on_page[m.group(1)] = keys_in_row
    for number, key in sorted(described.items(), key=lambda kv: int(kv[0])):
        if number not in on_page:
            print('%s: A/B target %s (%s) is missing from the table' % (doc, number, key))
            problems += 1
        elif key not in on_page[number]:
            print('%s: A/B target %s is %s, the table says %s' % (doc, number, key, on_page[number]))
            problems += 1
    return problems


CMAKE = os.path.join(ROOT, 'app', 'CMakeLists.txt')
CODEGEN_CONFIG = os.path.join(ROOT, 'app', 'renut_config.toml')
POST_CODEGEN = os.path.join(ROOT, 'tools', 'post_codegen.py')


def audit_codegen(doc):
    """docs/codegen-enhancements.md against app/renut_config.toml, app/CMakeLists.txt, tools/ and post_codegen.py."""
    problems = 0
    text = open(doc, encoding='utf-8').read()
    config = {}
    for line in open(CODEGEN_CONFIG, encoding='utf-8'):
        m = re.match(r'^\s*(\w+)\s*=\s*(.+?)\s*$', line)
        if m:
            config[m.group(1)] = m.group(2)
    # The generator table: | `option` | `value` | ...
    for m in re.finditer(r'^\| `(\w+)` \| `([^`]*)` \|', text, re.M):
        name, value = m.groups()
        if name.startswith('BANJO_'):
            continue
        if name not in config:
            print('%s: generator option `%s` is not set in app/renut_config.toml' % (doc, name))
            problems += 1
        elif config[name].replace(' ', '') != value.replace(' ', ''):
            print('%s: `%s` is %s in app/renut_config.toml, the page says %s' % (doc, name, config[name], value))
            problems += 1
    for name in config:
        if re.match(r'^(cr|ctr|xer|reserved|skip|dform|fct|fp_|div_|non_volatile|elide)', name) and '`%s`' % name not in text:
            print('%s: generator option `%s` (app/renut_config.toml) is missing from the page' % (doc, name))
            problems += 1
    # CMake options: every BANJO_ name in backquotes exists; the build-option table gives the default.
    cmake = open(CMAKE, encoding='utf-8').read()
    options = dict(re.findall(r'option\((BANJO_\w+) "[^"]*" (ON|OFF)\)', cmake))
    for name in sorted(set(re.findall(r'`(BANJO_\w+)`', text))):
        if name not in options and not name.startswith('BANJO_DEAD_DEFS'):  # BANJO_DEAD_DEFS(_HOLD/_NO_ABI): env
            print('%s: names `%s`, which app/CMakeLists.txt does not define' % (doc, name))
            problems += 1
    for m in re.finditer(r'^\| `(BANJO_\w+)` \| `(ON|OFF)` \|', text, re.M):
        name, value = m.groups()
        if name in options and options[name] != value:
            print('%s: `%s` defaults to %s, the page says %s' % (doc, name, options[name], value))
            problems += 1
    for name in options:
        if name not in text:
            print('%s: CMake option `%s` is missing from the page' % (doc, name))
            problems += 1
    for env in ('BANJO_DEAD_DEFS_OFF', 'BANJO_DEAD_DEFS_HOLD', 'BANJO_DEAD_DEFS_NO_ABI'):
        if '`%s' % env in text and env not in open(POST_CODEGEN, encoding='utf-8').read():
            print('%s: names the environment variable `%s`, which tools/post_codegen.py does not read' % (doc, env))
            problems += 1
    # Tools named exist.
    for name in sorted(set(re.findall(r'`tools/([\w./-]+?\.(?:py|bat|sh))`', text))):
        if not os.path.exists(os.path.join(ROOT, 'tools', name)):
            print('%s: names `tools/%s`, which does not exist' % (doc, name))
            problems += 1
    # The step table's order is post_codegen.py's.
    # (a run with '--restore' takes a later step's rewrite back out first: not a step of its own)
    runs = [m.group(1) for m in re.finditer(r"run\(tool\('(\w+)\.py'\)([^\n]*)", open(POST_CODEGEN, encoding='utf-8').read())
            if "'--restore'" not in m.group(2)]
    order = []
    for name in runs:
        if name not in order:
            order.append(name)
    table = [m.group(1) for m in re.finditer(r'^\| \d+ \| `tools/(\w+)\.py` \|', text, re.M)]
    if table != order:
        print('%s: the step table lists %s; tools/post_codegen.py runs %s' % (doc, table, order))
        problems += 1
    print('audit (codegen): %d problems' % problems)
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json')
    ap.add_argument('--audit', nargs='*')
    ap.add_argument('--audit-codegen', nargs='*')
    a = ap.parse_args()
    rows, defs, toml = keys()
    if a.json:
        json.dump(rows, open(a.json, 'w', encoding='utf-8'), indent=1)
        print('%d keys (%d CPU, %d non-CPU) -> %s' % (len(rows), sum(r['group'] == 'CPU' for r in rows),
                                                      sum(r['group'] != 'CPU' for r in rows), a.json))
    problems = 0
    if a.audit:
        problems += audit(a.audit, rows, defs, toml)
    for doc in a.audit_codegen or []:
        problems += audit_codegen(doc)
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
