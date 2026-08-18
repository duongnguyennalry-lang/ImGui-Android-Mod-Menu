#include "MainHeader.h"

// ================================================================
//  THROWIO MOD - AXIOM DEVELOPMENT
//  Dùng BNM - Không cần hardcode offset, tự tìm bằng tên class
// ================================================================

// ================================================================
//  FUNCTION POINTERS
// ================================================================

// PlayerBalance
void (*set_SoftMoney)(void* instance, long value);
void (*set_HardMoney)(void* instance, long value);
void (*set_Level)(void* instance, int value);
void (*set_Exp)(void* instance, int value);
void (*set_NoAds)(void* instance, bool value);
long (*get_SoftMoney)(void* instance);
long (*get_HardMoney)(void* instance);
int  (*get_Level)(void* instance);

// Character - God mode
void (*old_ApplyDamage)(void* instance, long damage, void* from, bool isCritical, void* extra);
void (*old_SetDeath)(void* instance, bool isDead);

// CharWeapon - Speed hack
void (*old_CharWeapon_update)(void* instance, float deltaTime);

// PlayerData - Anti-cheat bypass
void (*old_SaveLocal)(void* instance);

// ================================================================
//  TOGGLES
// ================================================================
namespace SWITCH {
    bool InfiniteMoney  = false;
    bool InfinitePremium = false;
    bool MaxLevel       = false;
    bool NoAds          = false;
    bool GodMode        = false;
    bool SpeedHack      = false;
    bool AntiCheat      = true;
}

// Cached instances
void* g_BalanceInstance = nullptr;

float speedMultiplier = 2.0f;

// ================================================================
//  HOOK: ApplyDamage - GOD MODE
// ================================================================
void hook_ApplyDamage(void* instance, long damage, void* from, bool isCritical, void* extra) {
    if (SWITCH::GodMode && instance != nullptr) {
        damage = 0; // Hủy toàn bộ damage
        return;
    }
    old_ApplyDamage(instance, damage, from, isCritical, extra);
}

// ================================================================
//  HOOK: SetDeath - Không cho chết
// ================================================================
void hook_SetDeath(void* instance, bool isDead) {
    if (SWITCH::GodMode && instance != nullptr) {
        isDead = false; // Luôn sống
    }
    old_SetDeath(instance, isDead);
}

// ================================================================
//  HOOK: CharWeapon::update - Speed hack
// ================================================================
void hook_CharWeapon_update(void* instance, float deltaTime) {
    if (SWITCH::SpeedHack && instance != nullptr) {
        deltaTime *= speedMultiplier;
    }
    old_CharWeapon_update(instance, deltaTime);
}

// ================================================================
//  HOOK: PlayerData::SaveLocal - Anti-cheat bypass
// ================================================================
void hook_SaveLocal(void* instance) {
    // Apply values trước khi save
    if (SWITCH::AntiCheat && g_BalanceInstance != nullptr) {
        if (SWITCH::InfiniteMoney && set_SoftMoney)
            set_SoftMoney(g_BalanceInstance, 0x7FFFFFFF);
        if (SWITCH::InfinitePremium && set_HardMoney)
            set_HardMoney(g_BalanceInstance, 0x7FFFFFFF);
        if (SWITCH::MaxLevel && set_Level) {
            set_Level(g_BalanceInstance, 99);
            if (set_Exp) set_Exp(g_BalanceInstance, 0x7FFFFFFF);
        }
        if (SWITCH::NoAds && set_NoAds)
            set_NoAds(g_BalanceInstance, true);
    }

    old_SaveLocal(instance);

    // Restore sau khi save
    if (SWITCH::AntiCheat && g_BalanceInstance != nullptr) {
        if (SWITCH::InfiniteMoney && set_SoftMoney)
            set_SoftMoney(g_BalanceInstance, 0x7FFFFFFF);
        if (SWITCH::InfinitePremium && set_HardMoney)
            set_HardMoney(g_BalanceInstance, 0x7FFFFFFF);
        if (SWITCH::MaxLevel && set_Level)
            set_Level(g_BalanceInstance, 99);
    }
}

