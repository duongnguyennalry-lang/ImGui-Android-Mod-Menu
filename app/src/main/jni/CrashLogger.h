// ================================================================
//  CrashLogger.h — AXIOM DEVELOPMENT
//  Dumps logcat + signal crash report to /sdcard/ThrowIO_Crash/
//  No PC needed. Read the .txt from any file manager after crash.
// ================================================================
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <android/log.h>
#include <unwind.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>

#define LOG_TAG   "ThrowIO_Axiom"
#define DUMP_DIR  "/sdcard/ThrowIO_Crash"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ================================================================
//  GLOBALS
// ================================================================
static char     g_logPath[256]   = {0};
static char     g_crashPath[256] = {0};
static FILE*    g_logFile        = nullptr;
static atomic_bool g_logRunning  = false;
static pthread_t   g_logThread;

// ================================================================
//  TIMESTAMP HELPER
// ================================================================
static void GetTimestamp(char* buf, size_t len) {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    strftime(buf, len, "%Y%m%d_%H%M%S", t);
}

// ================================================================
//  STACK UNWIND — pulls backtrace without libunwind
// ================================================================
struct BacktraceState {
    void**  current;
    void**  end;
};

static _Unwind_Reason_Code UnwindCallback(
        struct _Unwind_Context* ctx, void* arg) {
    BacktraceState* state = (BacktraceState*)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc) {
        if (state->current == state->end) return _URC_END_OF_STACK;
        *state->current++ = (void*)pc;
    }
    return _URC_NO_REASON;
}

static size_t CaptureBacktrace(void** buffer, size_t max) {
    BacktraceState state = { buffer, buffer + max };
    _Unwind_Backtrace(UnwindCallback, &state);
    return state.current - buffer;
}

// ================================================================
//  WRITE CRASH REPORT TO FILE
// ================================================================
static void WriteCrashReport(int sig, siginfo_t* info, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;

    char ts[64];
    GetTimestamp(ts, sizeof(ts));

    fprintf(f, "==============================================\n");
    fprintf(f, "  THROWIO MOD — AXIOM CRASH REPORT\n");
    fprintf(f, "  Time   : %s\n", ts);
    fprintf(f, "  Signal : %d (%s)\n", sig,
            sig == SIGSEGV ? "SIGSEGV" :
            sig == SIGABRT ? "SIGABRT" :
            sig == SIGBUS  ? "SIGBUS"  :
            sig == SIGILL  ? "SIGILL"  :
            sig == SIGFPE  ? "SIGFPE"  : "UNKNOWN");

    if (info) {
        fprintf(f, "  Fault addr : %p\n", info->si_addr);
        fprintf(f, "  si_code    : %d\n", info->si_code);
        fprintf(f, "  si_errno   : %d\n", info->si_errno);
    }

    fprintf(f, "==============================================\n\n");

    // Backtrace
    void*  bt[64];
    size_t count = CaptureBacktrace(bt, 64);
    fprintf(f, "--- BACKTRACE (%zu frames) ---\n", count);

    for (size_t i = 0; i < count; i++) {
        Dl_info dlinfo;
        if (dladdr(bt[i], &dlinfo)) {
            uintptr_t offset = (uintptr_t)bt[i] - (uintptr_t)dlinfo.dli_fbase;
            fprintf(f, "  #%02zu  %p  offset=0x%lx  %s  [%s]\n",
                    i,
                    bt[i],
                    (unsigned long)offset,
                    dlinfo.dli_sname ? dlinfo.dli_sname : "??",
                    dlinfo.dli_fname ? dlinfo.dli_fname : "??");
        } else {
            fprintf(f, "  #%02zu  %p  [unresolved]\n", i, bt[i]);
        }
    }

    fprintf(f, "\n--- END OF REPORT ---\n");
    fclose(f);

    // Also dump to logcat so it shows even in partial captures
    LOGE("=== CRASH REPORT WRITTEN TO: %s ===", path);
    for (size_t i = 0; i < count; i++) {
        Dl_info dlinfo;
        if (dladdr(bt[i], &dlinfo)) {
            uintptr_t off = (uintptr_t)bt[i] - (uintptr_t)dlinfo.dli_fbase;
            LOGE("  #%02zu %p off=0x%lx %s [%s]",
                 i, bt[i], (unsigned long)off,
                 dlinfo.dli_sname ? dlinfo.dli_sname : "??",
                 dlinfo.dli_fname ? dlinfo.dli_fname : "??");
        }
    }
}

