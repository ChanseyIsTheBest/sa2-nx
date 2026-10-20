#!/usr/bin/env python3
"""banjo-nx: writes app/thirdparty/plume/plume_vma_names.h, plume's Vulkan Memory Allocator (3.2.1) under names of its
own, so it never shares a symbol with the rexglue SDK's (3.3.0, sdk/src/ui/vulkan/vulkan_mem_alloc.cpp).

On the PC the SDK's copy is inside rexruntime.dll, so only the exported C functions could collide (lld-link: "vmaGetHeap
Budgets was replaced"); package 26 renamed those. On the Switch both copies are static code in one ELF, and VMA's C++
internals (VmaAllocator_T, VmaDeviceMemoryBlock, VmaBlockMetadata_TLSF, ... : 290 weak member functions, vtables and
template instances) had the same names in both. The linker keeps one definition of each, so plume's 3.2.1 objects ran
through the SDK's 3.3.0 code: package 27 crashed at boot with Renut's renderer, in VmaDeviceMemoryBlock::Map on a
mutex at a wrong address (its first upload buffer). Every identifier of the header that starts with Vma, the _T
structs of its handles, and the C functions are now renamed for plume's translation units (plume_vulkan.h includes
this header before vk_mem_alloc.h).

Check after a Switch build: nothing VMA is defined both in libplume.a and in the SDK's libraries
(tools/plume_vma_names.py --check app/out/switch).

usage: plume_vma_names.py [--plume app/thirdparty/plume] [--check <switch build dir>]
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NM = r'C:\devkitPro\devkitA64\bin\aarch64-none-elf-nm.exe'
CXXFILT = r'C:\devkitPro\devkitA64\bin\aarch64-none-elf-c++filt.exe'

HEADER = '''//
// banjo-nx: plume's Vulkan Memory Allocator under names of its own (tools/plume_vma_names.py writes this file).
// The rexglue SDK links its own VMA (another version, another configuration). On Windows it is inside rexruntime.dll
// and only its exported C functions collided; on the Switch both are static code in one ELF, and the C++ classes
// (VmaAllocator_T, VmaDeviceMemoryBlock, ...) collided too: the linker kept one definition of each, and plume's
// allocations ran through the SDK's code (package 27's boot crash). Included by plume_vulkan.h before vk_mem_alloc.h,
// so plume's implementation and every caller through plume_vulkan.h use these names.
//

#pragma once
'''


def names_of(header_text):
    functions = set(re.findall(r'VMA_CALL_POST\s+(vma[A-Za-z0-9_]+)\s*\(', header_text))
    identifiers = set(re.findall(r'\b(Vma[A-Za-z0-9_]*)\b', header_text))
    handles = set(re.findall(r'VK_DEFINE_(?:NON_DISPATCHABLE_)?HANDLE\s*\(\s*(Vma[A-Za-z0-9_]*)\s*\)', header_text))
    identifiers |= {h + '_T' for h in handles}
    return sorted(functions), sorted(identifiers)


def write(plume):
    h = open(os.path.join(plume, 'contrib', 'VulkanMemoryAllocator', 'include', 'vk_mem_alloc.h'), encoding='utf-8').read()
    functions, identifiers = names_of(h)
    assert len(functions) > 60 and len(identifiers) > 100, (len(functions), len(identifiers))
    lines = [HEADER, '// The C functions.']
    lines += [f'#define {n} plume_{n}' for n in functions]
    lines += ['', '// Every type, class, template and helper whose name starts with Vma, and the _T structs of the handles.']
    lines += [f'#define {n} plume_{n}' for n in identifiers]
    out = os.path.join(plume, 'plume_vma_names.h')
    text = '\n'.join(lines) + '\n'
    if not os.path.isfile(out) or open(out, encoding='utf-8').read() != text:
        open(out, 'w', encoding='utf-8', newline='\n').write(text)
    print(f'plume_vma_names: {len(functions)} functions and {len(identifiers)} identifiers -> {out}')


def defined(lib):
    out = subprocess.run([NM, '--defined-only', lib], capture_output=True, text=True, errors='replace').stdout
    return {p[2] for p in (l.split() for l in out.splitlines()) if len(p) == 3 and p[1] in 'TWVtwvRrDdBb'}


def check(build):
    plume = defined(os.path.join(build, 'plume', 'libplume.a'))
    sdk = set()
    sdk_dir = os.path.join(ROOT, 'sdk', 'out', 'switch-arm64')
    for name in os.listdir(sdk_dir):
        if name.endswith('.a'):
            sdk |= defined(os.path.join(sdk_dir, name))
    shared = sorted(s for s in plume & sdk if 'Vma' in s or 'vma' in s)
    if shared:
        demangled = subprocess.run([CXXFILT], input='\n'.join(shared), capture_output=True, text=True).stdout.split('\n')
        for d in demangled[:20]:
            print('  shared:', d[:160])
    print(f'plume_vma_names --check: {len(shared)} VMA symbols defined both in libplume.a and in the SDK')
    return 1 if shared else 0


def main():
    args = sys.argv[1:]
    if '--check' in args:
        sys.exit(check(args[args.index('--check') + 1]))
    plume = args[args.index('--plume') + 1] if '--plume' in args else os.path.join(ROOT, 'app', 'thirdparty', 'plume')
    write(plume)


if __name__ == '__main__':
    main()
