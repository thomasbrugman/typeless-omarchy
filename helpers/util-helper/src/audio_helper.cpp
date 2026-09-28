// muteAudio / unmuteAudio / isAudioMuted.
//
// Linux semantics of the original: "mute" does NOT touch the default sink.
// It mutes ("ducks") every other application's playback stream (PulseAudio
// sink input) that is currently audible, remembers which ones it muted, and
// unmuteAudio restores exactly those. State is process-wide and restored at
// process exit.
#include <stdlib.h>
#include <unistd.h>

#include <mutex>
#include <vector>

#include "common.h"

namespace {

struct DuckSnapshotEntry {
  uint32_t index;
  int muted;  // mute state to restore
};

struct DuckState {
  bool active = false;
  std::vector<DuckSnapshotEntry> snapshot;
};

std::mutex gDuckMutex;
DuckState gDuckState;
std::once_flag gRestoreAtExitOnce;

std::string ReadPropertyString(const pa_proplist* pl, const char* key) {
  if (!pl) return std::string();
  const char* v = pa_proplist_gets(pl, key);
  return v ? std::string(v) : std::string();
}

bool SetSinkInputMute(PulseSession* s, uint32_t index, int mute) {
  struct Result {
    bool done = false;
    bool success = false;
  } r;
  pa_operation* op = pa_context_set_sink_input_mute(
      s->context(), index, mute,
      [](pa_context*, int success, void* ud) {
        auto* rr = static_cast<Result*>(ud);
        rr->done = true;
        rr->success = success != 0;
      },
      &r);
  if (!op) return false;
  if (!s->WaitForOperation(op)) return false;
  return r.success;
}

struct CollectState {
  SinkInputDuckSelf self;
  bool done = false;
  bool failed = false;
  std::vector<DuckSnapshotEntry> entries;
};

void CollectDuckableSinkInputs(pa_context*, const pa_sink_input_info* info, int eol, void* ud) {
  auto* st = static_cast<CollectState*>(ud);
  if (eol < 0) {
    st->done = true;
    st->failed = true;
    return;
  }
  if (eol > 0) {
    st->done = true;
    return;
  }
  if (!info) return;
  SinkInputDuckIdentity id;
  std::string pidStr = ReadPropertyString(info->proplist, PA_PROP_APPLICATION_PROCESS_ID);
  if (!pidStr.empty()) {
    char* end = nullptr;
    unsigned long v = strtoul(pidStr.c_str(), &end, 10);
    if (end != pidStr.c_str() && end && *end == '\0') id.processId = (uint32_t)v;
  }
  id.applicationName = ReadPropertyString(info->proplist, PA_PROP_APPLICATION_NAME);
  id.processBinary = ReadPropertyString(info->proplist, PA_PROP_APPLICATION_PROCESS_BINARY);
  id.mediaRole = ReadPropertyString(info->proplist, PA_PROP_MEDIA_ROLE);
  id.corked = info->corked;
  if (ShouldSkipSinkInputForDuck(id, st->self)) return;
  if (info->mute) return;  // already muted by someone else: leave it
  st->entries.push_back(DuckSnapshotEntry{info->index, 0});
}

// Caller holds gDuckMutex.
bool RestoreDuckedSinkInputsLocked() {
  if (!gDuckState.active) return true;
  PulseSession session;
  if (!session.Connect("Typeless")) return false;
  std::vector<DuckSnapshotEntry> failed;
  failed.reserve(gDuckState.snapshot.size());
  for (const auto& e : gDuckState.snapshot) {
    if (!SetSinkInputMute(&session, e.index, e.muted)) failed.push_back(e);
  }
  gDuckState.snapshot = std::move(failed);
  if (gDuckState.snapshot.empty()) {
    gDuckState.active = false;
    return true;
  }
  return false;
}

int RestoreDuckedSinkInputs() {
  std::lock_guard<std::mutex> lk(gDuckMutex);
  return RestoreDuckedSinkInputsLocked() ? 1 : 0;
}

void RestoreDuckedSinkInputsAtExit() {
  if (!gDuckMutex.try_lock()) return;
  bool active = gDuckState.active;
  gDuckMutex.unlock();
  if (active) RestoreDuckedSinkInputs();
}

}  // namespace

UH_EXPORT int muteAudio() {
  try {
    std::call_once(gRestoreAtExitOnce, [] { atexit(RestoreDuckedSinkInputsAtExit); });
    std::lock_guard<std::mutex> lk(gDuckMutex);
    if (gDuckState.active) return 1;

    PulseSession session;
    if (!session.Connect("Typeless")) return 0;

    CollectState st;
    st.self.processId = (uint32_t)getpid();
    st.self.executablePath = ResolveProcessExecutablePath(st.self.processId);
    pa_operation* op = pa_context_get_sink_input_info_list(session.context(), CollectDuckableSinkInputs, &st);
    if (!op || !session.WaitForOperation(op) || st.failed) return 0;

    std::vector<DuckSnapshotEntry> muted;
    muted.reserve(st.entries.size());
    for (const auto& e : st.entries) {
      if (SetSinkInputMute(&session, e.index, 1)) muted.push_back(e);
    }
    if (!muted.empty()) {
      gDuckState.snapshot = std::move(muted);
      gDuckState.active = true;
    }
    return 1;
  } catch (...) {
    return 0;
  }
}

UH_EXPORT int unmuteAudio() {
  try {
    return RestoreDuckedSinkInputs();
  } catch (...) {
    return 0;
  }
}

UH_EXPORT int isAudioMuted() {
  std::lock_guard<std::mutex> lk(gDuckMutex);
  return gDuckState.active ? 1 : 0;
}
