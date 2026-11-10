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
#include "log.h"

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

#ifdef _WIN32
std::string regString(HKEY root, const char *sub, const char *value) {
    HKEY key = nullptr;
    if (RegOpenKeyExA(root, sub, 0, KEY_READ, &key) != ERROR_SUCCESS) return std::string();
    char buf[2048];
    DWORD size = sizeof(buf);
    DWORD type = 0;
    const LONG rc = RegQueryValueExA(key, value, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(buf), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return std::string();
    if (size > 0 && buf[size - 1] == '\0') --size;  // drop the trailing null
    return std::string(buf, size);
}

std::string joinPath(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

// Reads every "path" value from a Steam libraryfolders.vdf, so SteamVR is found
// even when it lives in a secondary library on another drive.
void collectLibraryPaths(const std::string &vdfPath, std::vector<std::string> &out) {
    FILE *f = std::fopen(vdfPath.c_str(), "r");
    if (!f) return;
    char line[4096];
    while (std::fgets(line, sizeof(line), f)) {
        const char *p = std::strstr(line, "\"path\"");
        if (!p) continue;
        p = std::strchr(p + 6, '"');
        if (!p) continue;
        const char *q = std::strchr(p + 1, '"');
        if (!q) continue;
        std::string raw(p + 1, static_cast<size_t>(q - (p + 1)));
        std::string path;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') {
                path += '\\';
                ++i;
            } else {
                path += raw[i];
            }
        }
        if (!path.empty()) out.push_back(path);
    }
    std::fclose(f);
}

std::vector<std::string> windowsRuntimeCandidates() {
    const char *name = "openvr_api.dll";
    std::vector<std::string> roots;
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (const char *sub : {"Software\\Valve\\Steam",
                                "SOFTWARE\\WOW6432Node\\Valve\\Steam"}) {
            std::string p = regString(root, sub, "InstallPath");
            if (p.empty()) p = regString(root, sub, "SteamPath");
            if (!p.empty()) roots.push_back(p);
        }
    }
    roots.push_back("C:\\Program Files (x86)\\Steam");
    roots.push_back("C:\\Program Files\\Steam");

    std::vector<std::string> libs = roots;
    for (const auto &r : roots) {
        collectLibraryPaths(joinPath(joinPath(r, "steamapps"), "libraryfolders.vdf"), libs);
    }

    std::vector<std::string> out;
    for (const auto &lib : libs) {
        out.push_back(joinPath(lib, "steamapps\\common\\SteamVR\\bin\\win64\\" + std::string(name)));
        out.push_back(joinPath(lib, "steamapps\\common\\SteamVR\\bin\\win32\\" + std::string(name)));
    }

    // SteamVR registers its own install path under the OpenVR key.
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        std::string rt = regString(root, "Software\\Valve\\OpenVR", "RuntimePath");
        if (!rt.empty()) {
            out.push_back(joinPath(rt, "bin\\win64\\" + std::string(name)));
            out.push_back(joinPath(rt, "bin\\win32\\" + std::string(name)));
        }
    }
    return out;
}
#endif

std::vector<std::string> libraryCandidates() {
    const std::string dir = executableDir();
    std::vector<std::string> out;
#ifdef _WIN32
    const char *name = "openvr_api.dll";
    out.push_back(dir + "\\" + name);
    out.push_back(dir + "\\lib\\" + name);
    if (const char *env = getenv("STEAMVR_OPENVR_PATH")) {
        out.push_back(env);
        out.push_back(std::string(env) + "\\" + name);
    }
    for (const auto &p : windowsRuntimeCandidates()) out.push_back(p);
    out.push_back(name);  // let the system loader search its paths
#else
    const char *name = "libopenvr_api.so";
    const std::string home = getenv("HOME") ? getenv("HOME") : "";
    const std::vector<std::string> extras = {
        home + "/.steam/steam/steamapps/common/SteamVR/bin/linux64/" + name,
        home + "/.local/share/Steam/steamapps/common/SteamVR/bin/linux64/" + name,
        home + "/.steam/root/steamapps/common/SteamVR/bin/linux64/" + name,
        home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/"
               "common/SteamVR/bin/linux64/" + name,
    };
    out.push_back(dir + "/" + name);
    out.push_back(dir + "/lib/" + name);
    if (const char *env = getenv("STEAMVR_OPENVR_PATH")) out.push_back(env);
    for (const auto &e : extras) out.push_back(e);
    out.push_back(name);  // let the system loader search its paths
#endif
    return out;
}

bool loadRuntime() {
    const std::vector<std::string> candidates = libraryCandidates();
    for (const auto &candidate : candidates) {
        g_lib = openLibrary(candidate);
        if (g_lib) {
            logger::info("Loaded OpenVR runtime from: " + candidate);
            return true;
        }
    }
    std::string tried;
    for (const auto &c : candidates) tried += "\n    " + c;
    logger::error("Could not load the OpenVR runtime library. Tried:" + tried);
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
