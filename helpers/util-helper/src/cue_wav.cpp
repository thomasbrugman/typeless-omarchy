// Minimal RIFF/WAVE PCM loader for short UI cue sounds.
#include <stdlib.h>
#include <string.h>

#include <fstream>

#include "common.h"

namespace {

constexpr size_t kMaxCueFileBytes = 2 * 1024 * 1024;
constexpr uint32_t kMaxCueDurationMs = 2000;

uint32_t ReadLe32(const unsigned char* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t ReadLe16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool ReadWholeFile(const std::string& path, std::vector<unsigned char>* data, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) {
    *error = "Failed to open WAV file";
    return false;
  }
  f.seekg(0, std::ios::end);
  std::streamoff size = f.tellg();
  if (size < 0) {
    *error = "Failed to read WAV file size";
    return false;
  }
  if (size < 12) {
    *error = "WAV file is too small";
    return false;
  }
  if ((size_t)size > kMaxCueFileBytes) {
    *error = "WAV file is too large for a cue";
    return false;
  }
  f.seekg(0, std::ios::beg);
  data->resize((size_t)size);
  if (!f.read(reinterpret_cast<char*>(data->data()), size)) {
    *error = "Failed to read WAV file";
    return false;
  }
  return true;
}

}  // namespace

bool LoadCueWavPcmFile(const std::string& path, CueWavPcm* out, std::string* error) {
  if (!out || !error) return false;
  out->pcmBytes.clear();
  error->clear();
  if (path.empty()) {
    *error = "WAV path is empty";
    return false;
  }
  std::vector<unsigned char> data;
  if (!ReadWholeFile(path, &data, error)) return false;
  if (data.size() < 12 || memcmp(data.data(), "RIFF", 4) != 0 || memcmp(data.data() + 8, "WAVE", 4) != 0) {
    *error = "Not a RIFF/WAVE file";
    return false;
  }
  bool haveFmt = false, haveData = false;
  uint16_t audioFormat = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  size_t dataOff = 0, dataLen = 0;
  size_t pos = 12;
  while (pos + 8 <= data.size()) {
    const unsigned char* h = data.data() + pos;
    uint32_t len = ReadLe32(h + 4);
    size_t body = pos + 8;
    if ((size_t)len > data.size() - body) {
      *error = "WAV chunk overruns file";
      return false;
    }
    if (memcmp(h, "fmt ", 4) == 0) {
      if (len < 16) {
        *error = "WAV fmt chunk is too small";
        return false;
      }
      const unsigned char* f = data.data() + body;
      audioFormat = ReadLe16(f);
      channels = ReadLe16(f + 2);
      rate = ReadLe32(f + 4);
      bits = ReadLe16(f + 14);
      haveFmt = true;
    } else if (memcmp(h, "data", 4) == 0) {
      dataOff = body;
      dataLen = len;
      haveData = true;
    }
    pos = body + len + (len & 1);
  }
  if (!haveFmt || !haveData) {
    *error = "WAV is missing fmt or data chunk";
    return false;
  }
  if (audioFormat != 1) {
    *error = "WAV audio format must be PCM, got " + std::to_string(audioFormat);
    return false;
  }
  if (channels == 0 || rate == 0 || bits == 0 || (bits & 7) != 0) {
    *error = "WAV format fields are invalid";
    return false;
  }
  uint32_t frameBytes = (uint32_t)(bits / 8) * channels;
  if (frameBytes == 0 || dataLen % frameBytes != 0) {
    *error = "WAV data size is not a multiple of the frame size";
    return false;
  }
  uint32_t frames = (uint32_t)(dataLen / frameBytes);
  uint32_t durationMs = (uint32_t)((uint64_t)frames * 1000u / rate);
  if (durationMs > kMaxCueDurationMs) {
    *error = "WAV duration is too long for a cue";
    return false;
  }
  out->sampleRate = rate;
  out->channelCount = channels;
  out->bitsPerSample = bits;
  out->durationMilliseconds = durationMs;
  out->pcmBytes.assign(data.begin() + dataOff, data.begin() + dataOff + dataLen);
  return true;
}
