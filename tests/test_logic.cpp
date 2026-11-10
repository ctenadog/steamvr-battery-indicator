// Unit tests for the pure logic: transform math, PNG output, battery reading
// against a fake IVRSystem function table, and device/role resolution.
// Build: g++ -std=c++17 -Isrc tests/test_logic.cpp -o /tmp/test_logic -ldl
#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "indicator.h"
#include "image.h"

// The functions under test live in main.cpp's anonymous namespace, so we include
// it here to reach them (a common lightweight approach for small projects).
#define main main_disabled
#include "../src/main.cpp"
#undef main

namespace {

void testTransform() {
    float pos[3] = {1.0f, 2.0f, 3.0f};
    float rot0[3] = {0.0f, 0.0f, 0.0f};
    HmdMatrix34_t m = makeTransform(pos, rot0);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            assert(std::fabs(m.m[i][j] - (i == j ? 1.0f : 0.0f)) < 1e-6f);
        }
        assert(std::fabs(m.m[i][3] - pos[i]) < 1e-6f);
    }
    // 90 degree yaw maps +X to -Z.
    float rot90[3] = {90.0f, 0.0f, 0.0f};
    HmdMatrix34_t r = makeTransform(rot0, rot90);
    assert(std::fabs(r.m[0][0]) < 1e-6f);
    assert(std::fabs(r.m[1][0]) < 1e-6f);
    assert(std::fabs(r.m[2][0] + 1.0f) < 1e-6f);
    std::printf("transform OK\n");
}

void testImageAndPng() {
    draw::Image img;
    indicator::BatteryState st;
    st.available = true;
    st.hasPercentage = true;
    st.percentage = 0.42f;
    st.charging = true;
    indicator::render(img, st, "hmd", 0.3f);
    assert(img.width == indicator::kTexW && img.height == indicator::kTexH);
    assert(img.pixels[3] == 0);  // transparent corner
    size_t idx = (static_cast<size_t>(128) * img.width + 60) * 4;
    assert(img.pixels[idx + 3] == 255);
    assert(img.pixels[idx] > 200 && img.pixels[idx + 1] > 150);
    const std::vector<uint8_t> bgra = indicator::toBgra(img);
    assert(bgra.size() == img.pixels.size());
    assert(bgra[0] == img.pixels[2] && bgra[2] == img.pixels[0]);
    assert(img.savePng("/tmp/test_out.png"));
    std::printf("image+png OK\n");
}

// --- Fake IVRSystem function table ----------------------------------------

struct FakeDevice {
    bool connected = false;
    ETrackedDeviceClass cls = ETrackedDeviceClass_TrackedDeviceClass_Invalid;
    ETrackedControllerRole role = ETrackedControllerRole_TrackedControllerRole_Invalid;
    bool provides = false;
    bool hasPct = false;
    float pct = 0.0f;
    bool charging = false;
};

std::vector<FakeDevice> g_devices;

FakeDevice *device(TrackedDeviceIndex_t i) {
    if (i >= g_devices.size()) return nullptr;
    return &g_devices[i];
}

