// ================================================================
//  THROWIO MOD - AXIOM DEVELOPMENT
//  CRASH FIX v4 — SIGSEGV TID MISMATCH + IMGUI UNPROTECTED
//
//  ROOT CAUSE (thật sự):
//    - Crash TID 17540 = Unity render thread chạy hook_eglSwapBuffers
//    - sigsetjmp set ở TID 17511 (hack_thread) → bảo vệ SAI THREAD
//    - siglongjmp cross-thread = UB → crash thêm lần nữa
//    - SetupImGui() gọi từ render thread không có bảo vệ gì
//    - SA_RESETHAND reset sau signal đầu → attempt 2+ không catch được
//
//  FIXES v4:
//    [1] thread_local sigjmp_buf — mỗi thread tự bảo vệ mình
//    [2] Signal guard wrap quanh SafeSetupImGui trong render thread
//    [3] RTLD_NOW|RTLD_NOLOAD → chỉ RTLD_NOLOAD
//    [4] SA_RESETHAND bỏ → dùng SA_SIGINFO để giữ handler
//    [5] Frame threshold tăng 90→150 cho EGL stable hơn
//    [6] Double-check EGL surface dim trước khi ImGui init
// ================================================================

#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include "MainHeader.h"
#include "CrashLogger.h"
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <pthread.h>

// ================================================================
//  FUNCTION POINTERS
// ================================================================
void (*set_SoftMoney)(void* instance, long  value) = nullptr;
void (*set_HardMoney)(void* instance, long  value) = nullptr;
void (*set_Level)    (void* instance, int   value) = nullptr;
void (*set_Exp)      (void* instance, int   value) = nullptr;
void (*set_NoAds)    (void* instance, bool  value) = nullptr;
long (*get_SoftMoney)(void* instance)              = nullptr;
long (*get_HardMoney)(void* instance)              = nullptr;
int  (*get_Level)    (void* instance)              = nullptr;

void (*old_ApplyDamage)      (void* instance, long damage, void* from, bool isCritical, void* extra) = nullptr;
void (*old_SetDeath)         (void* instance, bool isDead)     = nullptr;
void (*old_CharWeapon_update)(void* instance, float deltaTime) = nullptr;
void (*old_SaveLocal)        (void* instance)                  = nullptr;
void (*orig_set_SoftMoney)   (void* instance, long value)      = nullptr;

// ================================================================
//  TOGGLES
// ================================================================
namespace SWITCH {
    bool InfiniteMoney   = false;
    bool InfinitePremium = false;
    bool MaxLevel        = false;
    bool NoAds           = false;
    bool GodMode         = false;
    bool SpeedHack       = false;
    bool AntiCheat       = true;
}

// ================================================================
//  GLOBALS
// ================================================================
void*  g_BalanceInstance = nullptr;
float  speedMultiplier   = 2.0f;

static std::atomic<int>  g_frameCount{0};
static std::atomic<bool> g_eglReady{false};
static std::atomic<bool> g_imguiSetup{false};
static std::atomic<bool> g_hooksReady{false};
static std::atomic<bool> g_imguiFailed{false};
static std::atomic<int>  g_failCount{0};

static EGLDisplay g_savedDisplay = EGL_NO_DISPLAY;
static EGLContext g_savedContext = EGL_NO_CONTEXT;

