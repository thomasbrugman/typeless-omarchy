// Clean-room reimplementation of Typeless' libKeyboardHelper.so (Linux) for arm64.
//
// Behaviour was reconstructed from the x64 build (v2.2.0) so the JS side
// (keyboard-helper-child-process.js, loaded through koffi 2.14.1) sees the same
// C ABI and the same semantics:
//
//   const char* keyboard_say_hello(void)
//   bool  startMonitor(bool (*cb)(int32_t keyCode, int32_t isKeyDown, int32_t isBlocked, const char* pressingJson))
//   void  stopMonitor(void)
//   void  processEvents(void)                    // no-op (as in the original)
//   void  updateTargetShortcuts(const char* json) // [["RightAlt"],["RightAlt","Space"],...]
//   void  resetPressingKeycodes(void)             // calls cb(-9999,-9999,-9999,"[]") synchronously
//   void  setWatcherInterval(double seconds)
//   const char* getKeyboardLayoutInfo(bool detail)
//   const char* getKeyboardDeviceList(void)
//
// Key codes are raw Linux evdev codes (KEY_*). The pressing-keys JSON is
//   [{"keyCode":29,"keyName":"LeftCtrl","enKeyName":"LeftCtrl"},...]
//
// Full keyboards (EV_KEY + KEY_A + KEY_ENTER) are grabbed with EVIOCGRAB and
// re-emitted through a per-device uinput clone named "Typeless Keyboard
// Helper", so that keys belonging to a matched target shortcut can be
// swallowed -- exactly like the original. Additions over the original
// (documented in README-level comments below):
//   * devices that only carry some keyboard/consumer keys (media remotes,
//     USB-mic buttons, headset buttons, ...) are monitored passively
//     (never grabbed, no uinput clone);
//   * keyboards that also report EV_REL/EV_ABS (combo devices) are monitored
//     passively too, because the clone would drop their pointer events;
//   * the uinput clone mirrors the physical LED capabilities and LED state
//     written by the compositor is forwarded back to the physical keyboard;
//   * the callback dispatcher thread is detached instead of joined if it is
//     blocked inside a koffi callback during stop (avoids a JS-main-thread
//     deadlock);
//   * extra key names for keys the original did not know (it reported
//     "unknown-<code>" for those);
//   * full keyboards are monitored passively by default in this port;
//     env KEYBOARD_HELPER_NO_GRAB=0 -> grab them like the original;
//     env KEYBOARD_HELPER_DEVICE_FILTER=<substr> -> only devices whose name
//     contains <substr> (test aid).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define KH_EXPORT extern "C" __attribute__((visibility("default")))

typedef bool (*KeyboardCallback)(int32_t, int32_t, int32_t, const char*);

