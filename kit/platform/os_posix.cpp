// os_posix.cpp - the platform layer on macOS and Linux. The only file above
// third_party/ that may include a POSIX or Mach header besides os_win32.cpp.
#if !defined(__aarch64__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#include "os.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#if !defined(__aarch64__)
#include <ucontext.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

struct OsThread {
    pthread_t handle;
};

struct OsFiber {
#if defined(__aarch64__)
    // x19..x30, SP, and the ABI-preserved low halves of v8..v15.
    uint64_t context[21];
#else
    ucontext_t context;
#endif
    void *stack;
    OsFiberEntry entry;
    void *arg;
};

static thread_local OsFiber *t_fiber = nullptr;

#if defined(__aarch64__)
// Adapted from libco's public-domain AArch64 switching model; preserve the
// additional SIMD callee-saved lanes required by the native C++ ABI.
#ifdef __APPLE__
#define OS_FIBER_SWAP_SYMBOL "_os_fiber_swap"
#else
#define OS_FIBER_SWAP_SYMBOL "os_fiber_swap"
#endif
extern "C" void os_fiber_swap(uint64_t *from, const uint64_t *to);
__asm__(".text\n"
        ".globl " OS_FIBER_SWAP_SYMBOL "\n"
        OS_FIBER_SWAP_SYMBOL ":\n"
        "stp x19, x20, [x0, #0]\n"
        "stp x21, x22, [x0, #16]\n"
        "stp x23, x24, [x0, #32]\n"
        "stp x25, x26, [x0, #48]\n"
        "stp x27, x28, [x0, #64]\n"
        "stp x29, x30, [x0, #80]\n"
        "mov x9, sp\n"
        "str x9, [x0, #96]\n"
        "stp d8, d9, [x0, #104]\n"
        "stp d10, d11, [x0, #120]\n"
        "stp d12, d13, [x0, #136]\n"
        "stp d14, d15, [x0, #152]\n"
        "ldp x19, x20, [x1, #0]\n"
        "ldp x21, x22, [x1, #16]\n"
        "ldp x23, x24, [x1, #32]\n"
        "ldp x25, x26, [x1, #48]\n"
        "ldp x27, x28, [x1, #64]\n"
        "ldp x29, x30, [x1, #80]\n"
        "ldr x9, [x1, #96]\n"
        "mov sp, x9\n"
        "ldp d8, d9, [x1, #104]\n"
        "ldp d10, d11, [x1, #120]\n"
        "ldp d12, d13, [x1, #136]\n"
        "ldp d14, d15, [x1, #152]\n"
        "ret\n");

