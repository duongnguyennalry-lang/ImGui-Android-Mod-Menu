#include "MainHeader.h"
#include "CrashLogger.h" // <-- THÊM VÀO

// ================================================================
//  THROWIO MOD - AXIOM DEVELOPMENT
//  FIX: EGL race condition + CrashLogger integrated
// ================================================================

void (*set_SoftMoney)(void* instance, long value);
void (*set_HardMoney)(void* instance, long value);
void (*set_Level)    (void* instance, int  value);
void (*set_Exp)      (void* instance, int  value);
void (*set_NoAds)    (void* instance, bool value);
long (*get_SoftMoney)(void* instance);
long (*get_HardMoney)(void* instance);
int  (*get_Level)    (void* instance);

void (*old_ApplyDamage)      (void* instance, long damage, void* from, bool isCritical, void* extra);
void (*old_SetDeath)         (void* instance, bool isDead);
void (*old_CharWeapon_update)(void* instance, float deltaTime);
void (*old_SaveLocal)        (void* instance);
void (*orig_set_SoftMoney)   (void* instance, long value);

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

EGLBoolean (*old_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

// ================================================================
//  INLINE HELPERS
// ================================================================
static inline void ApplyWatchdog() {
    if (!g_BalanceInstance) return;
    if (SWITCH::InfiniteMoney   && set_SoftMoney) set_SoftMoney(g_BalanceInstance, 0x7FFFFFFF);
    if (SWITCH::InfinitePremium && set_HardMoney) set_HardMoney(g_BalanceInstance, 0x7FFFFFFF);
    if (SWITCH::MaxLevel        && set_Level)     set_Level    (g_BalanceInstance, 99);
    if (SWITCH::MaxLevel        && set_Exp)       set_Exp      (g_BalanceInstance, 0x7FFFFFFF);
    if (SWITCH::NoAds           && set_NoAds)     set_NoAds    (g_BalanceInstance, true);
}

// ================================================================
//  HOOK: Capture g_BalanceInstance
// ================================================================
void capture_set_SoftMoney(void* instance, long value) {
    if (instance && !g_BalanceInstance) {
        g_BalanceInstance = instance;
        LOGI(OBFUSCATE("ThrowIO: instance captured -> %p"), instance);
    }
    if (orig_set_SoftMoney) orig_set_SoftMoney(instance, value);
}

// ================================================================
//  HOOK: ApplyDamage
// ================================================================
void hook_ApplyDamage(void* instance, long damage, void* from, bool isCritical, void* extra) {
    if (SWITCH::GodMode && instance) return;
    if (old_ApplyDamage) old_ApplyDamage(instance, damage, from, isCritical, extra);
}

// ================================================================
//  HOOK: SetDeath
// ================================================================
void hook_SetDeath(void* instance, bool isDead) {
    if (SWITCH::GodMode && instance) isDead = false;
    if (old_SetDeath) old_SetDeath(instance, isDead);
}

// ================================================================
//  HOOK: CharWeapon::update
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
//  ImGui Style
// ================================================================
static void ApplyImGuiStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding    = 10.0f;
    st.FrameRounding     = 5.0f;
    st.ScrollbarRounding = 5.0f;
    st.GrabRounding      = 4.0f;
    st.WindowPadding     = ImVec2(12, 12);
    st.ItemSpacing       = ImVec2(8, 6);

    st.Colors[ImGuiCol_WindowBg]       = ImVec4(0.04f, 0.04f, 0.07f, 0.97f);
    st.Colors[ImGuiCol_TitleBg]        = ImVec4(0.0f,  0.12f, 0.3f,  1.0f);
    st.Colors[ImGuiCol_TitleBgActive]  = ImVec4(0.0f,  0.2f,  0.5f,  1.0f);
    st.Colors[ImGuiCol_FrameBg]        = ImVec4(0.1f,  0.1f,  0.16f, 1.0f);
    st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.15f, 0.25f, 1.0f);
    st.Colors[ImGuiCol_CheckMark]      = ImVec4(0.0f,  0.9f,  1.0f,  1.0f);
    st.Colors[ImGuiCol_SliderGrab]     = ImVec4(0.0f,  0.7f,  1.0f,  1.0f);
    st.Colors[ImGuiCol_Button]         = ImVec4(0.0f,  0.25f, 0.55f, 1.0f);
    st.Colors[ImGuiCol_ButtonHovered]  = ImVec4(0.0f,  0.4f,  0.8f,  1.0f);
    st.Colors[ImGuiCol_ButtonActive]   = ImVec4(0.0f,  0.6f,  1.0f,  1.0f);
    st.Colors[ImGuiCol_Header]         = ImVec4(0.0f,  0.3f,  0.6f,  0.8f);
    st.Colors[ImGuiCol_Separator]      = ImVec4(0.2f,  0.2f,  0.3f,  1.0f);
}