namespace kh {

static const char* kHelperDeviceName = "Typeless Keyboard Helper";
static const int kKeyMax = 0x2ff;           // original scans codes [0, 0x2ff)
static const int kResetSentinel = -9999;

static void DebugLog(const char* fmt, ...) {
  static int enabled = -1;
  if (enabled < 0) {
    const char* e = getenv("KEYBOARD_HELPER_DEBUG");
    enabled = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
  }
  if (!enabled) return;
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[keyboard-helper] ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

static uint64_t NowMs() {
  using namespace std::chrono;
  return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Key names
// ---------------------------------------------------------------------------

struct KeyNameEntry { int code; const char* name; };

// Exact table of the original library (key_mapping.cpp, 123 entries).
static const KeyNameEntry kOriginalNames[] = {
  {29, "LeftCtrl"}, {97, "RightCtrl"}, {42, "LeftShift"}, {54, "RightShift"},
  {56, "LeftAlt"}, {100, "RightAlt"}, {125, "LeftWin"}, {126, "RightWin"},
  {58, "CapsLock"},
  {30, "A"}, {48, "B"}, {46, "C"}, {32, "D"}, {18, "E"}, {33, "F"}, {34, "G"},
  {35, "H"}, {23, "I"}, {36, "J"}, {37, "K"}, {38, "L"}, {50, "M"}, {49, "N"},
  {24, "O"}, {25, "P"}, {16, "Q"}, {19, "R"}, {31, "S"}, {20, "T"}, {22, "U"},
  {47, "V"}, {17, "W"}, {45, "X"}, {21, "Y"}, {44, "Z"},
  {11, "0"}, {2, "1"}, {3, "2"}, {4, "3"}, {5, "4"}, {6, "5"}, {7, "6"},
  {8, "7"}, {9, "8"}, {10, "9"},
  {59, "F1"}, {60, "F2"}, {61, "F3"}, {62, "F4"}, {63, "F5"}, {64, "F6"},
  {65, "F7"}, {66, "F8"}, {67, "F9"}, {68, "F10"}, {87, "F11"}, {88, "F12"},
  {183, "F13"}, {184, "F14"}, {185, "F15"}, {186, "F16"}, {187, "F17"},
  {188, "F18"}, {189, "F19"}, {190, "F20"}, {191, "F21"}, {192, "F22"},
  {193, "F23"}, {194, "F24"},
  {28, "Enter"}, {96, "NumpadEnter"}, {57, "Space"}, {15, "Tab"},
  {14, "Backspace"}, {1, "Escape"},
  {103, "Up"}, {108, "Down"}, {105, "Left"}, {106, "Right"},
  {102, "Home"}, {107, "End"}, {111, "Delete"}, {110, "Insert"},
  {104, "PageUp"}, {109, "PageDown"},
  {69, "NumLock"}, {70, "ScrollLock"}, {119, "Pause"}, {210, "PrintScreen"},
  {127, "ContextMenu"},
  {82, "Numpad0"}, {79, "Numpad1"}, {80, "Numpad2"}, {81, "Numpad3"},
  {75, "Numpad4"}, {76, "Numpad5"}, {77, "Numpad6"}, {71, "Numpad7"},
  {72, "Numpad8"}, {73, "Numpad9"},
  {55, "NumpadMultiply"}, {78, "NumpadAdd"}, {74, "NumpadSubtract"},
  {83, "NumpadDecimal"}, {98, "NumpadDivide"},
  {12, "-"}, {13, "="}, {26, "["}, {27, "]"}, {39, ";"}, {40, "'"}, {41, "`"},
  {43, "\\"}, {51, ","}, {52, "."}, {53, "/"},
  {113, "VolumeMute"}, {114, "VolumeDown"}, {115, "VolumeUp"},
  {163, "MediaNext"}, {165, "MediaPrevious"}, {166, "MediaStop"},
  {164, "MediaPlayPause"},
};

// Names for keys the original did not know (it reported "unknown-<code>").
// Only used for codes absent from kOriginalNames. Names avoid '+' because the
// app joins shortcut key names with '+'.
static const KeyNameEntry kExtraNames[] = {
  {84, "Key84"}, {85, "Zenkakuhankaku"}, {86, "IntlBackslash"}, {89, "IntlRo"},
  {90, "Katakana"}, {91, "Hiragana"}, {92, "Henkan"}, {93, "KatakanaHiragana"},
  {94, "Muhenkan"}, {95, "NumpadJpComma"}, {99, "SysRq"}, {101, "LineFeed"},
  {112, "Macro"}, {116, "Power"}, {117, "NumpadEqual"}, {118, "NumpadPlusMinus"},
  {120, "Scale"}, {121, "NumpadComma"}, {122, "Hangeul"}, {123, "Hanja"},
  {124, "IntlYen"}, {128, "Stop"}, {129, "Again"}, {130, "Props"}, {131, "Undo"},
  {132, "Front"}, {133, "Copy"}, {134, "Open"}, {135, "Paste"}, {136, "Find"},
  {137, "Cut"}, {138, "Help"}, {139, "Menu"}, {140, "Calculator"}, {141, "Setup"},
  {142, "Sleep"}, {143, "WakeUp"}, {144, "File"}, {145, "SendFile"},
  {146, "DeleteFile"}, {147, "Xfer"}, {148, "Prog1"}, {149, "Prog2"}, {150, "WWW"},
  {151, "MsDos"}, {152, "ScreenLock"}, {153, "RotateDisplay"}, {154, "CycleWindows"},
  {155, "Mail"}, {156, "Bookmarks"}, {157, "Computer"}, {158, "BrowserBack"},
  {159, "BrowserForward"}, {160, "CloseCD"}, {161, "EjectCD"}, {162, "EjectCloseCD"},
  {167, "MediaRecord"}, {168, "MediaRewind"}, {169, "Phone"}, {170, "Iso"},
  {171, "Config"}, {172, "BrowserHome"}, {173, "BrowserRefresh"}, {174, "Exit"},
  {175, "Move"}, {176, "Edit"}, {177, "ScrollUp"}, {178, "ScrollDown"},
  {179, "NumpadLeftParen"}, {180, "NumpadRightParen"}, {181, "New"}, {182, "Redo"},
  {200, "PlayCD"}, {201, "PauseCD"}, {202, "Prog3"}, {203, "Prog4"},
  {204, "AllApplications"}, {205, "Suspend"}, {206, "Close"}, {207, "MediaPlay"},
  {208, "MediaFastForward"}, {209, "BassBoost"}, {211, "HP"}, {212, "Camera"},
  {213, "Sound"}, {214, "Question"}, {215, "Email"}, {216, "Chat"},
  {217, "BrowserSearch"}, {218, "Connect"}, {219, "Finance"}, {220, "Sport"},
  {221, "Shop"}, {222, "AltErase"}, {223, "Cancel"}, {224, "BrightnessDown"},
  {225, "BrightnessUp"}, {226, "Media"}, {227, "SwitchVideoMode"},
  {228, "KbdIllumToggle"}, {229, "KbdIllumDown"}, {230, "KbdIllumUp"}, {231, "Send"},
  {232, "Reply"}, {233, "ForwardMail"}, {234, "Save"}, {235, "Documents"},
  {236, "Battery"}, {237, "Bluetooth"}, {238, "WLAN"}, {239, "UWB"},
  {240, "Unknown240"}, {241, "VideoNext"}, {242, "VideoPrev"},
  {243, "BrightnessCycle"}, {244, "BrightnessAuto"}, {245, "DisplayOff"},
  {246, "WWAN"}, {247, "RFKill"}, {248, "MicMute"},
  {0x160, "OK"}, {0x161, "Select"}, {0x162, "Goto"}, {0x163, "Clear"},
  {0x164, "Power2"}, {0x165, "Option"}, {0x166, "Info"}, {0x167, "Time"},
  {0x168, "Vendor"}, {0x169, "Archive"}, {0x16a, "Program"}, {0x16b, "Channel"},
  {0x16c, "Favorites"}, {0x16d, "EPG"}, {0x16e, "PVR"}, {0x16f, "MHP"},
  {0x170, "Language"}, {0x171, "Title"}, {0x172, "Subtitle"}, {0x173, "Angle"},
  {0x174, "FullScreen"}, {0x175, "Mode"}, {0x176, "Keyboard"}, {0x177, "AspectRatio"},
  {0x178, "PC"}, {0x179, "TV"}, {0x17a, "TV2"}, {0x17b, "VCR"}, {0x17c, "VCR2"},
  {0x17d, "Sat"}, {0x17e, "Sat2"}, {0x17f, "CD"}, {0x180, "Tape"}, {0x181, "Radio"},
  {0x182, "Tuner"}, {0x183, "Player"}, {0x184, "Text"}, {0x185, "DVD"},
  {0x186, "Aux"}, {0x187, "MP3"}, {0x188, "Audio"}, {0x189, "Video"},
  {0x18a, "Directory"}, {0x18b, "List"}, {0x18c, "Memo"}, {0x18d, "Calendar"},
  {0x18e, "Red"}, {0x18f, "Green"}, {0x190, "Yellow"}, {0x191, "Blue"},
  {0x192, "ChannelUp"}, {0x193, "ChannelDown"}, {0x194, "First"}, {0x195, "Last"},
  {0x196, "AB"}, {0x197, "Next"}, {0x198, "Restart"}, {0x199, "Slow"},
  {0x19a, "Shuffle"}, {0x19b, "Break"}, {0x19c, "Previous"}, {0x19d, "Digits"},
  {0x19e, "Teen"}, {0x19f, "Twen"}, {0x1a0, "VideoPhone"}, {0x1a1, "Games"},
  {0x1a2, "ZoomIn"}, {0x1a3, "ZoomOut"}, {0x1a4, "ZoomReset"},
  {0x1a5, "WordProcessor"}, {0x1a6, "Editor"}, {0x1a7, "Spreadsheet"},
  {0x1a8, "GraphicsEditor"}, {0x1a9, "Presentation"}, {0x1aa, "Database"},
  {0x1ab, "News"}, {0x1ac, "VoiceMail"}, {0x1ad, "AddressBook"}, {0x1ae, "Messenger"},
  {0x1af, "DisplayToggle"}, {0x1b0, "SpellCheck"}, {0x1b1, "Logoff"},
  {0x1b2, "Dollar"}, {0x1b3, "Euro"}, {0x1b4, "FrameBack"}, {0x1b5, "FrameForward"},
  {0x1b6, "ContextMenu2"}, {0x1b7, "MediaRepeat"}, {0x1b8, "10ChannelsUp"},
  {0x1b9, "10ChannelsDown"}, {0x1ba, "Images"}, {0x1bc, "NotificationCenter"},
  {0x1bd, "PickupPhone"}, {0x1be, "HangupPhone"}, {0x1bf, "LinkPhone"},
  {0x1c0, "DelEol"}, {0x1c1, "DelEos"}, {0x1c2, "InsLine"}, {0x1c3, "DelLine"},
  {0x1d0, "Fn"}, {0x1d1, "FnEsc"}, {0x1d2, "FnF1"}, {0x1d3, "FnF2"},
  {0x1d4, "FnF3"}, {0x1d5, "FnF4"}, {0x1d6, "FnF5"}, {0x1d7, "FnF6"},
  {0x1d8, "FnF7"}, {0x1d9, "FnF8"}, {0x1da, "FnF9"}, {0x1db, "FnF10"},
  {0x1dc, "FnF11"}, {0x1dd, "FnF12"}, {0x1de, "Fn1"}, {0x1df, "Fn2"},
  {0x1e0, "FnD"}, {0x1e1, "FnE"}, {0x1e2, "FnF"}, {0x1e3, "FnS"}, {0x1e4, "FnB"},
  {0x1e5, "FnRightShift"},
  {0x1f1, "BrlDot1"}, {0x1f2, "BrlDot2"}, {0x1f3, "BrlDot3"}, {0x1f4, "BrlDot4"},
  {0x1f5, "BrlDot5"}, {0x1f6, "BrlDot6"}, {0x1f7, "BrlDot7"}, {0x1f8, "BrlDot8"},
  {0x1f9, "BrlDot9"}, {0x1fa, "BrlDot10"},
  {0x200, "Numeric0"}, {0x201, "Numeric1"}, {0x202, "Numeric2"}, {0x203, "Numeric3"},
  {0x204, "Numeric4"}, {0x205, "Numeric5"}, {0x206, "Numeric6"}, {0x207, "Numeric7"},
  {0x208, "Numeric8"}, {0x209, "Numeric9"}, {0x20a, "NumericStar"},
  {0x20b, "NumericPound"}, {0x20c, "NumericA"}, {0x20d, "NumericB"},
  {0x20e, "NumericC"}, {0x20f, "NumericD"}, {0x210, "CameraFocus"},
  {0x211, "WpsButton"}, {0x212, "TouchpadToggle"}, {0x213, "TouchpadOn"},
  {0x214, "TouchpadOff"}, {0x215, "CameraZoomIn"}, {0x216, "CameraZoomOut"},
  {0x217, "CameraUp"}, {0x218, "CameraDown"}, {0x219, "CameraLeft"},
  {0x21a, "CameraRight"}, {0x21b, "AttendantOn"}, {0x21c, "AttendantOff"},
  {0x21d, "AttendantToggle"}, {0x21e, "LightsToggle"},
  {0x230, "AlsToggle"}, {0x231, "RotateLockToggle"}, {0x232, "RefreshRateToggle"},
  {0x240, "Buttonconfig"}, {0x241, "TaskManager"}, {0x242, "Journal"},
  {0x243, "ControlPanel"}, {0x244, "AppSelect"}, {0x245, "Screensaver"},
  {0x246, "VoiceCommand"}, {0x247, "Assistant"}, {0x248, "KbdLayoutNext"},
  {0x249, "EmojiPicker"}, {0x24a, "Dictate"}, {0x24b, "CameraAccessEnable"},
  {0x24c, "CameraAccessDisable"}, {0x24d, "CameraAccessToggle"},
  {0x24e, "Accessibility"}, {0x24f, "DoNotDisturb"},
  {0x250, "BrightnessMin"}, {0x251, "BrightnessMax"},
  {0x260, "KbdInputAssistPrev"}, {0x261, "KbdInputAssistNext"},
  {0x262, "KbdInputAssistPrevGroup"}, {0x263, "KbdInputAssistNextGroup"},
  {0x264, "KbdInputAssistAccept"}, {0x265, "KbdInputAssistCancel"},
  {0x266, "RightUp"}, {0x267, "RightDown"}, {0x268, "LeftUp"}, {0x269, "LeftDown"},
  {0x26a, "RootMenu"}, {0x26b, "MediaTopMenu"}, {0x26c, "Numeric11"},
  {0x26d, "Numeric12"}, {0x26e, "AudioDesc"}, {0x26f, "3DMode"},
  {0x270, "NextFavorite"}, {0x271, "StopRecord"}, {0x272, "PauseRecord"},
  {0x273, "VOD"}, {0x274, "Unmute"}, {0x275, "FastReverse"}, {0x276, "SlowReverse"},
  {0x277, "Data"}, {0x278, "OnscreenKeyboard"}, {0x279, "PrivacyScreenToggle"},
  {0x27a, "SelectiveScreenshot"}, {0x27b, "NextElement"}, {0x27c, "PreviousElement"},
  {0x27d, "AutopilotEngageToggle"}, {0x27e, "MarkWaypoint"}, {0x27f, "Sos"},
  {0x280, "NavChart"}, {0x281, "FishingChart"}, {0x282, "SingleRangeRadar"},
  {0x283, "DualRangeRadar"}, {0x284, "RadarOverlay"}, {0x285, "TraditionalSonar"},
  {0x286, "ClearvuSonar"}, {0x287, "SidevuSonar"}, {0x288, "NavInfo"},
  {0x289, "BrightnessMenu"},
  {0x290, "Macro1"}, {0x291, "Macro2"}, {0x292, "Macro3"}, {0x293, "Macro4"},
  {0x294, "Macro5"}, {0x295, "Macro6"}, {0x296, "Macro7"}, {0x297, "Macro8"},
  {0x298, "Macro9"}, {0x299, "Macro10"}, {0x29a, "Macro11"}, {0x29b, "Macro12"},
  {0x29c, "Macro13"}, {0x29d, "Macro14"}, {0x29e, "Macro15"}, {0x29f, "Macro16"},
  {0x2a0, "Macro17"}, {0x2a1, "Macro18"}, {0x2a2, "Macro19"}, {0x2a3, "Macro20"},
  {0x2a4, "Macro21"}, {0x2a5, "Macro22"}, {0x2a6, "Macro23"}, {0x2a7, "Macro24"},
  {0x2a8, "Macro25"}, {0x2a9, "Macro26"}, {0x2aa, "Macro27"}, {0x2ab, "Macro28"},
  {0x2ac, "Macro29"}, {0x2ad, "Macro30"},
  {0x2b0, "MacroRecordStart"}, {0x2b1, "MacroRecordStop"},
  {0x2b2, "MacroPresetCycle"}, {0x2b3, "MacroPreset1"}, {0x2b4, "MacroPreset2"},
  {0x2b5, "MacroPreset3"}, {0x2b8, "KbdLcdMenu1"}, {0x2b9, "KbdLcdMenu2"},
  {0x2ba, "KbdLcdMenu3"}, {0x2bb, "KbdLcdMenu4"}, {0x2bc, "KbdLcdMenu5"},
};

static const std::unordered_map<int, std::string>& KeyCodeToNameMap() {
  static const std::unordered_map<int, std::string> m = [] {
    std::unordered_map<int, std::string> r;
    for (const auto& e : kOriginalNames) r.emplace(e.code, e.name);
    for (const auto& e : kExtraNames) r.emplace(e.code, e.name);  // emplace: originals win
    return r;
  }();
  return m;
}

static std::string GetKeyName(int code) {
  const auto& m = KeyCodeToNameMap();
  auto it = m.find(code);
  if (it != m.end()) return it->second;
  return "unknown-" + std::to_string(code);
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

static std::string EscapeJsonString(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += (char)c;
        }
    }
  }
  return out;
}

static std::string PressingKeysToJson(const int* keys, size_t n) {
  if (!keys || n == 0) return "[]";
  std::string out = "[";
  for (size_t i = 0; i < n; ++i) {
    if (i) out += ',';
    std::string name = EscapeJsonString(GetKeyName(keys[i]));
    out += "{\"keyCode\":";
    out += std::to_string(keys[i]);
    out += ",\"keyName\":\"";
    out += name;
    out += "\",\"enKeyName\":\"";
    out += name;
    out += "\"}";
  }
  out += "]";
  return out;
}

// Parses [["A","B"],["C"]] (same tolerant approach as the original: only
// bracketed groups of quoted strings are considered; JSON escapes are decoded).
static std::vector<std::vector<std::string>> ParseShortcutsJson(const std::string& raw) {
  std::vector<std::vector<std::string>> result;
  size_t b = 0, e = raw.size();
  while (b < e && isspace((unsigned char)raw[b])) ++b;
  while (e > b && isspace((unsigned char)raw[e - 1])) --e;
  if (e - b < 2 || raw[b] != '[' || raw[e - 1] != ']') return result;
  std::string s = raw.substr(b + 1, e - b - 2);
  size_t pos = 0;
  while (pos < s.size()) {
    size_t open = s.find('[', pos);
    if (open == std::string::npos) break;
    size_t i = open + 1;
    bool inStr = false;
    std::vector<std::string> group;
    std::string cur;
    for (; i < s.size(); ++i) {
      char c = s[i];
      if (inStr) {
        if (c == '\\' && i + 1 < s.size()) {
          char n = s[++i];
          switch (n) {
            case 'n': cur += '\n'; break;
            case 't': cur += '\t'; break;
            case 'r': cur += '\r'; break;
            case 'b': cur += '\b'; break;
            case 'f': cur += '\f'; break;
            case 'u':
              if (i + 4 < s.size()) {
                unsigned v = (unsigned)strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
                if (v < 0x80) cur += (char)v;
                else if (v < 0x800) { cur += (char)(0xc0 | (v >> 6)); cur += (char)(0x80 | (v & 0x3f)); }
                else { cur += (char)(0xe0 | (v >> 12)); cur += (char)(0x80 | ((v >> 6) & 0x3f)); cur += (char)(0x80 | (v & 0x3f)); }
                i += 4;
              }
              break;
            default: cur += n;
          }
        } else if (c == '"') {
          inStr = false;
          group.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      } else if (c == '"') {
        inStr = true;
      } else if (c == ']') {
        break;
      }
    }
    if (!group.empty()) result.push_back(group);
    pos = i + 1;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Keyboard state (keyboard_state.cpp)
// ---------------------------------------------------------------------------

struct KeyboardState {
  KeyboardCallback userCallback = nullptr;
  std::unordered_set<int> initKeySet;      // keys already down when a device was opened
  std::vector<int> pressingKeys;           // in press order
  std::unordered_set<int> filteredKeys;    // key-downs we swallowed
  std::unordered_map<int, uint64_t> keyDownTimestampsMilliseconds;
  std::vector<std::set<std::string>> targetShortcuts;
  std::set<std::string> currentKeySet;
  bool keySetDirty = true;
  bool isLocked = false;
  bool transaction = false;
  std::set<std::string> lockedShortcut;
  uint64_t lastKeyboardEventTimeMilliseconds = 0;
  double watcherIntervalSeconds = 5.0;

  void ResetAll() {
    userCallback = nullptr;
    initKeySet.clear();
    pressingKeys.clear();
    filteredKeys.clear();
    keyDownTimestampsMilliseconds.clear();
    targetShortcuts.clear();
    currentKeySet.clear();
    keySetDirty = true;
    isLocked = false;
    transaction = false;
    lockedShortcut.clear();
    lastKeyboardEventTimeMilliseconds = 0;
    watcherIntervalSeconds = 5.0;
  }
};

static std::mutex g_stateLock;  // original: spin mutex
static KeyboardState g_keyboardState;

static void UpdateCurrentKeySet(KeyboardState* s) {
  if (!s || !s->keySetDirty) return;
  s->currentKeySet.clear();
  for (int k : s->pressingKeys) s->currentKeySet.insert(GetKeyName(k));
  s->keySetDirty = false;
}

static const std::set<std::string>* GetMatchedShortcut(KeyboardState* s) {
  if (!s || s->targetShortcuts.empty()) return nullptr;
  UpdateCurrentKeySet(s);
  for (const auto& sc : s->targetShortcuts)
    if (sc == s->currentKeySet) return &sc;
  return nullptr;
}

static bool IsLockedShortcutContained(KeyboardState* s) {
  if (s->lockedShortcut.empty()) return false;
  UpdateCurrentKeySet(s);
  return std::includes(s->currentKeySet.begin(), s->currentKeySet.end(),
                       s->lockedShortcut.begin(), s->lockedShortcut.end());
}

static void ResetPressingState(KeyboardState* s) {
  if (!s) return;
  s->pressingKeys.clear();
  s->initKeySet.clear();
  s->filteredKeys.clear();
  s->keyDownTimestampsMilliseconds.clear();
  s->isLocked = false;
  s->lockedShortcut.clear();
  s->transaction = false;
  s->keySetDirty = true;
  s->currentKeySet.clear();
}

struct ProcessResult { bool blocked; bool valid; bool changed; };

static ProcessResult ProcessKeyEvent(KeyboardState* s, int code, bool isDown) {
  if (!s) return {false, false, false};
  s->lastKeyboardEventTimeMilliseconds = NowMs();
  auto& pk = s->pressingKeys;
  if (isDown) {
    bool changed = false;
    if (std::find(pk.begin(), pk.end(), code) == pk.end()) {
      pk.push_back(code);
      s->keySetDirty = true;
      changed = true;
      s->keyDownTimestampsMilliseconds[code] = NowMs();
    }
    if (!s->isLocked) {
      if (auto m = GetMatchedShortcut(s)) { s->isLocked = true; s->lockedShortcut = *m; }
    } else {
      if (auto m = GetMatchedShortcut(s)) s->lockedShortcut = *m;
    }
    if (!s->isLocked) {
      if (s->transaction && pk.empty()) s->transaction = false;
      return {false, true, changed};
    }
    s->transaction = true;
    if (pk.empty()) s->transaction = false;
    if (s->initKeySet.count(code)) return {false, true, changed};
    if (!s->lockedShortcut.empty() && !s->lockedShortcut.count(GetKeyName(code)))
      return {false, true, changed};
    s->filteredKeys.insert(code);
    return {true, true, changed};
  }

  auto it = std::find(pk.begin(), pk.end(), code);
  if (it != pk.end()) {
    pk.erase(it);
    s->keySetDirty = true;
    s->keyDownTimestampsMilliseconds.erase(code);
  }
  if (s->isLocked) {
    if (IsLockedShortcutContained(s)) {
      if (s->transaction && pk.empty()) s->transaction = false;
      if (s->initKeySet.count(code)) {
        s->initKeySet.erase(code);
      } else if (s->lockedShortcut.empty() || s->lockedShortcut.count(GetKeyName(code))) {
        s->filteredKeys.erase(code);
        return {true, true, false};
      }
    } else {
      s->isLocked = false;
      s->lockedShortcut.clear();
      if (s->transaction && pk.empty()) s->transaction = false;
    }
  } else if (s->transaction && pk.empty()) {
    s->transaction = false;
  }
  bool blocked = s->filteredKeys.erase(code) > 0;
  s->initKeySet.erase(code);
  return {blocked, true, false};
}

// Drops pressing keys that are no longer physically down. Returns true if
// anything changed.
static bool ReconcilePressingKeysWithPhysicalState(KeyboardState* s,
                                                   const std::unordered_set<int>& down) {
  if (!s) return false;
  std::vector<int> kept;
  kept.reserve(s->pressingKeys.size());
  bool removed = false;
  for (int k : s->pressingKeys) {
    if (down.count(k)) {
      kept.push_back(k);
    } else {
      s->keyDownTimestampsMilliseconds.erase(k);
      s->filteredKeys.erase(k);
      s->initKeySet.erase(k);
      removed = true;
    }
  }
  if (!removed) return false;
  s->pressingKeys.swap(kept);
  s->keySetDirty = true;
  if (s->isLocked && !IsLockedShortcutContained(s)) {
    s->isLocked = false;
    s->lockedShortcut.clear();
    s->transaction = false;
  } else if (s->transaction && s->pressingKeys.empty()) {
    s->transaction = false;
  }
  DebugLog("Reconciled stale pressing keys with physical state");
  return true;
}

static std::vector<std::set<std::string>> ParseTargetShortcutSets(const char* json) {
  std::vector<std::set<std::string>> out;
  if (!json) return out;
  for (auto& g : ParseShortcutsJson(json)) {
    std::set<std::string> st(g.begin(), g.end());
    st.erase(std::string());
    if (st.empty()) continue;
    if (std::find(out.begin(), out.end(), st) == out.end()) out.push_back(std::move(st));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Device helpers (device_info.cpp)
// ---------------------------------------------------------------------------

static inline bool TestBit(const unsigned char* bits, int bit) {
  return (bits[bit / 8] >> (bit % 8)) & 1;
}

static bool IsInputEventNodeName(const char* name) {
  if (!name || strncmp(name, "event", 5) != 0 || !name[5]) return false;
  for (const char* p = name + 5; *p; ++p)
    if (*p < '0' || *p > '9') return false;
  return true;
}

static std::string MakeInputEventPath(const char* name) {
  return std::string("/dev/input/") + name;
}

static std::vector<std::string> ListInputEventNodePaths() {
  std::vector<std::string> out;
  DIR* d = opendir("/dev/input");
  if (!d) return out;
  while (dirent* e = readdir(d))
    if (IsInputEventNodeName(e->d_name)) out.push_back(MakeInputEventPath(e->d_name));
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

static std::string ToLower(std::string s) {
  for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 32;
  return s;
}

enum class DeviceKind { None, Keyboard, Passive };

struct DeviceCaps {
  unsigned char ev[(EV_MAX + 8) / 8];
  unsigned char keys[(KEY_MAX + 8) / 8];
  unsigned char leds[(LED_MAX + 8) / 8];
  std::string name;
};

static bool QueryCaps(int fd, DeviceCaps* c) {
  memset(c->ev, 0, sizeof c->ev);
  memset(c->keys, 0, sizeof c->keys);
  memset(c->leds, 0, sizeof c->leds);
  c->name.clear();
  if (ioctl(fd, EVIOCGBIT(0, sizeof c->ev), c->ev) < 0) return false;
  if (TestBit(c->ev, EV_KEY) && ioctl(fd, EVIOCGBIT(EV_KEY, sizeof c->keys), c->keys) < 0) return false;
  if (TestBit(c->ev, EV_LED)) ioctl(fd, EVIOCGBIT(EV_LED, sizeof c->leds), c->leds);
  char name[256] = {0};
  if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) >= 0) c->name = name;
  return true;
}

static bool IsFullKeyboard(const DeviceCaps& c) {
  return TestBit(c.ev, EV_KEY) && TestBit(c.keys, KEY_A) && TestBit(c.keys, KEY_ENTER);
}

static bool HasKeyboardOrConsumerKeys(const DeviceCaps& c) {
  if (!TestBit(c.ev, EV_KEY)) return false;
  for (int k = 1; k < BTN_MISC; ++k) if (TestBit(c.keys, k)) return true;       // 1..0xff
  for (int k = KEY_OK; k < kKeyMax; ++k) {                                       // 0x160..0x2fe
    if (k >= BTN_DPAD_UP && k <= BTN_DPAD_RIGHT) continue;
    if (k >= BTN_TRIGGER_HAPPY) break;
    if (TestBit(c.keys, k)) return true;
  }
  return false;
}

static bool NameExcluded(const std::string& name) {
  if (name.find(kHelperDeviceName) != std::string::npos) return true;
  const char* f = getenv("KEYBOARD_HELPER_DEVICE_FILTER");
  if (f && *f && name.find(f) == std::string::npos) return true;
  return false;
}

static DeviceKind ClassifyDevice(const DeviceCaps& c) {
  if (NameExcluded(c.name)) return DeviceKind::None;
  if (IsFullKeyboard(c)) {
    // Passive by default so other hotkey readers (e.g. Wispr Flow) keep
    // working; KEYBOARD_HELPER_NO_GRAB=0 restores upstream grabbing.
    const char* ng = getenv("KEYBOARD_HELPER_NO_GRAB");
    bool noGrab = !(ng && strcmp(ng, "0") == 0);
    bool combo = TestBit(c.ev, EV_REL) || TestBit(c.ev, EV_ABS);
    return (noGrab || combo) ? DeviceKind::Passive : DeviceKind::Keyboard;
  }
  return HasKeyboardOrConsumerKeys(c) ? DeviceKind::Passive : DeviceKind::None;
}

struct KeyboardDeviceInfo { std::string deviceName; bool isBuiltIn; };

static std::vector<KeyboardDeviceInfo> EnumerateKeyboardDevices() {
  std::vector<KeyboardDeviceInfo> out;
  for (const auto& path : ListInputEventNodePaths()) {
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    DeviceCaps c;
    bool ok = QueryCaps(fd, &c) && IsFullKeyboard(c);
    close(fd);
    if (!ok) continue;
    std::string name = c.name.empty() ? path : c.name;
    if (name.find(kHelperDeviceName) != std::string::npos) continue;
    std::string ln = ToLower(name), lp = ToLower(path);
    bool builtIn = ln.find("at translated set 2") != std::string::npos ||
                   lp.find("platform-") != std::string::npos ||
                   ln.find("laptop") != std::string::npos ||
                   ln.find("built-in") != std::string::npos;
    out.push_back({name, builtIn});
  }
  std::sort(out.begin(), out.end(),
            [](const KeyboardDeviceInfo& a, const KeyboardDeviceInfo& b) { return a.deviceName < b.deviceName; });
  out.erase(std::unique(out.begin(), out.end(),
                        [](const KeyboardDeviceInfo& a, const KeyboardDeviceInfo& b) { return a.deviceName == b.deviceName; }),
            out.end());
  return out;
}

static std::string BuildKeyboardDeviceListJson() {
  std::string out = "[";
  bool first = true;
  for (const auto& d : EnumerateKeyboardDevices()) {
    if (!first) out += ',';
    first = false;
    out += "{\"deviceName\":\"" + EscapeJsonString(d.deviceName) + "\",\"isBuiltIn\":";
    out += d.isBuiltIn ? "true" : "false";
    out += "}";
  }
  out += "]";
  return out;
}

static std::string ReadXkbLayout() {
  std::ifstream f("/etc/default/keyboard");
  std::string line;
  while (f && std::getline(f, line)) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq), val = line.substr(eq + 1);
    if (!val.empty() && (val[0] == '"' || val[0] == '\'')) {
      size_t n = val.size() >= 2 ? val.size() - 2 : 0;
      val = val.substr(1, n);
    }
    if (key == "XKBLAYOUT" && !val.empty()) return val;
  }
  return "us";
}

struct DetailKey { int code; const char* normal; const char* shift; };
static const DetailKey kDetailKeys[] = {
  {2, "1", "!"}, {3, "2", "@"}, {4, "3", "#"}, {5, "4", "$"}, {6, "5", "%"},
  {7, "6", "^"}, {8, "7", "&"}, {9, "8", "*"}, {10, "9", "("}, {11, "0", ")"},
  {12, "-", "_"}, {13, "=", "+"}, {16, "q", "Q"}, {17, "w", "W"}, {18, "e", "E"},
  {19, "r", "R"}, {20, "t", "T"}, {21, "y", "Y"}, {22, "u", "U"}, {23, "i", "I"},
  {24, "o", "O"}, {25, "p", "P"}, {26, "[", "{"}, {27, "]", "}"}, {30, "a", "A"},
  {31, "s", "S"}, {32, "d", "D"}, {33, "f", "F"}, {34, "g", "G"}, {35, "h", "H"},
  {36, "j", "J"}, {37, "k", "K"}, {38, "l", "L"}, {39, ";", ":"}, {40, "'", "\""},
  {41, "`", "~"}, {43, "\\", "|"}, {44, "z", "Z"}, {45, "x", "X"}, {46, "c", "C"},
  {47, "v", "V"}, {48, "b", "B"}, {49, "n", "N"}, {50, "m", "M"}, {51, ",", "<"},
  {52, ".", ">"}, {53, "/", "?"}, {57, " ", " "},
};

static std::string BuildKeyboardLayoutInfoJson(bool detail) {
  std::string l = EscapeJsonString(ReadXkbLayout());
  std::string out = "[{";
  out += "\"layoutId\":\"" + l + "\",";
  out += "\"languageId\":\"" + l + "\",";
  out += "\"layoutName\":\"" + l + "\",";
  out += "\"languageName\":\"" + l + "\",";
  out += "\"languageNameEnglish\":\"" + l + "\",";
  out += "\"countryName\":\"\",";
  out += "\"displayName\":\"" + l + "\",";
  out += "\"layoutDisplayName\":\"" + l + "\",";
  out += "\"isSupportAltgr\":false";
  if (detail) {
    out += ",\"keys\":[";
    bool first = true;
    for (const auto& k : kDetailKeys) {
      if (!first) out += ',';
      first = false;
      out += "{\"vkCode\":" + std::to_string(k.code);
      out += ",\"keyName\":\"" + EscapeJsonString(GetKeyName(k.code)) + "\",";
      out += "\"scanCode\":" + std::to_string(k.code);
      out += ",\"normal\":\"" + EscapeJsonString(k.normal) + "\",";
      out += "\"shift\":\"" + EscapeJsonString(k.shift) + "\",";
      out += "\"altGr\":null,\"shiftAltGr\":null}";
    }
    out += "]";
  }
  out += "}]";
  return out;
}

// ---------------------------------------------------------------------------
// Callback dispatcher (a dedicated thread so the monitor thread never blocks
// on JS; koffi marshals calls from foreign threads onto the JS main thread and
// blocks the calling thread until the JS function returns)
// ---------------------------------------------------------------------------

struct PendingUserCallback {
  KeyboardCallback cb;
  int keyCode;
  bool isKeyDown;
  bool isBlocked;
  std::vector<int> keys;
};

struct Dispatcher {
  std::mutex m;
  std::condition_variable cv;
  std::deque<PendingUserCallback> queue;
  bool stop = false;
  bool inCallback = false;
  std::thread thread;
};

static std::mutex g_dispatcherPtrMutex;
static std::shared_ptr<Dispatcher> g_dispatcher;

static void CallbackDispatcherMain(std::shared_ptr<Dispatcher> d) {
  DebugLog("Callback dispatcher started");
  for (;;) {
    PendingUserCallback task;
    {
      std::unique_lock<std::mutex> lk(d->m);
      d->cv.wait(lk, [&] { return d->stop || !d->queue.empty(); });
      if (d->stop) break;
      task = std::move(d->queue.front());
      d->queue.pop_front();
      d->inCallback = true;
    }
    std::string json = PressingKeysToJson(task.keys.data(), task.keys.size());
    try {
      task.cb(task.keyCode, task.isKeyDown ? 1 : 0, task.isBlocked ? 1 : 0, json.c_str());
    } catch (...) {
      DebugLog("Exception in user callback");
    }
    {
      std::lock_guard<std::mutex> lk(d->m);
      d->inCallback = false;
    }
  }
  DebugLog("Callback dispatcher exiting");
}

static void StartCallbackDispatcher() {
  auto d = std::make_shared<Dispatcher>();
  d->thread = std::thread(CallbackDispatcherMain, d);
  std::lock_guard<std::mutex> lk(g_dispatcherPtrMutex);
  g_dispatcher = d;
}

static void StopCallbackDispatcher() {
  std::shared_ptr<Dispatcher> d;
  {
    std::lock_guard<std::mutex> lk(g_dispatcherPtrMutex);
    d.swap(g_dispatcher);
  }
  if (!d) return;
  bool busy;
  {
    std::lock_guard<std::mutex> lk(d->m);
    d->stop = true;
    d->queue.clear();
    busy = d->inCallback;
  }
  d->cv.notify_all();
  if (!d->thread.joinable()) return;
  if (busy || d->thread.get_id() == std::this_thread::get_id()) {
    // The dispatcher is waiting for the JS thread (which may be us). Let it
    // finish on its own; it holds a reference to its own context.
    d->thread.detach();
  } else {
    d->thread.join();
  }
}

static void EnqueueUserCallback(KeyboardCallback cb, int keyCode, bool isKeyDown, bool isBlocked,
                                const int* keys, size_t n) {
  if (!cb) return;
  std::shared_ptr<Dispatcher> d;
  {
    std::lock_guard<std::mutex> lk(g_dispatcherPtrMutex);
    d = g_dispatcher;
  }
  if (!d) return;
  {
    std::lock_guard<std::mutex> lk(d->m);
    if (d->stop) return;
    while (d->queue.size() >= 256) d->queue.pop_front();   // drop oldest (original cap)
    d->queue.push_back({cb, keyCode, isKeyDown, isBlocked, std::vector<int>(keys, keys + n)});
  }
  d->cv.notify_one();
}

// ---------------------------------------------------------------------------
// Monitor thread (monitor_thread.cpp)
// ---------------------------------------------------------------------------

struct DeviceContext {
  std::string path;
  std::string name;
  int eventFileDescriptor = -1;
  int uinputFileDescriptor = -1;          // -1 => passive (not grabbed)
  std::unordered_set<uint16_t> injectedDownKeys;
  bool recoveringFromDrop = false;
};

struct DeviceRegistry {
  std::unordered_map<int, std::unique_ptr<DeviceContext>> byFileDescriptor;
  std::unordered_map<std::string, int> pathToFileDescriptor;
  std::unordered_map<int, DeviceContext*> byUinputFd;
};

struct PendingHotplugOpen {
  std::string path;
  int retriesLeft;
  uint64_t nextAttemptMs;
};

static std::mutex g_lifecycleMutex;
static std::thread g_monitorThread;
static std::atomic<bool> g_shouldStop{false};
static std::atomic<bool> g_monitorRunning{false};
static std::atomic<bool> g_monitorExited{false};
static int g_stopEventFd = -1;

static bool WriteAll(int fd, const void* buf, size_t len) {
  for (;;) {
    ssize_t r = write(fd, buf, len);
    if (r < 0 && errno == EINTR) continue;
    return r == (ssize_t)len;
  }
}

static void EmitRaw(DeviceContext* ctx, uint16_t type, uint16_t code, int32_t value) {
  if (!ctx || ctx->uinputFileDescriptor < 0) return;
  input_event ev[2];
  memset(ev, 0, sizeof ev);
  ev[0].type = type; ev[0].code = code; ev[0].value = value;
  ev[1].type = EV_SYN; ev[1].code = SYN_REPORT; ev[1].value = 0;
  if (!WriteAll(ctx->uinputFileDescriptor, ev, sizeof ev))
    DebugLog("uinput write failed: %s", strerror(errno));
}

static void EmitTrackedKey(DeviceContext* ctx, uint16_t code, int value) {
  if (!ctx || ctx->uinputFileDescriptor < 0) return;
  input_event ev[2];
  memset(ev, 0, sizeof ev);
  ev[0].type = EV_KEY; ev[0].code = code; ev[0].value = value;
  ev[1].type = EV_SYN; ev[1].code = SYN_REPORT; ev[1].value = 0;
  if (!WriteAll(ctx->uinputFileDescriptor, ev, sizeof ev)) {
    DebugLog("uinput write failed: %s", strerror(errno));
    return;
  }
  if (value == 1) ctx->injectedDownKeys.insert(code);
  else if (value == 0) ctx->injectedDownKeys.erase(code);
}

static bool ReadKeyState(int fd, unsigned char* bits, size_t len) {
  memset(bits, 0, len);
  return ioctl(fd, EVIOCGKEY(len), bits) >= 0;
}

static void ReleaseStaleInjectedKeys(DeviceContext* ctx, const unsigned char* keyState) {
  if (!ctx || ctx->uinputFileDescriptor < 0 || ctx->injectedDownKeys.empty()) return;
  std::vector<uint16_t> stale;
  for (uint16_t k : ctx->injectedDownKeys)
    if (k >= kKeyMax || !TestBit(keyState, k)) stale.push_back(k);
  for (uint16_t k : stale) {
    DebugLog("releasing stale injected key %s", GetKeyName(k).c_str());
    EmitTrackedKey(ctx, k, 0);
  }
}

static void CloseDevice(DeviceContext* ctx) {
  if (!ctx) return;
  if (ctx->uinputFileDescriptor >= 0) {
    std::vector<uint16_t> down(ctx->injectedDownKeys.begin(), ctx->injectedDownKeys.end());
    for (uint16_t k : down) EmitTrackedKey(ctx, k, 0);
  }
  if (ctx->eventFileDescriptor >= 0) {
    if (ctx->uinputFileDescriptor >= 0) ioctl(ctx->eventFileDescriptor, EVIOCGRAB, 0);
    close(ctx->eventFileDescriptor);
    ctx->eventFileDescriptor = -1;
  }
  if (ctx->uinputFileDescriptor >= 0) {
    ioctl(ctx->uinputFileDescriptor, UI_DEV_DESTROY);
    close(ctx->uinputFileDescriptor);
    ctx->uinputFileDescriptor = -1;
  }
}

static int CreateUinputClone(const DeviceCaps& caps) {
  int ufd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (ufd < 0) {
    DebugLog("open /dev/uinput failed: %s", strerror(errno));
    return -1;
  }
  bool ok = ioctl(ufd, UI_SET_EVBIT, EV_KEY) >= 0 && ioctl(ufd, UI_SET_EVBIT, EV_SYN) >= 0 &&
            ioctl(ufd, UI_SET_EVBIT, EV_MSC) >= 0;
  if (!ok) { close(ufd); return -1; }
  // The original enables every key code 1..0x2ff on the clone.
  for (int k = 1; k < kKeyMax + 1 && k <= KEY_MAX; ++k) ioctl(ufd, UI_SET_KEYBIT, k);
  ioctl(ufd, UI_SET_MSCBIT, MSC_SCAN);
  if (TestBit(caps.ev, EV_LED)) {
    ioctl(ufd, UI_SET_EVBIT, EV_LED);
    for (int l = 0; l <= LED_MAX; ++l)
      if (TestBit(caps.leds, l)) ioctl(ufd, UI_SET_LEDBIT, l);
  }
  uinput_setup us;
  memset(&us, 0, sizeof us);
  us.id.bustype = BUS_USB;
  us.id.vendor = 0x5459;
  us.id.product = 0x4c45;
  us.id.version = 1;
  snprintf(us.name, sizeof us.name, "%s", kHelperDeviceName);
  if (ioctl(ufd, UI_DEV_SETUP, &us) < 0 || ioctl(ufd, UI_DEV_CREATE) < 0) {
    DebugLog("uinput setup failed: %s", strerror(errno));
    close(ufd);
    return -1;
  }
  return ufd;
}

static std::unique_ptr<DeviceContext> OpenKeyboardDevice(const std::string& path) {
  // O_RDWR so LED state can be written back to grabbed keyboards.
  int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return nullptr;
  DeviceCaps caps;
  if (!QueryCaps(fd, &caps)) { close(fd); return nullptr; }
  DeviceKind kind = ClassifyDevice(caps);
  if (kind == DeviceKind::None) { close(fd); return nullptr; }
  int ufd = -1;
  if (kind == DeviceKind::Keyboard) {
    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
      DebugLog("EVIOCGRAB %s failed: %s", path.c_str(), strerror(errno));
      close(fd);
      return nullptr;
    }
    ufd = CreateUinputClone(caps);
    if (ufd < 0) {
      ioctl(fd, EVIOCGRAB, 0);
      close(fd);
      return nullptr;
    }
  }
  auto ctx = std::make_unique<DeviceContext>();
  ctx->path = path;
  ctx->name = caps.name.empty() ? path : caps.name;
  ctx->eventFileDescriptor = fd;
  ctx->uinputFileDescriptor = ufd;
  DebugLog("opened %s (%s) %s", path.c_str(), ctx->name.c_str(), ufd >= 0 ? "grabbed" : "passive");
  return ctx;
}

static void EpollAdd(int epfd, int fd) {
  epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
  ev.data.fd = fd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

static void UnregisterDeviceByFileDescriptor(int epfd, DeviceRegistry* reg, int fd) {
  auto it = reg->byFileDescriptor.find(fd);
  if (it == reg->byFileDescriptor.end()) return;
  DeviceContext* ctx = it->second.get();
  epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
  if (ctx->uinputFileDescriptor >= 0) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, ctx->uinputFileDescriptor, nullptr);
    reg->byUinputFd.erase(ctx->uinputFileDescriptor);
  }
  DebugLog("closing %s", ctx->path.c_str());
  reg->pathToFileDescriptor.erase(ctx->path);
  CloseDevice(ctx);
  reg->byFileDescriptor.erase(it);
}

static bool TryAddDeviceByPath(int epfd, DeviceRegistry* reg, KeyboardState* state,
                               const std::string& path) {
  if (reg->pathToFileDescriptor.count(path)) return false;
  auto ctx = OpenKeyboardDevice(path);
  if (!ctx) return false;
  int fd = ctx->eventFileDescriptor;
  if (reg->byFileDescriptor.count(fd)) { CloseDevice(ctx.get()); return false; }
  EpollAdd(epfd, fd);
  if (ctx->uinputFileDescriptor >= 0) {
    EpollAdd(epfd, ctx->uinputFileDescriptor);
    reg->byUinputFd[ctx->uinputFileDescriptor] = ctx.get();
  }
  unsigned char ks[(KEY_MAX + 8) / 8];
  if (ReadKeyState(fd, ks, sizeof ks)) {
    std::lock_guard<std::mutex> lk(g_stateLock);
    for (int k = 0; k < kKeyMax; ++k)
      if (TestBit(ks, k)) state->initKeySet.insert(k);
  }
  reg->pathToFileDescriptor[path] = fd;
  reg->byFileDescriptor[fd] = std::move(ctx);
  return true;
}

static void CollectPhysicallyDownKeys(const DeviceRegistry& reg, std::unordered_set<int>* out) {
  unsigned char ks[(KEY_MAX + 8) / 8];
  for (const auto& kv : reg.byFileDescriptor) {
    const DeviceContext* ctx = kv.second.get();
    if (!ctx || ctx->eventFileDescriptor < 0) continue;
    if (!ReadKeyState(ctx->eventFileDescriptor, ks, sizeof ks)) continue;
    for (int k = 0; k < kKeyMax; ++k)
      if (TestBit(ks, k)) out->insert(k);
  }
}

static void CancelPendingHotplugPath(std::vector<PendingHotplugOpen>* pending, const std::string& path) {
  pending->erase(std::remove_if(pending->begin(), pending->end(),
                                [&](const PendingHotplugOpen& p) { return p.path == path; }),
                 pending->end());
}

static size_t CopyPressing(const KeyboardState* s, int* out) {
  size_t n = std::min<size_t>(s->pressingKeys.size(), 64);
  for (size_t i = 0; i < n; ++i) out[i] = s->pressingKeys[i];
  return n;
}

// Handles a SYN_REPORT after SYN_DROPPED: resynchronise with the kernel state.
static void ResyncAfterDrop(DeviceRegistry& reg, KeyboardState* state, DeviceContext* ctx) {
  unsigned char ks[(KEY_MAX + 8) / 8];
  if (!ReadKeyState(ctx->eventFileDescriptor, ks, sizeof ks)) memset(ks, 0, sizeof ks);
  std::vector<int> devDown;
  for (int k = 0; k < kKeyMax; ++k) if (TestBit(ks, k)) devDown.push_back(k);
  std::unordered_set<int> phys;
  CollectPhysicallyDownKeys(reg, &phys);

  int keys[64];
  size_t n = 0;
  KeyboardCallback cb = nullptr;
  std::unordered_set<int> filtered;
  bool changed;
  {
    std::lock_guard<std::mutex> lk(g_stateLock);
    changed = ReconcilePressingKeysWithPhysicalState(state, phys);
    for (int k : devDown) {
      auto& pk = state->pressingKeys;
      if (std::find(pk.begin(), pk.end(), k) == pk.end()) {
        ProcessKeyEvent(state, k, true);
        changed = true;
      }
    }
    filtered = state->filteredKeys;
    if (changed) { n = CopyPressing(state, keys); cb = state->userCallback; }
  }
  ReleaseStaleInjectedKeys(ctx, ks);
  if (ctx->uinputFileDescriptor >= 0) {
    for (int k : devDown)
      if (!ctx->injectedDownKeys.count((uint16_t)k) && !filtered.count(k))
        EmitTrackedKey(ctx, (uint16_t)k, 1);
  }
  if (changed) EnqueueUserCallback(cb, kResetSentinel, false, false, keys, n);
}

static void HandleDeviceEvent(DeviceRegistry& reg, KeyboardState* state, DeviceContext* ctx,
                              const input_event& ev) {
  if (!state) return;
  if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
    ctx->recoveringFromDrop = true;
    return;
  }
  if (ctx->recoveringFromDrop) {
    if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
      ctx->recoveringFromDrop = false;
      ResyncAfterDrop(reg, state, ctx);
    }
    return;
  }
  const bool grabbed = ctx->uinputFileDescriptor >= 0;
  if (ev.type == EV_KEY) {
    const uint16_t code = ev.code;
    if (ev.value == 2) {  // autorepeat
      bool drop;
      {
        std::lock_guard<std::mutex> lk(g_stateLock);
        drop = state->filteredKeys.count(code) || state->isLocked;
      }
      if (drop || !grabbed) return;
      if (!ctx->injectedDownKeys.count(code)) EmitTrackedKey(ctx, code, 1);
      EmitTrackedKey(ctx, code, 2);
      return;
    }
    const bool isDown = ev.value == 1;
    int keys[64];
    size_t n = 0;
    KeyboardCallback cb = nullptr;
    ProcessResult r;
    {
      std::lock_guard<std::mutex> lk(g_stateLock);
      r = ProcessKeyEvent(state, code, isDown);
      if (r.valid) { n = CopyPressing(state, keys); cb = state->userCallback; }
    }
    if (r.blocked && grabbed) {
      if (r.valid) EnqueueUserCallback(cb, code, isDown, true, keys, n);
      return;
    }
    if (grabbed) {
      if (ctx->injectedDownKeys.count(code)) {
        if (ev.value == 1) EmitTrackedKey(ctx, code, 0);
        EmitTrackedKey(ctx, code, ev.value);
      } else {
        if (ev.value == 0) EmitTrackedKey(ctx, code, 1);
        EmitTrackedKey(ctx, code, ev.value);
      }
    }
    if (r.valid) EnqueueUserCallback(cb, code, isDown, false, keys, n);
    return;
  }
  if (ev.type == EV_SYN || ev.type == EV_LED) return;   // EV_SYN is re-synthesised
  if (grabbed) EmitRaw(ctx, ev.type, ev.code, ev.value);
}

