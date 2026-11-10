// SteamVR overlay: headset battery / charging state on your hand.
// See the file header comment in README.md for usage.

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "runtime.h"
#include "image.h"
#include "indicator.h"
#include "log.h"

namespace {

// Overlay keys must be writable: the OpenVR runtime is handed a char* and a
// string literal would be undefined behaviour if it ever wrote to it.
char kOverlayKey[] = "openhands.hmd_battery";
char kOverlayName[] = "HMD Battery";

struct Options {
    std::string source = "hmd";    // hmd | left | right
    std::string anchor = "hand";   // hand | hmd
    std::string hand = "right";    // left | right
    float width = 0.14f;           // overlay width in meters
    float offset[3] = {0.0f, 0.055f, 0.075f};
    float rotate[3] = {180.0f, -18.0f, 0.0f};  // yaw, pitch, roll in degrees
    int fps = 15;
    float pollInterval = 1.0f;
    std::string preview;           // path for a one-off PNG render
    bool list = false;
    std::string logPath;           // empty = default next to the exe
    bool noLog = false;            // disable the log file entirely
};

void printUsage(const char *prog) {
    std::printf(
        "SteamVR battery indicator\n\n"
        "Usage: %s [options]\n\n"
        "Options:\n"
        "  --source {hmd|left|right}   device whose battery is shown (default: hmd)\n"
        "  --anchor {hand|hmd}         what the overlay is attached to (default: hand)\n"
        "  --hand {left|right}         controller used with --anchor hand (default: right)\n"
        "  --width METERS              overlay width in meters (default: 0.14)\n"
        "  --offset X Y Z              offset from the anchor, meters (default: 0 0.055 0.075)\n"
        "  --rotate YAW PITCH ROLL     rotation relative to the anchor, degrees (default: 180 -18 0)\n"
        "  --fps N                     animation refresh rate (default: 15)\n"
        "  --poll-interval SECONDS     seconds between battery readings (default: 1.0)\n"
        "  --preview PATH              render one frame to a PNG and exit (no SteamVR needed)\n"
        "  --list                      print tracked devices with their battery state and exit\n"
        "  --log PATH                  write the log file to PATH (default: next to the exe)\n"
        "  --no-log                    do not write a log file\n"
        "  -h, --help                  show this help\n",
        prog);
}

bool parseFloat3(int argc, char **argv, int &i, float out[3]) {
    if (i + 3 >= argc) return false;
    for (int k = 0; k < 3; ++k) {
        char *end = nullptr;
        out[k] = std::strtof(argv[i + 1 + k], &end);
        if (end == argv[i + 1 + k] || *end != '\0') return false;
    }
    i += 3;
    return true;
}

bool parseArgs(int argc, char **argv, Options &opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            std::exit(0);
        } else if (arg == "--source") {
            const char *v = value("--source");
            if (!v) return false;
            opt.source = v;
        } else if (arg == "--anchor") {
            const char *v = value("--anchor");
            if (!v) return false;
            opt.anchor = v;
        } else if (arg == "--hand") {
            const char *v = value("--hand");
            if (!v) return false;
            opt.hand = v;
        } else if (arg == "--width") {
            const char *v = value("--width");
            if (!v) return false;
            opt.width = std::strtof(v, nullptr);
        } else if (arg == "--offset") {
            if (!parseFloat3(argc, argv, i, opt.offset)) {
                std::fprintf(stderr, "--offset needs three numbers\n");
                return false;
            }
        } else if (arg == "--rotate") {
            if (!parseFloat3(argc, argv, i, opt.rotate)) {
                std::fprintf(stderr, "--rotate needs three numbers\n");
                return false;
            }
        } else if (arg == "--fps") {
            const char *v = value("--fps");
            if (!v) return false;
            opt.fps = std::atoi(v);
        } else if (arg == "--poll-interval") {
            const char *v = value("--poll-interval");
            if (!v) return false;
            opt.pollInterval = std::strtof(v, nullptr);
        } else if (arg == "--preview") {
            const char *v = value("--preview");
            if (!v) return false;
            opt.preview = v;
        } else if (arg == "--list") {
            opt.list = true;
        } else if (arg == "--log") {
            const char *v = value("--log");
            if (!v) return false;
            opt.logPath = v;
        } else if (arg == "--no-log") {
            opt.noLog = true;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            return false;
        }
    }
    if (opt.source != "hmd" && opt.source != "left" && opt.source != "right") {
        std::fprintf(stderr, "--source must be hmd, left or right\n");
        return false;
    }
    if (opt.anchor != "hand" && opt.anchor != "hmd") {
        std::fprintf(stderr, "--anchor must be hand or hmd\n");
        return false;
    }
    if (opt.hand != "left" && opt.hand != "right") {
        std::fprintf(stderr, "--hand must be left or right\n");
        return false;
    }
    if (opt.fps < 1) opt.fps = 1;
    if (opt.pollInterval <= 0.0f) opt.pollInterval = 1.0f;
    return true;
}

