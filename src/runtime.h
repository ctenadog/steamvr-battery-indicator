// SteamVR overlay that shows the headset battery / charging state on your hand.
//
// The overlay is attached to a controller (tracked-device relative), so it hangs
// in the air next to the hand and is always visible in the headset. The texture is
// drawn on the CPU (battery, charge bolt, percent text) and uploaded with
// SetOverlayRaw. The OpenVR runtime is loaded at run time, so the program builds
// and runs without linking against the SDK.
//
// Usage:
//   steamvr_battery_indicator                 headset battery on the right hand
//   steamvr_battery_indicator --hand left
//   steamvr_battery_indicator --source right  show controller battery instead
//   steamvr_battery_indicator --anchor hmd
//   steamvr_battery_indicator --preview out.png   render one frame, no SteamVR
//   steamvr_battery_indicator --list              list devices and their battery

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "openvr_capi.h"

// ---------------------------------------------------------------------------
// Dynamic loading of the OpenVR runtime
// ---------------------------------------------------------------------------

namespace {

using VrInitInternal = intptr_t (*)(EVRInitError *, EVRApplicationType);
using VrShutdownInternal = void (*)();
using VrIsRuntimeInstalled = bool (*)();
using VrGetGenericInterface = intptr_t (*)(const char *, EVRInitError *);
using VrGetErrorDescription = const char *(*)(EVRInitError);

void *g_lib = nullptr;

void *openLibrary(const std::string &path) {
#ifdef _WIN32
    return reinterpret_cast<void *>(LoadLibraryA(path.c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void *libSymbol(void *lib, const char *name) {
#ifdef _WIN32
    return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

std::string executableDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    std::string p(buf);
#endif
    size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

std::vector<std::string> libraryCandidates() {
    const std::string dir = executableDir();
#ifdef _WIN32
    const char *name = "openvr_api.dll";
    const std::vector<std::string> extras = {
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\" + std::string(name),
        "C:\\Program Files\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\" + std::string(name),
    };
#else
    const char *name = "libopenvr_api.so";
    const std::vector<std::string> extras = {
        std::string(getenv("HOME") ? getenv("HOME") : "") +
            "/.steam/steam/steamapps/common/SteamVR/bin/linux64/" + name,
        std::string(getenv("HOME") ? getenv("HOME") : "") +
            "/.local/share/Steam/steamapps/common/SteamVR/bin/linux64/" + name,
        std::string(getenv("HOME") ? getenv("HOME") : "") +
            "/.steam/root/steamapps/common/SteamVR/bin/linux64/" + name,
    };
#endif
    std::vector<std::string> out;
    out.push_back(dir + "/" + name);
    out.push_back(dir + "/lib/" + name);
    if (const char *env = getenv("STEAMVR_OPENVR_PATH")) out.push_back(env);
    for (const auto &e : extras) out.push_back(e);
    out.push_back(name);  // let the system loader search its paths
    return out;
}

bool loadRuntime() {
    for (const auto &candidate : libraryCandidates()) {
        g_lib = openLibrary(candidate);
        if (g_lib) return true;
    }
    return false;
}

struct Runtime {
    VrInitInternal initInternal = nullptr;
    VrShutdownInternal shutdownInternal = nullptr;
    VrIsRuntimeInstalled isRuntimeInstalled = nullptr;
    VrGetGenericInterface getGenericInterface = nullptr;
    VrGetErrorDescription errorDescription = nullptr;

    VR_IVRSystem_FnTable *system = nullptr;
    VR_IVROverlay_FnTable *overlay = nullptr;

    bool load() {
        if (!loadRuntime()) return false;
        initInternal = reinterpret_cast<VrInitInternal>(libSymbol(g_lib, "VR_InitInternal"));
        shutdownInternal = reinterpret_cast<VrShutdownInternal>(libSymbol(g_lib, "VR_ShutdownInternal"));
        isRuntimeInstalled = reinterpret_cast<VrIsRuntimeInstalled>(libSymbol(g_lib, "VR_IsRuntimeInstalled"));
        getGenericInterface = reinterpret_cast<VrGetGenericInterface>(libSymbol(g_lib, "VR_GetGenericInterface"));
        errorDescription = reinterpret_cast<VrGetErrorDescription>(
            libSymbol(g_lib, "VR_GetVRInitErrorAsEnglishDescription"));
        return initInternal && shutdownInternal && isRuntimeInstalled &&
               getGenericInterface && errorDescription;
    }

    bool start(EVRApplicationType type) {
        EVRInitError err = EVRInitError_VRInitError_None;
        initInternal(&err, type);
        if (err != EVRInitError_VRInitError_None) {
            std::fprintf(stderr, "Could not initialise OpenVR: %s\n", errorDescription(err));
            return false;
        }
        system = reinterpret_cast<VR_IVRSystem_FnTable *>(
            getGenericInterface(IVRSystem_Version, &err));
        if (err != EVRInitError_VRInitError_None || !system) {
            std::fprintf(stderr, "IVRSystem unavailable: %s\n", errorDescription(err));
            shutdownInternal();
            return false;
        }
        overlay = reinterpret_cast<VR_IVROverlay_FnTable *>(
            getGenericInterface(IVROverlay_Version, &err));
        if (err != EVRInitError_VRInitError_None || !overlay) {
            std::fprintf(stderr, "IVROverlay unavailable: %s\n", errorDescription(err));
            shutdownInternal();
            return false;
        }
        return true;
    }

    void stop() {
        if (shutdownInternal) shutdownInternal();
    }
};

}  // namespace