static void HandleUinputReadable(DeviceContext* ctx) {
  input_event evs[16];
  for (;;) {
    ssize_t r = read(ctx->uinputFileDescriptor, evs, sizeof evs);
    if (r <= 0) return;
    size_t n = (size_t)r / sizeof(input_event);
    for (size_t i = 0; i < n; ++i) {
      if (evs[i].type == EV_LED && ctx->eventFileDescriptor >= 0) {
        input_event out[2];
        memset(out, 0, sizeof out);
        out[0].type = EV_LED; out[0].code = evs[i].code; out[0].value = evs[i].value;
        out[1].type = EV_SYN; out[1].code = SYN_REPORT;
        (void)!write(ctx->eventFileDescriptor, out, sizeof out);
      }
    }
  }
}

static void RaiseThreadPriority() {
  sched_param sp;
  memset(&sp, 0, sizeof sp);
  sp.sched_priority = 20;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
    if (setpriority(PRIO_PROCESS, 0, -10) != 0)
      DebugLog("could not raise monitor thread priority");
  }
}

static void RunWatcher(DeviceRegistry& reg, KeyboardState* state, uint64_t* lastCheck) {
  double interval;
  {
    std::lock_guard<std::mutex> lk(g_stateLock);
    interval = state->watcherIntervalSeconds;
  }
  if (!(interval > 0)) return;
  uint64_t now = NowMs();
  double msD = interval * 1000.0;
  uint64_t intervalMs = msD >= 1.8446744073709552e19 ? UINT64_MAX : (uint64_t)msD;
  if (*lastCheck != 0 && now - *lastCheck < intervalMs) return;
  uint64_t lastEvent;
  {
    std::lock_guard<std::mutex> lk(g_stateLock);
    lastEvent = state->lastKeyboardEventTimeMilliseconds;
  }
  if (lastEvent != 0 && now - lastEvent < intervalMs) { *lastCheck = now; return; }

  unsigned char ks[(KEY_MAX + 8) / 8];
  for (auto& kv : reg.byFileDescriptor) {
    DeviceContext* ctx = kv.second.get();
    if (!ctx || ctx->injectedDownKeys.empty()) continue;
    if (!ReadKeyState(ctx->eventFileDescriptor, ks, sizeof ks)) memset(ks, 0, sizeof ks);
    ReleaseStaleInjectedKeys(ctx, ks);
  }
  std::unordered_set<int> phys;
  CollectPhysicallyDownKeys(reg, &phys);
  int keys[64];
  size_t n = 0;
  KeyboardCallback cb = nullptr;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lk(g_stateLock);
    if (!state->pressingKeys.empty() || state->isLocked || !state->filteredKeys.empty()) {
      changed = ReconcilePressingKeysWithPhysicalState(state, phys);
      if (changed) { n = CopyPressing(state, keys); cb = state->userCallback; }
    }
  }
  if (changed) EnqueueUserCallback(cb, kResetSentinel, false, false, keys, n);
  *lastCheck = now;
}

