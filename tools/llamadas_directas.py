# nfsmw - direct calls to game functions that have no hook.
#
# With GCC every recompiled function sub_X is a weak alias of __imp__sub_X (DEFINE_REX_FUNC, nfsmw_pch.h),
# so that one of our REX_HOOK_RAW(sub_X) replaces it at link time. The codegen always calls sub_X, and the
# compiler cannot inline a weak function into another (a hook could replace it): not within the same file
# and not with LTO. This step changes `sub_X(ctx, base);` into `__imp__sub_X(ctx, base);` when sub_X has no
# hook, which is exactly the same function, but can now be inlined.
#
# Any 82xxxxxx address that appears in the app or SDK sources or in the toml files counts as hooked (the
# D3D trace builds its hooks with sub_##addr). That is slightly more than needed and safe; it is also
# checked against the sub_ symbols the compiled objects really define (--comprobar-objetos).
#
# It does not touch nfsmw_init.cpp or nfsmw_register.cpp: the dispatch table for indirect calls still
# points to sub_X and sees the hooks. It has to be run again after every codegen.
#
# Usage: py tools/llamadas_directas.py [--deshacer] [--gen <generated folder>] [--enganchadas <list>]
#   --gen and --enganchadas: for the tree of another edition (tools/editions/crear_arbol.py), with the
#   list of hooked addresses of the Spanish edition already translated.
import os
import re
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(RAIZ, 'app', 'generated', 'default')
FUENTES = [os.path.join(RAIZ, 'app', 'src'), os.path.join(RAIZ, 'sdk', 'src'),
           os.path.join(RAIZ, 'sdk', 'include'), os.path.join(RAIZ, 'app', 'overrides.toml'),
           os.path.join(RAIZ, 'app', 'huecos.toml'), os.path.join(RAIZ, 'app', 'nfsmw_manifest.toml'),
           os.path.join(RAIZ, 'app', 'CMakeLists.txt')]


def code_address_re(gen):
    """renut-ultimate (2026-11-16, port 144; tw2de_nx's): the guest code addresses the hook sources name: 82xxxxxx, and
    83xxxxxx too when the game's code runs there (its function table, <project>_init.cpp, has entries at 0x83000000 and
    above: TW2's sub_833C2518 ...); a hook there was not seen, so its calls went past it. A tree without such entries
    reads the sources as before (an 83xxxxxx number there is not a function)."""
    import glob as _glob
    for p in _glob.glob(os.path.join(gen, '*_init.cpp')):
        if re.search(r'\{ 0x83[0-9A-Fa-f]{6},', open(p, encoding='utf-8', errors='ignore').read()):
            return r'(?<![0-9A-Fa-f])(8[23][0-9A-Fa-f]{6})(?![0-9A-Fa-f])'
    return r'(?<![0-9A-Fa-f])(82[0-9A-Fa-f]{6})(?![0-9A-Fa-f])'


def enganchadas():
    nombres = set()
    addr_re = code_address_re(GEN)  # port 144
    for raiz in FUENTES:
        if os.path.isfile(raiz):
            rutas = [raiz]
        else:
            rutas = [os.path.join(d, f) for d, _, fs in os.walk(raiz) for f in fs
                     if not f.endswith(('.a', '.obj', '.o', '.png', '.jpg', '.bin'))]
        for ruta in rutas:
            try:
                texto = open(ruta, encoding='utf-8', errors='ignore').read()
            except OSError:
                continue
            for m in re.findall(addr_re, texto):
                nombres.add('sub_' + m.upper())
    return nombres


def main():
    deshacer = '--deshacer' in sys.argv
    gen = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else GEN
    if '--enganchadas' in sys.argv:
        con_gancho = set(open(sys.argv[sys.argv.index('--enganchadas') + 1], encoding='utf-8').read().split())
    else:
        con_gancho = enganchadas()
    llamada = re.compile(r'(?<![\w])sub_([0-9A-F]{8})\(ctx, base\);')
    directa = re.compile(r'__imp__sub_([0-9A-F]{8})\(ctx, base\);')
    cambiadas = 0
    ficheros = 0
    for f in sorted(os.listdir(gen)):
        if not (f.startswith('nfsmw_recomp.') and f.endswith('.cpp')):
            continue
        ruta = os.path.join(gen, f)
        texto = open(ruta, encoding='utf-8').read()
        if deshacer:
            nuevo, n = directa.subn(lambda m: 'sub_%s(ctx, base);' % m.group(1), texto)
        else:
            def cambiar(m):
                return m.group(0) if 'sub_' + m.group(1) in con_gancho else '__imp__sub_%s(ctx, base);' % m.group(1)
            nuevo = llamada.sub(cambiar, texto)
            n = len(llamada.findall(texto)) - len(llamada.findall(nuevo))
        if nuevo != texto:
            open(ruta, 'w', encoding='utf-8', newline='\n').write(nuevo)
            ficheros += 1
        cambiadas += n
    print('%s: %d llamadas en %d ficheros; %d direcciones con gancho respetadas' %
          ('deshecho' if deshacer else 'directas', cambiadas, ficheros, len(con_gancho)))


if __name__ == '__main__':
    main()
