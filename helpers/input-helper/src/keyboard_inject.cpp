// In-process /dev/uinput virtual keyboard (mirrors the original keyboard_inject.cpp).
//
// The kernel input layer is the only injection path that reaches every native
// Wayland surface on Hyprland; libinput -> compositor routes these events like
// a physical keyboard. Chords are written as one contiguous batch with no sleep
// between modifier-down and key-down (a quiet gap makes some compositors drop
// the modifier).
#include "keyboard_inject.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <mutex>

#include "common.h"

namespace InputHelper {
namespace KeyboardInject {
namespace {

std::mutex &g_deviceMutex = *new std::mutex();  // leaked: used by detached threads
int g_fd = -1;
bool g_deviceReady = false;

const char kDeviceName[] = "Typeless InputHelper Virtual Keyboard";

// Original enables LCTRL, C, V, BACKSPACE, INSERT, LSHIFT; that covers every
// chord we send (Ctrl+Shift+V / Ctrl+Shift+C included).
const int kKeys[] = {KEY_LEFTCTRL, KEY_C, KEY_V, KEY_BACKSPACE, KEY_INSERT, KEY_LEFTSHIFT};

bool WriteEvent(int fd, unsigned short type, unsigned short code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    for (;;) {
        ssize_t n = write(fd, &ev, sizeof ev);
        if (n == (ssize_t)sizeof ev) return true;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {
            SleepMs(1);
            continue;
        }
        Log("[KeyboardInject] write failed: %s", strerror(errno));
        return false;
    }
}

bool EmitKey(int fd, unsigned short code, int value)
{
    return WriteEvent(fd, EV_KEY, code, value) && WriteEvent(fd, EV_SYN, SYN_REPORT, 0);
}

bool CreateDeviceLocked()
{
    if (g_deviceReady && g_fd >= 0) return true;

    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        Log("[KeyboardInject] Failed to open /dev/uinput: %s", strerror(errno));
        return false;
    }
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        Log("[KeyboardInject] UI_SET_EVBIT failed: %s", strerror(errno));
        close(fd);
        return false;
    }
    for (int key : kKeys) {
        if (ioctl(fd, UI_SET_KEYBIT, key) < 0) {
            Log("[KeyboardInject] UI_SET_KEYBIT(%d) failed: %s", key, strerror(errno));
            close(fd);
            return false;
        }
    }
    struct uinput_setup setup;
    memset(&setup, 0, sizeof setup);
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x5459;   // same ids as the original device
    setup.id.product = 0x494e;
    strncpy(setup.name, kDeviceName, UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        Log("[KeyboardInject] UI_DEV_CREATE failed: %s", strerror(errno));
        close(fd);
        return false;
    }
    // Let udev/libinput/the compositor enumerate the device before the first key.
    SleepMs(200);
    g_fd = fd;
    g_deviceReady = true;
    Log("[KeyboardInject] Virtual keyboard ready");
    return true;
}

// Press modifiers, tap key, release modifiers in reverse order, no gaps.
bool SendChordLocked(const unsigned short *mods, int modCount, unsigned short key)
{
    int fd = g_fd;
    int pressed = 0;
    bool ok = true;
    for (; pressed < modCount; pressed++) {
        if (!EmitKey(fd, mods[pressed], 1)) {
            ok = false;
            break;
        }
    }
    if (ok) {
        ok = EmitKey(fd, key, 1);
        if (ok) ok = EmitKey(fd, key, 0);
    }
    // Always release whatever we pressed so nothing stays stuck.
    for (int i = pressed - 1; i >= 0; i--) {
        if (!EmitKey(fd, mods[i], 0)) ok = false;
    }
    return ok;
}

bool SendChord(Chord chord)
{
    static const unsigned short ctrl[] = {KEY_LEFTCTRL};
    static const unsigned short ctrlShift[] = {KEY_LEFTCTRL, KEY_LEFTSHIFT};
    static const unsigned short shift[] = {KEY_LEFTSHIFT};
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    if (!CreateDeviceLocked()) return false;
    switch (chord) {
        case Chord::CtrlV: return SendChordLocked(ctrl, 1, KEY_V);
        case Chord::CtrlShiftV: return SendChordLocked(ctrlShift, 2, KEY_V);
        case Chord::ShiftInsert: return SendChordLocked(shift, 1, KEY_INSERT);
        case Chord::CtrlC: return SendChordLocked(ctrl, 1, KEY_C);
        case Chord::CtrlShiftC: return SendChordLocked(ctrlShift, 2, KEY_C);
    }
    return false;
}

}  // namespace

bool EnsureDevice()
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    return CreateDeviceLocked();
}

bool SimulatePaste(Chord chord)
{
    if (SendChord(chord)) return true;
    // Original: if the Ctrl+V chord cannot be written, retry with Shift+Insert.
    if (chord == Chord::CtrlV) return SendChord(Chord::ShiftInsert);
    return false;
}

bool SimulateCopy(Chord chord)
{
    return SendChord(chord);
}

bool SimulateBackspace(int count)
{
    if (count <= 0) return true;
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    if (!CreateDeviceLocked()) return false;
    for (int i = 0; i < count; i++) {
        if (!EmitKey(g_fd, KEY_BACKSPACE, 1) || !EmitKey(g_fd, KEY_BACKSPACE, 0)) return false;
        if (i + 1 < count) SleepMs(10);
    }
    return true;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    if (g_fd >= 0) {
        ioctl(g_fd, UI_DEV_DESTROY);
        close(g_fd);
    }
    g_fd = -1;
    g_deviceReady = false;
}

const char *DeviceName()
{
    return kDeviceName;
}

}  // namespace KeyboardInject
}  // namespace InputHelper