static void MonitorThreadMain(KeyboardState* state) {
  DebugLog("Monitor thread started");
  RaiseThreadPriority();
  int epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) {
    g_monitorRunning = false;
    g_monitorExited = true;
    return;
  }
  if (g_stopEventFd >= 0) EpollAdd(epfd, g_stopEventFd);
  int inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (inotifyFd >= 0) {
    if (inotify_add_watch(inotifyFd, "/dev/input", IN_CREATE | IN_DELETE | IN_ATTRIB) < 0) {
      close(inotifyFd);
      inotifyFd = -1;
    } else {
      EpollAdd(epfd, inotifyFd);
      DebugLog("Hotplug watch enabled on /dev/input");
    }
  }

  DeviceRegistry reg;
  std::vector<PendingHotplugOpen> pending;
  for (const auto& p : ListInputEventNodePaths()) TryAddDeviceByPath(epfd, &reg, state, p);

  if (reg.byFileDescriptor.empty() && inotifyFd < 0) {
    DebugLog("No keyboards opened and hotplug unavailable");
    close(epfd);
    g_monitorRunning = false;
    g_monitorExited = true;
    return;
  }
  g_monitorRunning = true;

  uint64_t lastWatcherCheck = 0;
  epoll_event events[32];
  std::vector<char> buf(sizeof(input_event) * 64);
  alignas(inotify_event) char ibuf[4096];

  while (!g_shouldStop) {
    int nev = epoll_wait(epfd, events, 32, 200);
    if (nev < 0) {
      if (errno == EINTR) continue;
      DebugLog("epoll_wait failed: %s", strerror(errno));
      break;
    }

    if (!pending.empty()) {
      uint64_t now = NowMs();
      std::vector<PendingHotplugOpen> keep;
      for (auto& p : pending) {
        if (g_shouldStop) { keep.push_back(p); continue; }
        if (reg.pathToFileDescriptor.count(p.path)) continue;
        if (now < p.nextAttemptMs) { keep.push_back(p); continue; }
        if (TryAddDeviceByPath(epfd, &reg, state, p.path)) continue;
        if (--p.retriesLeft > 0) { p.nextAttemptMs = now + 25; keep.push_back(p); }
      }
      pending.swap(keep);
    }

    for (int i = 0; i < nev; ++i) {
      int fd = events[i].data.fd;
      uint32_t evmask = events[i].events;
      if (fd == g_stopEventFd) {
        uint64_t v;
        (void)!read(fd, &v, sizeof v);
        continue;
      }
      if (fd == inotifyFd) {
        for (;;) {
          ssize_t len = read(inotifyFd, ibuf, sizeof ibuf);
          if (len <= 0) break;
          for (ssize_t off = 0; off < len;) {
            auto* ie = reinterpret_cast<inotify_event*>(ibuf + off);
            off += sizeof(inotify_event) + ie->len;
            if (!ie->len || !IsInputEventNodeName(ie->name)) continue;
            std::string path = MakeInputEventPath(ie->name);
            if (ie->mask & IN_DELETE) {
              CancelPendingHotplugPath(&pending, path);
              auto it = reg.pathToFileDescriptor.find(path);
              if (it != reg.pathToFileDescriptor.end())
                UnregisterDeviceByFileDescriptor(epfd, &reg, it->second);
            } else if (ie->mask & (IN_CREATE | IN_ATTRIB)) {
              if (reg.pathToFileDescriptor.count(path)) continue;
              if (TryAddDeviceByPath(epfd, &reg, state, path)) {
                CancelPendingHotplugPath(&pending, path);
              } else {
                bool already = false;
                for (auto& p : pending) if (p.path == path) { already = true; break; }
                if (!already) pending.push_back({path, 8, NowMs()});
              }
            }
          }
        }
        continue;
      }
      auto uit = reg.byUinputFd.find(fd);
      if (uit != reg.byUinputFd.end()) {
        HandleUinputReadable(uit->second);
        continue;
      }
      auto it = reg.byFileDescriptor.find(fd);
      if (it == reg.byFileDescriptor.end()) continue;
      DeviceContext* ctx = it->second.get();
      if (evmask & (EPOLLERR | EPOLLHUP)) {
        UnregisterDeviceByFileDescriptor(epfd, &reg, fd);
        continue;
      }
      bool gone = false;
      for (;;) {
        ssize_t r = read(fd, buf.data(), buf.size());
        if (r < 0) {
          if (errno == EAGAIN || errno == EINTR) break;
          gone = true;   // ENODEV / ENOENT / other errors
          break;
        }
        if (r == 0) { gone = true; break; }
        if (r % sizeof(input_event)) break;
        size_t n = (size_t)r / sizeof(input_event);
        auto* evs = reinterpret_cast<input_event*>(buf.data());
        for (size_t k = 0; k < n; ++k) HandleDeviceEvent(reg, state, ctx, evs[k]);
      }
      if (gone) UnregisterDeviceByFileDescriptor(epfd, &reg, fd);
    }

    RunWatcher(reg, state, &lastWatcherCheck);
  }

  std::vector<int> fds;
  for (auto& kv : reg.byFileDescriptor) fds.push_back(kv.first);
  for (int fd : fds) UnregisterDeviceByFileDescriptor(epfd, &reg, fd);
  if (inotifyFd >= 0) close(inotifyFd);
  close(epfd);
  g_monitorRunning = false;
  g_monitorExited = true;
  DebugLog("Monitor thread exiting");
}