// ================================================================
//  ImGui MENU
// ================================================================
EGLBoolean (*old_eglSwapBuffers)(...);
EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    eglQuerySurface(dpy, surface, EGL_WIDTH, &glWidth);
    eglQuerySurface(dpy, surface, EGL_HEIGHT, &glHeight);

    if (!setup) {
        SetupImGui();
        setup = true;
    }

    // ================================================================
    //  WATCHDOG - Apply cheats mỗi frame
    // ================================================================
    if (g_BalanceInstance != nullptr) {
        if (SWITCH::InfiniteMoney && set_SoftMoney)
            set_SoftMoney(g_BalanceInstance, 0x7FFFFFFF);
        if (SWITCH::InfinitePremium && set_HardMoney)
            set_HardMoney(g_BalanceInstance, 0x7FFFFFFF);
        if (SWITCH::MaxLevel && set_Level) {
            set_Level(g_BalanceInstance, 99);
            if (set_Exp) set_Exp(g_BalanceInstance, 0x7FFFFFFF);
        }
        if (SWITCH::NoAds && set_NoAds)
            set_NoAds(g_BalanceInstance, true);
    }

    // ================================================================
    //  DRAW MENU
    // ================================================================
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 8.0f;
    st.FrameRounding = 4.0f;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.05f, 0.08f, 0.97f);
    st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.0f, 0.2f, 0.5f, 1.0f);
    st.Colors[ImGuiCol_CheckMark] = ImVec4(0.0f, 0.9f, 1.0f, 1.0f);

    ImGui::SetNextWindowSize(ImVec2(360, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("THROWIO MOD - AXIOM");

    ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "AXIOM DEVELOPMENT");
    ImGui::Separator();

    // Status
    ImGui::TextColored(
        g_BalanceInstance ? ImVec4(0,1,0,1) : ImVec4(1,0,0,1),
        g_BalanceInstance ? "Ket noi: OK" : "Dang doi (vao game)..."
    );

    ImGui::Spacing();

    // ECONOMY
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "[ TIEN TE ]");
    ImGui::Checkbox("Tien Mem Vo Han", &SWITCH::InfiniteMoney);
    ImGui::Checkbox("Tien Premium Vo Han", &SWITCH::InfinitePremium);
    ImGui::Checkbox("Bo Quang Cao (No Ads)", &SWITCH::NoAds);

    if (ImGui::Button("Cap Nhat Tien Ngay", ImVec2(-1, 35))) {
        if (g_BalanceInstance != nullptr) {
            if (set_SoftMoney) set_SoftMoney(g_BalanceInstance, 0x7FFFFFFF);
            if (set_HardMoney) set_HardMoney(g_BalanceInstance, 0x7FFFFFFF);
        }
    }

    ImGui::Spacing();

    // PROGRESSION
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "[ TIEN TRINH ]");
    ImGui::Checkbox("Max Level 99", &SWITCH::MaxLevel);

    if (ImGui::Button("Len Cap Ngay", ImVec2(-1, 35))) {
        if (g_BalanceInstance != nullptr) {
            if (set_Level) set_Level(g_BalanceInstance, 99);
            if (set_Exp) set_Exp(g_BalanceInstance, 0x7FFFFFFF);
        }
    }

    ImGui::Spacing();

    // COMBAT
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "[ CHIEN DAU ]");
    ImGui::Checkbox("God Mode (Bat Tu)", &SWITCH::GodMode);
    ImGui::Checkbox("Speed Hack", &SWITCH::SpeedHack);
    if (SWITCH::SpeedHack) {
        ImGui::SliderFloat("Toc Do", &speedMultiplier, 1.0f, 5.0f);
    }

    ImGui::Spacing();

    // ANTI-CHEAT
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "[ BAO MAT ]");
    ImGui::Checkbox("Bypass Anti-Cheat", &SWITCH::AntiCheat);

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
        (void **)&old_RegisterNatives
    );
    return JNI_VERSION_1_6;
}