EGLBoolean (*old_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

// ================================================================
//  [FIX v4] THREAD-LOCAL sigjmp_buf
//  Bug cũ: global g_bnm_jmp → siglongjmp cross-thread = UB
//  Fix: mỗi thread có jmp_buf riêng → longjmp đúng stack
// ================================================================
static thread_local sigjmp_buf  tl_crash_jmp;
static thread_local volatile bool tl_in_guard = false;

// Signal handler — chỉ longjmp nếu thread này đang trong guard
static void ThreadSafeCrashHandler(int sig, siginfo_t* info, void* ctx) {
    if (tl_in_guard) {
        tl_in_guard = false;
        siglongjmp(tl_crash_jmp, 1);
    }
    // Thread khác crash → abort gracefully
    LOGE(OBFUSCATE("ThrowIO: unguarded SIGSEGV sig=%d — thread không trong guard"), sig);
}

// ================================================================
//  Signal Guard Scope Helper
//  Dùng cho BẤT KỲ thread nào cần protect, không chỉ hack_thread
// ================================================================
struct SignalGuard {
    struct sigaction old_sa_segv;
    struct sigaction old_sa_bus;
    bool active = false;

    bool install() {
        struct sigaction sa;
        sa.sa_sigaction = ThreadSafeCrashHandler;
        // [FIX] Bỏ SA_RESETHAND — handler giữ nguyên sau signal
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);

        if (sigaction(SIGSEGV, &sa, &old_sa_segv) != 0) return false;
        if (sigaction(SIGBUS,  &sa, &old_sa_bus)  != 0) {
            sigaction(SIGSEGV, &old_sa_segv, nullptr);
            return false;
        }
        tl_in_guard = true;
        active = true;
        return true;
    }

    void restore() {
        if (!active) return;
        tl_in_guard = false;
        sigaction(SIGSEGV, &old_sa_segv, nullptr);
        sigaction(SIGBUS,  &old_sa_bus,  nullptr);
        active = false;
    }

    ~SignalGuard() { restore(); }
};

// ================================================================
//  IsValidPtr — dùng mincore, validate user-space range
// ================================================================
static bool IsValidPtr(const void* ptr, size_t sz = sizeof(void*)) {
    if (!ptr) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < 0x10000ULL)            return false;
    if (addr > 0x7fffffffffffULL)     return false;
    if (addr & 0x1)                   return false;

    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) pageSize = 4096;

    uintptr_t page = addr & ~static_cast<uintptr_t>(pageSize - 1);
    size_t alignedSz = (sz + static_cast<size_t>(pageSize) - 1)
                       & ~static_cast<size_t>(pageSize - 1);
    if (alignedSz == 0) alignedSz = static_cast<size_t>(pageSize);

    unsigned char vec = 0;
    return mincore(reinterpret_cast<void*>(page), alignedSz, &vec) == 0;
}

// ================================================================
//  IsEGLContextCurrent
// ================================================================
static bool IsEGLContextCurrent() {
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext  ctx = eglGetCurrentContext();
    if (dpy == EGL_NO_DISPLAY || ctx == EGL_NO_CONTEXT) return false;
    if (g_savedContext != EGL_NO_CONTEXT && ctx != g_savedContext) {
        LOGE(OBFUSCATE("ThrowIO: EGL context mismatch saved=%p current=%p"),
             g_savedContext, ctx);
        return false;
    }
    return true;
}

