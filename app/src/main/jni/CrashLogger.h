// ================================================================
//  CrashLogger.h — AXIOM DEVELOPMENT — v3 FULL DETAIL
//  Logs: signal, registers (arm64), fp unwind, memory dump,
//        lib map, thread info, fault addr decode
// ================================================================
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <android/log.h>
#include <unwind.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdarg.h>

#define LOG_TAG  "ThrowIO_Axiom"
#define DUMP_DIR "/sdcard/ThrowIO_Crash"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ================================================================
//  GLOBALS
// ================================================================
static char        g_logPath[256]   = {0};
static char        g_crashPath[256] = {0};
static FILE*       g_logFile        = nullptr;
static atomic_bool g_logRunning     = false;
static pthread_t   g_logThread;
static pthread_mutex_t g_logMutex   = PTHREAD_MUTEX_INITIALIZER;

// Alternate stack — bắt crash kể cả khi stack overflow
static uint8_t g_altStackBuf[SIGSTKSZ * 2];

// Old handlers — chain lại sau khi log xong
static struct sigaction g_oldSigSEGV;
static struct sigaction g_oldSigABRT;
static struct sigaction g_oldSigBUS;
static struct sigaction g_oldSigILL;
static struct sigaction g_oldSigFPE;

// ================================================================
//  HELPERS
// ================================================================
static void GetTimestamp(char* buf, size_t len) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    strftime(buf, len, "%Y%m%d_%H%M%S", &t);
}

// Write đồng thời vào file + logcat — dùng trong signal handler
// Không dùng fprintf trong signal handler (không async-signal-safe)
// Dùng write() fd trực tiếp
static int g_crashFd = -1;

static void fd_write(const char* s) {
    if (g_crashFd >= 0) write(g_crashFd, s, strlen(s));
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "%s", s);
}

static void fd_writef(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fd_write(buf);
}

// ================================================================
//  MEMORY SAFE READ — dùng mincore trước khi deref
// ================================================================
static bool mem_readable(uintptr_t addr, size_t sz) {
    if (!addr || addr > 0x7fffffffffffULL) return false;
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    uintptr_t page = addr & ~(uintptr_t)(ps - 1);
    size_t    asz  = (sz + (size_t)ps - 1) & ~((size_t)ps - 1);
    if (!asz) asz = (size_t)ps;
    unsigned char vec = 0;
    return mincore((void*)page, asz, &vec) == 0;
}

// ================================================================
//  RESOLVE ADDRESS — lib name + offset + symbol
// ================================================================
static void resolve_addr(uintptr_t addr) {
    if (!addr) { fd_write("    [NULL]\n"); return; }
    Dl_info di;
    if (dladdr((void*)addr, &di) && di.dli_fname) {
        uintptr_t off    = addr - (uintptr_t)di.dli_fbase;
        uintptr_t symOff = di.dli_saddr
            ? addr - (uintptr_t)di.dli_saddr : 0;
        const char* lib = di.dli_fname;
        // Rút ngắn path — chỉ lấy tên file
        const char* slash = strrchr(lib, '/');
        if (slash) lib = slash + 1;
        if (di.dli_sname) {
            fd_writef("    lib=%-30s  off=0x%08lx  sym=%s+0x%lx\n",
                      lib, (unsigned long)off,
                      di.dli_sname, (unsigned long)symOff);
        } else {
            fd_writef("    lib=%-30s  off=0x%08lx  sym=??\n",
                      lib, (unsigned long)off);
        }
    } else {
        fd_writef("    addr=0x%lx  [not in any loaded lib]\n", (unsigned long)addr);
    }
}

// ================================================================
//  BACKTRACE via _Unwind
// ================================================================
struct UnwindState {
    void**  frames;
    size_t  count;
    size_t  max;
    bool    skip; // skip first frame (signal trampoline)
};

static _Unwind_Reason_Code unwind_cb(struct _Unwind_Context* ctx, void* arg) {
    UnwindState* s = (UnwindState*)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (!pc) return _URC_NO_REASON;
    if (s->skip) { s->skip = false; return _URC_NO_REASON; }
    if (s->count < s->max) s->frames[s->count++] = (void*)pc;
    return (s->count < s->max) ? _URC_NO_REASON : _URC_END_OF_STACK;
}