// ================================================================
//  HACK THREAD - BNM (Tìm offset bằng tên, không cần hardcode)
// ================================================================
void *hack_thread(void *) {
    // Đợi libil2cpp load
    do { sleep(1); } while (!isLibraryLoaded(targetLibName));
    address = findLibrary(targetLibName);

    LOGI(OBFUSCATE("ThrowIO: lib loaded, attaching BNM..."));

    AttachIl2Cpp();

    // Screen size cho ImGui
    Menu::Screen_get_height = (int (*)()) OBFBNM("UnityEngine", "Screen", "get_height", 0);
    Menu::Screen_get_width  = (int (*)()) OBFBNM("UnityEngine", "Screen", "get_width",  0);

    // ================================================================
    //  TÌM CLASS VÀ OFFSET BẰNG TÊN (BNM)
    // ================================================================

    // PlayerBalance class
    auto balanceClass = getClass(OBFUSCATE("PlayerBalance"), OBFUSCATE("ThrowIO"));

    // Setter methods
    AddPointer(set_SoftMoney,  getOffset(balanceClass, OBFUSCATE("set_SoftMoney")));
    AddPointer(set_HardMoney,  getOffset(balanceClass, OBFUSCATE("set_HardMoney")));
    AddPointer(set_Level,      getOffset(balanceClass, OBFUSCATE("set_Level")));
    AddPointer(set_Exp,        getOffset(balanceClass, OBFUSCATE("set_Exp")));
    AddPointer(set_NoAds,      getOffset(balanceClass, OBFUSCATE("set_NoAds")));

    // Getter methods (để đọc giá trị hiện tại)
    AddPointer(get_SoftMoney,  getOffset(balanceClass, OBFUSCATE("get_SoftMoney")));
    AddPointer(get_HardMoney,  getOffset(balanceClass, OBFUSCATE("get_HardMoney")));
    AddPointer(get_Level,      getOffset(balanceClass, OBFUSCATE("get_Level")));

    // Character class
    auto charClass = getClass(OBFUSCATE("Character"), OBFUSCATE("ThrowIO"));

    // PlayerData class
    auto playerDataClass = getClass(OBFUSCATE("PlayerData"), OBFUSCATE("ThrowIO"));

    // CharWeapon class
    auto charWeaponClass = getClass(OBFUSCATE("CharWeapon"), OBFUSCATE("ThrowIO"));

    DetachIl2Cpp();

    // ================================================================
    //  CÀI HOOKS
    // ================================================================

    // God mode - hook ApplyDamage
    DHK(getOffset(charClass, OBFUSCATE("ApplyDamage")),
        hook_ApplyDamage, old_ApplyDamage);

    // God mode - hook SetDeath
    DHK(getOffset(charClass, OBFUSCATE("SetDeath")),
        hook_SetDeath, old_SetDeath);

    // Speed hack - hook CharWeapon::update
    DHK(getOffset(charWeaponClass, OBFUSCATE("update")),
        hook_CharWeapon_update, old_CharWeapon_update);

    // Anti-cheat bypass - hook SaveLocal
    DHK(getOffset(playerDataClass, OBFUSCATE("SaveLocal")),
        hook_SaveLocal, old_SaveLocal);

    LOGI(OBFUSCATE("ThrowIO: All hooks installed!"));

    // ================================================================
    //  TÌM BALANCE INSTANCE (Dùng PlayerData để lấy balance)
    // ================================================================
    // Chạy thread riêng để tìm instance
    std::thread([&]() {
        while (g_BalanceInstance == nullptr) {
            sleep(2);
            // Instance sẽ được capture từ hook SaveLocal
        }
    }).detach();

    return nullptr;
}

// ================================================================
//  ENTRY POINT
// ================================================================
__attribute__((constructor))
void lib_main() {
    // Hook eglSwapBuffers để vẽ ImGui menu
    auto eglhandle = dlopen(OBFUSCATE("libEGL.so"), RTLD_LAZY);
    auto eglSwapBuffers = dlsym(eglhandle, OBFUSCATE("eglSwapBuffers"));
    DHK(eglSwapBuffers, hook_eglSwapBuffers, old_eglSwapBuffers);

    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}