// ================================================================
//  IsGLReady
// ================================================================
static bool IsGLReady() {
    while (glGetError() != GL_NO_ERROR) {}

    GLint major = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    if (glGetError() != GL_NO_ERROR || major < 2) return false;

    GLint viewport[4] = {0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (glGetError() != GL_NO_ERROR || viewport[2] <= 0 || viewport[3] <= 0)
        return false;

    return true;
}

// ================================================================
//  ApplyWatchdog
// ================================================================
static inline void ApplyWatchdog() {
    void* inst = g_BalanceInstance;
    if (!inst || !IsValidPtr(inst)) return;
    if (SWITCH::InfiniteMoney   && set_SoftMoney) set_SoftMoney(inst, 0x7FFFFFFF);
    if (SWITCH::InfinitePremium && set_HardMoney) set_HardMoney(inst, 0x7FFFFFFF);
    if (SWITCH::MaxLevel        && set_Level)     set_Level(inst, 99);
    if (SWITCH::MaxLevel        && set_Exp)       set_Exp(inst, 0x7FFFFFFF);
    if (SWITCH::NoAds           && set_NoAds)     set_NoAds(inst, true);
}

// ================================================================
//  HOOKs
// ================================================================
void capture_set_SoftMoney(void* instance, long value) {
    if (instance && IsValidPtr(instance) && !g_BalanceInstance) {
        g_BalanceInstance = instance;
        LOGI(OBFUSCATE("ThrowIO: balance instance captured -> %p"), instance);
    }
    if (orig_set_SoftMoney) orig_set_SoftMoney(instance, value);
}

void hook_ApplyDamage(void* instance, long damage, void* from, bool isCritical, void* extra) {
    if (SWITCH::GodMode && instance) return;
    if (old_ApplyDamage) old_ApplyDamage(instance, damage, from, isCritical, extra);
}

void hook_SetDeath(void* instance, bool isDead) {
    if (SWITCH::GodMode && instance) isDead = false;
    if (old_SetDeath) old_SetDeath(instance, isDead);
}

void hook_CharWeapon_update(void* instance, float deltaTime) {
    if (SWITCH::SpeedHack && instance) deltaTime *= speedMultiplier;
    if (old_CharWeapon_update) old_CharWeapon_update(instance, deltaTime);
}

void hook_SaveLocal(void* instance) {
    if (SWITCH::AntiCheat) ApplyWatchdog();
    if (old_SaveLocal) old_SaveLocal(instance);
    if (SWITCH::AntiCheat) ApplyWatchdog();
}

// ================================================================
//  ImGui Style
// ================================================================
static void ApplyImGuiStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding    = 10.0f;
    st.FrameRounding     =  5.0f;
    st.ScrollbarRounding =  5.0f;
    st.GrabRounding      =  4.0f;
    st.WindowPadding     = ImVec2(12, 12);
    st.ItemSpacing       = ImVec2( 8,  6);
    st.Colors[ImGuiCol_WindowBg]       = ImVec4(0.04f, 0.04f, 0.07f, 0.97f);
    st.Colors[ImGuiCol_TitleBg]        = ImVec4(0.0f,  0.12f, 0.30f, 1.0f);
    st.Colors[ImGuiCol_TitleBgActive]  = ImVec4(0.0f,  0.20f, 0.50f, 1.0f);
    st.Colors[ImGuiCol_FrameBg]        = ImVec4(0.10f, 0.10f, 0.16f, 1.0f);
    st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.15f, 0.25f, 1.0f);
    st.Colors[ImGuiCol_CheckMark]      = ImVec4(0.0f,  0.90f, 1.0f,  1.0f);
    st.Colors[ImGuiCol_SliderGrab]     = ImVec4(0.0f,  0.70f, 1.0f,  1.0f);
    st.Colors[ImGuiCol_Button]         = ImVec4(0.0f,  0.25f, 0.55f, 1.0f);
    st.Colors[ImGuiCol_ButtonHovered]  = ImVec4(0.0f,  0.40f, 0.80f, 1.0f);
    st.Colors[ImGuiCol_ButtonActive]   = ImVec4(0.0f,  0.60f, 1.0f,  1.0f);
    st.Colors[ImGuiCol_Header]         = ImVec4(0.0f,  0.30f, 0.60f, 0.8f);
    st.Colors[ImGuiCol_Separator]      = ImVec4(0.20f, 0.20f, 0.30f, 1.0f);
}