VR_IVRSystem_FnTable makeFakeSystem() {
    VR_IVRSystem_FnTable t{};
    t.IsTrackedDeviceConnected = [](TrackedDeviceIndex_t i) -> bool {
        FakeDevice *d = device(i);
        return d && d->connected;
    };
    t.GetTrackedDeviceClass = [](TrackedDeviceIndex_t i) -> ETrackedDeviceClass {
        FakeDevice *d = device(i);
        return d ? d->cls : ETrackedDeviceClass_TrackedDeviceClass_Invalid;
    };
    t.GetControllerRoleForTrackedDeviceIndex =
        [](TrackedDeviceIndex_t i) -> ETrackedControllerRole {
        FakeDevice *d = device(i);
        return d ? d->role : ETrackedControllerRole_TrackedControllerRole_Invalid;
    };
    t.GetTrackedDeviceIndexForControllerRole = [](ETrackedControllerRole role) -> TrackedDeviceIndex_t {
        for (size_t i = 0; i < g_devices.size(); ++i) {
            if (g_devices[i].connected && g_devices[i].role == role &&
                g_devices[i].cls == ETrackedDeviceClass_TrackedDeviceClass_Controller) {
                return static_cast<TrackedDeviceIndex_t>(i);
            }
        }
        return k_unTrackedDeviceIndexInvalid;
    };
    t.GetStringTrackedDeviceProperty = [](TrackedDeviceIndex_t, ETrackedDeviceProperty,
                                          char *buf, uint32_t size,
                                          ETrackedPropertyError *err) -> uint32_t {
        if (err) *err = ETrackedPropertyError_TrackedProp_Success;
        const char *name = "fake";
        if (buf && size) std::snprintf(buf, size, "%s", name);
        return static_cast<uint32_t>(std::strlen(name) + 1);
    };
    t.GetBoolTrackedDeviceProperty = [](TrackedDeviceIndex_t i, ETrackedDeviceProperty prop,
                                        ETrackedPropertyError *err) -> bool {
        FakeDevice *d = device(i);
        if (!d) { if (err) *err = ETrackedPropertyError_TrackedProp_InvalidDevice; return false; }
        if (prop == ETrackedDeviceProperty_Prop_DeviceProvidesBatteryStatus_Bool) {
            if (err) *err = ETrackedPropertyError_TrackedProp_Success;
            return d->provides;
        }
        if (prop == ETrackedDeviceProperty_Prop_DeviceIsCharging_Bool) {
            if (err) *err = ETrackedPropertyError_TrackedProp_Success;
            return d->charging;
        }
        if (err) *err = ETrackedPropertyError_TrackedProp_UnknownProperty;
        return false;
    };
    t.GetFloatTrackedDeviceProperty = [](TrackedDeviceIndex_t i, ETrackedDeviceProperty prop,
                                         ETrackedPropertyError *err) -> float {
        FakeDevice *d = device(i);
        if (!d) { if (err) *err = ETrackedPropertyError_TrackedProp_InvalidDevice; return 0.0f; }
        if (prop == ETrackedDeviceProperty_Prop_DeviceBatteryPercentage_Float) {
            if (!d->hasPct) {
                if (err) *err = ETrackedPropertyError_TrackedProp_ValueNotProvidedByDevice;
                return 0.0f;
            }
            if (err) *err = ETrackedPropertyError_TrackedProp_Success;
            return d->pct;
        }
        if (err) *err = ETrackedPropertyError_TrackedProp_UnknownProperty;
        return 0.0f;
    };
    return t;
}

void testBatteryReading() {
    g_devices.clear();
    g_devices.resize(64);
    g_devices[0].connected = true;
    g_devices[0].provides = true;
    g_devices[0].hasPct = true;
    g_devices[0].pct = 0.62f;
    g_devices[0].charging = true;

    g_devices[1].connected = true;
    g_devices[1].cls = ETrackedDeviceClass_TrackedDeviceClass_Controller;
    g_devices[1].role = ETrackedControllerRole_TrackedControllerRole_RightHand;
    g_devices[1].provides = true;
    g_devices[1].hasPct = true;
    g_devices[1].pct = 0.30f;

    g_devices[2].connected = true;
    g_devices[2].cls = ETrackedDeviceClass_TrackedDeviceClass_Controller;
    g_devices[2].role = ETrackedControllerRole_TrackedControllerRole_LeftHand;
    g_devices[2].provides = true;
    g_devices[2].hasPct = true;
    g_devices[2].pct = 0.90f;
    g_devices[2].charging = true;

    g_devices[3].connected = true;  // provides battery, but no value
    g_devices[3].provides = true;

    VR_IVRSystem_FnTable sys = makeFakeSystem();

    BatteryReading hmd = readBattery(&sys, 0);
    assert(hmd.available && hmd.hasPercentage);
    assert(std::fabs(hmd.percentage - 0.62f) < 1e-6f);
    assert(hmd.charging);

    BatteryReading noValue = readBattery(&sys, 3);
    assert(noValue.available && !noValue.hasPercentage && !noValue.charging);

    BatteryReading invalid = readBattery(&sys, k_unTrackedDeviceIndexInvalid);
    assert(!invalid.available);

    assert(resolveSource(&sys, "hmd") == 0);
    assert(resolveSource(&sys, "right") == 1);
    assert(resolveSource(&sys, "left") == 2);

    g_devices[1].connected = false;
    g_devices[2].connected = false;
    assert(resolveSource(&sys, "right") == k_unTrackedDeviceIndexInvalid);
    std::printf("battery reading OK\n");
}

}  // namespace

int main() {
    testTransform();
    testImageAndPng();
    testBatteryReading();
    std::printf("ALL TESTS PASSED\n");
    return 0;
}
