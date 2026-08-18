#include "MainHeader.h"
#include "CrashLogger.h"

// ================================================================
//  THROWIO MOD - AXIOM DEVELOPMENT
//  VERSION: FINAL — EGL race fix + BNM fence + ImGui safe init
//  All fixes merged, production ready
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

void (*old_ApplyDamage)      (void* instance, long damage, void* from, bool isCritical, void* extra) = nullptr;
void (*old_SetDeath)         (void* instance, bool isDead)      = nullptr;
void (*old_CharWeapon_update)(void* instance, float deltaTime)  = nullptr;
void (*old_SaveLocal)        (void* instance)                   = nullptr;
void (*orig_set_SoftMoney)   (void* instance, long value)       = nullptr;

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

static EGLDisplay        g_savedDisplay = EGL_NO_DISPLAY;
static EGLContext        g_savedContext = EGL_NO_CONTEXT;

EGLBoolean (*old_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

// ================================================================
//  UTIL: Validate EGL context còn live
// ================================================================
static bool IsEGLContextCurrent() {
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext  ctx = eglGetCurrentContext();
    if (dpy == EGL_NO_DISPLAY || ctx == EGL_NO_CONTEXT) return false;
    if (g_savedContext != EGL_NO_CONTEXT && ctx != g_savedContext) {
        LOGE(OBFUSCATE("ThrowIO: EGL context mismatch — saved=%p current=%p"),
             g_savedContext, ctx);
        return false;
    }
    return true;
}

// ================================================================
//  UTIL: Validate pointer bộ nhớ trước khi deref
// ================================================================
static bool IsValidPtr(const void* ptr, size_t sz = 1) {
    if (!ptr) return false;
    unsigned char vec = 0;
    uintptr_t page = reinterpret_cast<uintptr_t>(ptr)
                     & ~static_cast<uintptr_t>(sysconf(_SC_PAGESIZE) - 1);
    return mincore(reinterpret_cast<void*>(page), sz, &vec) == 0;
}

// ================================================================
//  UTIL: ApplyWatchdog — safe local copy tránh race
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
//  HOOK: Capture g_BalanceInstance
// ================================================================
void capture_set_SoftMoney(void* instance, long value) {
    if (instance && IsValidPtr(instance) && !g_BalanceInstance) {
        g_BalanceInstance = instance;
        LOGI(OBFUSCATE("ThrowIO: instance captured -> %p"), instance);
    }
    if (orig_set_SoftMoney) orig_set_SoftMoney(instance, value);
}

// ================================================================
//  HOOK: ApplyDamage — GodMode block
// ================================================================
void hook_ApplyDamage(void* instance, long damage, void* from, bool isCritical, void* extra) {
    if (SWITCH::GodMode && instance) return;
    if (old_ApplyDamage) old_ApplyDamage(instance, damage, from, isCritical, extra);
}

// ================================================================
//  HOOK: SetDeath — GodMode force alive
// ================================================================
void hook_SetDeath(void* instance, bool isDead) {
    if (SWITCH::GodMode && instance) isDead = false;
    if (old_SetDeath) old_SetDeath(instance, isDead);
}

// ================================================================
//  HOOK: CharWeapon::update — SpeedHack
// ================================================================
void hook_CharWeapon_update(void* instance, float deltaTime) {
    if (SWITCH::SpeedHack && instance) deltaTime *= speedMultiplier;
    if (old_CharWeapon_update) old_CharWeapon_update(instance, deltaTime);
}

// ================================================================
//  HOOK: SaveLocal — Anti-Cheat sandwich
// ================================================================
void hook_SaveLocal(void* instance) {
    if (SWITCH::AntiCheat) ApplyWatchdog();
    if (old_SaveLocal) old_SaveLocal(instance);
    if (SWITCH::AntiCheat) ApplyWatchdog();
}

// ================================================================
//  ImGui Style — dark blue theme
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
//  SafeSetupImGui — validate GL state trước khi init
// ================================================================
static bool SafeSetupImGui(EGLDisplay dpy, EGLSurface surface) {
    if (!IsEGLContextCurrent()) {
        LOGE(OBFUSCATE("ThrowIO: SafeSetupImGui — no valid EGL context, skip"));
        return false;
    }

    g_savedDisplay = eglGetCurrentDisplay();
    g_savedContext = eglGetCurrentContext();

    while (glGetError() != GL_NO_ERROR) {}

    SetupImGui();

    GLenum glErr = glGetError();
    if (glErr != GL_NO_ERROR) {
        LOGE(OBFUSCATE("ThrowIO: SetupImGui GL error 0x%x — will retry"), glErr);
        return false;
    }

    ApplyImGuiStyle();
    LOGI(OBFUSCATE("ThrowIO: ImGui setup OK — ctx=%p dpy=%p"), g_savedContext, g_savedDisplay);
    return true;
}

// ================================================================
//  HOOK: eglSwapBuffers — main render + ImGui
// ================================================================
EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (!old_eglSwapBuffers) return EGL_FALSE;

    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surface, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);

    if (w <= 0 || h <= 0) return old_eglSwapBuffers(dpy, surface);

    glWidth  = w;
    glHeight = h;

    int frame = g_frameCount.fetch_add(1, std::memory_order_relaxed);

    if (frame < 30) return old_eglSwapBuffers(dpy, surface);

    if (g_imguiFailed.load(std::memory_order_acquire)) {
        if (frame % 60 != 0) return old_eglSwapBuffers(dpy, surface);
        LOGI(OBFUSCATE("ThrowIO: retrying ImGui setup — frame %d"), frame);
        g_imguiFailed.store(false, std::memory_order_release);
        g_imguiSetup.store(false, std::memory_order_release);
        g_eglReady.store(false,   std::memory_order_release);
    }

    if (!g_imguiSetup.load(std::memory_order_acquire)) {
        bool ok = SafeSetupImGui(dpy, surface);
        if (ok) {
            g_imguiSetup.store(true, std::memory_order_release);
            g_eglReady.store(true,   std::memory_order_release);
        } else {
            g_imguiFailed.store(true, std::memory_order_release);
            return old_eglSwapBuffers(dpy, surface);
        }
    }

    if (!g_eglReady.load(std::memory_order_acquire))
        return old_eglSwapBuffers(dpy, surface);

    if (!IsEGLContextCurrent()) {
        LOGE(OBFUSCATE("ThrowIO: EGL context lost — frame %d, resetting ImGui"), frame);
        g_imguiSetup.store(false, std::memory_order_release);
        g_eglReady.store(false,   std::memory_order_release);
        g_savedContext = EGL_NO_CONTEXT;
        g_savedDisplay = EGL_NO_DISPLAY;
        return old_eglSwapBuffers(dpy, surface);
    }

    if (g_hooksReady.load(std::memory_order_acquire)) {
        ApplyWatchdog();
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowSize(ImVec2(370, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 10),    ImGuiCond_FirstUseEver);
    ImGui::Begin(OBFUSCATE("THROWIO MOD - AXIOM"), nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

    ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f),
                       OBFUSCATE("  AXIOM DEVELOPMENT"));
    ImGui::Separator();
    ImGui::Spacing();

    bool connected = (g_BalanceInstance != nullptr);
    bool hooksLive = g_hooksReady.load(std::memory_order_acquire);

    ImGui::TextColored(
        hooksLive ? ImVec4(0.2f, 1.0f, 0.2f,  1.0f)
                  : ImVec4(1.0f, 0.85f, 0.0f, 1.0f),
        hooksLive ? OBFUSCATE("  [OK] Hooks Live")
                  : OBFUSCATE("  [..] Dang Hook — cho chut...")
    );
    ImGui::TextColored(
        connected ? ImVec4(0.2f, 1.0f, 0.2f,  1.0f)
                  : ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
        connected ? OBFUSCATE("  [OK] Instance Bat Duoc")
                  : OBFUSCATE("  [..] Chua Co Instance — vao man choi")
    );

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.0f, 1.0f), OBFUSCATE(" TIEN TE"));
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

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.0f, 1.0f), OBFUSCATE(" TIEN TRINH"));
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

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.0f, 1.0f), OBFUSCATE(" CHIEN DAU"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("God Mode (Bat Tu)"), &SWITCH::GodMode);
    ImGui::Checkbox(OBFUSCATE("Speed Hack"),        &SWITCH::SpeedHack);
    if (SWITCH::SpeedHack) {
        ImGui::Spacing();
        ImGui::SliderFloat(OBFUSCATE("Toc Do x"), &speedMultiplier, 1.0f, 5.0f);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.0f, 1.0f), OBFUSCATE(" BAO MAT"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("Bypass Anti-Cheat"), &SWITCH::AntiCheat);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f),
        OBFUSCATE("frame: %d | ctx: %p | inst: %p"),
        frame,
        reinterpret_cast<void*>(g_savedContext),
        g_BalanceInstance
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

    UnityPlayer_cls = globalEnv->FindClass(
        OBFUSCATE("com/unity3d/player/UnityPlayer"));
    if (!UnityPlayer_cls) {
        LOGE(OBFUSCATE("ThrowIO: UnityPlayer class not found"));
        return JNI_VERSION_1_6;
    }

    UnityPlayer_CurrentActivity_fid = globalEnv->GetStaticFieldID(
        UnityPlayer_cls,
        OBFUSCATE("currentActivity"),
        OBFUSCATE("Landroid/app/Activity;")
    );
    if (!UnityPlayer_CurrentActivity_fid) {
        LOGE(OBFUSCATE("ThrowIO: currentActivity field not found"));
        return JNI_VERSION_1_6;
    }

    DobbyHook(
        reinterpret_cast<void*>(globalEnv->functions->RegisterNatives),
        reinterpret_cast<void*>(hook_RegisterNatives),
        reinterpret_cast<void**>(&old_RegisterNatives)
    );

    LOGI(OBFUSCATE("ThrowIO: JNI_OnLoad OK"));
    return JNI_VERSION_1_6;
}

