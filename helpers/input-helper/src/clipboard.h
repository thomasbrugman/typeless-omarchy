#pragma once

#include <string>
#include <utility>
#include <vector>

namespace InputHelper {
namespace Clipboard {

// A copy of the whole clipboard: every offered MIME type with its bytes.
struct Snapshot {
    std::vector<std::pair<std::string, std::string>> entries;
    bool Empty() const { return entries.empty(); }
    std::string PlainText() const;
};

// Save the current clipboard (all MIME types when possible, else plain text).
Snapshot Save();
// Restore a snapshot; an empty snapshot clears the clipboard (original behavior).
bool Restore(const Snapshot &snapshot);

bool SetText(const std::string &text);
bool SetRichText(const std::string &html, const std::string &text);
bool Clear();
std::string GetText();

}  // namespace Clipboard
}  // namespace InputHelper
