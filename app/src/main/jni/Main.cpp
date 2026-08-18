#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include "MainHeader.h"
#include "CrashLogger.h"
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <pthread.h>
#include <dlfcn.h>

// ================================================================
//  THROWIO MOD - AXIOM DEVELOPMENT
//  FINAL v4 — BNM::Method<> removed, getOffset+AddPointer only
// ================================================================

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

void (*old_ApplyDamage)      (void*, long, void*, bool, void*) = nullptr;
void (*old_SetDeath)         (void*, bool)  = nullptr;
void (*old_CharWeapon_update)(void*, float) = nullptr;
void (*old_SaveLocal)        (void*)        = nullptr;
void (*orig_set_SoftMoney)   (void*, long)  = nullptr;

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
    bool ForceUpdateMoney = false;
    bool ForceMaxLevel    = false;
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
//  IL2CPP DOMAIN VALIDATION TYPEDEFS
// ================================================================
typedef void*        (*fn_domain_get)();
typedef void**       (*fn_domain_get_assemblies)(void* domain, size_t* count);
typedef void*        (*fn_assembly_get_image)(void* assembly);
typedef const char*  (*fn_image_get_name)(void* image);

// ================================================================
//  MEMORY VALIDATION
// ================================================================
static bool IsValidPtr(const void* ptr, size_t sz = sizeof(void*)) {
    if (!ptr) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < 0x10000ULL)        return false;
    if (addr > 0x7fffffffffffULL) return false;
    if (addr & 0x1)               return false;

    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;

    uintptr_t page    = addr & ~static_cast<uintptr_t>(ps - 1);
    size_t    aligned = (sz + static_cast<size_t>(ps) - 1)
                      & ~static_cast<size_t>(ps - 1);
    if (!aligned) aligned = static_cast<size_t>(ps);

    unsigned char vec = 0;
    return mincore(reinterpret_cast<void*>(page), aligned, &vec) == 0;
}

// ================================================================
//  WAIT FOR IL2CPP ASSEMBLIES — probe domain trực tiếp
// ================================================================
static bool ValidateAssemblyEntry(
    fn_assembly_get_image agf,
    fn_image_get_name     igf,
    void* asm_)
{
    if (!IsValidPtr(asm_)) return false;
    void* img = agf(asm_);
    if (!IsValidPtr(img)) return false;
    const char* name = igf(img);
    if (!IsValidPtr((void*)name, 4)) return false;
    return name[0] != '\0';
}

static bool WaitForAssembliesReady(int maxWaitMs = 20000) {
    void* lib = dlopen("libil2cpp.so", RTLD_NOLOAD);
    if (!lib) { usleep(8000000); return true; }

    auto domain_get  = (fn_domain_get)          dlsym(lib, "il2cpp_domain_get");
    auto dom_get_asm = (fn_domain_get_assemblies)dlsym(lib, "il2cpp_domain_get_assemblies");
    auto asm_get_img = (fn_assembly_get_image)   dlsym(lib, "il2cpp_assembly_get_image");
    auto img_get_nm  = (fn_image_get_name)       dlsym(lib, "il2cpp_image_get_name");

    if (!domain_get || !dom_get_asm || !asm_get_img || !img_get_nm) {
        dlclose(lib);
        usleep(8000000);
        return true;
    }

    const int step  = 200;
    int elapsed     = 0;
    int stableCount = 0;

    while (elapsed < maxWaitMs) {
        void* domain = domain_get();
        if (!IsValidPtr(domain)) { stableCount = 0; goto retry; }

        {
            size_t count    = 0;
            void** assemblies = dom_get_asm(domain, &count);
            if (!IsValidPtr(assemblies) || count < 10 || count > 4096) {
                stableCount = 0; goto retry;
            }

            bool allValid = true;
            for (size_t i = 0; i < count; i++) {
                if (!ValidateAssemblyEntry(asm_get_img, img_get_nm, assemblies[i])) {
                    allValid = false; break;
                }
            }

            if (allValid) {
                if (++stableCount >= 3) { dlclose(lib); return true; }
            } else {
                stableCount = 0;
            }
        }

        retry:
        usleep(step * 1000);
        elapsed += step;
    }

    dlclose(lib);
    return false;
}