// ================================================================
//  HACK THREAD
// ================================================================
void* hack_thread(void*) {
    do { sleep(1); } while (!isLibraryLoaded(targetLibName));
    address = findLibrary(targetLibName);
    LOGI(OBFUSCATE("ThrowIO: il2cpp detected @ %p"), reinterpret_cast<void*>(address));

    usleep(500000);

    bool attached = false;
    for (int attempt = 0; attempt < 10 && !attached; attempt++) {
        std::atomic_thread_fence(std::memory_order_seq_cst);

        AttachIl2Cpp();

        // getClass() xử lý OBFUSCATE bên trong — KHÔNG đụng vào
        auto probe = getClass(OBFUSCATE("PlayerBalance"), OBFUSCATE("ThrowIO"));
        if (probe) {
            attached = true;
            LOGI(OBFUSCATE("ThrowIO: BNM attached — attempt %d"), attempt);
        } else {
            LOGE(OBFUSCATE("ThrowIO: attach attempt %d failed, retry in 300ms"), attempt);
            DetachIl2Cpp();
            usleep(300000);
        }
    }

    if (!attached) {
        LOGE(OBFUSCATE("ThrowIO: FATAL — BNM attach failed after 10 attempts"));
        return nullptr;
    }

    Menu::Screen_get_height = reinterpret_cast<int(*)()>(
        OBFBNM("UnityEngine", "Screen", "get_height", 0));
    Menu::Screen_get_width  = reinterpret_cast<int(*)()>(
        OBFBNM("UnityEngine", "Screen", "get_width",  0));

    // ================================================================
    //  CLASS LOOKUP
    //  Explicit type BNM::LoadClass thay vì auto
    //  getClass() vẫn giữ nguyên — nó handle OBFUSCATE đúng cách
    //  KHÔNG dùng BNM::LoadClass constructor trực tiếp vì OBFUSCATE
    //  trả ay::obfuscated_data không convert sang std::string được
    // ================================================================
    BNM::LoadClass balanceClass    = getClass(OBFUSCATE("PlayerBalance"), OBFUSCATE("ThrowIO"));
    BNM::LoadClass charClass       = getClass(OBFUSCATE("Character"),     OBFUSCATE("ThrowIO"));
    BNM::LoadClass playerDataClass = getClass(OBFUSCATE("PlayerData"),    OBFUSCATE("ThrowIO"));
    BNM::LoadClass charWeaponClass = getClass(OBFUSCATE("CharWeapon"),    OBFUSCATE("ThrowIO"));

    LOGI(OBFUSCATE("Classes: balance=%d char=%d pdata=%d cweapon=%d"),
         (bool)balanceClass,
         (bool)charClass,
         (bool)playerDataClass,
         (bool)charWeaponClass);

    // ================================================================
    //  THE ACTUAL FIX — boss man đây là cái đang xảy ra:
    //
    //  VẤN ĐỀ GỐC:
    //    Lambda nhận void* cls
    //    BNM::LoadClass có HAI operator conversion:
    //      - operator IL2CPP::Il2CppType*()   (line 309)
    //      - operator MonoType*()              (line 310)
    //    Cả hai đều convert sang void* được → compiler không biết chọn cái nào
    //    → error: conversion from 'LoadClass' to 'void *' is ambiguous
    //
    //  FIX:
    //    1. Lambda nhận BNM::LoadClass thẳng — không cần conversion ở call site
    //    2. Bên trong lambda, dùng static_cast<IL2CPP::Il2CppType*> tường minh
    //       → chọn đúng MỘT conversion operator
    //    3. Sau đó cast sang void* để pass vào getOffset(void*, name)
    //    → Không còn ambiguity ở BẤT KỲ điểm nào
    // ================================================================
    auto safe_offset = [](BNM::LoadClass cls, const char* name) -> uintptr_t {
        if (!cls) {
            LOGE(OBFUSCATE("ThrowIO: null class cho [%s]"), name);
            return 0;
        }
        // Tường minh chọn IL2CPP::Il2CppType* — giết ambiguity
        IL2CPP::Il2CppType* typePtr = static_cast<IL2CPP::Il2CppType*>(cls);
        void* clsVoid               = static_cast<void*>(typePtr);
        auto  off                   = getOffset(clsVoid, name);
        if (!off) LOGE(OBFUSCATE("ThrowIO: offset 0 cho [%s], đm"), name);
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

    LOGI(OBFUSCATE("Offsets: SM=%p HM=%p LV=%p EX=%p AD=%p SD=%p CW=%p SL=%p"),
         reinterpret_cast<void*>(off_setSoftMoney),
         reinterpret_cast<void*>(off_setHardMoney),
         reinterpret_cast<void*>(off_setLevel),
         reinterpret_cast<void*>(off_setExp),
         reinterpret_cast<void*>(off_applyDamage),
         reinterpret_cast<void*>(off_setDeath),
         reinterpret_cast<void*>(off_cwUpdate),
         reinterpret_cast<void*>(off_saveLocal));

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
    LOGI(OBFUSCATE("ThrowIO: All hooks installed — that's what the hell is going on"));
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
        LOGE(OBFUSCATE("ThrowIO: FATAL — libEGL.so unavailable after 30 retries"));
        return;
    }

    void* eglSwapBuffersSym = dlsym(eglhandle, OBFUSCATE("eglSwapBuffers"));
    if (!eglSwapBuffersSym) {
        LOGE(OBFUSCATE("ThrowIO: FATAL — eglSwapBuffers symbol missing: %s"), dlerror());
        return;
    }

    DHK(eglSwapBuffersSym, hook_eglSwapBuffers, old_eglSwapBuffers);
    LOGI(OBFUSCATE("ThrowIO: eglSwapBuffers hooked -> %p"), eglSwapBuffersSym);

    pthread_t ptid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);

    if (pthread_create(&ptid, &attr, hack_thread, nullptr) != 0) {
        LOGE(OBFUSCATE("ThrowIO: pthread_create failed"));
    }
    pthread_attr_destroy(&attr);

    LOGI(OBFUSCATE("ThrowIO: lib_main done — thread launched"));
}