// ---------------------------------------------------------------------------
// Battery reading
// ---------------------------------------------------------------------------

struct BatteryReading {
    bool available = false;
    bool hasPercentage = false;
    float percentage = 0.0f;
    bool charging = false;
    std::string name;
};

BatteryReading readBattery(VR_IVRSystem_FnTable *system, uint32_t device) {
    BatteryReading r;
    if (!system || device == k_unTrackedDeviceIndexInvalid) return r;
    if (!system->IsTrackedDeviceConnected(device)) return r;

    char nameBuf[256] = {0};
    system->GetStringTrackedDeviceProperty(
        device, ETrackedDeviceProperty_Prop_TrackingSystemName_String, nameBuf,
        sizeof(nameBuf), nullptr);
    r.name = nameBuf;

    ETrackedPropertyError err = ETrackedPropertyError_TrackedProp_Success;
    const bool provides = system->GetBoolTrackedDeviceProperty(
        device, ETrackedDeviceProperty_Prop_DeviceProvidesBatteryStatus_Bool, &err);
    const bool providesOk = (err == ETrackedPropertyError_TrackedProp_Success);

    err = ETrackedPropertyError_TrackedProp_Success;
    const float pct = system->GetFloatTrackedDeviceProperty(
        device, ETrackedDeviceProperty_Prop_DeviceBatteryPercentage_Float, &err);
    const bool pctOk = (err == ETrackedPropertyError_TrackedProp_Success);

    err = ETrackedPropertyError_TrackedProp_Success;
    const bool charging = system->GetBoolTrackedDeviceProperty(
        device, ETrackedDeviceProperty_Prop_DeviceIsCharging_Bool, &err);
    const bool chargingOk = (err == ETrackedPropertyError_TrackedProp_Success);

    if (!pctOk && !(providesOk && provides)) return r;

    r.available = true;
    if (pctOk) {
        r.hasPercentage = true;
        r.percentage = std::min(1.0f, std::max(0.0f, pct));
    }
    r.charging = chargingOk && charging;
    return r;
}

// Human-readable one-line summary of a reading, used for log lines.
std::string describe(const BatteryReading &r) {
    std::string s;
    if (!r.available) return "unavailable";
    if (r.hasPercentage) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::lround(r.percentage * 100)));
        s = buf;
    } else {
        s = "--";
    }
    s += r.charging ? " charging" : " discharging";
    return s;
}

uint32_t findController(VR_IVRSystem_FnTable *system, const std::string &hand) {
    const ETrackedControllerRole role = (hand == "left")
        ? ETrackedControllerRole_TrackedControllerRole_LeftHand
        : ETrackedControllerRole_TrackedControllerRole_RightHand;
    const uint32_t idx = system->GetTrackedDeviceIndexForControllerRole(role);
    if (idx != k_unTrackedDeviceIndexInvalid) return idx;
    for (uint32_t i = 1; i < k_unMaxTrackedDeviceCount; ++i) {
        if (!system->IsTrackedDeviceConnected(i)) continue;
        if (system->GetTrackedDeviceClass(i) !=
            ETrackedDeviceClass_TrackedDeviceClass_Controller) {
            continue;
        }
        if (system->GetControllerRoleForTrackedDeviceIndex(i) == role) return i;
    }
    return k_unTrackedDeviceIndexInvalid;
}

uint32_t resolveSource(VR_IVRSystem_FnTable *system, const std::string &source) {
    if (source == "hmd") return k_unTrackedDeviceIndex_Hmd;
    return findController(system, source);
}

