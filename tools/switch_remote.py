#!/usr/bin/env python3
"""The console over the network (renut-ultimate, 2026-11-13: rr6-nx's tools/switch_remote.py of 2026-11-04 as sasrt-nx
grew it, for any port of the base), through two sysmodules run under Atmosphere: sys-botbase (masagrator's fork; TCP
6000: controller presses, screenshots, the running title, memory reads) and sys-ftpd-light (TCP 5000, anonymous: the SD
card). Outbound connections only; nothing on this PC listens. docs/porting-a-360-game-prompt.md, "The console", has
the setup and the order of a console run.

The rule: on the SD card only this port's own folder may be touched. Every ftp path is checked against it and refused
otherwise, relative paths are taken from it, nothing on the card is ever deleted (there is no delete command), and raw
takes only commands that cannot reach the card or write memory. The Album is read only; /atmosphere is not read. Every
console run needs the console owner's word first.

    python tools/switch_remote.py [--host IP] [--root /switch/<port>] CMD ...

  shot [OUT.jpg]             the screen as the console shows it (pixelPeek)
  click A [B ...]            taps, one after the other (A B X Y L R ZL ZR PLUS MINUS HOME CAPTURE
                             DUP DDOWN DLEFT DRIGHT LSTICK RSTICK)
  hold BTN [ms]              press and release after ms (default 400)
  chord A B X Y [ms]         several buttons held together, released after ms (the shadow dump's A+B+X+Y)
  stick L|R X Y [ms]         a stick at X, Y in -1..1 (y up), released after ms (default 300)
  title | version | heap     getTitleID, getVersion, getHeapBase
  peek ADDR SIZE             SIZE bytes at the absolute address, hex
  ls [PATH]                  ftp listing (default: the port's folder)
  get REMOTE [LOCAL]         ftp download (a folder: every file in it, one level)
  put LOCAL REMOTE           ftp upload (overwrites a file of that name in the folder)
  mv REMOTE NEW              ftp rename inside the port's folder; refused when NEW exists (it would be replaced)
  raw "CMD ..."              a sys-botbase command as it is, the reply printed
  album-ls [PATH]            the Album, read only: /Nintendo/Album... or /emuMMC/<folder>/Nintendo/Album...
  album-get REMOTE [LOCAL]   one Album file downloaded (read only; nothing in the Album is written or deleted)
  crash-ls                   Atmosphere's crash reports, read only: /atmosphere/crash_reports
  crash-get NAME [LOCAL]     one crash report downloaded (read only; nothing there is written or deleted)
  stall-dump                 an empty logs/rex/stall_dump in the port's folder: the running game's stall watchdog
                             (banjo_stall_watch_*) dumps every thread and the guest waits, then deletes it

The host comes from --host, else the SWITCH_HOST environment variable, else out/switch_host.txt's first line. The
port's folder on the card comes from --root, else SWITCH_SD_ROOT, else out/switch_sd_root.txt's first line (for example
/switch/katamari-nx); with none set, nothing on the card is touched. In Git Bash set MSYS_NO_PATHCONV=1 for commands
that pass card paths such as /switch/..., or it rewrites them.
"""
import os
import posixpath
import socket
import sys
import time
import struct
from ftplib import FTP, error_perm, error_temp

BOT_PORT, FTP_PORT = 6000, 5000
SD_ROOT = None  # the port's folder on the card: set by root_of() from --root, SWITCH_SD_ROOT or out/switch_sd_root.txt
RAW_ALLOWED = ('click', 'press', 'release', 'setStick', 'pixelPeek', 'getTitleID', 'getVersion', 'getHeapBase', 'peekAbsolute',
               'configure',
               # sasrt-nx (2026-11-10): the virtual controller dropped while the console slept (presses ignored after
               # the wake); detaching makes the next press attach a new one. No memory, no files.
               'detachController')


