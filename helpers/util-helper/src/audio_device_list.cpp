// getAudioDevicesJSON / getAudioDevicesJSONAsync.
//
// Output (single line, no whitespace other than ", " between devices):
// {"inputDevices":[{"deviceID":"<pulse source name>","name":"<description>",
//   "manufacturer":"<device.vendor.name|device.product.name>",
//   "transportType":"Built-in|USB|Bluetooth|PCI|FireWire|Thunderbolt|Virtual|Unknown",
//   "isInput":true,"isOutput":false,"isDefault":<bool>,"sampleRate":N,"channels":N,
//   "volume":<avg/65536>,"isMuted":<bool>,"isAlive":<bool>}, ...],
//  "outputDevices":[...same, from sinks...],
//  "defaultInputDeviceID":"<name>"|null,"defaultOutputDeviceID":"<name>"|null}
// On failure: {"error": "Failed to enumerate PulseAudio devices"}
#include <stdio.h>
#include <string.h>

#include <sstream>
#include <thread>
#include <vector>

#include "common.h"

namespace {

constexpr const char kPulseMonitorSourceSuffix[] = ".monitor";

struct AudioDeviceEntry {
  std::string deviceId;
  std::string name;
  std::string manufacturer;
  std::string transportType;
  bool isInput = false;
  bool isOutput = false;
  bool isDefault = false;
  int sampleRate = 0;
  int channels = 0;
  float volume = 0.0f;
  bool hasVolume = false;
  bool isMuted = false;
  bool hasMute = false;
  bool isAlive = true;
};

struct DeviceListState {
  std::string defaultSinkName;
  std::string defaultSourceName;
  std::vector<AudioDeviceEntry> inputDevices;
  std::vector<AudioDeviceEntry> outputDevices;
  bool serverInfoDone = false;
  bool sinkListDone = false;
  bool sourceListDone = false;
  bool failed = false;
};

std::string EscapeJsonString(const std::string& s) {
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
        if (c <= 0x1f) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back((char)c);
        }
    }
  }
  return out;
}

std::string BuildManufacturer(const pa_proplist* pl) {
  if (!pl) return std::string();
  const char* vendor = pa_proplist_gets(pl, PA_PROP_DEVICE_VENDOR_NAME);
  std::string v = vendor ? vendor : "";
  if (!v.empty()) return v;
  const char* product = pa_proplist_gets(pl, PA_PROP_DEVICE_PRODUCT_NAME);
  return product ? std::string(product) : std::string();
}

const char* ResolveTransportType(const pa_proplist* pl) {
  const char* bus = pl ? pa_proplist_gets(pl, PA_PROP_DEVICE_BUS) : nullptr;
  if (bus && *bus) {
    std::string b;
    for (const char* p = bus; *p; ++p) {
      char c = *p;
      if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
      b.push_back(c);
    }
    if (b.find("bluetooth") != std::string::npos) return "Bluetooth";
    if (b == "bluez") return "Bluetooth";
    if (b == "usb") return "USB";
    if (b == "pci" || b == "pcie") return "PCI";
    if (b == "firewire" || b == "ieee1394") return "FireWire";
    if (b == "thunderbolt") return "Thunderbolt";
    if (b == "virtual") return "Virtual";
    if (b == "isa" || b == "hda" || b == "internal") return "Built-in";
    return "Unknown";
  }
  const char* ff = pl ? pa_proplist_gets(pl, PA_PROP_DEVICE_FORM_FACTOR) : nullptr;
  if (!ff) return "Unknown";
  if (!strcmp(ff, "internal") || !strcmp(ff, "handset") || !strcmp(ff, "speaker")) return "Built-in";
  return "Unknown";
}

template <typename Info>
void FillCommon(AudioDeviceEntry& e, const Info* info) {
  e.manufacturer = BuildManufacturer(info->proplist);
  e.transportType = ResolveTransportType(info->proplist);
  e.sampleRate = (int)info->sample_spec.rate;
  e.channels = (int)info->sample_spec.channels;
  if (info->volume.channels != 0) {
    e.volume = (float)pa_cvolume_avg(&info->volume) * (1.0f / 65536.0f);
  } else {
    e.volume = 0.0f;
  }
  e.hasVolume = true;
  e.isMuted = info->mute != 0;
  e.hasMute = true;
  e.isAlive = (int)info->state != -3;  // PA_*_UNLINKED
}

void OnServerInfo(pa_context*, const pa_server_info* info, void* ud) {
  auto* st = static_cast<DeviceListState*>(ud);
  if (!info) {
    st->failed = true;
    return;
  }
  if (info->default_sink_name) st->defaultSinkName = info->default_sink_name;
  if (info->default_source_name) st->defaultSourceName = info->default_source_name;
}

void OnSinkInfo(pa_context*, const pa_sink_info* info, int eol, void* ud) {
  auto* st = static_cast<DeviceListState*>(ud);
  if (eol < 0) {
    st->failed = true;
    return;
  }
  if (!info) return;
  AudioDeviceEntry e;
  e.deviceId = info->name ? info->name : "";
  e.name = info->description ? info->description : "Speakers";
  FillCommon(e, info);
  e.isOutput = true;
  e.isDefault = !st->defaultSinkName.empty() && e.deviceId == st->defaultSinkName;
  st->outputDevices.emplace_back(std::move(e));
}