// ================================================================
//  [FIX v4] SafeSetupImGui — giờ có SignalGuard cho render thread
//  Bug cũ: SetupImGui() gọi từ TID 17540 không có bảo vệ gì
//  Fix: wrap SetupImGui() bằng SignalGuard thread-local
// ================================================================
static bool SafeSetupImGui(EGLDisplay dpy, EGLSurface surface) {
    if (!IsEGLContextCurrent()) {
        LOGE(OBFUSCATE("ThrowIO: SafeSetupImGui — no valid EGL context"));
        return false;
    }
    if (!IsGLReady()) {
        LOGE(OBFUSCATE("ThrowIO: SafeSetupImGui — GL not ready"));
        return false;
    }

    // [FIX v4] Extra check: surface dimension từ EGL query
    EGLint w = 0, h = 0;
    if (eglQuerySurface(dpy, surface, EGL_WIDTH,  &w) != EGL_TRUE ||
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &h) != EGL_TRUE ||
        w <= 0 || h <= 0) {
        LOGE(OBFUSCATE("ThrowIO: SafeSetupImGui — surface dim invalid %dx%d"), w, h);
        return false;
    }

    g_savedDisplay = eglGetCurrentDisplay();
    g_savedContext = eglGetCurrentContext();
    while (glGetError() != GL_NO_ERROR) {}

    // ── [FIX v4] Wrap SetupImGui() với signal guard ────────────────
    // Render thread (TID 17540) giờ tự bảo vệ mình
    bool setupOk = false;
    {
        SignalGuard guard;
        if (!guard.install()) {
            LOGE(OBFUSCATE("ThrowIO: SignalGuard install failed — skip ImGui init"));
            return false;
        }

        if (sigsetjmp(tl_crash_jmp, 1) == 0) {
            // Thử SetupImGui trong guard
            SetupImGui();
            setupOk = true;
        } else {
            // Crash bị bắt ngay trong render thread
            LOGE(OBFUSCATE("ThrowIO: SetupImGui CRASH bị bắt — render thread safe"));
            guard.restore();
            return false;
        }
        guard.restore();
    }

    if (!setupOk) return false;

    ImGuiContext* imCtx = ImGui::GetCurrentContext();
    if (!imCtx) {
        LOGE(OBFUSCATE("ThrowIO: SetupImGui returned null context"));
        return false;
    }

    GLenum glErr = glGetError();
    if (glErr != GL_NO_ERROR) {
        LOGE(OBFUSCATE("ThrowIO: SetupImGui GL error 0x%x"), glErr);
        return false;
    }

    ApplyImGuiStyle();
    LOGI(OBFUSCATE("ThrowIO: ImGui setup OK ctx=%p dpy=%p dim=%dx%d"),
         g_savedContext, g_savedDisplay, w, h);
    return true;
}

