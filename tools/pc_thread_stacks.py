#!/usr/bin/env python3
"""blur-nx (2026-11-10): every thread's stack of a running renut.exe on the PC, for a stall (no console debugger on this
PC). Each thread is suspended in turn, its context read, its stack walked with dbghelp's StackWalk64 and named from the
PDB next to the executable (SymFromAddr with line numbers); then it is resumed. Read-only for the process otherwise.

  python tools/pc_thread_stacks.py [--pid N | --name renut.exe] [--frames 24] [--out FILE]
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import sys

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
dbg = ctypes.WinDLL('dbghelp', use_last_error=True)

PROCESS_ALL_ACCESS = 0x1F0FFF
THREAD_ALL_ACCESS = 0x1F03FF
TH32CS_SNAPTHREAD = 0x4
TH32CS_SNAPPROCESS = 0x2
CONTEXT_FULL = 0x10000B
MACHINE_AMD64 = 0x8664


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ThreadID', wt.DWORD),
                ('th32OwnerProcessID', wt.DWORD), ('tpBasePri', wt.LONG), ('tpDeltaPri', wt.LONG),
                ('dwFlags', wt.DWORD)]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD),
                ('th32ParentProcessID', wt.DWORD), ('pcPriClassBase', wt.LONG), ('dwFlags', wt.DWORD),
                ('szExeFile', ctypes.c_wchar * 260)]


class ADDRESS64(ctypes.Structure):
    _fields_ = [('Offset', ctypes.c_uint64), ('Segment', wt.WORD), ('Mode', wt.DWORD)]


class KDHELP64(ctypes.Structure):
    _fields_ = [('Thread', ctypes.c_uint64), ('ThCallbackStack', wt.DWORD), ('ThCallbackBStore', wt.DWORD),
                ('NextCallback', wt.DWORD), ('FramePointer', wt.DWORD), ('KiCallUserMode', ctypes.c_uint64),
                ('KeUserCallbackDispatcher', ctypes.c_uint64), ('SystemRangeStart', ctypes.c_uint64),
                ('KiUserExceptionDispatcher', ctypes.c_uint64), ('StackBase', ctypes.c_uint64),
                ('StackLimit', ctypes.c_uint64), ('BuildVersion', wt.DWORD), ('RetpolineStubFunctionTableSize', wt.DWORD),
                ('RetpolineStubFunctionTable', ctypes.c_uint64), ('RetpolineStubOffset', wt.DWORD),
                ('RetpolineStubSize', wt.DWORD), ('Reserved0', ctypes.c_uint64 * 2)]


class STACKFRAME64(ctypes.Structure):
    _fields_ = [('AddrPC', ADDRESS64), ('AddrReturn', ADDRESS64), ('AddrFrame', ADDRESS64), ('AddrStack', ADDRESS64),
                ('AddrBStore', ADDRESS64), ('FuncTableEntry', ctypes.c_void_p), ('Params', ctypes.c_uint64 * 4),
                ('Far', wt.BOOL), ('Virtual', wt.BOOL), ('Reserved', ctypes.c_uint64 * 3), ('KdHelp', KDHELP64)]


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 512)]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.DWORD), ('Key', ctypes.c_void_p), ('LineNumber', wt.DWORD),
                ('FileName', ctypes.c_char_p), ('Address', ctypes.c_uint64)]


dbg.SymFunctionTableAccess64.restype = ctypes.c_void_p
dbg.SymFunctionTableAccess64.argtypes = [wt.HANDLE, ctypes.c_uint64]
dbg.SymGetModuleBase64.restype = ctypes.c_uint64
dbg.SymGetModuleBase64.argtypes = [wt.HANDLE, ctypes.c_uint64]
FTA = ctypes.WINFUNCTYPE(ctypes.c_void_p, wt.HANDLE, ctypes.c_uint64)
GMB = ctypes.WINFUNCTYPE(ctypes.c_uint64, wt.HANDLE, ctypes.c_uint64)
dbg.StackWalk64.argtypes = [wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.POINTER(STACKFRAME64), ctypes.c_void_p,
                            ctypes.c_void_p, FTA, GMB, ctypes.c_void_p]
dbg.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFO)]
dbg.SymGetLineFromAddr64.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(wt.DWORD),
                                     ctypes.POINTER(IMAGEHLP_LINE64)]
dbg.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]
k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    pids = []
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        if e.szExeFile.lower() == name.lower():
            pids.append(e.th32ProcessID)
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return pids


def threads(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    e = THREADENTRY32()
    e.dwSize = ctypes.sizeof(e)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(e))
    while ok:
        if e.th32OwnerProcessID == pid:
            out.append(e.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out


def name_of(hproc, addr):
    sym = SYMBOL_INFO()
    sym.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - 512 + 1
    sym.MaxNameLen = 500
    disp = ctypes.c_uint64(0)
    s = '0x%X' % addr
    if dbg.SymFromAddr(hproc, addr, ctypes.byref(disp), ctypes.byref(sym)):
        s = '%s+0x%X' % (sym.Name.decode(errors='replace'), disp.value)
    line = IMAGEHLP_LINE64()
    line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
    d2 = wt.DWORD(0)
    if dbg.SymGetLineFromAddr64(hproc, addr, ctypes.byref(d2), ctypes.byref(line)) and line.FileName:
        s += ' (%s:%d)' % (os.path.basename(line.FileName.decode(errors='replace')), line.LineNumber)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int)
    ap.add_argument('--name', default='renut.exe')
    ap.add_argument('--frames', type=int, default=24)
    ap.add_argument('--out')
    a = ap.parse_args()
    pid = a.pid or (find_pid(a.name) or [None])[0]
    if not pid:
        sys.exit('no process ' + a.name)
    hproc = k32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    dbg.SymSetOptions(0x2 | 0x10 | 0x200)  # UNDNAME, LOAD_LINES, FAIL_CRITICAL_ERRORS
    dbg.SymInitializeW(hproc, None, True)
    fta = FTA(dbg.SymFunctionTableAccess64)
    gmb = GMB(dbg.SymGetModuleBase64)
    lines = []
    raw = ctypes.create_string_buffer(1232 + 16)
    base = (ctypes.addressof(raw) + 15) & ~15
    for tid in threads(pid):
        ht = k32.OpenThread(THREAD_ALL_ACCESS, False, tid)
        if not ht:
            continue
        k32.SuspendThread(ht)
        try:
            ctypes.memset(base, 0, 1232)
            ctypes.c_uint32.from_address(base + 0x30).value = CONTEXT_FULL
            if not k32.GetThreadContext(ht, ctypes.c_void_p(base)):
                continue
            rip = ctypes.c_uint64.from_address(base + 0xF8).value
            rsp = ctypes.c_uint64.from_address(base + 0x98).value
            rbp = ctypes.c_uint64.from_address(base + 0xA0).value
            sf = STACKFRAME64()
            sf.AddrPC.Offset, sf.AddrPC.Mode = rip, 3
            sf.AddrStack.Offset, sf.AddrStack.Mode = rsp, 3
            sf.AddrFrame.Offset, sf.AddrFrame.Mode = rbp, 3
            frames = []
            for _ in range(a.frames):
                if not dbg.StackWalk64(MACHINE_AMD64, hproc, ht, ctypes.byref(sf), ctypes.c_void_p(base), None, fta,
                                       gmb, None):
                    break
                if not sf.AddrPC.Offset:
                    break
                frames.append(name_of(hproc, sf.AddrPC.Offset))
            lines.append('== thread %d' % tid)
            lines += ['   #%02d %s' % (i, f) for i, f in enumerate(frames)]
        finally:
            k32.ResumeThread(ht)
            k32.CloseHandle(ht)
    dbg.SymCleanup(hproc)
    text = '\n'.join(lines) + '\n'
    if a.out:
        open(a.out, 'w').write(text)
    sys.stdout.write(text)


if __name__ == '__main__':
    main()