// ================================================================
//  EGL / GL STATE VALIDATORS
// ================================================================
static bool IsEGLContextCurrent() {
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext  ctx = eglGetCurrentContext();
    if (dpy == EGL_NO_DISPLAY || ctx == EGL_NO_CONTEXT) return false;
    if (g_savedContext != EGL_NO_CONTEXT && ctx != g_savedContext) return false;
    return true;
}

static bool IsGLReady() {
    while (glGetError() != GL_NO_ERROR) {}
    GLint major = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    if (glGetError() != GL_NO_ERROR || major < 2) return false;

    GLint vp[4] = {0};
    glGetIntegerv(GL_VIEWPORT, vp);
    if (glGetError() != GL_NO_ERROR || vp[2] <= 0 || vp[3] <= 0) return false;

    glFlush();
    glFinish();
    return glGetError() == GL_NO_ERROR;
}

// ================================================================
//  WATCHDOG
// ================================================================
static inline void ApplyWatchdog() {
    void* inst = g_BalanceInstance;
    if (!inst || !IsValidPtr(inst)) return;
    if (SWITCH::InfiniteMoney   && set_SoftMoney) set_SoftMoney(inst, 0x7FFFFFFF);
    if (SWITCH::InfinitePremium && set_HardMoney) set_HardMoney(inst, 0x7FFFFFFF);
    if (SWITCH::MaxLevel        && set_Level)     set_Level    (inst, 99);
    if (SWITCH::MaxLevel        && set_Exp)       set_Exp      (inst, 0x7FFFFFFF);
    if (SWITCH::NoAds           && set_NoAds)     set_NoAds    (inst, true);
}

// ================================================================
//  HOOKS
// ================================================================
void capture_set_SoftMoney(void* instance, long value) {
    if (instance && IsValidPtr(instance) && !g_BalanceInstance) {
        g_BalanceInstance = instance;
        LOGI(OBFUSCATE("ThrowIO: balance instance -> %p"), instance);
    }
    if (orig_set_SoftMoney) orig_set_SoftMoney(instance, value);
}

void hook_ApplyDamage(void* inst, long dmg, void* from, bool crit, void* extra) {
    if (SWITCH::GodMode && inst) return;
    if (old_ApplyDamage) old_ApplyDamage(inst, dmg, from, crit, extra);
}

void hook_SetDeath(void* inst, bool isDead) {
    if (SWITCH::GodMode && inst) isDead = false;
    if (old_SetDeath) old_SetDeath(inst, isDead);
}

void hook_CharWeapon_update(void* inst, float dt) {
    if (SWITCH::SpeedHack && inst) dt *= speedMultiplier;

    void* balInst = g_BalanceInstance;
    if (balInst && IsValidPtr(balInst)) {
        if (SWITCH::ForceUpdateMoney) {
            if (set_SoftMoney) set_SoftMoney(balInst, 0x7FFFFFFF);
            if (set_HardMoney) set_HardMoney(balInst, 0x7FFFFFFF);
            SWITCH::ForceUpdateMoney = false;
        }
        if (SWITCH::ForceMaxLevel) {
            if (set_Level) set_Level(balInst, 99);
            if (set_Exp)   set_Exp  (balInst, 0x7FFFFFFF);
            SWITCH::ForceMaxLevel = false;
        }
    }

    if (SWITCH::AntiCheat) ApplyWatchdog();
    if (old_CharWeapon_update) old_CharWeapon_update(inst, dt);
}

void hook_SaveLocal(void* inst) {
    if (SWITCH::AntiCheat) ApplyWatchdog();
    if (old_SaveLocal) old_SaveLocal(inst);
    if (SWITCH::AntiCheat) ApplyWatchdog();
}

// ================================================================
//  IMGUI STYLE
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
//  IMGUI INIT — full GL + EGL validation
// ================================================================
static bool SafeSetupImGui(EGLDisplay dpy, EGLSurface surface) {
    if (!IsEGLContextCurrent()) return false;

    EGLint w = 0, h = 0;
    if (eglQuerySurface(dpy, surface, EGL_WIDTH,  &w) != EGL_TRUE ||
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &h) != EGL_TRUE ||
        w <= 0 || h <= 0) return false;

    if (!IsGLReady()) return false;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;

    g_savedDisplay = eglGetCurrentDisplay();
    g_savedContext = eglGetCurrentContext();
    while (glGetError() != GL_NO_ERROR) {}

    SetupImGui();
    if (!ImGui::GetCurrentContext()) return false;

    ApplyImGuiStyle();
    LOGI(OBFUSCATE("ThrowIO: ImGui OK — ctx=%p"), g_savedContext);
    return true;
}