// ================================================================
//  HOOK: eglSwapBuffers
// ================================================================
EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (!old_eglSwapBuffers) return EGL_FALSE;

    if (dpy == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE)
        return old_eglSwapBuffers(dpy, surface);
    if (eglGetCurrentContext() == EGL_NO_CONTEXT)
        return old_eglSwapBuffers(dpy, surface);

    EGLint w = 0, h = 0;
    if (eglQuerySurface(dpy, surface, EGL_WIDTH,  &w) != EGL_TRUE ||
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &h) != EGL_TRUE)
        return old_eglSwapBuffers(dpy, surface);
    if (w <= 0 || h <= 0) return old_eglSwapBuffers(dpy, surface);

    glWidth  = w;
    glHeight = h;

    int frame = g_frameCount.fetch_add(1, std::memory_order_relaxed);

    // [FIX v4] Tăng 90 → 150 — cho EGL context stable hơn trước khi setup ImGui
    if (frame < 150) return old_eglSwapBuffers(dpy, surface);

    if (g_imguiFailed.load(std::memory_order_acquire)) {
        int fails = g_failCount.load(std::memory_order_relaxed);
        int retryInterval = (fails < 3) ? 90 : (fails < 6) ? 150 : 240;
        if (frame % retryInterval != 0) return old_eglSwapBuffers(dpy, surface);
        LOGI(OBFUSCATE("ThrowIO: retry ImGui — frame=%d fail#%d"), frame, fails);
        g_imguiFailed.store(false, std::memory_order_release);
        g_imguiSetup.store(false,  std::memory_order_release);
        g_eglReady.store(false,    std::memory_order_release);
    }

    if (!g_imguiSetup.load(std::memory_order_acquire)) {
        bool ok = SafeSetupImGui(dpy, surface);
        if (ok) {
            g_failCount.store(0, std::memory_order_relaxed);
            g_imguiSetup.store(true, std::memory_order_release);
            g_eglReady.store(true,   std::memory_order_release);
        } else {
            g_failCount.fetch_add(1, std::memory_order_relaxed);
            g_imguiFailed.store(true, std::memory_order_release);
            return old_eglSwapBuffers(dpy, surface);
        }
    }

    if (!g_eglReady.load(std::memory_order_acquire))
        return old_eglSwapBuffers(dpy, surface);

    if (!IsEGLContextCurrent()) {
        LOGE(OBFUSCATE("ThrowIO: EGL context lost frame=%d — reset"), frame);
        g_imguiSetup.store(false, std::memory_order_release);
        g_eglReady.store(false,   std::memory_order_release);
        g_savedContext = EGL_NO_CONTEXT;
        g_savedDisplay = EGL_NO_DISPLAY;
        return old_eglSwapBuffers(dpy, surface);
    }

    ImGuiContext* imCtx = ImGui::GetCurrentContext();
    if (!imCtx) {
        LOGE(OBFUSCATE("ThrowIO: ImGui ctx null — reset"));
        g_imguiSetup.store(false, std::memory_order_release);
        g_eglReady.store(false,   std::memory_order_release);
        return old_eglSwapBuffers(dpy, surface);
    }

    if (g_hooksReady.load(std::memory_order_acquire)) ApplyWatchdog();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    // ================================================================
    //  DRAW MENU
    // ================================================================
    ImGui::SetNextWindowSize(ImVec2(370, 540), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos (ImVec2(10,  10),  ImGuiCond_FirstUseEver);
    ImGui::Begin(OBFUSCATE("THROWIO MOD - AXIOM"), nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

    ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f),
                       OBFUSCATE("  AXIOM DEVELOPMENT"));
    ImGui::Separator();
    ImGui::Spacing();

    bool connected = (g_BalanceInstance != nullptr);
    bool hooksLive = g_hooksReady.load(std::memory_order_acquire);

    ImGui::TextColored(
        hooksLive ? ImVec4(0.2f,1.0f,0.2f,1.0f) : ImVec4(1.0f,0.85f,0.0f,1.0f),
        hooksLive ? OBFUSCATE("  [OK] Hooks Live")
                  : OBFUSCATE("  [..] Dang Hook — cho chut...")
    );
    ImGui::TextColored(
        connected ? ImVec4(0.2f,1.0f,0.2f,1.0f) : ImVec4(1.0f,0.35f,0.35f,1.0f),
        connected ? OBFUSCATE("  [OK] Instance Bat Duoc")
                  : OBFUSCATE("  [..] Chua Co Instance — vao man choi")
    );

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    ImGui::TextColored(ImVec4(1.0f,0.75f,0.0f,1.0f), OBFUSCATE(" TIEN TE"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("Tien Mem Vo Han"),       &SWITCH::InfiniteMoney);
    ImGui::Checkbox(OBFUSCATE("Tien Premium Vo Han"),   &SWITCH::InfinitePremium);
    ImGui::Checkbox(OBFUSCATE("Bo Quang Cao (No Ads)"), &SWITCH::NoAds);
    ImGui::Spacing();
    if (ImGui::Button(OBFUSCATE("Cap Nhat Tien Ngay"), ImVec2(-1.0f, 36.0f))) {
        void* inst = g_BalanceInstance;
        if (inst && IsValidPtr(inst)) {
            if (set_SoftMoney) set_SoftMoney(inst, 0x7FFFFFFF);
            if (set_HardMoney) set_HardMoney(inst, 0x7FFFFFFF);
        }
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    ImGui::TextColored(ImVec4(1.0f,0.75f,0.0f,1.0f), OBFUSCATE(" TIEN TRINH"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("Max Level 99"), &SWITCH::MaxLevel);
    ImGui::Spacing();
    if (ImGui::Button(OBFUSCATE("Len Cap Ngay"), ImVec2(-1.0f, 36.0f))) {
        void* inst = g_BalanceInstance;
        if (inst && IsValidPtr(inst)) {
            if (set_Level) set_Level(inst, 99);
            if (set_Exp)   set_Exp  (inst, 0x7FFFFFFF);
        }
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    ImGui::TextColored(ImVec4(1.0f,0.75f,0.0f,1.0f), OBFUSCATE(" CHIEN DAU"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("God Mode (Bat Tu)"), &SWITCH::GodMode);
    ImGui::Checkbox(OBFUSCATE("Speed Hack"),        &SWITCH::SpeedHack);
    if (SWITCH::SpeedHack) {
        ImGui::Spacing();
        ImGui::SliderFloat(OBFUSCATE("Toc Do x"), &speedMultiplier, 1.0f, 5.0f);
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    ImGui::TextColored(ImVec4(1.0f,0.75f,0.0f,1.0f), OBFUSCATE(" BAO MAT"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("Bypass Anti-Cheat"), &SWITCH::AntiCheat);

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.4f,0.4f,0.4f,1.0f),
        OBFUSCATE("frame=%d | inst=%p | retry=%d"),
        frame, g_BalanceInstance,
        g_failCount.load(std::memory_order_relaxed)
    );

    ImGui::End();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    return old_eglSwapBuffers(dpy, surface);
}

// ================================================================
//  JNI_OnLoad
// ================================================================
extern "C"
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void* reserved) {
    JNIEnv* globalEnv = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&globalEnv), JNI_VERSION_1_6) != JNI_OK) {
        LOGE(OBFUSCATE("ThrowIO: JNI_OnLoad GetEnv failed"));
        return JNI_VERSION_1_6;
    }

    UnityPlayer_cls = globalEnv->FindClass(OBFUSCATE("com/unity3d/player/UnityPlayer"));
    if (!UnityPlayer_cls) {
        LOGE(OBFUSCATE("ThrowIO: UnityPlayer class not found"));
        return JNI_VERSION_1_6;
    }

    UnityPlayer_CurrentActivity_fid = globalEnv->GetStaticFieldID(
        UnityPlayer_cls,
        OBFUSCATE("currentActivity"),
        OBFUSCATE("Landroid/app/Activity;")
    );

    DobbyHook(
        reinterpret_cast<void*>(globalEnv->functions->RegisterNatives),
        reinterpret_cast<void*>(hook_RegisterNatives),
        reinterpret_cast<void**>(&old_RegisterNatives)
    );

    LOGI(OBFUSCATE("ThrowIO: JNI_OnLoad OK"));
    return JNI_VERSION_1_6;
}

