// a11y_ipc.h - wire encoding for the typeless-atspi-helper protocol.
//
// Framing: every message is a 4-byte little-endian unsigned length L
// (1 <= L <= 8 MiB) followed by L bytes of compact UTF-8 JSON. One request
// frame is answered by exactly one response frame on the same connection;
// connections are persistent. Malformed requests close the connection.
#pragma once

#include <cstddef>
#include <string>

#include "a11y_types.h"

namespace LinuxShared {
namespace A11y {

inline constexpr size_t kMaxFrameBytes = 0x800000;  // 8 MiB

std::string EscapeJsonString(const std::string &in);

std::string EncodeRequest(const A11yRequest &request);
bool DecodeRequest(const std::string &json, A11yRequest &request);
std::string EncodeResponse(const A11yResponse &response);
bool DecodeResponse(const std::string &json, A11yResponse &response);

const char *A11yErrorName(A11yError error);

// Minimal "find first key" JSON readers used by both sides (and by the
// libs to parse payloads). They search for the first occurrence of "key"
// followed by ':' anywhere in the text, like the original implementation.
bool JsonReadString(const std::string &json, const char *key, std::string &out);
bool JsonReadInt(const std::string &json, const char *key, long &out);
bool JsonReadBool(const std::string &json, const char *key, bool &out);

// Blocking frame I/O on a stream socket. WriteFrame uses MSG_NOSIGNAL.
// ReadFrame waits at most timeoutMs in total (<0 = forever).
bool WriteFrame(int fd, const std::string &payload);
bool ReadFrame(int fd, std::string &payload, int timeoutMs);

}  // namespace A11y
}  // namespace LinuxShared
