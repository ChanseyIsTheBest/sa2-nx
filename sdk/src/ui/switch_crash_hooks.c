/*
 * Silent deaths on Switch: abort(), exit() and whatever goes to stderr.
 *
 * newlib's abort() returns to the HOME menu without a word (measured on the first
 * boot of the SDK, see switch_libc_supplement.c). rex::FatalError ends there, as do
 * std::terminate (an uncaught exception, a std::thread destroyed without join) and
 * Mesa's aborts. And they all warn on stderr first, which goes nowhere in an NRO.
 *
 * It has to be in the executable, in rexglue_switch_startup, and not in a library:
 * the definition of abort has to reach the link before the one in libc.a to
 * replace it.
 *
 *   stderr  -> <NRO folder>/logs/rex/rex_stderr.log, unbuffered, rewritten on every boot; blur-nx (2026-11-18):
 *              only while logging is on (RexSwitchStderrToFile, from logging.cpp), opened at its first write; with
 *              every log off the last 8 KB stay in memory and go into rex_crash.log if the game crashes
 *   abort() -> <NRO folder>/logs/rex/rex_crash.log with the stack, and error 2345-0101
 *   exit()  -> the same, error 2345-0103. The orderly shutdown does not go through exit()
 *              (see windowed_app_main_switch.cpp), so getting here means some library
 *              decided to end the process on its own.
 *
 * The rexglue logs go to <NRO folder>/logs/rex/ and not to sdmc:/switch/, so they do not mix with
 * those of other programs or with the game's, which go to <NRO folder>/logs/. The folder comes from
 * argv[0], which libnx fills in before the constructors. Without argv[0], sdmc:/switch/ is used.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

/* Filled in by libnx (argv.c), which does not declare them in its headers. */
extern int __system_argc;
extern char** __system_argv;

/* Ends in '/'. Computed once at startup and never changes: the fault handler reads it without allocating memory. */
static char g_rex_log_dir[FS_MAX_PATH] = "sdmc:/switch/";

const char* RexSwitchLogDir(void) {
    return g_rex_log_dir;
}

static void RexSwitchInitLogDir(void) {
    if (__system_argc < 1 || !__system_argv || !__system_argv[0]) {
        return;
    }
    const char* nro = __system_argv[0];
    const char* bar = strrchr(nro, '/');
    if (!bar) {
        return;
    }
    /* without a device ("/switch/..."), the SD one: the path must also work for fsdevTranslatePath */
    const char* vulkan_device = strchr(nro, ':') ? "" : "sdmc:";
    char dir[FS_MAX_PATH];
    int n = snprintf(dir, sizeof(dir), "%s%.*slogs", vulkan_device, (int)(bar - nro + 1), nro);
    if (n <= 0 || (size_t)n + sizeof("/rex/") > sizeof(dir)) {
        return;
    }
    mkdir(dir, 0777);
    strcat(dir, "/rex");
    mkdir(dir, 0777);
    struct stat st;
    if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        strcat(dir, "/");
        strcpy(g_rex_log_dir, dir);
    }
}

void RexSwitchCrashLog(const char* reason, const ThreadExceptionDump* ctx, u64 stack, u64 pc);

void abort(void) {
    RexSwitchCrashLog("abort()", NULL, (u64)__builtin_frame_address(0),
                      (u64)__builtin_return_address(0));
    diagAbortWithResult(MAKERESULT(Module_Libnx, 101));
}

static void RexSwitchExitHook(void) {
    RexSwitchCrashLog("exit() outside an orderly shutdown", NULL, (u64)__builtin_frame_address(0),
                      (u64)__builtin_return_address(0));
    diagAbortWithResult(MAKERESULT(Module_Libnx, 103));
}

/*
 * blur-nx (2026-11-18, the user: "final nro just make sure all logging is off"; crash reports only): stderr goes to a
 * device of its own ("rexstderr:"). Before, rex_stderr.log was made at every boot and the SDK's modules (SaltyNX,
 * APM) wrote their status lines to it in every run, so a release with every log off still wrote a log each start.
 * Now, until the logger says logging is on (RexSwitchStderrToFile(1), from sdk/src/core/logging.cpp), what reaches
 * stderr is kept in memory, the last REX_STDERR_KEEP bytes, and a crash report carries it (RexSwitchStderrRecent, read
 * by RexSwitchCrashLog into rex_crash.log): nothing is written in normal play, and a crash still says what stderr saw
 * before it (Mesa's, libnx's, an uncaught exception's message). With logging on, the kept bytes and everything after
 * go to rex_stderr.log, opened at the first write. A failed open drops what is written, as stderr did with no file.
 */