// ================================================================
//  WaitForIl2CppDomainReady
//  [FIX v4] RTLD_NOW|RTLD_NOLOAD → chỉ RTLD_NOLOAD
//  RTLD_NOW trên lib đã load là redundant và có thể gây side effect
// ================================================================
static bool WaitForIl2CppDomainReady(uintptr_t baseAddr, int maxWaitMs = 5000) {
    typedef void* (*domain_get_fn)();

    // [FIX] Chỉ RTLD_NOLOAD — không relocate lại
    void* libHandle = dlopen(OBFUSCATE("libil2cpp.so"), RTLD_NOLOAD);
    if (!libHandle) {
        usleep(static_cast<useconds_t>(maxWaitMs) * 1000);
        return true;
    }

    auto domain_get = reinterpret_cast<domain_get_fn>(
        dlsym(libHandle, OBFUSCATE("il2cpp_domain_get")));

    if (!domain_get) {
        dlclose(libHandle);
        usleep(static_cast<useconds_t>(maxWaitMs) * 1000);
        return true;
    }

    int elapsed = 0;
    const int step = 100;
    while (elapsed < maxWaitMs) {
        void* domain = domain_get();
        if (domain && IsValidPtr(domain)) {
            LOGI(OBFUSCATE("ThrowIO: il2cpp domain ptr ready @ %p (waited %dms)"),
                 domain, elapsed);
            dlclose(libHandle);
            return true;
        }
        usleep(step * 1000);
        elapsed += step;
    }

    LOGE(OBFUSCATE("ThrowIO: domain NOT ready after %dms"), maxWaitMs);
    dlclose(libHandle);
    return false;
}

