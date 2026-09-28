#pragma once

#include <string>

namespace InputHelper {
namespace AccessibilityReader {

// Mirrors the original InputState (field order/meaning).
struct InputState {
    bool valid = false;
    std::string error;
    int startIndex = 0;  // code points before the selection/caret
    int endIndex = 0;    // startIndex + code points of the selection
    std::string beforeText;
    std::string selectedText;
    std::string afterText;
};

void Initialize();  // make sure the AT-SPI helper is (being) started
InputState GetCurrentInputState();
std::string GetSelectedText();
std::string ToJson(const InputState &state);
void RecoverAfterResume();
void Cleanup();

}  // namespace AccessibilityReader
}  // namespace InputHelper