def root_of(args):
    """The port's folder on the card (/switch/<something>), from --root, SWITCH_SD_ROOT or out/switch_sd_root.txt."""
    root = None
    if '--root' in args:
        i = args.index('--root')
        root = args[i + 1]
        del args[i:i + 2]
    elif os.environ.get('SWITCH_SD_ROOT'):
        root = os.environ['SWITCH_SD_ROOT']
    else:
        p = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'switch_sd_root.txt')
        if os.path.exists(p):
            root = open(p, encoding='utf-8').readline().strip()
    if root:
        root = posixpath.normpath('/' + root.replace('\\', '/').lstrip('/'))
        if not root.startswith('/switch/') or root.count('/') != 2:
            sys.exit('refused: the port\'s folder must be one folder under /switch/ (got %s)' % root)
    return root


def sd_path(path=''):
    """The path on the card, refused unless it is inside SD_ROOT."""
    if not SD_ROOT:
        sys.exit('no port folder on the card: pass --root /switch/<port>, set SWITCH_SD_ROOT, or write it into '
                 'out/switch_sd_root.txt')
    if ':' in path:  # Git Bash turns /x into C:/.../x before this sees it: never take that for a path in the folder
        sys.exit('refused: %s looks like a Windows path (use a path relative to %s)' % (path, SD_ROOT))
    full = posixpath.normpath(posixpath.join(SD_ROOT, path.replace('\\', '/')))
    if full != SD_ROOT and not full.startswith(SD_ROOT + '/'):
        sys.exit('refused: %s is outside %s' % (path, SD_ROOT))
    return full


# blur-nx (2026-11-13, the user: "You can open the folder the crash report in"): Atmosphere's crash reports, read only,
# for crash-ls and crash-get: one folder, listed, and its files downloaded by name. Nothing here writes, renames or
# deletes, and no other part of /atmosphere is reached.
CRASH_ROOT = '/atmosphere/crash_reports'


def crash_path(name=None):
    if name is None:
        return CRASH_ROOT
    if ':' in name or '/' in name or chr(92) in name or name in ('', '.', '..'):
        sys.exit('refused: %s is not a file name in %s' % (name, CRASH_ROOT))
    return CRASH_ROOT + '/' + name


# sasrt-nx (2026-11-09, the user: "grab screenshots I took from the emunand album, do not delete any files"): the Album
# read only, for album-ls and album-get, which only list and download. The paths: /Nintendo/Album... and
# /emuMMC/<folder>/Nintendo/Album..., and for finding that folder, listings of /emuMMC, /emuMMC/<folder> and
# /emuMMC/<folder>/Nintendo. Nothing here writes, renames or deletes.
def album_path(path, listing=False):
    if ':' in path:
        sys.exit('refused: %s looks like a Windows path' % path)
    full = posixpath.normpath('/' + path.replace('\\', '/').lstrip('/'))
    parts = [x for x in full.split('/') if x]
    ok = parts[:2] == ['Nintendo', 'Album'] or (len(parts) >= 4 and parts[0] == 'emuMMC' and parts[2:4] == ['Nintendo', 'Album'])
    if listing and not ok:
        ok = (len(parts) == 1 and parts[0] == 'emuMMC') or (len(parts) == 2 and parts[0] == 'emuMMC') or              (len(parts) == 3 and parts[0] == 'emuMMC' and parts[2] == 'Nintendo')
    if not ok:
        sys.exit('refused: %s is not the Album (read only)' % path)
    return full


def host_of(args):
    if '--host' in args:
        i = args.index('--host')
        host = args[i + 1]
        del args[i:i + 2]
        return host
    if os.environ.get('SWITCH_HOST'):
        return os.environ['SWITCH_HOST']
    here = os.path.dirname(os.path.abspath(__file__))
    p = os.path.join(here, '..', 'out', 'switch_host.txt')
    if os.path.exists(p):
        return open(p, encoding='utf-8').readline().strip()
    sys.exit('no host: pass --host IP, set SWITCH_HOST, or write the IP into out/switch_host.txt')