// ================================================================
//  HACK THREAD — [FIX v4] SignalGuard đúng cách
// ================================================================
void* hack_thread(void*) {
    do { sleep(1); } while (!isLibraryLoaded(targetLibName));
    address = findLibrary(targetLibName);
    LOGI(OBFUSCATE("ThrowIO: il2cpp detected @ %p"),
         reinterpret_cast<void*>(address));

    usleep(1500000);

    WaitForIl2CppDomainReady(address, 3000);

    LOGI(OBFUSCATE("ThrowIO: domain ptr valid — waiting 5s for assemblies populate..."));
    usleep(5000000);
    LOGI(OBFUSCATE("ThrowIO: assemblies buffer done — proceeding to BNM attach"));

    bool attached = false;

    for (int attempt = 0; attempt < 15 && !attached; attempt++) {
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // ── [FIX v4] Dùng SignalGuard thay vì manual sigaction ───────
        // Thread-local jmp_buf — không cross-thread nữa
        SignalGuard guard;
        if (!guard.install()) {
            LOGE(OBFUSCATE("ThrowIO: guard install fail attempt %d"), attempt);
            usleep(1000000);
            continue;
        }

        bool crashed = false;
        if (sigsetjmp(tl_crash_jmp, 1) == 0) {
            LOGI(OBFUSCATE("ThrowIO: BNM attach attempt %d..."), attempt);
            AttachIl2Cpp();

            auto probe = getClass(OBFUSCATE("PlayerBalance"), OBFUSCATE("ThrowIO"));
            if (probe) {
                attached = true;
                LOGI(OBFUSCATE("ThrowIO: BNM attached OK — attempt %d"), attempt);
            } else {
                LOGE(OBFUSCATE("ThrowIO: probe null attempt %d"), attempt);
                DetachIl2Cpp();
            }
        } else {
            crashed = true;
            LOGE(OBFUSCATE("ThrowIO: CRASH CAUGHT attempt %d — retry"), attempt);
            DetachIl2Cpp();
        }

        guard.restore();

        if (!attached) {
            int delayUs = attempt < 3  ? 1000000 :
                          attempt < 7  ? 2000000 :
                                         3000000;
            LOGI(OBFUSCATE("ThrowIO: retry in %dms"), delayUs / 1000);
            usleep(delayUs);
        }
    }

    if (!attached) {
        LOGE(OBFUSCATE("ThrowIO: FATAL — BNM attach failed after 15 attempts"));
        return nullptr;
    }

    // ================================================================
    //  BNM: OFFSETS
    // ================================================================
    Menu::Screen_get_height = reinterpret_cast<int(*)()>(
        OBFBNM("UnityEngine", "Screen", "get_height", 0));
    Menu::Screen_get_width  = reinterpret_cast<int(*)()>(
        OBFBNM("UnityEngine", "Screen", "get_width",  0));

    BNM::LoadClass balanceClass    = getClass(OBFUSCATE("PlayerBalance"), OBFUSCATE("ThrowIO"));
    BNM::LoadClass charClass       = getClass(OBFUSCATE("Character"),     OBFUSCATE("ThrowIO"));
    BNM::LoadClass playerDataClass = getClass(OBFUSCATE("PlayerData"),    OBFUSCATE("ThrowIO"));
    BNM::LoadClass charWeaponClass = getClass(OBFUSCATE("CharWeapon"),    OBFUSCATE("ThrowIO"));

    LOGI(OBFUSCATE("ThrowIO: classes — balance=%d char=%d pdata=%d cweapon=%d"),
         (bool)balanceClass, (bool)charClass,
         (bool)playerDataClass, (bool)charWeaponClass);

    auto safe_offset = [](BNM::LoadClass cls, const char* name) -> uintptr_t {
        if (!cls) { LOGE(OBFUSCATE("ThrowIO: null class [%s]"), name); return 0; }
        uintptr_t off = getOffset(cls, name, 0);
        if (!off) LOGE(OBFUSCATE("ThrowIO: offset=0 [%s]"), name);
        else      LOGI(OBFUSCATE("ThrowIO: [%s] = 0x%lx"), name, (unsigned long)off);
        return off;
    };

    auto off_setSoftMoney = safe_offset(balanceClass,    OBFUSCATE("set_SoftMoney"));
    auto off_setHardMoney = safe_offset(balanceClass,    OBFUSCATE("set_HardMoney"));
    auto off_setLevel     = safe_offset(balanceClass,    OBFUSCATE("set_Level"));
    auto off_setExp       = safe_offset(balanceClass,    OBFUSCATE("set_Exp"));
    auto off_setNoAds     = safe_offset(balanceClass,    OBFUSCATE("set_NoAds"));
    auto off_getSoftMoney = safe_offset(balanceClass,    OBFUSCATE("get_SoftMoney"));
    auto off_getHardMoney = safe_offset(balanceClass,    OBFUSCATE("get_HardMoney"));
    auto off_getLevel     = safe_offset(balanceClass,    OBFUSCATE("get_Level"));
    auto off_applyDamage  = safe_offset(charClass,       OBFUSCATE("ApplyDamage"));
    auto off_setDeath     = safe_offset(charClass,       OBFUSCATE("SetDeath"));
    auto off_cwUpdate     = safe_offset(charWeaponClass, OBFUSCATE("update"));
    auto off_saveLocal    = safe_offset(playerDataClass, OBFUSCATE("SaveLocal"));

    if (off_setSoftMoney) AddPointer(set_SoftMoney, off_setSoftMoney);
    if (off_setHardMoney) AddPointer(set_HardMoney, off_setHardMoney);
    if (off_setLevel)     AddPointer(set_Level,     off_setLevel);
    if (off_setExp)       AddPointer(set_Exp,       off_setExp);
    if (off_setNoAds)     AddPointer(set_NoAds,     off_setNoAds);
    if (off_getSoftMoney) AddPointer(get_SoftMoney, off_getSoftMoney);
    if (off_getHardMoney) AddPointer(get_HardMoney, off_getHardMoney);
    if (off_getLevel)     AddPointer(get_Level,     off_getLevel);

    DetachIl2Cpp();
    std::atomic_thread_fence(std::memory_order_seq_cst);

    if (off_setSoftMoney) DHK(off_setSoftMoney, capture_set_SoftMoney,  orig_set_SoftMoney);
    if (off_applyDamage)  DHK(off_applyDamage,  hook_ApplyDamage,       old_ApplyDamage);
    if (off_setDeath)     DHK(off_setDeath,      hook_SetDeath,          old_SetDeath);
    if (off_cwUpdate)     DHK(off_cwUpdate,      hook_CharWeapon_update, old_CharWeapon_update);
    if (off_saveLocal)    DHK(off_saveLocal,      hook_SaveLocal,         old_SaveLocal);

    g_hooksReady.store(true, std::memory_order_release);
    LOGI(OBFUSCATE("ThrowIO: fuck yeah — all hooks live, AXIOM MOD READY boss man"));
    return nullptr;
}