static void SignalStop() {
  g_shouldStop = true;
  if (g_stopEventFd >= 0) {
    uint64_t one = 1;
    (void)!write(g_stopEventFd, &one, sizeof one);
  }
}

static bool StartMonitorThread(KeyboardState* state, KeyboardCallback cb) {
  std::unique_lock<std::mutex> lk(g_lifecycleMutex);
  if (g_monitorRunning || g_monitorThread.joinable() || !state || !cb) return false;
  {
    std::lock_guard<std::mutex> sl(g_stateLock);
    state->ResetAll();
    state->userCallback = cb;
  }
  g_shouldStop = false;
  g_monitorExited = false;
  g_stopEventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (g_stopEventFd < 0) {
    std::lock_guard<std::mutex> sl(g_stateLock);
    state->ResetAll();
    return false;
  }
  StartCallbackDispatcher();
  g_monitorThread = std::thread(MonitorThreadMain, state);
  lk.unlock();

  for (int i = 0; i < 300; ++i) {
    if (g_monitorRunning) return true;
    if (g_monitorExited) break;
    struct timespec ts = {0, 10 * 1000 * 1000};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
  }
  lk.lock();
  if (g_monitorRunning) return true;
  SignalStop();
  if (g_monitorThread.joinable()) g_monitorThread.join();
  StopCallbackDispatcher();
  if (g_stopEventFd >= 0) { close(g_stopEventFd); g_stopEventFd = -1; }
  std::lock_guard<std::mutex> sl(g_stateLock);
  state->ResetAll();
  return false;
}