class Bot:
    """One sys-botbase connection. Commands are lines ending in \\r\\n; replies (where there is one) end in \\n."""

    def __init__(self, host, timeout=10.0):
        self.s = socket.create_connection((host, BOT_PORT), timeout=timeout)
        self.send('configure echoCommands 0')

    def send(self, cmd):
        self.s.sendall((cmd + '\r\n').encode('ascii'))

    def ask(self, cmd, timeout=15.0):
        self.send(cmd)
        self.s.settimeout(timeout)
        data = b''
        while not data.endswith(b'\n'):
            chunk = self.s.recv(1 << 16)
            if not chunk:
                break
            data += chunk
        return data.decode('ascii', 'replace').strip()

    def shot(self, out):
        hexjpg = self.ask('pixelPeek', timeout=30.0)
        raw = bytes.fromhex(hexjpg)
        open(out, 'wb').write(raw)
        return len(raw)

    def chord(self, buttons, ms):
        """Several buttons held together (the shadow dump's A+B+X+Y), released after ms."""
        for btn in buttons:
            self.send('press ' + btn.upper())
        time.sleep(ms / 1000.0)
        for btn in buttons:
            self.send('release ' + btn.upper())

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def ftp_open(host):
    f = FTP()
    f.connect(host, FTP_PORT, timeout=20)
    f.login()  # anonymous
    return f


# sys-ftpd-light (2026-11-08, sasrt-nx's first run): a few transfers in one session, then 451 on every PASV and the
# server resets connections for a second or two. So every file gets a connection of its own, with retries.
def ftp_retry(host, what, tries=5):
    last = None
    for attempt in range(tries):
        f = None
        try:
            f = ftp_open(host)
            return what(f)
        except Exception as e:  # 451, a reset, a timeout: wait and try again on a new connection
            last = e
            time.sleep(1.0 + attempt * 1.5)
        finally:
            if f is not None:
                try:
                    f.quit()
                except Exception:
                    try:
                        f.close()
                    except Exception:
                        pass
    raise last


def rst_close(sock):
    """Close with a reset (SO_LINGER 0): neither side keeps the socket in TIME_WAIT."""
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass


def ftp_get_many(host, items, log=print, pause=0.2):
    """items: (remote, local, size). sys-ftpd-light (2026-11-08) runs out of sockets after about two transfers when it
    closes each data connection itself (they linger in TIME_WAIT on the console: 451 on every PASV for about 40 s).
    So one control session for all files, each read to its known size, and the PC resets the data connection itself
    before the server closes it. A file already there at its size is kept. Returns the remote paths that failed."""
    failed = []
    f = None
    for remote, local, size in items:
        if os.path.exists(local) and os.path.getsize(local) == size:
            continue
        for attempt in range(8):
            try:
                if f is None:
                    f = ftp_open(host)
                    f.voidcmd('TYPE I')
                conn, _ = f.ntransfercmd('RETR ' + remote)
                got = 0
                conn.settimeout(30)
                with open(local, 'wb') as out:
                    while got < size:
                        chunk = conn.recv(min(1 << 16, size - got))
                        if not chunk:
                            break
                        out.write(chunk)
                        got += len(chunk)
                rst_close(conn)
                try:
                    f.getresp()  # 226, or 426 after the reset: the bytes are all here either way
                except Exception:
                    pass
                if got != size:
                    raise IOError('%s: %d of %d bytes' % (remote, got, size))
                log('%s %s %d' % (time.strftime('%H:%M:%S'), posixpath.basename(remote), got))
                break
            except Exception as e:
                if not isinstance(e, (error_temp, error_perm)) and f is not None:
                    rst_close(f.sock)  # the control connection is gone or confused: a new one
                    f = None
                log('%s retry %s (%s)' % (time.strftime('%H:%M:%S'), posixpath.basename(remote), str(e).strip()))
                time.sleep(1.0 + attempt * 2.0)
        else:
            failed.append(remote)
        time.sleep(pause)
    if f is not None:
        rst_close(f.sock)
    return failed