HmdMatrix34_t makeTransform(const float pos[3], const float rotDeg[3]) {
    const float yaw = rotDeg[0] * 3.14159265358979f / 180.0f;
    const float pitch = rotDeg[1] * 3.14159265358979f / 180.0f;
    const float roll = rotDeg[2] * 3.14159265358979f / 180.0f;
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);
    const float ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    const float rx[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
    const float rz[3][3] = {{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}};
    float rxy[3][3], r[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            rxy[i][j] = ry[i][0] * rx[0][j] + ry[i][1] * rx[1][j] + ry[i][2] * rx[2][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i][j] = rxy[i][0] * rz[0][j] + rxy[i][1] * rz[1][j] + rxy[i][2] * rz[2][j];
    HmdMatrix34_t m{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m.m[i][j] = r[i][j];
        m.m[i][3] = pos[i];
    }
    return m;
}

// ---------------------------------------------------------------------------
// Overlay application
// ---------------------------------------------------------------------------

class OverlayApp {
public:
    explicit OverlayApp(const Options &opt) : opt_(opt) {}

    bool start(Runtime &rt) {
        rt_ = &rt;
        if (!checkOverlayTable(rt.overlay)) return false;
        logger::info("Creating overlay...");
        EVROverlayError err = rt.overlay->CreateOverlay(kOverlayKey, kOverlayName, &handle_);
        if (err == EVROverlayError_VROverlayError_KeyInUse) {
            err = rt.overlay->FindOverlay(kOverlayKey, &handle_);
        }
        if (err != EVROverlayError_VROverlayError_None) {
            logger::error(std::string("Could not create overlay: ") +
                          rt.overlay->GetOverlayErrorNameFromEnum(err));
            std::fprintf(stderr, "Could not create overlay: %s\n",
                         rt.overlay->GetOverlayErrorNameFromEnum(err));
            return false;
        }
        logger::info("Overlay created (handle set).");
        rt.overlay->SetOverlayWidthInMeters(handle_, opt_.width);
        rt.overlay->SetOverlaySortOrder(handle_, 50);
        rt.overlay->SetOverlayFlag(handle_, VROverlayFlags_VisibleInDashboard, true);
        // The texture is uploaded as non-premultiplied BGRA.
        rt.overlay->SetOverlayFlag(handle_, VROverlayFlags_IsPremultiplied, false);

        logger::info("Attaching overlay to the anchor device...");
        syncAnchor();
        device_ = resolveSource(rt.system, opt_.source);
        logger::info("Reading initial battery state...");
        reading_ = readBattery(rt.system, device_);
        logger::info("Rendering the first frame...");
        upload(0.0f);
        logger::info("First frame uploaded.");
        return true;
    }

    void tick() {
        const double now = nowSeconds();
        if (now - lastPoll_ >= opt_.pollInterval) {
            lastPoll_ = now;
            syncAnchor();
            device_ = resolveSource(rt_->system, opt_.source);
            reading_ = readBattery(rt_->system, device_);
            const bool shouldShow = anchor_ != k_unTrackedDeviceIndexInvalid &&
                                    device_ != k_unTrackedDeviceIndexInvalid;
            setVisible(shouldShow);
        }
        if (reading_.available != lastAvail_ || reading_.hasPercentage != lastHasPct_ ||
            reading_.percentage != lastPct_ || reading_.charging != lastCharging_ ||
            reading_.charging) {
            upload(static_cast<float>(now));
            if (reading_.available != lastAvail_ || reading_.hasPercentage != lastHasPct_ ||
                reading_.percentage != lastPct_ || reading_.charging != lastCharging_) {
                logger::info("Battery " + describe(reading_));
            }
            lastAvail_ = reading_.available;
            lastHasPct_ = reading_.hasPercentage;
            lastPct_ = reading_.percentage;
            lastCharging_ = reading_.charging;
        }
    }

    void stop() {
        if (rt_ && rt_->overlay && handle_ != k_ulOverlayHandleInvalid) {
            rt_->overlay->HideOverlay(handle_);
            rt_->overlay->DestroyOverlay(handle_);
            handle_ = k_ulOverlayHandleInvalid;
        }
    }

private:
    // A function table that comes back from the runtime can be missing entries
    // if the installed SteamVR is older than the header. Calling a null pointer
    // would crash, so report the first missing function instead.
    static bool checkOverlayTable(VR_IVROverlay_FnTable *t) {
        struct Entry { const char *name; const void *fn; };
        const Entry entries[] = {
            {"CreateOverlay", reinterpret_cast<const void *>(t->CreateOverlay)},
            {"SetOverlayWidthInMeters", reinterpret_cast<const void *>(t->SetOverlayWidthInMeters)},
            {"SetOverlaySortOrder", reinterpret_cast<const void *>(t->SetOverlaySortOrder)},
            {"SetOverlayFlag", reinterpret_cast<const void *>(t->SetOverlayFlag)},
            {"SetOverlayTransformTrackedDeviceRelative",
             reinterpret_cast<const void *>(t->SetOverlayTransformTrackedDeviceRelative)},
            {"SetOverlayRaw", reinterpret_cast<const void *>(t->SetOverlayRaw)},
            {"ShowOverlay", reinterpret_cast<const void *>(t->ShowOverlay)},
            {"HideOverlay", reinterpret_cast<const void *>(t->HideOverlay)},
            {"DestroyOverlay", reinterpret_cast<const void *>(t->DestroyOverlay)},
            {"GetOverlayErrorNameFromEnum",
             reinterpret_cast<const void *>(t->GetOverlayErrorNameFromEnum)},
        };
        for (const auto &e : entries) {
            if (!e.fn) {
                logger::error(std::string("The OpenVR overlay interface is missing ") +
                              e.name + ". Update SteamVR and try again.");
                return false;
            }
        }
        return true;
    }

    static double nowSeconds() {
        using clock = std::chrono::steady_clock;
        return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    }

    void syncAnchor() {
        const uint32_t target = (opt_.anchor == "hmd")
            ? k_unTrackedDeviceIndex_Hmd
            : findController(rt_->system, opt_.hand);
        if (target == anchor_) return;
        anchor_ = target;
        if (target != k_unTrackedDeviceIndexInvalid) {
            HmdMatrix34_t m = makeTransform(opt_.offset, opt_.rotate);
            rt_->overlay->SetOverlayTransformTrackedDeviceRelative(handle_, target, &m);
        }
    }

    void setVisible(bool visible) {
        if (visible == visible_) return;
        visible_ = visible;
        if (visible) rt_->overlay->ShowOverlay(handle_);
        else rt_->overlay->HideOverlay(handle_);
    }

    void upload(float t) {
        indicator::BatteryState state;
        state.available = reading_.available;
        state.hasPercentage = reading_.hasPercentage;
        state.percentage = reading_.percentage;
        state.charging = reading_.charging;
        indicator::render(canvas_, state, opt_.source, t);
        const std::vector<uint8_t> bgra = indicator::toBgra(canvas_);
        rt_->overlay->SetOverlayRaw(handle_,
                                    const_cast<uint8_t *>(bgra.data()),
                                    indicator::kTexW, indicator::kTexH, 4);
    }

    const Options &opt_;
    Runtime *rt_ = nullptr;
    VROverlayHandle_t handle_ = k_ulOverlayHandleInvalid;
    uint32_t anchor_ = k_unTrackedDeviceIndexInvalid;
    uint32_t device_ = k_unTrackedDeviceIndexInvalid;
    BatteryReading reading_;
    draw::Image canvas_;
    double lastPoll_ = -1.0;
    bool visible_ = false;
    bool lastAvail_ = false;
    bool lastHasPct_ = false;
    float lastPct_ = -1.0f;
    bool lastCharging_ = false;
};

void describeDevices(VR_IVRSystem_FnTable *system) {
    for (uint32_t i = 0; i < k_unMaxTrackedDeviceCount; ++i) {
        if (!system->IsTrackedDeviceConnected(i)) continue;
        const ETrackedDeviceClass cls = system->GetTrackedDeviceClass(i);
        const BatteryReading r = readBattery(system, i);
        std::string role;
        if (cls == ETrackedDeviceClass_TrackedDeviceClass_Controller) {
            const auto rr = system->GetControllerRoleForTrackedDeviceIndex(i);
            if (rr == ETrackedControllerRole_TrackedControllerRole_LeftHand) role = "left";
            else if (rr == ETrackedControllerRole_TrackedControllerRole_RightHand) role = "right";
            else role = "?";
        }
        char pctTxt[32];
        if (r.hasPercentage) std::snprintf(pctTxt, sizeof(pctTxt), "%d%%", static_cast<int>(std::lround(r.percentage * 100)));
        else std::strcpy(pctTxt, "--");
        const char *chg = r.available ? (r.charging ? " charging" : " discharging") : "";
        std::printf("[%u] class=%d role=%s battery=%s%s\n", i, static_cast<int>(cls),
                    role.c_str(), pctTxt, chg);
    }
}

int runPreview(const Options &opt) {
    indicator::BatteryState state;
    state.available = true;
    state.hasPercentage = true;
    state.percentage = 0.42f;
    state.charging = true;
    draw::Image img;
    indicator::render(img, state, opt.source, 0.3f);
    if (!img.savePng(opt.preview)) {
        std::fprintf(stderr, "Could not write %s\n", opt.preview.c_str());
        return 1;
    }
    std::printf("wrote %s\n", opt.preview.c_str());
    return 0;
}

int runLive(const Options &opt) {
    Runtime rt;
    if (!rt.load()) {
        logger::error("Could not load the OpenVR runtime library (libopenvr_api). "
                   "Start SteamVR, or set STEAMVR_OPENVR_PATH to the library path.");
        std::fprintf(stderr,
                     "Could not load the OpenVR runtime library (libopenvr_api).\n"
                     "Start SteamVR, or set STEAMVR_OPENVR_PATH to the library path.\n");
        return 1;
    }
    if (!rt.isRuntimeInstalled()) {
        logger::error("SteamVR runtime not found. Start SteamVR and try again.");
        std::fprintf(stderr, "SteamVR runtime not found. Start SteamVR and try again.\n");
        return 1;
    }
    logger::info("OpenVR runtime loaded; initialising overlay application.");
    if (!rt.start(EVRApplicationType_VRApplication_Overlay)) {
        logger::error("OpenVR initialisation failed (see message above).");
        return 1;
    }
    logger::info("OpenVR initialised.");

    if (opt.list) {
        describeDevices(rt.system);
        rt.stop();
        return 0;
    }

    OverlayApp app(opt);
    if (!app.start(rt)) {
        logger::error("Could not create the overlay.");
        rt.stop();
        return 1;
    }
    logger::info("Overlay attached to " + opt.anchor + "/" + opt.hand +
              ", source=" + opt.source + ".");
    std::printf("Overlay attached to %s/%s, source=%s. Press Ctrl+C to quit.\n",
                opt.anchor.c_str(), opt.hand.c_str(), opt.source.c_str());

    VREvent_t event{};
    const auto frame = std::chrono::milliseconds(1000 / opt.fps);
    for (;;) {
        while (rt.system->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == EVREventType_VREvent_Quit) {
                logger::info("Quit event received; shutting down.");
                rt.system->AcknowledgeQuit_Exiting();
                app.stop();
                rt.stop();
                return 0;
            }
        }
        app.tick();
        std::this_thread::sleep_for(frame);
    }
}

}  // namespace