static void fiber_start() {
    OsFiber *fiber = t_fiber;
#else
static void fiber_start(uint32_t low, uint32_t high) {
    uintptr_t address = (uintptr_t)low | ((uintptr_t)high << 32);
    OsFiber *fiber = (OsFiber *)address;
    t_fiber = fiber;
#endif
    fiber->entry(fiber->arg);
    abort(); // a guest fiber entry must hand control to another fiber or exit
}

extern "C" {

OsThread *os_thread_create(void *(*fn)(void *), void *arg, size_t stack_bytes) {
    OsThread *t = (OsThread *)calloc(1, sizeof *t);
    if (!t)
        return nullptr;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_bytes)
        pthread_attr_setstacksize(&attr, stack_bytes);
    int rc = pthread_create(&t->handle, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(t);
        return nullptr;
    }
    return t;
}
void os_thread_join(OsThread *t) {
    pthread_join(t->handle, nullptr);
    free(t);
}
void os_thread_detach(OsThread *t) {
    pthread_detach(t->handle);
    free(t);
}
void os_thread_prefer_performance(void) {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}
void os_thread_exit(void) {
    pthread_exit(nullptr);
}
OsThreadId os_thread_self(void) {
    return (OsThreadId)(uintptr_t)pthread_self();
}
OsThreadId os_thread_id_of(const OsThread *t) {
    return (OsThreadId)(uintptr_t)t->handle;
}

OsFiber *os_fiber_current(void) {
    if (!t_fiber) {
        OsFiber *fiber = (OsFiber *)calloc(1, sizeof *fiber);
        if (!fiber) {
            return nullptr;
        }
#if !defined(__aarch64__)
        if (getcontext(&fiber->context) != 0) {
            free(fiber);
            return nullptr;
        }
#endif
        t_fiber = fiber;
    }
    return t_fiber;
}
OsFiber *os_fiber_create(size_t host_stack_bytes, OsFiberEntry entry, void *arg) {
    if (!entry || host_stack_bytes < 65536)
        return nullptr;
    OsFiber *fiber = (OsFiber *)calloc(1, sizeof *fiber);
    if (!fiber)
        return nullptr;
    fiber->stack = malloc(host_stack_bytes);
    if (!fiber->stack) {
        free(fiber->stack);
        free(fiber);
        return nullptr;
    }
    fiber->entry = entry;
    fiber->arg = arg;
#if defined(__aarch64__)
    fiber->context[11] = (uintptr_t)fiber_start; // x30 / initial return target
    fiber->context[12] = ((uintptr_t)fiber->stack + host_stack_bytes) & ~(uintptr_t)15;
#else
    if (getcontext(&fiber->context) != 0) {
        free(fiber->stack);
        free(fiber);
        return nullptr;
    }
    fiber->context.uc_stack.ss_sp = fiber->stack;
    fiber->context.uc_stack.ss_size = host_stack_bytes;
    fiber->context.uc_link = nullptr;
    uintptr_t address = (uintptr_t)fiber;
    makecontext(&fiber->context, (void (*)())fiber_start, 2, (uint32_t)address,
                (uint32_t)(address >> 32));
#endif
    return fiber;
}
void os_fiber_switch(OsFiber *to) {
    OsFiber *from = os_fiber_current();
    if (!from || !to || from == to)
        abort();
    t_fiber = to;
#if defined(__aarch64__)
    os_fiber_swap(from->context, to->context);
#else
    if (swapcontext(&from->context, &to->context) != 0)
        abort();
#endif
}
void os_fiber_destroy(OsFiber *fiber) {
    if (!fiber || fiber == t_fiber)
        abort();
    free(fiber->stack);
    free(fiber);
}

void *os_vm_reserve(size_t bytes) {
    void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
void os_vm_release(void *p, size_t bytes) {
    munmap(p, bytes);
}

void *os_dlopen(const char *path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
void *os_dlopen_noload(const char *path) {
    return dlopen(path, RTLD_NOLOAD | RTLD_LOCAL);
}
void *os_dlsym(void *handle, const char *name) {
    return dlsym(handle, name);
}
int os_dlclose(void *handle) {
    return dlclose(handle);
}
const char *os_dlerror(void) {
    const char *e = dlerror();
    return e ? e : "unknown dynamic loader error";
}
const char *os_plugin_extension(void) {
#ifdef __APPLE__
    return ".dylib";
#else
    return ".so";
#endif
}

static void fill_stat(const struct stat &st, OsStat *out) {
    out->size = (uint64_t)st.st_size;
    out->atime = (int64_t)st.st_atime;
    out->mtime = (int64_t)st.st_mtime;
    out->ctime = (int64_t)st.st_ctime;
    out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    out->is_regular = S_ISREG(st.st_mode) ? 1 : 0;
    out->is_symlink = S_ISLNK(st.st_mode) ? 1 : 0;
    out->is_readonly = (st.st_mode & S_IWUSR) ? 0 : 1;
}
int os_stat(const char *path, OsStat *out) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    fill_stat(st, out);
    return 0;
}
int os_lstat(const char *path, OsStat *out) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    fill_stat(st, out);
    return 0;
}
int os_mkdir(const char *path) {
    return mkdir(path, 0755);
}
int os_rename(const char *from, const char *to) {
    return rename(from, to);
}
int os_unlink(const char *path) {
    return unlink(path);
}
int os_rmdir(const char *path) {
    return rmdir(path);
}
int os_getcwd(char *buf, size_t cap) {
    return getcwd(buf, cap) ? 0 : -1;
}
int os_chdir(const char *path) {
    return chdir(path);
}
int os_listdir(const char *dir, OsListDirFn fn, void *user) {
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    while (struct dirent *e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (fn(e->d_name, user) != 0)
            break;
    }
    closedir(d);
    return 0;
}
int os_mkstemp(char *template_path) {
    return mkstemp(template_path);
}

int os_mkdtemp(char *template_path) {
    return mkdtemp(template_path) ? 0 : -1;
}

const char *os_temp_dir(void) {
    const char *t = getenv("TMPDIR");
    static char buf[4096];
    if (!t || !*t)
        return "/tmp";
    size_t n = strlen(t);
    if (n >= sizeof buf)
        return "/tmp";
    memcpy(buf, t, n + 1);
    while (n > 1 && buf[n - 1] == '/')
        buf[--n] = 0;
    return buf;
}

const char *os_null_device(void) {
    return "/dev/null";
}

int os_free_space(const char *path, uint64_t *bytes_out) {
    struct statvfs vfs;
    if (!bytes_out || statvfs(path, &vfs) != 0)
        return -1;
    *bytes_out = uint64_t(vfs.f_bavail) * uint64_t(vfs.f_frsize);
    return 0;
}
int os_set_mtime(const char *path, int64_t mtime) {
    struct timeval times[2];
    times[0].tv_sec = times[1].tv_sec = (time_t)mtime;
    times[0].tv_usec = times[1].tv_usec = 0;
    return utimes(path, times);
}
int os_registry_read(const char *, const char *, char *, size_t) {
    return -1;
}
int os_user_data_dir(const char *app, char *buf, size_t cap) {
    const char *home = getenv("HOME");
    char base[4096];
#ifdef __APPLE__
    if (!home || !*home)
        return -1;
    snprintf(base, sizeof base, "%s/Library/Application Support", home);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg)
        snprintf(base, sizeof base, "%s", xdg);
    else if (home && *home)
        snprintf(base, sizeof base, "%s/.local/share", home);
    else
        return -1;
#endif
    int n = snprintf(buf, cap, "%s/%s", base, app);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

extern "C" char **environ;

int os_spawn(const char *const argv[], int64_t *pid_out) {
    pid_t pid;
    if (posix_spawn(&pid, argv[0], NULL, NULL, (char *const *)argv, environ) != 0)
        return -1;
    *pid_out = pid;
    return 0;
}

int os_wait(int64_t pid, int *exit_code) {
    int status = 0;
    if (waitpid((pid_t)pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status))
        *exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        *exit_code = 128 + WTERMSIG(status);
    else
        *exit_code = -1;
    return 0;
}

static int native_flags(int flags) {
    int f = 0;
    switch (flags & 3) {
    case OS_O_WRONLY:
        f = O_WRONLY;
        break;
    case OS_O_RDWR:
        f = O_RDWR;
        break;
    default:
        f = O_RDONLY;
        break;
    }
    if (flags & OS_O_CREAT)
        f |= O_CREAT;
    if (flags & OS_O_EXCL)
        f |= O_EXCL;
    if (flags & OS_O_TRUNC)
        f |= O_TRUNC;
    return f;
}
int os_fd_open(const char *path, int flags) {
    return open(path, native_flags(flags), 0644);
}
int64_t os_fd_read(int fd, void *buf, size_t n) {
    return (int64_t)read(fd, buf, n);
}
int64_t os_fd_write(int fd, const void *buf, size_t n) {
    return (int64_t)write(fd, buf, n);
}
int64_t os_fd_seek(int fd, int64_t off, int whence) {
    return (int64_t)lseek(fd, (off_t)off, whence);
}
int os_fd_close(int fd) {
    return close(fd);
}
int os_fd_dup(int fd) {
    return dup(fd);
}
int os_fd_fsync(int fd) {
    return fsync(fd);
}
int os_fd_truncate(int fd, int64_t length) {
    return ftruncate(fd, (off_t)length);
}
int os_fd_stat(int fd, OsStat *out) {
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;
    fill_stat(st, out);
    return 0;
}
void *os_fdopen(int fd, const char *mode) {
    return fdopen(fd, mode);
}

int os_exe_path(char *buf, size_t cap) {
#ifdef __EMSCRIPTEN__
    // The web build's files are packaged under /app: resources in /app/resources.
    snprintf(buf, cap, "/app/app");
    return 0;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)cap;
    return _NSGetExecutablePath(buf, &size) == 0 ? 0 : -1;
#else
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return 0;
#endif
}