def ftp_names(host, remote):
    """The names in a folder on the card (one level)."""
    def run(f):
        f.cwd(remote)
        return [n.split('/')[-1] for n in f.nlst()]
    return ftp_retry(host, run)


def ftp_get_file(host, remote, local, size=None):
    """One file, on a connection of its own; kept if a file of that size is already there."""
    if size is not None and os.path.exists(local) and os.path.getsize(local) == size:
        return 'kept'
    def run(f):
        with open(local, 'wb') as out:
            f.retrbinary('RETR ' + remote, out.write, blocksize=1 << 16)
        return os.path.getsize(local)
    return ftp_retry(host, run)


def ftp_put_file(host, local, remote):
    """One file, on a connection of its own; its size on the card checked afterwards."""
    def run(f):
        with open(local, 'rb') as src:
            f.storbinary('STOR ' + remote, src, blocksize=1 << 16)
        f.voidcmd('TYPE I')
        return f.size(remote)
    got = ftp_retry(host, run)
    want = os.path.getsize(local)
    if got != want:
        raise IOError('%s on the card is %s bytes, not %d' % (remote, got, want))
    return got


def ftp_sizes(host, remote):
    """{name: size} of a folder's files (LIST parsed; folders left out)."""
    def run(f):
        f.cwd(remote)
        lines = []
        f.retrlines('LIST', lines.append)
        out = {}
        for line in lines:
            parts = line.split(None, 8)
            if len(parts) == 9 and not line.startswith('d'):
                out[parts[8]] = int(parts[4])
        return out
    return ftp_retry(host, run)


