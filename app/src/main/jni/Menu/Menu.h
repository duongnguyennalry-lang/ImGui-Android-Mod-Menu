#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES2/gl2platform.h>
#include <fstream>
#include <sstream>
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_android.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include <stdio.h>
#include <android/native_window_jni.h>
#include <list>
#include <vector>
#include <string.h>
#include <pthread.h>
#include <thread>
#include <cstring>
#include <jni.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <dlfcn.h>
#include "ByNameModding/BNM.hpp"
#include "Includes/Custom_Obfuscate.h"
#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.h"
#include "KittyMemory/MemoryPatch.h"
#include "Includes/Dobby/dobby.h"
#define targetLibName OBFUSCATE("libil2cpp.so")
#include "Includes/Macros.h"
#include "Includes/Loader.h"
#include "Includes/JNIStuff.h"
#include "Includes/FileWrapper.h"
#include "Menu/Menu.h"
#include "Color.h"

using namespace BNM;
using namespace Menu;

// ================================================================
//  THROWIO MOD SWITCHES - Trạng thái các chức năng
// ================================================================
namespace SWITCH {
    // TIỀN TỆ
    inline bool InfiniteMoney   = false;
    inline bool InfinitePremium = false;
    inline bool NoAds           = false;

    // TIẾN TRÌNH
    inline bool MaxLevel        = false;

    // CHIẾN ĐẤU
    inline bool GodMode         = false;
    inline bool SpeedHack       = false;

    // BẢO MẬT
    inline bool AntiCheat       = true;
}

// Speed hack multiplier
inline float speedMultiplier = 2.0f;

// Cached balance instance
inline void* g_BalanceInstance = nullptr;