void OnSourceInfo(pa_context*, const pa_source_info* info, int eol, void* ud) {
  auto* st = static_cast<DeviceListState*>(ud);
  if (eol < 0) {
    st->failed = true;
    return;
  }
  if (!info || !info->name) return;
  size_t n = strlen(info->name);
  const size_t sl = sizeof(kPulseMonitorSourceSuffix) - 1;
  if (n >= sl && strcmp(info->name + n - sl, kPulseMonitorSourceSuffix) == 0) return;
  AudioDeviceEntry e;
  e.deviceId = info->name;
  e.name = info->description ? info->description : "Microphone";
  FillCommon(e, info);
  e.isInput = true;
  e.isDefault = !st->defaultSourceName.empty() && e.deviceId == st->defaultSourceName;
  st->inputDevices.emplace_back(std::move(e));
}

bool CollectAudioDevices(DeviceListState* st) {
  PulseSession session;
  if (!session.Connect("Typeless")) return false;
  pa_context* c = session.context();

  pa_operation* op = pa_context_get_server_info(c, OnServerInfo, st);
  if (!op) return false;
  if (!session.WaitForOperation(op)) {
    st->failed = true;
    return false;
  }
  st->serverInfoDone = true;

  op = pa_context_get_sink_info_list(c, OnSinkInfo, st);
  if (!op) return false;
  if (!session.WaitForOperation(op)) {
    st->failed = true;
    return false;
  }
  st->sinkListDone = true;

  op = pa_context_get_source_info_list(c, OnSourceInfo, st);
  if (!op) return false;
  if (!session.WaitForOperation(op)) {
    st->failed = true;
    return false;
  }
  st->sourceListDone = true;
  return !st->failed && st->serverInfoDone && st->sinkListDone && st->sourceListDone;
}

void AppendDeviceJson(std::ostringstream& os, const AudioDeviceEntry& e, bool needComma) {
  if (needComma) os << ", ";
  os << "{\"deviceID\":\"" << EscapeJsonString(e.deviceId) << "\"";
  os << ",\"name\":\"" << EscapeJsonString(e.name) << "\"";
  os << ",\"manufacturer\":\"" << EscapeJsonString(e.manufacturer) << "\"";
  os << ",\"transportType\":\"" << e.transportType << "\"";
  os << ",\"isInput\":" << (e.isInput ? "true" : "false");
  os << ",\"isOutput\":" << (e.isOutput ? "true" : "false");
  os << ",\"isDefault\":" << (e.isDefault ? "true" : "false");
  os << ",\"sampleRate\":" << e.sampleRate << ",\"channels\":" << e.channels;
  os << ",\"volume\":";
  if (e.hasVolume) {
    os << (double)e.volume;
  } else {
    os << 0;
  }
  os << ",\"isMuted\":";
  if (e.hasMute) {
    os << (e.isMuted ? "true" : "false");
  } else {
    os << "null";
  }
  os << ",\"isAlive\":" << (e.isAlive ? "true" : "false");
  os << "}";
}

std::string GetAudioDevicesJsonInternal() {
  DeviceListState st;
  if (!CollectAudioDevices(&st)) return "{\"error\": \"Failed to enumerate PulseAudio devices\"}";
  std::ostringstream os;
  os << "{\"inputDevices\":[";
  for (size_t i = 0; i < st.inputDevices.size(); ++i) AppendDeviceJson(os, st.inputDevices[i], i != 0);
  os << "],\"outputDevices\":[";
  for (size_t i = 0; i < st.outputDevices.size(); ++i) AppendDeviceJson(os, st.outputDevices[i], i != 0);
  os << "],\"defaultInputDeviceID\":";
  if (!st.defaultSourceName.empty()) {
    os << "\"" << EscapeJsonString(st.defaultSourceName) << "\"";
  } else {
    os << "null";
  }
  os << ",\"defaultOutputDeviceID\":";
  if (!st.defaultSinkName.empty()) {
    os << "\"" << EscapeJsonString(st.defaultSinkName) << "\"";
  } else {
    os << "null";
  }
  os << "}";
  return os.str();
}

}  // namespace

typedef void (*AudioMonitorCallback)(char*);

UH_EXPORT const char* getAudioDevicesJSON() {
  // koffi copies the returned C string; keep it alive per calling thread.
  static thread_local std::string result;
  try {
    result = GetAudioDevicesJsonInternal();
  } catch (...) {
    result = "{\"error\": \"Exception in audio device enumeration\"}";
  }
  return result.c_str();
}

UH_EXPORT void getAudioDevicesJSONAsync(AudioMonitorCallback cb) {
  if (!cb) return;
  try {
    std::thread([cb] {
      std::string json;
      try {
        json = GetAudioDevicesJsonInternal();
      } catch (...) {
        json = "{\"error\": \"Exception in audio device enumeration\"}";
      }
      // koffi marshals calls from foreign threads onto the JS thread and
      // blocks here until the JS callback has run, so json stays valid.
      cb(const_cast<char*>(json.c_str()));
    }).detach();
  } catch (...) {
  }
}