static void StopMonitorThread() {
  std::lock_guard<std::mutex> lk(g_lifecycleMutex);
  if (!g_monitorThread.joinable()) return;
  SignalStop();
  if (g_monitorThread.get_id() == std::this_thread::get_id()) g_monitorThread.detach();
  else g_monitorThread.join();
  StopCallbackDispatcher();
  if (g_stopEventFd >= 0) { close(g_stopEventFd); g_stopEventFd = -1; }
  g_monitorRunning = false;
}

// query caches (the original keeps the returned strings in static storage)
static std::mutex g_queryCacheMutex;
static std::string g_deviceListCache;
static std::string g_layoutInfoSimpleCache, g_layoutInfoDetailCache;
static bool g_layoutInfoSimpleCached = false, g_layoutInfoDetailCached = false;

static void EmergencyStopMonitorOnProcessExit() {
  StopMonitorThread();
  std::lock_guard<std::mutex> sl(g_stateLock);
  g_keyboardState.ResetAll();
}

// Registered lazily (first startMonitor) so it runs before this library's
// static destructors at process exit.
static void RegisterExitHandler() {
  static std::once_flag once;
  std::call_once(once, [] { atexit(EmergencyStopMonitorOnProcessExit); });
}

}  // namespace kh

using namespace kh;

