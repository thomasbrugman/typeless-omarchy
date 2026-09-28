// Clean-room arm64 reimplementation of Typeless libUtilHelper.so (Linux).
// Shared internal declarations.
#pragma once

#include <pulse/pulseaudio.h>

#include <chrono>
#include <functional>
#include <string>

#define UH_EXPORT extern "C" __attribute__((visibility("default")))

// ---- PulseSession (pulse_session.cpp) -------------------------------------
// Private pa_mainloop + pa_context used synchronously by one call.
class PulseSession {
 public:
  PulseSession() = default;
  ~PulseSession();
  PulseSession(const PulseSession&) = delete;
  PulseSession& operator=(const PulseSession&) = delete;

  // Connects with a 5 s timeout. appName null/empty -> "Typeless".
  bool Connect(const char* appName);
  // Iterates the mainloop (blocking) until pred() is true or timeout.
  bool IterateUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout);
  // Waits (10 s) for an operation to complete and unrefs it. Returns false on
  // timeout (operation cancelled) or if it did not end in PA_OPERATION_DONE.
  bool WaitForOperation(pa_operation* op);
  void Cleanup();

  pa_context* context() const { return context_; }
  const std::string& error() const { return error_; }

 private:
  pa_mainloop* mainloop_ = nullptr;
  pa_context* context_ = nullptr;
  std::string error_;
};

// ---- internal_helper.cpp ---------------------------------------------------
std::string GetXdgConfigHome();
std::string GetXdgDataHome();
bool EnsureDirectoryExists(const std::string& path);  // mkdir -p, mode 0755

// ---- sink_input_duck.cpp ---------------------------------------------------
struct SinkInputDuckIdentity {
  uint32_t processId = 0;
  std::string applicationName;
  std::string processBinary;
  std::string mediaRole;
  int corked = 0;
};
struct SinkInputDuckSelf {
  uint32_t processId = 0;
  std::string executablePath;
};
std::string ResolveProcessExecutablePath(uint32_t pid);
bool ShouldSkipSinkInputForDuck(const SinkInputDuckIdentity& id, const SinkInputDuckSelf& self);

// ---- cue_wav.cpp -----------------------------------------------------------
#include <vector>
struct CueWavPcm {
  uint32_t sampleRate = 0;
  uint16_t channelCount = 0;
  uint16_t bitsPerSample = 0;
  uint32_t durationMilliseconds = 0;
  std::vector<unsigned char> pcmBytes;
};
bool LoadCueWavPcmFile(const std::string& path, CueWavPcm* out, std::string* error);
void BoostCueWavPcmToTargetPeak(CueWavPcm* pcm, double targetPeak);

// ---- a11y_client.cpp -------------------------------------------------------
namespace a11y_client {
bool EnsureReady();
int IsDesktopReachable();
void Disconnect();
}  // namespace a11y_client