// ================================================================
//  HOOK: eglSwapBuffers
// ================================================================
EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (!old_eglSwapBuffers) return EGL_FALSE;

    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surface, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);

    if (w <= 0 || h <= 0) return old_eglSwapBuffers(dpy, surface);

    glWidth  = w;
    glHeight = h;

    int frame = g_frameCount.fetch_add(1);

    if (frame < 8) return old_eglSwapBuffers(dpy, surface);

    if (!g_imguiSetup.load()) {
        SetupImGui();
        ApplyImGuiStyle();
        g_imguiSetup.store(true);
        g_eglReady.store(true);
    }

    if (!g_eglReady.load()) return old_eglSwapBuffers(dpy, surface);

    ApplyWatchdog();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowSize(ImVec2(370, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 10),    ImGuiCond_FirstUseEver);
    ImGui::Begin(OBFUSCATE("THROWIO MOD - AXIOM"), nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

    ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), OBFUSCATE("  AXIOM DEVELOPMENT"));
    ImGui::Separator();
    ImGui::Spacing();

    bool connected = (g_BalanceInstance != nullptr);
    ImGui::TextColored(
        connected ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f) : ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
        connected ? OBFUSCATE("  [OK] Connected")
                  : OBFUSCATE("  [..] Waiting — vao man choi")
    );
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── TIEN TE ────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.0f, 1.0f), OBFUSCATE(" TIEN TE"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("Tien Mem Vo Han"),       &SWITCH::InfiniteMoney);
    ImGui::Checkbox(OBFUSCATE("Tien Premium Vo Han"),   &SWITCH::InfinitePremium);
    ImGui::Checkbox(OBFUSCATE("Bo Quang Cao (No Ads)"), &SWITCH::NoAds);
    ImGui::Spacing();
    if (ImGui::Button(OBFUSCATE("Cap Nhat Tien Ngay"), ImVec2(-1.0f, 36.0f))) {
        if (g_BalanceInstance) {
            if (set_SoftMoney) set_SoftMoney(g_BalanceInstance, 0x7FFFFFFF);
            if (set_HardMoney) set_HardMoney(g_BalanceInstance, 0x7FFFFFFF);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── TIEN TRINH ─────────────────────────────────────────────
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.0f, 1.0f), OBFUSCATE(" TIEN TRINH"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("Max Level 99"), &SWITCH::MaxLevel);
    ImGui::Spacing();
    if (ImGui::Button(OBFUSCATE("Len Cap Ngay"), ImVec2(-1.0f, 36.0f))) {
        if (g_BalanceInstance) {
            if (set_Level) set_Level(g_BalanceInstance, 99);
            if (set_Exp)   set_Exp  (g_BalanceInstance, 0x7FFFFFFF);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── CHIEN DAU ──────────────────────────────────────────────
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

    // ── BAO MAT ────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.0f, 1.0f), OBFUSCATE(" BAO MAT"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("Bypass Anti-Cheat"), &SWITCH::AntiCheat);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── DEBUG ──────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f),
                       OBFUSCATE("frame: %d | log: /sdcard/ThrowIO_Crash/"), frame);

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
JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *globalEnv;
    vm->GetEnv((void **) &globalEnv, JNI_VERSION_1_6);

    UnityPlayer_cls = globalEnv->FindClass(
        OBFUSCATE("com/unity3d/player/UnityPlayer"));
    UnityPlayer_CurrentActivity_fid = globalEnv->GetStaticFieldID(
        UnityPlayer_cls,
        OBFUSCATE("currentActivity"),
        OBFUSCATE("Landroid/app/Activity;")
    );
    DobbyHook(
        (void*)globalEnv->functions->RegisterNatives,
        (void*)hook_RegisterNatives,
        (void**)&old_RegisterNatives
    );
    return JNI_VERSION_1_6;
}

// ================================================================
//  HACK THREAD
// ================================================================
void *hack_thread(void *) {
    do { sleep(1); } while (!isLibraryLoaded(targetLibName));
    address = findLibrary(targetLibName);

    LOGI(OBFUSCATE("ThrowIO: il2cpp loaded, attaching BNM..."));
    AttachIl2Cpp();

    Menu::Screen_get_height = (int(*)()) OBFBNM("UnityEngine", "Screen", "get_height", 0);
    Menu::Screen_get_width  = (int(*)()) OBFBNM("UnityEngine", "Screen", "get_width",  0);

    auto balanceClass    = getClass(OBFUSCATE("PlayerBalance"), OBFUSCATE("ThrowIO"));
    auto charClass       = getClass(OBFUSCATE("Character"),     OBFUSCATE("ThrowIO"));
    auto playerDataClass = getClass(OBFUSCATE("PlayerData"),    OBFUSCATE("ThrowIO"));
    auto charWeaponClass = getClass(OBFUSCATE("CharWeapon"),    OBFUSCATE("ThrowIO"));

    LOGI(OBFUSCATE("Classes: balance=%p char=%p pdata=%p cweapon=%p"),
         balanceClass, charClass, playerDataClass, charWeaponClass);

    auto off_setSoftMoney = balanceClass    ? getOffset(balanceClass,    OBFUSCATE("set_SoftMoney")) : 0;
    auto off_setHardMoney = balanceClass    ? getOffset(balanceClass,    OBFUSCATE("set_HardMoney")) : 0;
    auto off_setLevel     = balanceClass    ? getOffset(balanceClass,    OBFUSCATE("set_Level"))     : 0;
    auto off_setExp       = balanceClass    ? getOffset(balanceClass,    OBFUSCATE("set_Exp"))       : 0;
    auto off_setNoAds     = balanceClass    ? getOffset(balanceClass,    OBFUSCATE("set_NoAds"))     : 0;
    auto off_getSoftMoney = balanceClass    ? getOffset(balanceClass,    OBFUSCATE("get_SoftMoney")) : 0;
    auto off_getHardMoney = balanceClass    ? getOffset(balanceClass,    OBFUSCATE("get_HardMoney")) : 0;
    auto off_getLevel     = balanceClass    ? getOffset(balanceClass,    OBFUSCATE("get_Level"))     : 0;
    auto off_applyDamage  = charClass       ? getOffset(charClass,       OBFUSCATE("ApplyDamage"))   : 0;
    auto off_setDeath     = charClass       ? getOffset(charClass,       OBFUSCATE("SetDeath"))      : 0;
    auto off_cwUpdate     = charWeaponClass ? getOffset(charWeaponClass, OBFUSCATE("update"))        : 0;
    auto off_saveLocal    = playerDataClass ? getOffset(playerDataClass, OBFUSCATE("SaveLocal"))     : 0;

    LOGI(OBFUSCATE("Offsets: SM=%p HM=%p LV=%p EX=%p AD=%p SD=%p CW=%p SL=%p"),
         (void*)off_setSoftMoney, (void*)off_setHardMoney,
         (void*)off_setLevel,     (void*)off_setExp,
         (void*)off_applyDamage,  (void*)off_setDeath,
         (void*)off_cwUpdate,     (void*)off_saveLocal);

    if (off_setSoftMoney) AddPointer(set_SoftMoney, off_setSoftMoney);
    if (off_setHardMoney) AddPointer(set_HardMoney, off_setHardMoney);
    if (off_setLevel)     AddPointer(set_Level,     off_setLevel);
    if (off_setExp)       AddPointer(set_Exp,       off_setExp);
    if (off_setNoAds)     AddPointer(set_NoAds,     off_setNoAds);
    if (off_getSoftMoney) AddPointer(get_SoftMoney, off_getSoftMoney);
    if (off_getHardMoney) AddPointer(get_HardMoney, off_getHardMoney);
    if (off_getLevel)     AddPointer(get_Level,     off_getLevel);

    DetachIl2Cpp();

    if (off_setSoftMoney) DHK(off_setSoftMoney, capture_set_SoftMoney, orig_set_SoftMoney);
    if (off_applyDamage)  DHK(off_applyDamage,  hook_ApplyDamage,      old_ApplyDamage);
    if (off_setDeath)     DHK(off_setDeath,      hook_SetDeath,         old_SetDeath);
    if (off_cwUpdate)     DHK(off_cwUpdate,      hook_CharWeapon_update,old_CharWeapon_update);
    if (off_saveLocal)    DHK(off_saveLocal,      hook_SaveLocal,        old_SaveLocal);

    LOGI(OBFUSCATE("ThrowIO: All hooks installed!"));
    return nullptr;
}

// ================================================================
//  ENTRY POINT
// ================================================================
__attribute__((constructor))
void lib_main() {
    // ── CRASH LOGGER — PHẢI LÀ DÒNG ĐẦU TIÊN ─────────────────
    InitCrashLogger();

    // ── EGL hook ───────────────────────────────────────────────
    void* eglhandle = nullptr;
    for (int retry = 0; retry < 20 && !eglhandle; retry++) {
        eglhandle = dlopen(OBFUSCATE("libEGL.so"), RTLD_NOW | RTLD_GLOBAL);
        if (!eglhandle) usleep(50000);
    }

    if (!eglhandle) {
        LOGE(OBFUSCATE("ThrowIO: FATAL — libEGL.so not found after retries"));
        return;
    }

    auto eglSwapBuffersSym = dlsym(eglhandle, OBFUSCATE("eglSwapBuffers"));
    if (!eglSwapBuffersSym) {
        LOGE(OBFUSCATE("ThrowIO: FATAL — eglSwapBuffers not resolved"));
        return;
    }

    DHK(eglSwapBuffersSym, hook_eglSwapBuffers, old_eglSwapBuffers);
    LOGI(OBFUSCATE("ThrowIO: eglSwapBuffers hooked -> %p"), eglSwapBuffersSym);

    pthread_t ptid;
    pthread_create(&ptid, nullptr, hack_thread, nullptr);
}