// ================================================================
//  HOOK: eglSwapBuffers
// ================================================================
EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (!old_eglSwapBuffers) return EGL_FALSE;

    if (dpy == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE ||
        eglGetCurrentContext() == EGL_NO_CONTEXT)
        return old_eglSwapBuffers(dpy, surface);

    EGLint w = 0, h = 0;
    if (eglQuerySurface(dpy, surface, EGL_WIDTH,  &w) != EGL_TRUE ||
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &h) != EGL_TRUE ||
        w <= 0 || h <= 0)
        return old_eglSwapBuffers(dpy, surface);

    glWidth  = w;
    glHeight = h;

    int frame = g_frameCount.fetch_add(1, std::memory_order_relaxed);

    // 300 frame đầu = ~5s — đủ cho Unity splash xong
    if (frame < 300) return old_eglSwapBuffers(dpy, surface);

    // Retry với backoff theo số lần fail
    if (g_imguiFailed.load(std::memory_order_acquire)) {
        int fails   = g_failCount.load(std::memory_order_relaxed);
        int retryAt = (fails < 3) ? 120 : (fails < 6) ? 240 : 360;
        if (frame % retryAt != 0) return old_eglSwapBuffers(dpy, surface);
        g_imguiFailed.store(false, std::memory_order_release);
        g_imguiSetup.store(false,  std::memory_order_release);
        g_eglReady.store(false,    std::memory_order_release);
        LOGI(OBFUSCATE("ThrowIO: retry ImGui — frame=%d fails=%d"), frame, fails);
    }

    if (!g_imguiSetup.load(std::memory_order_acquire)) {
        if (SafeSetupImGui(dpy, surface)) {
            g_failCount.store(0,    std::memory_order_relaxed);
            g_imguiSetup.store(true, std::memory_order_release);
            g_eglReady.store(true,   std::memory_order_release);
        } else {
            g_failCount.fetch_add(1, std::memory_order_relaxed);
            g_imguiFailed.store(true, std::memory_order_release);
            return old_eglSwapBuffers(dpy, surface);
        }
    }

    if (!g_eglReady.load(std::memory_order_acquire) || !IsEGLContextCurrent()) {
        g_imguiSetup.store(false, std::memory_order_release);
        g_eglReady.store(false,   std::memory_order_release);
        g_savedContext = EGL_NO_CONTEXT;
        return old_eglSwapBuffers(dpy, surface);
    }

    if (g_hooksReady.load(std::memory_order_acquire)) ApplyWatchdog();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowSize(ImVec2(370, 540), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos (ImVec2(10,  10),  ImGuiCond_FirstUseEver);
    ImGui::Begin(OBFUSCATE("THROWIO MOD - AXIOM"), nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

    ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), OBFUSCATE("  AXIOM DEVELOPMENT"));
    ImGui::Separator(); ImGui::Spacing();

    bool connected = (g_BalanceInstance != nullptr);
    bool hooksLive = g_hooksReady.load(std::memory_order_acquire);

    ImGui::TextColored(
        hooksLive ? ImVec4(0.2f,1.0f,0.2f,1.0f) : ImVec4(1.0f,0.85f,0.0f,1.0f),
        hooksLive ? OBFUSCATE("  [OK] Hooks Live")
                  : OBFUSCATE("  [..] Dang Hook..."));
    ImGui::TextColored(
        connected ? ImVec4(0.2f,1.0f,0.2f,1.0f) : ImVec4(1.0f,0.35f,0.35f,1.0f),
        connected ? OBFUSCATE("  [OK] Instance Bat Duoc")
                  : OBFUSCATE("  [..] Chua Co Instance — vao man choi"));

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    // ── TIEN TE ────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(1.0f,0.75f,0.0f,1.0f), OBFUSCATE(" TIEN TE"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("Tien Mem Vo Han"),       &SWITCH::InfiniteMoney);
    ImGui::Checkbox(OBFUSCATE("Tien Premium Vo Han"),   &SWITCH::InfinitePremium);
    ImGui::Checkbox(OBFUSCATE("Bo Quang Cao (No Ads)"), &SWITCH::NoAds);
    ImGui::Spacing();
    if (ImGui::Button(OBFUSCATE("Cap Nhat Tien Ngay"), ImVec2(-1.0f, 36.0f)))
        SWITCH::ForceUpdateMoney = true;

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    // ── TIEN TRINH ─────────────────────────────────────────────
    ImGui::TextColored(ImVec4(1.0f,0.75f,0.0f,1.0f), OBFUSCATE(" TIEN TRINH"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("Max Level 99"), &SWITCH::MaxLevel);
    ImGui::Spacing();
    if (ImGui::Button(OBFUSCATE("Len Cap Ngay"), ImVec2(-1.0f, 36.0f)))
        SWITCH::ForceMaxLevel = true;

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    // ── CHIEN DAU ──────────────────────────────────────────────
    ImGui::TextColored(ImVec4(1.0f,0.75f,0.0f,1.0f), OBFUSCATE(" CHIEN DAU"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("God Mode (Bat Tu)"), &SWITCH::GodMode);
    ImGui::Checkbox(OBFUSCATE("Speed Hack"),        &SWITCH::SpeedHack);
    if (SWITCH::SpeedHack) {
        ImGui::Spacing();
        ImGui::SliderFloat(OBFUSCATE("Toc Do x"), &speedMultiplier, 1.0f, 5.0f);
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    // ── BAO MAT ────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(1.0f,0.75f,0.0f,1.0f), OBFUSCATE(" BAO MAT"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("Bypass Anti-Cheat"), &SWITCH::AntiCheat);

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    // ── DEBUG ──────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.4f,0.4f,0.4f,1.0f),
        OBFUSCATE("frame:%d | ctx:%p | inst:%p"),
        frame,
        reinterpret_cast<void*>(g_savedContext),
        g_BalanceInstance);

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
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
        return JNI_VERSION_1_6;

    LOGI(OBFUSCATE("ThrowIO: JNI_OnLoad OK"));
    return JNI_VERSION_1_6;
}

// ================================================================
//  HACK THREAD
//  FIX: Không dùng BNM::Method<> — dùng getOffset + AddPointer
//  BNM::Method<> không có trong version này, LoadClass -> void* ambiguous
// ================================================================
void* hack_thread(void*) {
    do { sleep(1); } while (!isLibraryLoaded(targetLibName));
    address = findLibrary(targetLibName);
    LOGI(OBFUSCATE("ThrowIO: il2cpp @ %p"), reinterpret_cast<void*>(address));

    usleep(1500000); // 1.5s buffer — domain init async

    if (!WaitForAssembliesReady(20000)) {
        LOGE(OBFUSCATE("ThrowIO: assemblies never ready — bail"));
        return nullptr;
    }

    // ── Attach dengan retry + class probe validation ───────────
    bool attached = false;
    for (int attempt = 0; attempt < 5 && !attached; attempt++) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        AttachIl2Cpp();

        auto probe = getClass(OBFUSCATE("PlayerBalance"), OBFUSCATE("ThrowIO"));
        if (probe) {
            attached = true;
            LOGI(OBFUSCATE("ThrowIO: BNM attached — attempt %d"), attempt);
        } else {
            LOGE(OBFUSCATE("ThrowIO: attach attempt %d failed"), attempt);
            DetachIl2Cpp();
            usleep(2000000);
        }
    }

    if (!attached) {
        LOGE(OBFUSCATE("ThrowIO: FATAL — attach failed"));
        return nullptr;
    }

    // ── Class lookup ───────────────────────────────────────────
    BNM::LoadClass balanceClass    = getClass(OBFUSCATE("PlayerBalance"), OBFUSCATE("ThrowIO"));
    BNM::LoadClass charClass       = getClass(OBFUSCATE("Character"),     OBFUSCATE("ThrowIO"));
    BNM::LoadClass playerDataClass = getClass(OBFUSCATE("PlayerData"),    OBFUSCATE("ThrowIO"));
    BNM::LoadClass charWeaponClass = getClass(OBFUSCATE("CharWeapon"),    OBFUSCATE("ThrowIO"));

    LOGI(OBFUSCATE("Classes: bal=%p char=%p pdata=%p cwep=%p"),
         balanceClass, charClass, playerDataClass, charWeaponClass);

    // ================================================================
    //  FIX UTAMA: auto generic lambda — tránh ambiguous void* cast
    //  BNM::LoadClass có 2 conversion operator (Il2CppType* & MonoType*)
    //  void* cls → compiler không biết chọn cái nào → 13 lỗi
    //  auto cls  → compiler deduce exact LoadClass type → build clean
    // ================================================================
    auto safe_offset = [](auto cls, const char* name) -> uintptr_t {
        if (!cls) {
            LOGE(OBFUSCATE("ThrowIO: null class for %s"), name);
            return 0;
        }
        auto off = getOffset(cls, name);
        if (!off) LOGE(OBFUSCATE("ThrowIO: offset not found: %s"), name);
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

    LOGI(OBFUSCATE("Off: SM=%p HM=%p LV=%p EX=%p AD=%p SD=%p CW=%p SL=%p"),
         reinterpret_cast<void*>(off_setSoftMoney),
         reinterpret_cast<void*>(off_setHardMoney),
         reinterpret_cast<void*>(off_setLevel),
         reinterpret_cast<void*>(off_setExp),
         reinterpret_cast<void*>(off_applyDamage),
         reinterpret_cast<void*>(off_setDeath),
         reinterpret_cast<void*>(off_cwUpdate),
         reinterpret_cast<void*>(off_saveLocal));

    // ── AddPointer — bind offset ke function pointer ───────────
    if (off_setSoftMoney) AddPointer(set_SoftMoney, off_setSoftMoney);
    if (off_setHardMoney) AddPointer(set_HardMoney, off_setHardMoney);
    if (off_setLevel)     AddPointer(set_Level,     off_setLevel);
    if (off_setExp)       AddPointer(set_Exp,       off_setExp);
    if (off_setNoAds)     AddPointer(set_NoAds,     off_setNoAds);
    if (off_getSoftMoney) AddPointer(get_SoftMoney, off_getSoftMoney);
    if (off_getHardMoney) AddPointer(get_HardMoney, off_getHardMoney);
    if (off_getLevel)     AddPointer(get_Level,     off_getLevel);

    DetachIl2Cpp();

    // Fence — semua pointer writes visible sebelum hook dipasang
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // ── DobbyHook install ──────────────────────────────────────
    if (off_setSoftMoney) DHK(off_setSoftMoney, capture_set_SoftMoney,  orig_set_SoftMoney);
    if (off_applyDamage)  DHK(off_applyDamage,  hook_ApplyDamage,       old_ApplyDamage);
    if (off_setDeath)     DHK(off_setDeath,      hook_SetDeath,          old_SetDeath);
    if (off_cwUpdate)     DHK(off_cwUpdate,      hook_CharWeapon_update, old_CharWeapon_update);
    if (off_saveLocal)    DHK(off_saveLocal,      hook_SaveLocal,         old_SaveLocal);

    g_hooksReady.store(true, std::memory_order_release);
    LOGI(OBFUSCATE("ThrowIO: All hooks live — fuck yeah"));
    return nullptr;
}

// ================================================================
//  ENTRY POINT
// ================================================================
__attribute__((constructor))
void lib_main() {
    InitCrashLogger();
    LOGI(OBFUSCATE("ThrowIO: lib_main start"));

    // EGL dlopen với retry backoff
    void* eglhandle = nullptr;
    for (int retry = 0; retry < 30 && !eglhandle; retry++) {
        eglhandle = dlopen(OBFUSCATE("libEGL.so"), RTLD_NOW | RTLD_GLOBAL);
        if (!eglhandle) {
            LOGE(OBFUSCATE("ThrowIO: EGL retry %d — %s"), retry, dlerror());
            usleep(100000);
        }
    }

    if (!eglhandle) {
        LOGE(OBFUSCATE("ThrowIO: FATAL — libEGL.so unavailable"));
        return;
    }

    void* eglSwapSym = dlsym(eglhandle, OBFUSCATE("eglSwapBuffers"));
    if (!eglSwapSym) {
        LOGE(OBFUSCATE("ThrowIO: FATAL — eglSwapBuffers missing"));
        return;
    }

    DHK(eglSwapSym, hook_eglSwapBuffers, old_eglSwapBuffers);
    LOGI(OBFUSCATE("ThrowIO: eglSwapBuffers hooked -> %p"), eglSwapSym);

    // 4MB stack — BNM class scan cần nhiều stack
    pthread_t      ptid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);

    if (pthread_create(&ptid, &attr, hack_thread, nullptr) != 0)
        LOGE(OBFUSCATE("ThrowIO: pthread_create failed"));

    pthread_attr_destroy(&attr);
    LOGI(OBFUSCATE("ThrowIO: lib_main done"));
}
