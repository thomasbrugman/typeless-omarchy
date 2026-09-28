#pragma once

namespace InputHelper {
namespace KeyboardInject {

enum class Chord {
    CtrlV,        // default paste (original behavior)
    CtrlShiftV,   // terminal paste
    ShiftInsert,  // original's fallback when Ctrl+V could not be written
    CtrlC,        // default copy (original behavior)
    CtrlShiftC,   // terminal copy (Ctrl+C would send SIGINT)
};

bool EnsureDevice();
bool SimulatePaste(Chord chord);
bool SimulateCopy(Chord chord);
bool SimulateBackspace(int count);
void Shutdown();

// Testing hook: path of the evdev node backing the virtual keyboard ("" if unknown).
const char *DeviceName();

}  // namespace KeyboardInject
}  // namespace InputHelper