static size_t capture_bt(void** buf, size_t max) {
    UnwindState s = { buf, 0, max, false };
    _Unwind_Backtrace(unwind_cb, &s);
    return s.count;
}

// ================================================================
//  ARM64 REGISTER DUMP
// ================================================================
#if defined(__aarch64__)
static void dump_registers(mcontext_t* mc) {
    fd_write("--- REGISTERS (arm64) ---\n");
    for (int i = 0; i <= 28; i++) {
        char line[80];
        snprintf(line, sizeof(line), "  x%-2d = 0x%016llx",
                 i, (unsigned long long)mc->regs[i]);
        fd_write(line);
        if (i % 2 == 1) fd_write("\n");
    }
    fd_write("\n");
    fd_writef("  x29 (fp)  = 0x%016llx\n", (unsigned long long)mc->regs[29]);
    fd_writef("  x30 (lr)  = 0x%016llx\n", (unsigned long long)mc->regs[30]);
    fd_writef("  sp        = 0x%016llx\n", (unsigned long long)mc->sp);
    fd_writef("  pc        = 0x%016llx\n", (unsigned long long)mc->pc);
    fd_writef("  pstate    = 0x%016llx\n", (unsigned long long)mc->pstate);
    fd_write("\n");
}

// Walk frame pointer chain (arm64: fp[0]=prev_fp, fp[1]=lr)
static void fp_unwind(mcontext_t* mc) {
    fd_write("--- FP UNWIND ---\n");
    uintptr_t fp = (uintptr_t)mc->regs[29];
    uintptr_t pc = (uintptr_t)mc->pc;
    int frame = 0;

    fd_writef("  #%02d  0x%lx  (PC)\n", frame++, (unsigned long)pc);
    resolve_addr(pc);

    // LR
    uintptr_t lr = (uintptr_t)mc->regs[30];
    if (lr) {
        fd_writef("  #%02d  0x%lx  (LR)\n", frame++, (unsigned long)lr);
        resolve_addr(lr);
    }

    while (fp && frame < 48) {
        if (!mem_readable(fp, 16)) break;
        if (fp % 8 != 0) break;

        uintptr_t prev_fp  = *(uintptr_t*)fp;
        uintptr_t ret_addr = *(uintptr_t*)(fp + 8);

        if (!ret_addr || !mem_readable(ret_addr, 4)) break;

        fd_writef("  #%02d  0x%lx\n", frame++, (unsigned long)(ret_addr - 4));
        resolve_addr(ret_addr - 4);

        if (prev_fp <= fp) break;
        fp = prev_fp;
    }
    fd_write("\n");
}
#endif

// ================================================================
//  MEMORY HEX DUMP around fault addr
// ================================================================
static void dump_memory(uintptr_t faultAddr) {
    fd_write("--- MEMORY NEAR FAULT ADDR ---\n");

    if (!faultAddr || faultAddr < 0x1000) {
        fd_write("  Fault addr = NULL / unmapped — pure null pointer deref\n");
        fd_writef("  Meaning: code tried to read/write address 0x%lx\n\n",
                  (unsigned long)faultAddr);
        return;
    }

    // Dump 128 bytes trước + 128 bytes sau fault addr
    uintptr_t start = (faultAddr >= 0x80) ? (faultAddr - 0x80) : 0;
    start &= ~0xFULL; // align 16

    fd_writef("  [base=0x%lx fault=0x%lx]\n", (unsigned long)start,
              (unsigned long)faultAddr);

    for (int row = 0; row < 16; row++) {
        uintptr_t rowAddr = start + row * 16;
        char line[128];
        int  pos = 0;

        pos += snprintf(line + pos, sizeof(line) - pos,
                        "  0x%lx: ", (unsigned long)rowAddr);

        for (int col = 0; col < 16; col++) {
            uintptr_t byteAddr = rowAddr + col;
            if (mem_readable(byteAddr, 1)) {
                pos += snprintf(line + pos, sizeof(line) - pos,
                                "%02x ", *(uint8_t*)byteAddr);
            } else {
                pos += snprintf(line + pos, sizeof(line) - pos, "?? ");
            }
        }

        // ASCII side
        pos += snprintf(line + pos, sizeof(line) - pos, " |");
        for (int col = 0; col < 16; col++) {
            uintptr_t byteAddr = rowAddr + col;
            if (mem_readable(byteAddr, 1)) {
                uint8_t b = *(uint8_t*)byteAddr;
                pos += snprintf(line + pos, sizeof(line) - pos,
                                "%c", (b >= 0x20 && b < 0x7f) ? b : '.');
            } else {
                pos += snprintf(line + pos, sizeof(line) - pos, "?");
            }
        }
        pos += snprintf(line + pos, sizeof(line) - pos, "|\n");
        fd_write(line);
    }
    fd_write("\n");
}

