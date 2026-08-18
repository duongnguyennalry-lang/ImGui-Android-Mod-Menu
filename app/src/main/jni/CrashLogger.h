// ================================================================
//  CrashLogger.h — AXIOM DEVELOPMENT — PATCHED
//  Fix: sigaltstack + localtime_r + mutex on g_logFile
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
static char        g_logPath[256]   = {0};
static char        g_crashPath[256] = {0};
static FILE*       g_logFile        = nullptr;
static atomic_bool g_logRunning     = false;
static pthread_t   g_logThread;

// FIX: mutex bảo vệ g_logFile — tránh race giữa signal handler và logcat thread
static pthread_mutex_t g_logMutex = PTHREAD_MUTEX_INITIALIZER;

// FIX: alternate signal stack — bắt được crash kể cả stack overflow
static stack_t g_altStack;
static uint8_t g_altStackBuf[SIGSTKSZ * 2]; // 2x SIGSTKSZ cho an toàn

// ================================================================
//  FIX: localtime_r — thread-safe timestamp
// ================================================================
static void GetTimestamp(char* buf, size_t len) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t); // FIX: localtime() không thread-safe
    strftime(buf, len, "%Y%m%d_%H%M%S", &t);
}

// ================================================================
//  STACK UNWIND
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
//  WRITE CRASH REPORT
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
        fprintf(f, "  Fault addr : %p\n",  info->si_addr);
        fprintf(f, "  si_code    : %d\n",  info->si_code);
        fprintf(f, "  si_errno   : %d\n",  info->si_errno);
    }
    fprintf(f, "==============================================\n\n");

    void*  bt[64];
    size_t count = CaptureBacktrace(bt, 64);
    fprintf(f, "--- BACKTRACE (%zu frames) ---\n", count);

    for (size_t i = 0; i < count; i++) {
        Dl_info dlinfo;
        if (dladdr(bt[i], &dlinfo)) {
            uintptr_t offset = (uintptr_t)bt[i] - (uintptr_t)dlinfo.dli_fbase;
            fprintf(f, "  #%02zu  %p  offset=0x%lx  %s  [%s]\n",
                    i, bt[i], (unsigned long)offset,
                    dlinfo.dli_sname ? dlinfo.dli_sname : "??",
                    dlinfo.dli_fname ? dlinfo.dli_fname : "??");
        } else {
            fprintf(f, "  #%02zu  %p  [unresolved]\n", i, bt[i]);
        }
    }

    fprintf(f, "\n--- END OF REPORT ---\n");
    fclose(f);

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
//  SIGNAL HANDLERS
// ================================================================
static struct sigaction g_oldSigSEGV;
static struct sigaction g_oldSigABRT;
static struct sigaction g_oldSigBUS;
static struct sigaction g_oldSigILL;
static struct sigaction g_oldSigFPE;

static void CrashSignalHandler(int sig, siginfo_t* info, void* ctx) {
    atomic_store(&g_logRunning, false);

    // FIX: lock trước khi đụng g_logFile
    pthread_mutex_lock(&g_logMutex);
    if (g_logFile) {
        fprintf(g_logFile, "\n[CRASH DETECTED — signal %d]\n", sig);
        fflush(g_logFile);
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    pthread_mutex_unlock(&g_logMutex);

    WriteCrashReport(sig, info, g_crashPath);

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
//  FIX: setup sigaltstack TRƯỚC khi register — bắt được stack overflow
// ================================================================
static void RegisterCrashHandlers() {
    // FIX: alternate stack — không có cái này thì SA_ONSTACK vô dụng
    g_altStack.ss_sp    = g_altStackBuf;
    g_altStack.ss_size  = sizeof(g_altStackBuf);
    g_altStack.ss_flags = 0;
    if (sigaltstack(&g_altStack, nullptr) != 0) {
        LOGE("ThrowIO: sigaltstack failed — errno %d (%s)", errno, strerror(errno));
        // Không bail — vẫn register handler, chỉ mất stack overflow protection
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = CrashSignalHandler;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK; // SA_ONSTACK giờ mới có tác dụng
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, &g_oldSigSEGV);
    sigaction(SIGABRT, &sa, &g_oldSigABRT);
    sigaction(SIGBUS,  &sa, &g_oldSigBUS);
    sigaction(SIGILL,  &sa, &g_oldSigILL);
    sigaction(SIGFPE,  &sa, &g_oldSigFPE);

    LOGI("ThrowIO: crash handlers registered — altstack @ %p", g_altStackBuf);
}

// ================================================================
//  LOGCAT CAPTURE THREAD
// ================================================================
static void* LogcatCaptureThread(void*) {
    pthread_mutex_lock(&g_logMutex);
    if (!g_logFile) {
        pthread_mutex_unlock(&g_logMutex);
        return nullptr;
    }
    pthread_mutex_unlock(&g_logMutex);

    system("logcat -c");

    FILE* pipe = popen(
        "logcat -v threadtime ThrowIO_Axiom:V il2cpp:E Unity:E *:S",
        "r"
    );
    if (!pipe) {
        LOGE("ThrowIO: logcat pipe failed — errno %d", errno);
        return nullptr;
    }

    char   line[1024];
    size_t totalBytes = 0;
    const  size_t maxBytes = 4 * 1024 * 1024;

    while (atomic_load(&g_logRunning) && fgets(line, sizeof(line), pipe)) {
        size_t len = strlen(line);

        // FIX: lock per write — tránh race với signal handler
        pthread_mutex_lock(&g_logMutex);
        if (!g_logFile) {
            pthread_mutex_unlock(&g_logMutex);
            break;
        }
        fwrite(line, 1, len, g_logFile);
        fflush(g_logFile);
        pthread_mutex_unlock(&g_logMutex);

        totalBytes += len;

        if (totalBytes >= maxBytes) {
            pthread_mutex_lock(&g_logMutex);
            if (g_logFile) {
                fclose(g_logFile);
                char bakPath[300];
                snprintf(bakPath, sizeof(bakPath), "%s.bak", g_logPath);
                rename(g_logPath, bakPath);
                g_logFile = fopen(g_logPath, "w");
                if (g_logFile) {
                    fprintf(g_logFile, "[LOG ROTATED — prev saved as .bak]\n\n");
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
//  INIT — gọi TRƯỚC TIÊN trong lib_main()
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
        fprintf(g_logFile, "==============================================\n");
        fprintf(g_logFile, "  THROWIO MOD — AXIOM LIVE LOG\n");
        fprintf(g_logFile, "  Started: %s\n", ts);
        fprintf(g_logFile, "  Log   : %s\n",  g_logPath);
        fprintf(g_logFile, "  Crash : %s\n",  g_crashPath);
        fprintf(g_logFile, "==============================================\n\n");
        fflush(g_logFile);
        LOGI("ThrowIO: logging to %s", g_logPath);
    } else {
        LOGE("ThrowIO: cannot open log — errno %d (%s)", errno, strerror(errno));
    }
    pthread_mutex_unlock(&g_logMutex);

    RegisterCrashHandlers();

    // FIX: detach thread — không cần join, tự clean up khi xong
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    atomic_store(&g_logRunning, true);
    pthread_create(&g_logThread, &attr, LogcatCaptureThread, nullptr);
    pthread_attr_destroy(&attr);
}
