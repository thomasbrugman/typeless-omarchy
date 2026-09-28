// a11y_ipc.cpp - clean-room reimplementation of Typeless' linux-shared
// A11y wire codec (EncodeRequest/DecodeRequest/EncodeResponse/DecodeResponse,
// WriteFrame/ReadFrame). Output bytes match the x64 original.
#include "a11y_ipc.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace LinuxShared {
namespace A11y {
namespace {

inline bool IsJsonSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Finds the first occurrence of "key" that is followed (after optional
// whitespace) by ':' and returns the offset just after the ':' and any
// following whitespace.
bool FindKey(const std::string &json, const char *key, size_t &valuePos)
{
    std::string needle = std::string("\"") + key + "\"";
    size_t from = 0;
    while (from < json.size()) {
        size_t hit = json.find(needle, from);
        if (hit == std::string::npos)
            return false;
        size_t p = hit + needle.size();
        while (p < json.size() && IsJsonSpace(json[p]))
            ++p;
        if (p < json.size() && json[p] == ':') {
            ++p;
            while (p < json.size() && IsJsonSpace(json[p]))
                ++p;
            valuePos = p;
            return true;
        }
        from = hit + 1;
    }
    return false;
}

bool ReadJsonIntKey(const std::string &json, const char *key, long &out)
{
    size_t p = 0;
    if (!FindKey(json, key, p))
        return false;
    while (p < json.size() && IsJsonSpace(json[p]))
        ++p;
    if (p >= json.size())
        return false;
    const char *start = json.c_str() + p;
    char *end = nullptr;
    long long v = std::strtoll(start, &end, 10);
    out = static_cast<long>(v);
    return end != start;
}

bool ReadJsonBoolKey(const std::string &json, const char *key, bool &out)
{
    size_t p = 0;
    if (!FindKey(json, key, p))
        return false;
    while (p < json.size() && IsJsonSpace(json[p]))
        ++p;
    if (p >= json.size())
        return false;
    if (json.compare(p, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (json.compare(p, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

// Parses a JSON string value. Escapes: \n \r \t map to control chars, any
// other escaped char is taken literally (so \uXXXX is NOT decoded - this is
// how the original behaves; our encoder never emits \u for printable text).
bool ReadJsonStringKey(const std::string &json, const char *key, std::string &out)
{
    size_t p = 0;
    if (!FindKey(json, key, p))
        return false;
    while (p < json.size() && IsJsonSpace(json[p]))
        ++p;
    if (p >= json.size() || json[p] != '"')
        return false;
    ++p;
    out.clear();
    while (p < json.size()) {
        char c = json[p];
        if (c == '"')
            return true;
        if (c == '\\' && p + 1 < json.size()) {
            char e = json[p + 1];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(e); break;
            }
            p += 2;
            continue;
        }
        out.push_back(c);
        ++p;
    }
    return false;
}

void AppendU64(std::string &s, uint64_t v)
{
    s += std::to_string(v);
}

}  // namespace

std::string EscapeJsonString(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
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
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

const char *A11yErrorName(A11yError error)
{
    switch (error) {
    case A11yError::kOk: return "ok";
    case A11yError::kUnavailable: return "unavailable";
    case A11yError::kTimeout: return "timeout";
    case A11yError::kDefunct: return "defunct";
    case A11yError::kAborted: return "aborted";
    case A11yError::kDisabled: return "disabled";
    default: return "invalid";
    }
}

std::string EncodeRequest(const A11yRequest &r)
{
    std::string s = "{";
    s += "\"id\":";
    AppendU64(s, r.id);
    s += ",";
    s += "\"cmd\":\"" + EscapeJsonString(r.command) + "\",";
    s += "\"timeoutMs\":" + std::to_string(r.timeoutMs) + ",";
    s += "\"processId\":" + std::to_string(r.processId) + ",";
    s += "\"beforeChars\":" + std::to_string(r.beforeChars) + ",";
    s += "\"afterChars\":" + std::to_string(r.afterChars) + ",";
    s += "\"maxLength\":" + std::to_string(r.maxLength) + ",";
    s += std::string("\"reassert\":") + (r.reassert ? "true" : "false") + ",";
    s += "\"windowTitle\":\"" + EscapeJsonString(r.windowTitle) + "\",";
    s += "\"busName\":\"" + EscapeJsonString(r.busName) + "\",";
    s += "\"objectPath\":\"" + EscapeJsonString(r.objectPath) + "\",";
    s += "\"generation\":";
    AppendU64(s, r.generation);
    s += ",";
    s += "\"foregroundNote\":\"" + EscapeJsonString(r.foregroundNote) + "\",";
    s += "\"walkMaxDepth\":" + std::to_string(r.walkLimits.maxDepth) + ",";
    s += "\"walkMaxBreadth\":" + std::to_string(r.walkLimits.maxBreadth) + ",";
    s += "\"walkMaxNodes\":" + std::to_string(r.walkLimits.maxNodes) + ",";
    s += "\"parentWalkMaxLevels\":" + std::to_string(r.parentWalkLimits.maxLevels) + ",";
    s += "\"editableMaxAncestorLevels\":" +
         std::to_string(r.editableSearchLimits.maxAncestorLevels) + ",";
    s += "\"editableMaxDescendantNodes\":" +
         std::to_string(r.editableSearchLimits.maxDescendantNodes) + ",";
    s += "\"editableMaxBreadth\":" + std::to_string(r.editableSearchLimits.maxBreadth) + ",";
    s += "\"editableMaxInputTextLength\":" +
         std::to_string(r.editableSearchLimits.maxInputTextLength) + ",";
    s += "\"textMaxCharsPerCall\":" + std::to_string(r.textReadLimits.maxCharsPerCall) + ",";
    s += "\"focusScanTimeoutMs\":" + std::to_string(r.focusScanTimeoutMs);
    s += "}";
    return s;
}

bool DecodeRequest(const std::string &json, A11yRequest &r)
{
    r = A11yRequest();
    long v = 0;
    if (ReadJsonIntKey(json, "id", v)) r.id = static_cast<uint64_t>(v);
    ReadJsonStringKey(json, "cmd", r.command);
    if (ReadJsonIntKey(json, "timeoutMs", v)) r.timeoutMs = static_cast<int>(v);
    if (ReadJsonIntKey(json, "processId", v)) r.processId = static_cast<int>(v);
    if (ReadJsonIntKey(json, "beforeChars", v)) r.beforeChars = static_cast<int>(v);
    if (ReadJsonIntKey(json, "afterChars", v)) r.afterChars = static_cast<int>(v);
    if (ReadJsonIntKey(json, "maxLength", v)) r.maxLength = static_cast<int>(v);
    ReadJsonBoolKey(json, "reassert", r.reassert);
    ReadJsonStringKey(json, "windowTitle", r.windowTitle);
    ReadJsonStringKey(json, "busName", r.busName);
    ReadJsonStringKey(json, "objectPath", r.objectPath);
    if (ReadJsonIntKey(json, "generation", v)) r.generation = static_cast<uint64_t>(v);
    ReadJsonStringKey(json, "foregroundNote", r.foregroundNote);
    if (ReadJsonIntKey(json, "walkMaxDepth", v)) r.walkLimits.maxDepth = static_cast<int>(v);
    if (ReadJsonIntKey(json, "walkMaxBreadth", v)) r.walkLimits.maxBreadth = static_cast<int>(v);
    if (ReadJsonIntKey(json, "walkMaxNodes", v)) r.walkLimits.maxNodes = static_cast<int>(v);
    if (ReadJsonIntKey(json, "parentWalkMaxLevels", v))
        r.parentWalkLimits.maxLevels = static_cast<int>(v);
    if (ReadJsonIntKey(json, "editableMaxAncestorLevels", v))
        r.editableSearchLimits.maxAncestorLevels = static_cast<int>(v);
    if (ReadJsonIntKey(json, "editableMaxDescendantNodes", v))
        r.editableSearchLimits.maxDescendantNodes = static_cast<int>(v);
    if (ReadJsonIntKey(json, "editableMaxBreadth", v))
        r.editableSearchLimits.maxBreadth = static_cast<int>(v);
    if (ReadJsonIntKey(json, "editableMaxInputTextLength", v))
        r.editableSearchLimits.maxInputTextLength = static_cast<int>(v);
    if (ReadJsonIntKey(json, "textMaxCharsPerCall", v))
        r.textReadLimits.maxCharsPerCall = static_cast<int>(v);
    if (ReadJsonIntKey(json, "focusScanTimeoutMs", v)) r.focusScanTimeoutMs = static_cast<int>(v);
    return !r.command.empty();
}

std::string EncodeResponse(const A11yResponse &r)
{
    std::string s = "{";
    s += "\"id\":";
    AppendU64(s, r.id);
    s += ",";
    s += std::string("\"ok\":") + (r.ok ? "true" : "false") + ",";
    s += std::string("\"error\":\"") + EscapeJsonString(A11yErrorName(r.error)) + "\",";
    s += "\"failReason\":\"" + EscapeJsonString(r.failReason) + "\",";
    s += "\"generation\":";
    AppendU64(s, r.generation);
    s += ",";
    s += "\"payload\":";
    s += r.payloadJson.empty() ? std::string("{}") : r.payloadJson;
    s += "}";
    return s;
}

bool DecodeResponse(const std::string &json, A11yResponse &r)
{
    r = A11yResponse();  // ok=false, error=kInvalid
    size_t p = 0;
    while (p < json.size() && IsJsonSpace(json[p]))
        ++p;
    if (p >= json.size() || json[p] != '{')
        return false;

    long v = 0;
    if (ReadJsonIntKey(json, "id", v)) r.id = static_cast<uint64_t>(v);
    ReadJsonBoolKey(json, "ok", r.ok);
    std::string err;
    ReadJsonStringKey(json, "error", err);
    if (err.empty() || err == "ok") r.error = A11yError::kOk;
    else if (err == "unavailable") r.error = A11yError::kUnavailable;
    else if (err == "timeout") r.error = A11yError::kTimeout;
    else if (err == "defunct") r.error = A11yError::kDefunct;
    else if (err == "aborted") r.error = A11yError::kAborted;
    else if (err == "disabled") r.error = A11yError::kDisabled;
    else r.error = A11yError::kInvalid;
    ReadJsonStringKey(json, "failReason", r.failReason);
    if (ReadJsonIntKey(json, "generation", v)) r.generation = static_cast<uint64_t>(v);

    // payload: first balanced {...} object after "payload":
    r.payloadJson = "{}";
    size_t pos = 0;
    if (FindKey(json, "payload", pos)) {
        while (pos < json.size() && IsJsonSpace(json[pos]))
            ++pos;
        if (pos < json.size() && json[pos] == '{') {
            int depth = 0;
            bool inString = false, escape = false;
            for (size_t i = pos; i < json.size(); ++i) {
                char c = json[i];
                if (inString) {
                    if (escape) escape = false;
                    else if (c == '\\') escape = true;
                    else if (c == '"') inString = false;
                    continue;
                }
                if (c == '"') inString = true;
                else if (c == '{') ++depth;
                else if (c == '}') {
                    if (--depth == 0) {
                        r.payloadJson = json.substr(pos, i - pos + 1);
                        break;
                    }
                }
            }
        }
    }
    return true;
}

bool JsonReadString(const std::string &json, const char *key, std::string &out)
{
    return ReadJsonStringKey(json, key, out);
}

bool JsonReadInt(const std::string &json, const char *key, long &out)
{
    return ReadJsonIntKey(json, key, out);
}

bool JsonReadBool(const std::string &json, const char *key, bool &out)
{
    return ReadJsonBoolKey(json, key, out);
}

namespace {

bool SendAll(int fd, const void *data, size_t len)
{
    const char *p = static_cast<const char *>(data);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool ReadAll(int fd, void *data, size_t len, std::chrono::steady_clock::time_point deadline,
             bool forever)
{
    char *p = static_cast<char *>(data);
    while (len > 0) {
        int waitMs = -1;
        if (!forever) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now())
                            .count();
            if (left <= 0)
                return false;
            waitMs = static_cast<int>(left);
        }
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = ::poll(&pfd, 1, waitMs);
        if (pr == 0)
            return false;
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) && !(pfd.revents & POLLIN))
            return false;
        ssize_t n = ::recv(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

bool WriteFrame(int fd, const std::string &payload)
{
    if (fd < 0 || payload.size() > kMaxFrameBytes)
        return false;
    uint32_t n = static_cast<uint32_t>(payload.size());
    unsigned char hdr[4] = {static_cast<unsigned char>(n & 0xff),
                            static_cast<unsigned char>((n >> 8) & 0xff),
                            static_cast<unsigned char>((n >> 16) & 0xff),
                            static_cast<unsigned char>((n >> 24) & 0xff)};
    if (!SendAll(fd, hdr, sizeof hdr))
        return false;
    return SendAll(fd, payload.data(), payload.size());
}

bool ReadFrame(int fd, std::string &payload, int timeoutMs)
{
    payload.clear();
    if (fd < 0)
        return false;
    bool forever = timeoutMs < 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(forever ? 0 : timeoutMs);
    unsigned char hdr[4];
    if (!ReadAll(fd, hdr, sizeof hdr, deadline, forever))
        return false;
    uint32_t n = static_cast<uint32_t>(hdr[0]) | (static_cast<uint32_t>(hdr[1]) << 8) |
                 (static_cast<uint32_t>(hdr[2]) << 16) | (static_cast<uint32_t>(hdr[3]) << 24);
    if (n == 0 || n > kMaxFrameBytes)
        return false;
    payload.resize(n);
    if (!ReadAll(fd, &payload[0], n, deadline, forever)) {
        payload.clear();
        return false;
    }
    return true;
}

}  // namespace A11y
}  // namespace LinuxShared