// ================================================================
//  LOADED LIB SNAPSHOT
// ================================================================
static void dump_libs() {
    fd_write("--- LOADED LIBS (key) ---\n");

    const char* libs[] = {
        "libDuongdev.so",
        "libunity.so",
        "libil2cpp.so",
        "libEGL.so",
        "libGLESv3.so",
        "libGLESv2.so",
        nullptr
    };

    for (int i = 0; libs[i]; i++) {
        void* h = dlopen(libs[i], RTLD_NOW | RTLD_NOLOAD);
        if (h) {
            Dl_info di;
            dladdr(h, &di);
            fd_writef("  [LOADED]  %-25s  base=%p\n",
                      libs[i],
                      di.dli_fbase ? di.dli_fbase : h);
            dlclose(h);
        } else {
            fd_writef("  [MISSING] %s\n", libs[i]);
        }
    }

    // /proc/self/maps — top 40 lines
    fd_write("\n--- /proc/self/maps (first 40 lines) ---\n");
    FILE* maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[256];
        int  cnt = 0;
        while (fgets(line, sizeof(line), maps) && cnt < 40) {
            fd_write("  ");
            fd_write(line);
            cnt++;
        }
        fclose(maps);
    }
    fd_write("\n");
}

// ================================================================
//  SI_CODE DECODE
// ================================================================
static const char* decode_sicode(int sig, int code) {
    if (sig == SIGSEGV) {
        switch (code) {
            case SEGV_MAPERR:  return "SEGV_MAPERR (addr not mapped — null/bad ptr)";
            case SEGV_ACCERR:  return "SEGV_ACCERR (permission denied — no exec/write)";
            case SEGV_BNDERR:  return "SEGV_BNDERR (bounds check failed)";
            case SEGV_PKUERR:  return "SEGV_PKUERR (protection key violation)";
        }
    } else if (sig == SIGBUS) {
        switch (code) {
            case BUS_ADRALN:   return "BUS_ADRALN (unaligned memory access)";
            case BUS_ADRERR:   return "BUS_ADRERR (non-existent physical addr)";
            case BUS_OBJERR:   return "BUS_OBJERR (object-specific hw error)";
        }
    } else if (sig == SIGILL) {
        switch (code) {
            case ILL_ILLOPC:   return "ILL_ILLOPC (illegal opcode)";
            case ILL_ILLOPN:   return "ILL_ILLOPN (illegal operand)";
            case ILL_ILLADR:   return "ILL_ILLADR (illegal addressing mode)";
            case ILL_ILLTRP:   return "ILL_ILLTRP (illegal trap)";
            case ILL_PRVOPC:   return "ILL_PRVOPC (privileged opcode)";
        }
    } else if (sig == SIGFPE) {
        switch (code) {
            case FPE_INTDIV:   return "FPE_INTDIV (integer divide by zero)";
            case FPE_INTOVF:   return "FPE_INTOVF (integer overflow)";
            case FPE_FLTDIV:   return "FPE_FLTDIV (float divide by zero)";
        }
    }
    return "UNKNOWN";
}