def main():
    global SD_ROOT
    args = sys.argv[1:]
    host = host_of(args)
    SD_ROOT = root_of(args)
    if not args:
        sys.exit(__doc__)
    cmd, rest = args[0], args[1:]
    if cmd in ('shot', 'click', 'hold', 'chord', 'stick', 'title', 'version', 'heap', 'peek', 'raw'):
        b = Bot(host)
        try:
            if cmd == 'shot':
                out = rest[0] if rest else time.strftime('shot_%H%M%S.jpg')
                print(out, b.shot(out), 'bytes')
            elif cmd == 'click':
                for btn in rest:
                    b.send('click ' + btn.upper())
                    time.sleep(0.25)
                print('clicked', ' '.join(rest))
            elif cmd == 'hold':
                ms = int(rest[1]) if len(rest) > 1 else 400
                b.send('press ' + rest[0].upper())
                time.sleep(ms / 1000.0)
                b.send('release ' + rest[0].upper())
                print('held', rest[0], ms, 'ms')
            elif cmd == 'chord':
                buttons = [r for r in rest if not r.isdigit()]
                ms = int(rest[-1]) if rest and rest[-1].isdigit() else 400
                b.chord(buttons, ms)
                print('chord', ' '.join(buttons), ms, 'ms')
            elif cmd == 'stick':
                which = 'LEFT' if rest[0].upper().startswith('L') else 'RIGHT'
                x, y = float(rest[1]), float(rest[2])
                ms = int(rest[3]) if len(rest) > 3 else 300
                b.send('setStick %s %d %d' % (which, int(x * 32767), int(y * 32767)))
                time.sleep(ms / 1000.0)
                b.send('setStick %s 0 0' % which)
                print('stick', which, x, y, ms, 'ms')
            elif cmd == 'title':
                print(b.ask('getTitleID'))
            elif cmd == 'version':
                print(b.ask('getVersion'))
            elif cmd == 'heap':
                print(b.ask('getHeapBase'))
            elif cmd == 'peek':
                print(b.ask('peekAbsolute %s %s' % (rest[0], rest[1])))
            elif cmd == 'raw':
                if not rest or rest[0].split(' ')[0] not in RAW_ALLOWED:
                    sys.exit('refused: raw takes only ' + ', '.join(RAW_ALLOWED))
                print(b.ask(' '.join(rest)))
        finally:
            b.close()
    elif cmd in ('album-ls', 'album-get'):
        # sasrt-nx (2026-11-09): the Album, read only (album_path above).
        if cmd == 'album-ls':
            where = album_path(rest[0] if rest else '/Nintendo/Album', listing=True)
            lines = []
            ftp_retry(host, lambda f: (f.cwd(where), f.retrlines('LIST', lines.append)))
            for line in lines:
                print(line)
        else:
            remote = album_path(rest[0])
            local = rest[1] if len(rest) > 1 else os.path.basename(remote)
            print(local, ftp_get_file(host, remote, local), 'bytes')
    elif cmd in ('crash-ls', 'crash-get'):
        # blur-nx (2026-11-13): Atmosphere's crash reports, read only (crash_path above).
        if cmd == 'crash-ls':
            lines = []
            ftp_retry(host, lambda f: (f.cwd(crash_path()), f.retrlines('LIST', lines.append)))
            for line in lines:
                print(line)
        else:
            remote = crash_path(rest[0] if rest else '')
            local = rest[1] if len(rest) > 1 else rest[0]
            print(local, ftp_get_file(host, remote, local), 'bytes')
    elif cmd == 'stall-dump':
        # blur-nx (2026-11-14, app/src/profiler/stall_watch_switch.cpp): the watchdog looks for the file once a second and
        # deletes it before it dumps, so only the upload is checked (a size check afterwards could race the delete).
        import io
        remote = sd_path('logs/rex/stall_dump')
        ftp_retry(host, lambda f: f.storbinary('STOR ' + remote, io.BytesIO(b'')))
        print('requested a stall dump:', remote, time.strftime('%H:%M:%S'))
    elif cmd in ('ls', 'get', 'put', 'mv'):
        # Refused before any connection is made: the card path is rest[1] for put, rest[0] for the others (both for mv).
        sd_path(rest[1] if cmd == 'put' else (rest[0] if rest else ''))
        if cmd == 'mv':
            sd_path(rest[1])
        if cmd == 'ls':
            lines = []
            ftp_retry(host, lambda f: (f.cwd(sd_path(rest[0] if rest else '')), f.retrlines('LIST', lines.append)))
            for line in lines:
                print(line)
        elif cmd == 'get':
            remote = sd_path(rest[0])
            local = rest[1] if len(rest) > 1 else os.path.basename(remote.rstrip('/')) or 'root'
            try:
                sizes = ftp_sizes(host, remote)
            except Exception:
                sizes = None  # not a folder: one file
            if sizes is None:
                print(local, ftp_get_file(host, remote, local), 'bytes')
            else:
                os.makedirs(local, exist_ok=True)
                failed = []
                for name in sorted(sizes):
                    try:
                        got = ftp_get_file(host, remote + '/' + name, os.path.join(local, name), sizes[name])
                        print(name, got)
                    except Exception as e:
                        failed.append(name)
                        print('FAILED', name, e)
                    time.sleep(0.3)
                print('%d files, %d failed' % (len(sizes), len(failed)))
        elif cmd == 'put':
            print('put', rest[0], '->', sd_path(rest[1]), ftp_put_file(host, rest[0], sd_path(rest[1])), 'bytes')
        elif cmd == 'mv':
            # blur-nx (2026-11-18): a rename inside the port's folder (both paths checked), in place of a delete: an A/B of
            # packed and raw game files sets the raw ones aside and puts them back. Here it also refuses a destination
            # that exists: a rename over it would replace it, and this tool deletes nothing on the card.
            src, dst = sd_path(rest[0]), sd_path(rest[1])
            parent, name = posixpath.split(dst)
            listed = []
            ftp_retry(host, lambda f: (f.cwd(parent), f.retrlines('NLST', listed.append)))
            if any(posixpath.basename(n.rstrip('/')) == name for n in listed):
                sys.exit('refused: %s exists (a rename over it would replace it)' % dst)
            ftp_retry(host, lambda f: f.rename(src, dst))
            print('renamed', src, '->', dst)
    else:
        sys.exit('unknown command %s (no delete command: nothing on the card is deleted)' % cmd)


if __name__ == '__main__':
    main()