#ifdef _WIN32
namespace {

LONG WINAPI crashHandler(EXCEPTION_POINTERS *info) {
    char buf[256];
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const void *addr = info->ExceptionRecord->ExceptionAddress;
    std::snprintf(buf, sizeof(buf),
                  "CRASH: unhandled exception 0x%08lX at %p. Please report this.",
                  static_cast<unsigned long>(code), addr);
    logger::raw(buf);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace
#endif

int main(int argc, char **argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        printUsage(argv[0]);
        return 1;
    }
    if (!opt.noLog) {
        const bool ok = logger::open(opt.logPath);
        logger::info(std::string("SteamVR battery indicator started. ") +
                  (ok ? "Log file: " + (opt.logPath.empty() ? logger::defaultPath() : opt.logPath)
                      : "Could not open the log file; logging to the console only."));
        logger::info("Options: source=" + opt.source + " anchor=" + opt.anchor +
                  " hand=" + opt.hand + " fps=" + std::to_string(opt.fps) +
                  " poll=" + std::to_string(opt.pollInterval) + "s");
    }
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);
#endif
    int rc;
    if (!opt.preview.empty()) rc = runPreview(opt);
    else rc = runLive(opt);
    logger::info("Exiting with code " + std::to_string(rc) + ".");
    logger::close();
    return rc;
}