// ================================================================
//  SIGNAL HANDLER
// ================================================================
static void CrashSignalHandler(int sig, siginfo_t* si, void* uctx) {
    // Dừng logcat thread
    atomic_store(&g_logRunning, false);

    // Đóng log file
    pthread_mutex_lock(&g_logMutex);
    if (g_logFile) {
        fprintf(g_logFile, "\n[CRASH DETECTED — signal %d]\n", sig);
        fflush(g_logFile);
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    pthread_mutex_unlock(&g_logMutex);

    // Mở crash file với raw fd — async-signal-safe
    char ts[32];
    {
        time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &t);
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/crash_%s.txt", DUMP_DIR, ts);
    g_crashFd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    // ── HEADER ──
    fd_write("==============================================\n");
    fd_write("  THROWIO MOD — AXIOM CRASH REPORT v3\n");
    fd_writef("  Time       : %s\n", ts);
    fd_writef("  PID        : %d\n", getpid());
    fd_writef("  TID        : %d\n", gettid());
    fd_writef("  Signal     : %d (%s)\n", sig,
        sig == SIGSEGV ? "SIGSEGV" :
        sig == SIGABRT ? "SIGABRT" :
        sig == SIGBUS  ? "SIGBUS"  :
        sig == SIGILL  ? "SIGILL"  :
        sig == SIGFPE  ? "SIGFPE"  : "UNKNOWN");

    uintptr_t faultAddr = si ? (uintptr_t)si->si_addr : 0;
    fd_writef("  Fault addr : 0x%lx", (unsigned long)faultAddr);
    if (!faultAddr) fd_write("  ← NULL POINTER DEREF");
    fd_write("\n");

    if (si) {
        fd_writef("  si_code    : %d = %s\n", si->si_code,
                  decode_sicode(sig, si->si_code));
        fd_writef("  si_errno   : %d\n", si->si_errno);
    }

    // Kernel info
    struct utsname uts;
    if (uname(&uts) == 0) {
        fd_writef("  Kernel     : %s %s\n", uts.sysname, uts.release);
        fd_writef("  Machine    : %s\n",    uts.machine);
    }
    fd_write("==============================================\n\n");

    // ── REGISTERS + FP UNWIND (arm64 only) ──
#if defined(__aarch64__)
    ucontext_t* uc = (ucontext_t*)uctx;
    if (uc) {
        mcontext_t* mc = &uc->uc_mcontext;
        dump_registers(mc);
        fp_unwind(mc);
    }
#endif

    // ── _Unwind backtrace ──
    fd_write("--- BACKTRACE (_Unwind) ---\n");
    void*  bt[64];
    size_t btCount = capture_bt(bt, 64);
    fd_writef("  [%zu frames]\n", btCount);
    for (size_t i = 0; i < btCount; i++) {
        fd_writef("  #%02zu  0x%lx\n", i, (unsigned long)(uintptr_t)bt[i]);
        resolve_addr((uintptr_t)bt[i]);
    }
    fd_write("\n");

    // ── MEMORY DUMP ──
    dump_memory(faultAddr);

    // ── LIB MAP ──
    dump_libs();

    fd_write("==============================================\n");
    fd_write("  END OF CRASH REPORT\n");
    fd_write("==============================================\n");

    if (g_crashFd >= 0) { close(g_crashFd); g_crashFd = -1; }

    LOGE("=== CRASH REPORT -> %s ===", path);

    // Chain về handler cũ (Android debuggerd)
    struct sigaction* old =
        sig == SIGSEGV ? &g_oldSigSEGV :
        sig == SIGABRT ? &g_oldSigABRT :
        sig == SIGBUS  ? &g_oldSigBUS  :
        sig == SIGILL  ? &g_oldSigILL  :
                         &g_oldSigFPE;
    sigaction(sig, old, nullptr);
    raise(sig);
}

// ================================================================
//  REGISTER HANDLERS
// ================================================================
static void RegisterCrashHandlers() {
    // Alternate stack — bắt stack overflow crash
    stack_t ss;
    ss.ss_sp    = g_altStackBuf;
    ss.ss_size  = sizeof(g_altStackBuf);
    ss.ss_flags = 0;
    if (sigaltstack(&ss, nullptr) != 0) {
        LOGE("ThrowIO: sigaltstack failed errno=%d (%s)", errno, strerror(errno));
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = CrashSignalHandler;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, &g_oldSigSEGV);
    sigaction(SIGABRT, &sa, &g_oldSigABRT);
    sigaction(SIGBUS,  &sa, &g_oldSigBUS);
    sigaction(SIGILL,  &sa, &g_oldSigILL);
    sigaction(SIGFPE,  &sa, &g_oldSigFPE);

    LOGI("ThrowIO: crash handlers registered — altstack=%p size=%zu",
         g_altStackBuf, sizeof(g_altStackBuf));
}

// ================================================================
//  LOGCAT THREAD
// ================================================================
static void* LogcatCaptureThread(void*) {
    pthread_mutex_lock(&g_logMutex);
    if (!g_logFile) { pthread_mutex_unlock(&g_logMutex); return nullptr; }
    pthread_mutex_unlock(&g_logMutex);

    system("logcat -c");

    FILE* pipe = popen(
        "logcat -v threadtime ThrowIO_Axiom:V il2cpp:E Unity:E *:S",
        "r"
    );
    if (!pipe) {
        LOGE("ThrowIO: logcat pipe failed errno=%d", errno);
        return nullptr;
    }

    char   line[1024];
    size_t totalBytes = 0;
    const  size_t maxBytes = 4 * 1024 * 1024; // 4MB rotate

    while (atomic_load(&g_logRunning) && fgets(line, sizeof(line), pipe)) {
        size_t len = strlen(line);

        pthread_mutex_lock(&g_logMutex);
        if (!g_logFile) { pthread_mutex_unlock(&g_logMutex); break; }
        fwrite(line, 1, len, g_logFile);
        fflush(g_logFile);
        pthread_mutex_unlock(&g_logMutex);

        totalBytes += len;
        if (totalBytes >= maxBytes) {
            pthread_mutex_lock(&g_logMutex);
            if (g_logFile) {
                fclose(g_logFile);
                char bak[300];
                snprintf(bak, sizeof(bak), "%s.bak", g_logPath);
                rename(g_logPath, bak);
                g_logFile = fopen(g_logPath, "w");
                if (g_logFile) {
                    fprintf(g_logFile, "[LOG ROTATED — prev at %s]\n\n", bak);
                    fflush(g_logFile);
                }
            }
            pthread_mutex_unlock(&g_logMutex);
            totalBytes = 0;
        }
    }

    pclose(pipe);
    return nullptr;
}

// ================================================================
//  INIT — gọi ĐẦU TIÊN trong lib_main()
// ================================================================
static void InitCrashLogger() {
    mkdir(DUMP_DIR, 0777);

    char ts[64];
    GetTimestamp(ts, sizeof(ts));

    snprintf(g_logPath,   sizeof(g_logPath),   "%s/log_%s.txt",   DUMP_DIR, ts);
    snprintf(g_crashPath, sizeof(g_crashPath),  "%s/crash_%s.txt", DUMP_DIR, ts);

    pthread_mutex_lock(&g_logMutex);
    g_logFile = fopen(g_logPath, "w");
    if (g_logFile) {
        fprintf(g_logFile,
            "==============================================\n"
            "  THROWIO MOD — AXIOM LIVE LOG\n"
            "  Started: %s\n"
            "  Log   : %s\n"
            "  Crash : %s\n"
            "==============================================\n\n",
            ts, g_logPath, g_crashPath);
        fflush(g_logFile);
        LOGI("ThrowIO: logging to %s", g_logPath);
    } else {
        LOGE("ThrowIO: cannot open log errno=%d (%s)", errno, strerror(errno));
    }
    pthread_mutex_unlock(&g_logMutex);

    RegisterCrashHandlers();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    atomic_store(&g_logRunning, true);
    pthread_create(&g_logThread, &attr, LogcatCaptureThread, nullptr);
    pthread_attr_destroy(&attr);
}