#define REX_STDERR_KEEP 8192
static char g_rex_stderr_keep[REX_STDERR_KEEP];
static size_t g_rex_stderr_kept;
static volatile int g_rex_stderr_to_file;
static int g_rex_stderr_fd = -1;
static int g_rex_stderr_failed;

static int RexStderrFile(void) {
    if (g_rex_stderr_fd < 0 && !g_rex_stderr_failed) {
        char path[FS_MAX_PATH];
        snprintf(path, sizeof(path), "%srex_stderr.log", g_rex_log_dir);
        g_rex_stderr_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (g_rex_stderr_fd < 0) g_rex_stderr_failed = 1;
    }
    return g_rex_stderr_fd;
}

/* The bytes kept: the last REX_STDERR_KEEP written (called under stderr's lock, as every write is). */
static void RexStderrKeep(const char* ptr, size_t len) {
    if (len >= REX_STDERR_KEEP) {
        memcpy(g_rex_stderr_keep, ptr + len - REX_STDERR_KEEP, REX_STDERR_KEEP);
        g_rex_stderr_kept = REX_STDERR_KEEP;
        return;
    }
    if (g_rex_stderr_kept + len > REX_STDERR_KEEP) {
        const size_t drop = g_rex_stderr_kept + len - REX_STDERR_KEEP;
        memmove(g_rex_stderr_keep, g_rex_stderr_keep + drop, g_rex_stderr_kept - drop);
        g_rex_stderr_kept -= drop;
    }
    memcpy(g_rex_stderr_keep + g_rex_stderr_kept, ptr, len);
    g_rex_stderr_kept += len;
}

static ssize_t RexStderrWrite(struct _reent* r, void* fd, const char* ptr, size_t len) {
    (void)r;
    (void)fd;
    if (!g_rex_stderr_to_file) {
        RexStderrKeep(ptr, len);
        return (ssize_t)len;
    }
    const int file = RexStderrFile();
    if (file < 0) return (ssize_t)len;
    return write(file, ptr, len);
}

/* sdk/src/core/logging.cpp, once the log levels are known: on (1) when anything is logged. */
void RexSwitchStderrToFile(int on) {
    if (!on || g_rex_stderr_to_file) return;
    flockfile(stderr);
    g_rex_stderr_to_file = 1;
    const int file = RexStderrFile();
    if (file >= 0 && g_rex_stderr_kept) write(file, g_rex_stderr_keep, g_rex_stderr_kept);
    g_rex_stderr_kept = 0;
    funlockfile(stderr);
}

/* For the crash report: what stderr saw while it was kept in memory (NULL when it goes to rex_stderr.log). Read
 * without the lock: a crash may come from inside a write. */
const char* RexSwitchStderrRecent(size_t* len) {
    *len = g_rex_stderr_to_file ? 0 : g_rex_stderr_kept;
    return *len ? g_rex_stderr_keep : NULL;
}

static int RexStderrOpen(struct _reent* r, void* file, const char* path, int flags, int mode) {
    (void)r;
    (void)file;
    (void)path;
    (void)flags;
    (void)mode;
    return 0;
}

static int RexStderrClose(struct _reent* r, void* fd) {
    (void)r;
    (void)fd;
    return 0;
}

static const devoptab_t g_rex_stderr_device = {
    .name = "rexstderr",
    .structSize = sizeof(int),
    .open_r = RexStderrOpen,
    .close_r = RexStderrClose,
    .write_r = RexStderrWrite,
};

/*
 * Priority 101, the first one allowed: before any static constructor of the
 * SDK, which can also die. The SD is already mounted: __appInit runs before the
 * constructors.
 */
__attribute__((constructor(101)))
static void RexSwitchCrashHooksInit(void) {
    RexSwitchInitLogDir();
    if (AddDevice(&g_rex_stderr_device) >= 0 && freopen("rexstderr:/", "w", stderr)) {
        setvbuf(stderr, NULL, _IONBF, 0);
    } else {
        char path[FS_MAX_PATH];  /* the device refused: the file at once, as before */
        snprintf(path, sizeof(path), "%srex_stderr.log", g_rex_log_dir);
        if (freopen(path, "w", stderr)) {
            setvbuf(stderr, NULL, _IONBF, 0);
        }
    }
    atexit(RexSwitchExitHook);
}