static OsFaultFn g_fault_fn = nullptr;
static void fault_trampoline(int sig) {
    const char *what = sig == SIGSEGV   ? "SIGSEGV"
                       : sig == SIGBUS  ? "SIGBUS"
                       : sig == SIGABRT ? "an abort from the runtime"
                                        : "a fatal signal";
    g_fault_fn(what);
}
int os_install_fault_handlers(OsFaultFn fn) {
    g_fault_fn = fn;
    signal(SIGSEGV, fault_trampoline);
    signal(SIGBUS, fault_trampoline);
    signal(SIGABRT, fault_trampoline);
    return 1;
}

void os_write_stderr_raw(const char *s, size_t n) {
    ssize_t ignored = write(2, s, n);
    (void)ignored;
}
void os_exit_immediately(int code) {
    _exit(code);
}

int os_localtime(int64_t seconds, struct tm *out) {
    time_t t = (time_t)seconds;
    return localtime_r(&t, out) ? 0 : -1;
}
int os_gmtime(int64_t seconds, struct tm *out) {
    time_t t = (time_t)seconds;
    return gmtime_r(&t, out) ? 0 : -1;
}

uint64_t os_monotonic_ns(void) {
#ifdef __APPLE__
    // CLOCK_MONOTONIC on Darwin works out the boot time on every call
    // (gettimeofday); the uptime clock is a plain mach_absolute_time read.
    // Every import call reads this clock, so the difference is large.
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t os_wall_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}
void os_sleep_us(uint64_t us) {
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000ull);
    ts.tv_nsec = (long)((us % 1000000ull) * 1000ull);
    nanosleep(&ts, nullptr);
}

int os_strcasecmp(const char *a, const char *b) {
    return strcasecmp(a, b);
}

int os_setenv(const char *name, const char *value) {
    return setenv(name, value, 1);
}
int os_unsetenv(const char *name) {
    return unsetenv(name);
}

} // extern "C"