// ================================================================
//  SIGNAL HANDLER
// ================================================================
static struct sigaction g_oldSigSEGV;
static struct sigaction g_oldSigABRT;
static struct sigaction g_oldSigBUS;
static struct sigaction g_oldSigILL;
static struct sigaction g_oldSigFPE;

static void CrashSignalHandler(int sig, siginfo_t* info, void* ctx) {
    // Stop logcat thread immediately
    atomic_store(&g_logRunning, false);

    // Close live log file cleanly
    if (g_logFile) {
        fprintf(g_logFile, "\n[CRASH DETECTED — signal %d]\n", sig);
        fflush(g_logFile);
        fclose(g_logFile);
        g_logFile = nullptr;
    }

    WriteCrashReport(sig, info, g_crashPath);

    // Re-raise with original handler so Android can also process it
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
//  REGISTER SIGNAL HANDLERS
// ================================================================
static void RegisterCrashHandlers() {
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

    LOGI("ThrowIO: crash signal handlers registered");
}

// ================================================================
//  LOGCAT CAPTURE THREAD
//  Pipes logcat -v threadtime into a .txt file on /sdcard/
//  Rotates file if > 4MB to avoid filling storage
// ================================================================
static void* LogcatCaptureThread(void*) {
    if (!g_logFile) return nullptr;

    // Clear previous logcat buffer first
    system("logcat -c");

    // Open logcat pipe — filter to our tag + il2cpp errors
    FILE* pipe = popen(
        "logcat -v threadtime ThrowIO_Axiom:V il2cpp:E Unity:E *:S",
        "r"
    );

    if (!pipe) {
        LOGE("ThrowIO: failed to open logcat pipe — errno %d", errno);
        return nullptr;
    }

    char line[1024];
    size_t totalBytes = 0;
    const size_t maxBytes = 4 * 1024 * 1024; // 4MB cap

    while (atomic_load(&g_logRunning) && fgets(line, sizeof(line), pipe)) {
        if (!g_logFile) break;

        size_t len = strlen(line);
        fwrite(line, 1, len, g_logFile);
        fflush(g_logFile);

        totalBytes += len;

        // Rotate if too large
        if (totalBytes >= maxBytes) {
            fclose(g_logFile);

            // Rename old → .bak
            char bakPath[300];
            snprintf(bakPath, sizeof(bakPath), "%s.bak", g_logPath);
            rename(g_logPath, bakPath);

            g_logFile = fopen(g_logPath, "w");
            if (!g_logFile) break;

            fprintf(g_logFile, "[LOG ROTATED — previous saved as .bak]\n\n");
            fflush(g_logFile);
            totalBytes = 0;
        }
    }

    pclose(pipe);
    return nullptr;
}

// ================================================================
//  INIT — call this FIRST in lib_main()
// ================================================================
static void InitCrashLogger() {
    // Create output directory
    mkdir(DUMP_DIR, 0777);

    char ts[64];
    GetTimestamp(ts, sizeof(ts));

    // Paths
    snprintf(g_logPath,   sizeof(g_logPath),   "%s/log_%s.txt",   DUMP_DIR, ts);
    snprintf(g_crashPath, sizeof(g_crashPath),  "%s/crash_%s.txt", DUMP_DIR, ts);

    // Open live log file
    g_logFile = fopen(g_logPath, "w");
    if (g_logFile) {
        fprintf(g_logFile, "==============================================\n");
        fprintf(g_logFile, "  THROWIO MOD — AXIOM LIVE LOG\n");
        fprintf(g_logFile, "  Started: %s\n", ts);
        fprintf(g_logFile, "  Log   : %s\n", g_logPath);
        fprintf(g_logFile, "  Crash : %s\n", g_crashPath);
        fprintf(g_logFile, "==============================================\n\n");
        fflush(g_logFile);
        LOGI("ThrowIO: logging to %s", g_logPath);
    } else {
        LOGE("ThrowIO: cannot open log file — errno %d (%s)", errno, strerror(errno));
    }

    // Register crash signal handlers
    RegisterCrashHandlers();

    // Start logcat capture thread
    atomic_store(&g_logRunning, true);
    pthread_create(&g_logThread, nullptr, LogcatCaptureThread, nullptr);
}
