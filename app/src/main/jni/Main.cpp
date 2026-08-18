#include "MainHeader.h"

// ================================================================
//  THROWIO MOD - AXIOM DEVELOPMENT
// ================================================================

// ================================================================
//  FUNCTION POINTERS
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

// Capture instance
void (*orig_set_SoftMoney)(void* instance, long value);

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

// eglSwapBuffers — đúng signature, KHÔNG variadic
EGLBoolean (*old_eglSwapBuffers)(EGLDisplay, EGLSurface);

// ================================================================
//  HOOK: Capture g_BalanceInstance từ set_SoftMoney
// ================================================================
void capture_set_SoftMoney(void* instance, long value) {
    if (instance && !g_BalanceInstance) {
        g_BalanceInstance = instance;
        LOGI(OBFUSCATE("ThrowIO: g_BalanceInstance captured -> %p"), instance);
    }
    if (orig_set_SoftMoney) orig_set_SoftMoney(instance, value);
}

// ================================================================
//  HOOK: ApplyDamage — God Mode
// ================================================================
void hook_ApplyDamage(void* instance, long damage, void* from, bool isCritical, void* extra) {
    if (SWITCH::GodMode && instance != nullptr) {
        return; // Hủy damage hoàn toàn
    }
    if (old_ApplyDamage) old_ApplyDamage(instance, damage, from, isCritical, extra);
}

// ================================================================
//  HOOK: SetDeath — Không cho chết
// ================================================================
void hook_SetDeath(void* instance, bool isDead) {
    if (SWITCH::GodMode && instance != nullptr) {
        isDead = false;
    }
    if (old_SetDeath) old_SetDeath(instance, isDead);
}

// ================================================================
//  HOOK: CharWeapon::update — Speed Hack
// ================================================================
void hook_CharWeapon_update(void* instance, float deltaTime) {
    if (SWITCH::SpeedHack && instance != nullptr) {
        deltaTime *= speedMultiplier;
    }
    if (old_CharWeapon_update) old_CharWeapon_update(instance, deltaTime);
}

// ================================================================
//  HOOK: PlayerData::SaveLocal — Anti-Cheat Bypass
// ================================================================
void hook_SaveLocal(void* instance) {
    if (SWITCH::AntiCheat && g_BalanceInstance != nullptr) {
        if (SWITCH::InfiniteMoney   && set_SoftMoney) set_SoftMoney(g_BalanceInstance, 0x7FFFFFFF);
        if (SWITCH::InfinitePremium && set_HardMoney) set_HardMoney(g_BalanceInstance, 0x7FFFFFFF);
        if (SWITCH::MaxLevel        && set_Level)     set_Level    (g_BalanceInstance, 99);
        if (SWITCH::MaxLevel        && set_Exp)       set_Exp      (g_BalanceInstance, 0x7FFFFFFF);
        if (SWITCH::NoAds           && set_NoAds)     set_NoAds    (g_BalanceInstance, true);
    }

    if (old_SaveLocal) old_SaveLocal(instance);

    if (SWITCH::AntiCheat && g_BalanceInstance != nullptr) {
        if (SWITCH::InfiniteMoney   && set_SoftMoney) set_SoftMoney(g_BalanceInstance, 0x7FFFFFFF);
        if (SWITCH::InfinitePremium && set_HardMoney) set_HardMoney(g_BalanceInstance, 0x7FFFFFFF);
        if (SWITCH::MaxLevel        && set_Level)     set_Level    (g_BalanceInstance, 99);
    }
}

