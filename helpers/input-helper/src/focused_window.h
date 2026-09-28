#pragma once

#include <string>
#include <vector>

namespace InputHelper {
namespace FocusedWindow {

struct Info {
    bool known = false;  // compositor answered
    std::string windowClass;
    std::string initialClass;
    std::string title;
    std::vector<std::string> tags;
    int pid = 0;
};

// Asks the compositor for the focused window. Hyprland only (IPC socket);
// other sessions return known=false. Bounded by timeoutMs.
Info Query(int timeoutMs);

// True when the focused window is a terminal emulator, where Ctrl+V/Ctrl+C
// mean "literal ^V" / SIGINT and clipboard paste/copy are Ctrl+Shift+V/C.
bool IsTerminal(const Info &info);

}  // namespace FocusedWindow
}  // namespace InputHelper