KH_EXPORT const char* keyboard_say_hello() {
  return "Hello from C++ shared library - Keyboard Helper (Linux)!";
}

KH_EXPORT bool startMonitor(KeyboardCallback cb) {
  if (!cb) {
    DebugLog("startMonitor: callback is null");
    return false;
  }
  RegisterExitHandler();
  bool ok = StartMonitorThread(&g_keyboardState, cb);
  DebugLog(ok ? "startMonitor: success" : "startMonitor: failed to start monitor thread");
  return ok;
}

KH_EXPORT void stopMonitor() {
  StopMonitorThread();
  std::lock_guard<std::mutex> sl(g_stateLock);
  g_keyboardState.ResetAll();
}

KH_EXPORT void updateTargetShortcuts(const char* json) {
  if (!json) {
    DebugLog("updateTargetShortcuts: json is null");
    return;
  }
  auto sets = ParseTargetShortcutSets(json);
  std::lock_guard<std::mutex> sl(g_stateLock);
  g_keyboardState.targetShortcuts = std::move(sets);
}

KH_EXPORT void resetPressingKeycodes() {
  KeyboardCallback cb;
  {
    std::lock_guard<std::mutex> sl(g_stateLock);
    ResetPressingState(&g_keyboardState);
    cb = g_keyboardState.userCallback;
  }
  if (cb) {
    try {
      cb(kResetSentinel, kResetSentinel, kResetSentinel, "[]");
    } catch (...) {
      DebugLog("Exception in resetPressingKeycodes callback");
    }
  }
}

KH_EXPORT void setWatcherInterval(double seconds) {
  if (!(seconds > 0)) return;
  std::lock_guard<std::mutex> sl(g_stateLock);
  g_keyboardState.watcherIntervalSeconds = seconds;
}

KH_EXPORT void processEvents() {}

KH_EXPORT const char* getKeyboardLayoutInfo(bool detail) {
  std::lock_guard<std::mutex> lk(g_queryCacheMutex);
  if (detail) {
    if (!g_layoutInfoDetailCached) {
      g_layoutInfoDetailCache = BuildKeyboardLayoutInfoJson(true);
      g_layoutInfoDetailCached = true;
    }
    return g_layoutInfoDetailCache.c_str();
  }
  if (!g_layoutInfoSimpleCached) {
    g_layoutInfoSimpleCache = BuildKeyboardLayoutInfoJson(false);
    g_layoutInfoSimpleCached = true;
  }
  return g_layoutInfoSimpleCache.c_str();
}

KH_EXPORT const char* getKeyboardDeviceList() {
  std::lock_guard<std::mutex> lk(g_queryCacheMutex);
  g_deviceListCache = BuildKeyboardDeviceListJson();
  return g_deviceListCache.c_str();
}