// ================================================================
//  HOOK: eglSwapBuffers — Vẽ ImGui Menu
// ================================================================
EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    eglQuerySurface(dpy, surface, EGL_WIDTH,  &glWidth);
    eglQuerySurface(dpy, surface, EGL_HEIGHT, &glHeight);

    if (!setup) {
        SetupImGui();
        setup = true;
    }

    // Watchdog — apply mỗi frame
    if (g_BalanceInstance != nullptr) {
        if (SWITCH::InfiniteMoney   && set_SoftMoney) set_SoftMoney(g_BalanceInstance, 0x7FFFFFFF);
        if (SWITCH::InfinitePremium && set_HardMoney) set_HardMoney(g_BalanceInstance, 0x7FFFFFFF);
        if (SWITCH::MaxLevel        && set_Level)     set_Level    (g_BalanceInstance, 99);
        if (SWITCH::MaxLevel        && set_Exp)       set_Exp      (g_BalanceInstance, 0x7FFFFFFF);
        if (SWITCH::NoAds           && set_NoAds)     set_NoAds    (g_BalanceInstance, true);
    }

    // ── ImGui Frame ───────────────────────────────────────────────
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 8.0f;
    st.FrameRounding  = 4.0f;
    st.Colors[ImGuiCol_WindowBg]      = ImVec4(0.05f, 0.05f, 0.08f, 0.97f);
    st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.0f,  0.2f,  0.5f,  1.0f);
    st.Colors[ImGuiCol_CheckMark]     = ImVec4(0.0f,  0.9f,  1.0f,  1.0f);

    ImGui::SetNextWindowSize(ImVec2(360, 420), ImGuiCond_FirstUseEver);
    ImGui::Begin(OBFUSCATE("THROWIO MOD - AXIOM"));

    ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), OBFUSCATE("AXIOM DEVELOPMENT"));
    ImGui::Separator();

    // Status
    ImGui::TextColored(
        g_BalanceInstance ? ImVec4(0,1,0,1) : ImVec4(1,0.3f,0.3f,1),
        g_BalanceInstance ? OBFUSCATE("Ket noi: OK") : OBFUSCATE("Dang doi... (vao man choi)")
    );

    ImGui::Spacing();

    // ── TIỀN TỆ ──────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), OBFUSCATE("[ TIEN TE ]"));
    ImGui::Checkbox(OBFUSCATE("Tien Mem Vo Han"),       &SWITCH::InfiniteMoney);
    ImGui::Checkbox(OBFUSCATE("Tien Premium Vo Han"),   &SWITCH::InfinitePremium);
    ImGui::Checkbox(OBFUSCATE("Bo Quang Cao (No Ads)"), &SWITCH::NoAds);
    if (ImGui::Button(OBFUSCATE("Cap Nhat Tien Ngay"), ImVec2(-1, 35))) {
        if (g_BalanceInstance) {
            if (set_SoftMoney) set_SoftMoney(g_BalanceInstance, 0x7FFFFFFF);
            if (set_HardMoney) set_HardMoney(g_BalanceInstance, 0x7FFFFFFF);
        }
    }

    ImGui::Spacing();

    // ── TIẾN TRÌNH ────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), OBFUSCATE("[ TIEN TRINH ]"));
    ImGui::Checkbox(OBFUSCATE("Max Level 99"), &SWITCH::MaxLevel);
    if (ImGui::Button(OBFUSCATE("Len Cap Ngay"), ImVec2(-1, 35))) {
        if (g_BalanceInstance) {
            if (set_Level) set_Level(g_BalanceInstance, 99);
            if (set_Exp)   set_Exp  (g_BalanceInstance, 0x7FFFFFFF);
        }
    }

    ImGui::Spacing();

    // ── CHIẾN ĐẤU ────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), OBFUSCATE("[ CHIEN DAU ]"));
    ImGui::Checkbox(OBFUSCATE("God Mode (Bat Tu)"), &SWITCH::GodMode);
    ImGui::Checkbox(OBFUSCATE("Speed Hack"),        &SWITCH::SpeedHack);
    if (SWITCH::SpeedHack)
        ImGui::SliderFloat(OBFUSCATE("Toc Do"), &speedMultiplier, 1.0f, 5.0f);

    ImGui::Spacing();

    // ── BẢO MẬT ──────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), OBFUSCATE("[ BAO MAT ]"));
    ImGui::Checkbox(OBFUSCATE("Bypass Anti-Cheat"), &SWITCH::AntiCheat);

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

    UnityPlayer_cls = globalEnv->FindClass(OBFUSCATE("com/unity3d/player/UnityPlayer"));
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
    // Đợi libil2cpp load xong
    do { sleep(1); } while (!isLibraryLoaded(targetLibName));
    address = findLibrary(targetLibName);

    LOGI(OBFUSCATE("ThrowIO: il2cpp loaded, attaching BNM..."));
    AttachIl2Cpp();

    Menu::Screen_get_height = (int(*)()) OBFBNM("UnityEngine", "Screen", "get_height", 0);
    Menu::Screen_get_width  = (int(*)()) OBFBNM("UnityEngine", "Screen", "get_width",  0);

    // ── Tìm class ──────────────────────────────────────────────
    auto balanceClass    = getClass(OBFUSCATE("PlayerBalance"), OBFUSCATE("ThrowIO"));
    auto charClass       = getClass(OBFUSCATE("Character"),     OBFUSCATE("ThrowIO"));
    auto playerDataClass = getClass(OBFUSCATE("PlayerData"),    OBFUSCATE("ThrowIO"));
    auto charWeaponClass = getClass(OBFUSCATE("CharWeapon"),    OBFUSCATE("ThrowIO"));

    LOGI(OBFUSCATE("Classes: balance=%p char=%p pdata=%p cweapon=%p"),
         balanceClass, charClass, playerDataClass, charWeaponClass);

    // ── Lấy offset — null check trước ──────────────────────────
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

    // ── Gán function pointer ────────────────────────────────────
    if (off_setSoftMoney) AddPointer(set_SoftMoney, off_setSoftMoney);
    if (off_setHardMoney) AddPointer(set_HardMoney, off_setHardMoney);
    if (off_setLevel)     AddPointer(set_Level,     off_setLevel);
    if (off_setExp)       AddPointer(set_Exp,       off_setExp);
    if (off_setNoAds)     AddPointer(set_NoAds,     off_setNoAds);
    if (off_getSoftMoney) AddPointer(get_SoftMoney, off_getSoftMoney);
    if (off_getHardMoney) AddPointer(get_HardMoney, off_getHardMoney);
    if (off_getLevel)     AddPointer(get_Level,     off_getLevel);

    DetachIl2Cpp();

    // ── DHK — null check hết, KHÔNG DHK(0) ─────────────────────
    // Hook set_SoftMoney để bắt instance
    if (off_setSoftMoney)
        DHK(off_setSoftMoney, capture_set_SoftMoney, orig_set_SoftMoney);

    if (off_applyDamage)
        DHK(off_applyDamage, hook_ApplyDamage, old_ApplyDamage);

    if (off_setDeath)
        DHK(off_setDeath, hook_SetDeath, old_SetDeath);

    if (off_cwUpdate)
        DHK(off_cwUpdate, hook_CharWeapon_update, old_CharWeapon_update);

    if (off_saveLocal)
        DHK(off_saveLocal, hook_SaveLocal, old_SaveLocal);

    LOGI(OBFUSCATE("ThrowIO: All hooks installed!"));
    return nullptr;
}

// ================================================================
//  ENTRY POINT
// ================================================================
__attribute__((constructor))
void lib_main() {
    auto eglhandle      = dlopen(OBFUSCATE("libEGL.so"), RTLD_LAZY);
    auto eglSwapBuffers = dlsym(eglhandle, OBFUSCATE("eglSwapBuffers"));
    DHK(eglSwapBuffers, hook_eglSwapBuffers, old_eglSwapBuffers);

    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}
