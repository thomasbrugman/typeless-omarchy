// playSoundFile: plays a short 16-bit PCM WAV "cue" through PulseAudio.
// Returns 1 = played (backend "pulse"), 0 = invalid WAV ("wav-invalid"),
// -1 = PulseAudio unavailable/failed ("pulse-unavailable").
#include <stdio.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "common.h"

namespace {

std::mutex gCuePlaybackMutex;
std::unordered_map<std::string, CueWavPcm> gCueWavCache;

void LogCueBackend(const char* backend, const std::string& detail) {
  if (detail.empty()) {
    fprintf(stderr, "cue.backend=%s\n", backend);
  } else {
    fprintf(stderr, "cue.backend=%s %s\n", backend, detail.c_str());
  }
}

struct StreamReadyState {
  bool ready = false;
  bool failed = false;
};

void StreamStateCallback(pa_stream* s, void* ud) {
  auto* st = static_cast<StreamReadyState*>(ud);
  switch (pa_stream_get_state(s)) {
    case PA_STREAM_READY:
      st->ready = true;
      break;
    case PA_STREAM_UNCONNECTED:
    case PA_STREAM_CREATING:
      break;
    default:
      st->failed = true;
      break;
  }
}

void DrainCallback(pa_stream*, int, void* ud) { *static_cast<bool*>(ud) = true; }

bool WriteAllPcm(PulseSession* s, pa_stream* stream, const CueWavPcm& pcm, StreamReadyState* st) {
  const auto& bytes = pcm.pcmBytes;
  size_t off = 0;
  while (off < bytes.size()) {
    if (!s->IterateUntil([&] { return st->failed || pa_stream_writable_size(stream) > 0; },
                         std::chrono::milliseconds(2000)))
      return false;
    if (st->failed) return false;
    size_t w = pa_stream_writable_size(stream);
    if (w == (size_t)-1) return false;
    size_t n = std::min(w, bytes.size() - off);
    if (pa_stream_write(stream, bytes.data() + off, n, nullptr, 0, PA_SEEK_RELATIVE) < 0) return false;
    off += n;
  }
  return true;
}

int PlayCueWavOnPulse(const CueWavPcm& pcm) {
  PulseSession session;
  if (!session.Connect("Typeless")) {
    LogCueBackend("pulse-unavailable", session.error());
    return -1;
  }
  if (pcm.bitsPerSample != 16) {
    LogCueBackend("wav-invalid", "unsupported sample format");
    return 0;
  }
  pa_sample_spec ss;
  ss.format = PA_SAMPLE_S16LE;
  ss.rate = pcm.sampleRate;
  ss.channels = (uint8_t)pcm.channelCount;
  if (!pa_sample_spec_valid(&ss)) {
    LogCueBackend("wav-invalid", "unsupported sample format");
    return 0;
  }
  pa_proplist* pl = pa_proplist_new();
  if (!pl) {
    LogCueBackend("pulse-unavailable", "failed to allocate stream properties");
    return -1;
  }
  pa_proplist_sets(pl, PA_PROP_MEDIA_NAME, "Typeless cue");
  pa_proplist_sets(pl, PA_PROP_APPLICATION_NAME, "Typeless");
  pa_stream* stream = pa_stream_new_with_proplist(session.context(), "Typeless cue", &ss, nullptr, pl);
  pa_proplist_free(pl);
  if (!stream) {
    LogCueBackend("pulse-unavailable", "failed to create playback stream");
    return -1;
  }
  StreamReadyState st;
  pa_stream_set_state_callback(stream, StreamStateCallback, &st);
  auto closeStream = [&] {
    pa_stream_set_state_callback(stream, nullptr, nullptr);
    pa_stream_disconnect(stream);
    pa_stream_unref(stream);
  };

  pa_cvolume cv;
  pa_cvolume_set(&cv, ss.channels, PA_VOLUME_NORM);
  const pa_stream_flags_t flags = (pa_stream_flags_t)(PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE |
                                                      PA_STREAM_ADJUST_LATENCY);
  if (pa_stream_connect_playback(stream, nullptr, nullptr, flags, &cv, nullptr) < 0) {
    closeStream();
    LogCueBackend("pulse-unavailable", "failed to connect playback stream");
    return -1;
  }
  bool ok = session.IterateUntil([&] { return st.ready || st.failed; }, std::chrono::milliseconds(5000));
  if (!ok || st.failed || !st.ready) {
    closeStream();
    LogCueBackend("pulse-unavailable", "playback stream did not become ready");
    return -1;
  }
  if (!WriteAllPcm(&session, stream, pcm, &st)) {
    closeStream();
    LogCueBackend("pulse-unavailable", "failed to write cue PCM");
    return -1;
  }
  bool drained = false;
  pa_operation* op = pa_stream_drain(stream, DrainCallback, &drained);
  if (!op) {
    closeStream();
    LogCueBackend("pulse-unavailable", "failed to drain cue stream");
    return -1;
  }
  if (!session.IterateUntil([&] { return drained; },
                            std::chrono::milliseconds((long)pcm.durationMilliseconds + 500))) {
    pa_operation_cancel(op);
    LogCueBackend("pulse-drain-timeout", session.error());
  }
  pa_operation_set_state_callback(op, nullptr, nullptr);
  pa_operation_unref(op);
  closeStream();
  return 1;
}

}  // namespace

UH_EXPORT int playSoundFile(const char* path) {
  try {
    std::lock_guard<std::mutex> lk(gCuePlaybackMutex);
    if (!path || !*path) {
      LogCueBackend("wav-invalid", "path is empty");
      return 0;
    }
    std::string key(path);
    auto it = gCueWavCache.find(key);
    if (it == gCueWavCache.end()) {
      CueWavPcm pcm;
      std::string err;
      if (!LoadCueWavPcmFile(key, &pcm, &err)) {
        LogCueBackend("wav-invalid", err);
        return 0;
      }
      it = gCueWavCache.emplace(key, std::move(pcm)).first;
    }
    return PlayCueWavOnPulse(it->second);
  } catch (...) {
    return -1;
  }
}
