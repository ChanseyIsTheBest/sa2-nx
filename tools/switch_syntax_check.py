import os
import re
import subprocess
import sys

# Syntax-only compile of app sources with the switch-dist flags (from ninja -t commands; no object, no LTO), and a lint
# for self-initialised locals ('const int ab = ab;', 2026-10-27: GCC reports those only from its optimiser, so
# -fsyntax-only misses them). Usage: syntax_check.py <object path under the build dir>...
NINJA = r'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
BUILD = r'D:/Banjo kazooie port/banjo-nx/app/out/switch-dist'
SRC = r'D:/Banjo kazooie port/banjo-nx/app/src/'
SELF_INIT = re.compile(r'^\s*(?:const\s+|static\s+|constexpr\s+)*[A-Za-z_][\w:<>,\s\*&]*?\s[\*&]?(\w+)\s*=\s*\1\s*;', re.M)


def tokens(cmd):
    # Spaces split words except inside double quotes (ninja's Windows command lines quote the paths mid-word).
    out, cur, quoted = [], '', False
    for ch in cmd:
        if ch == '"':
            quoted = not quoted
            continue
        if ch == ' ' and not quoted:
            if cur:
                out.append(cur)
            cur = ''
            continue
        cur += ch
    if cur:
        out.append(cur)
    return out


procs = []
for obj in sys.argv[1:]:
    cmd = subprocess.run([NINJA, '-t', 'commands', obj], cwd=BUILD, capture_output=True, text=True).stdout.strip().splitlines()[-1]
    out, skip = [], False
    for a in tokens(cmd):
        if skip:
            skip = False
            continue
        if a in ('-o', '-MF', '-MT', '-MQ'):
            skip = True
            continue
        if a.startswith('-flto') or a in ('-MD', '-MMD', '-c') or a.startswith('-fprofile'):
            continue
        out.append(a)
    out += ['-fsyntax-only']
    procs.append((obj, subprocess.Popen(out, cwd=BUILD, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)))

rc = 0
for obj in sys.argv[1:]:
    if '/src/' in obj:
        rel = obj.split('/src/', 1)[1][:-len('.obj')]
        path = SRC + rel
    else:  # plume/CMakeFiles/plume.dir/<file>.cpp.obj
        rel = obj.split('/')[-1][:-len('.obj')]
        path = SRC.replace('/src/', '/thirdparty/plume/') + rel
    if not os.path.exists(path):  # an SDK object (rexglue-sdk/... or the app's copy of an SDK file)
        sdk_rel = rel.split('sdk/src/')[-1]
        if 'CMakeFiles/' in sdk_rel and '.dir/' in sdk_rel:  # rexglue-sdk/src/<dir>/CMakeFiles/<lib>.dir/<file>
            sdk_rel = sdk_rel.split('CMakeFiles/')[0] + sdk_rel.split('.dir/', 1)[1]
        path = SRC.replace('/app/src/', '/sdk/src/') + sdk_rel
    if not os.path.exists(path):
        print('  (no lint: source not found for', obj + ')')
        continue
    text = open(path, encoding='utf-8').read()
    for m in SELF_INIT.finditer(text):
        print('  SELF-INIT', rel, text.count('\n', 0, m.start()) + 1, m.group(0).strip())
        rc = 1
for obj, p in procs:
    o = p.communicate()[0].decode('utf-8', 'replace')
    lines = [l for l in o.splitlines() if 'error' in l]
    print(obj.split('/')[-1], 'exit', p.returncode)
    for l in lines[:20]:
        print('  ', l[:300])
    rc |= p.returncode
sys.exit(rc)