// ================================================================
//  ENTRY POINT
// ================================================================
__attribute__((constructor))
void lib_main() {
    InitCrashLogger();
    LOGI(OBFUSCATE("ThrowIO: lib_main start"));

    void* eglhandle = nullptr;
    for (int retry = 0; retry < 30 && !eglhandle; retry++) {
        eglhandle = dlopen(OBFUSCATE("libEGL.so"), RTLD_NOW | RTLD_GLOBAL);
        if (!eglhandle) {
            LOGE(OBFUSCATE("ThrowIO: libEGL.so retry %d — %s"), retry, dlerror());
            usleep(100000);
        }
    }

    if (!eglhandle) {
        LOGE(OBFUSCATE("ThrowIO: FATAL — libEGL.so unavailable"));
        return;
    }

    void* eglSwapBuffersSym = dlsym(eglhandle, OBFUSCATE("eglSwapBuffers"));
    if (!eglSwapBuffersSym) {
        LOGE(OBFUSCATE("ThrowIO: FATAL — eglSwapBuffers not found: %s"), dlerror());
        return;
    }

    DHK(eglSwapBuffersSym, hook_eglSwapBuffers, old_eglSwapBuffers);
    LOGI(OBFUSCATE("ThrowIO: eglSwapBuffers hooked -> %p"), eglSwapBuffersSym);

    pthread_t ptid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);

    if (pthread_create(&ptid, &attr, hack_thread, nullptr) != 0)
        LOGE(OBFUSCATE("ThrowIO: pthread_create failed"));

    pthread_attr_destroy(&attr);
    LOGI(OBFUSCATE("ThrowIO: lib_main done — thread launched"));
}
