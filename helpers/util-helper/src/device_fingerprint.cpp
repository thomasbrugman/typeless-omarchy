// getDeviceId(appName): a persistent random UUID (not hardware derived).
//
// Stored in two places, each used to repair the other:
//  * libsecret: schema "com.typeless.device.identifier" (attributes
//    service=<app>.deviceIdentifier, account=<app>.security.auth_key),
//    collection "default", label "<app>.deviceIdentifier"
//    (skipped when TYPELESS_SKIP_SECRET_STORE is set to a non-"0" value)
//  * file: $XDG_CONFIG_HOME/<app>/Cache/device.cache (no trailing newline)
// New IDs come from /proc/sys/kernel/random/uuid (fallback /dev/urandom v4).
#include <libsecret/secret.h>
#include <stdio.h>
#include <stdlib.h>

#include <cctype>
#include <fstream>

#include "common.h"

namespace {

const SecretSchema* GetDeviceSecretSchema() {
  static const SecretSchema schema = {
      "com.typeless.device.identifier",
      SECRET_SCHEMA_NONE,
      {
          {"service", SECRET_SCHEMA_ATTRIBUTE_STRING},
          {"account", SECRET_SCHEMA_ATTRIBUTE_STRING},
          {nullptr, (SecretSchemaAttributeType)0},
      },
      0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  return &schema;
}

// ^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$
bool IsValidUuid(const std::string& s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-') return false;
    } else if (!isxdigit((unsigned char)s[i])) {
      return false;
    }
  }
  return true;
}

std::string GenerateUuid() {
  {
    std::ifstream f("/proc/sys/kernel/random/uuid");
    if (f.is_open()) {
      std::string s;
      f >> s;
      if (IsValidUuid(s)) return s;
    }
  }
  unsigned char b[16] = {0};
  {
    std::ifstream f("/dev/urandom", std::ios::binary);
    if (f.is_open()) f.read(reinterpret_cast<char*>(b), sizeof(b));
  }
  b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
  b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
  char buf[40];
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
           b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return buf;
}

std::string ServiceAttr(const std::string& app) { return app + ".deviceIdentifier"; }
std::string AccountAttr(const std::string& app) { return app + ".security.auth_key"; }

std::string LoadUuidFromSecretStore(const std::string& app) {
  GError* error = nullptr;
  std::string service = ServiceAttr(app), account = AccountAttr(app);
  gchar* pw = secret_password_lookup_sync(GetDeviceSecretSchema(), nullptr, &error, "service", service.c_str(),
                                          "account", account.c_str(), nullptr);
  if (error) {
    g_error_free(error);
    if (pw) secret_password_free(pw);
    return std::string();
  }
  if (!pw) return std::string();
  std::string v(pw);
  secret_password_free(pw);
  return v;
}

void SaveUuidToSecretStore(const std::string& app, const std::string& uuid) {
  GError* error = nullptr;
  std::string service = ServiceAttr(app), account = AccountAttr(app);
  secret_password_store_sync(GetDeviceSecretSchema(), SECRET_COLLECTION_DEFAULT, service.c_str(), uuid.c_str(),
                             nullptr, &error, "service", service.c_str(), "account", account.c_str(), nullptr);
  if (error) g_error_free(error);
}

std::string GetCacheFilePath(const std::string& app) {
  std::string base = GetXdgConfigHome();
  if (base.empty()) return std::string();
  return base + "/" + app + "/Cache/" + "device.cache";
}

std::string LoadUuidFromFile(const std::string& app) {
  std::string path = GetCacheFilePath(app);
  if (path.empty()) return std::string();
  std::ifstream f(path);
  if (!f.is_open()) return std::string();
  std::string s;
  f >> s;
  return IsValidUuid(s) ? s : std::string();
}

void SaveUuidToFile(const std::string& app, const std::string& uuid) {
  std::string path = GetCacheFilePath(app);
  if (path.empty()) return;
  size_t slash = path.rfind('/');
  if (slash == std::string::npos) return;
  if (!EnsureDirectoryExists(path.substr(0, slash))) return;
  std::ofstream f(path, std::ios::out | std::ios::trunc);
  if (f.is_open()) f << uuid;
}

bool SkipSecretStore() {
  const char* v = getenv("TYPELESS_SKIP_SECRET_STORE");
  return v && *v && *v != '0';
}

}  // namespace

UH_EXPORT const char* getDeviceId(const char* appName) {
  static thread_local std::string result;
  try {
    std::string app = (appName && *appName) ? appName : "Typeless";
    const bool skipSecret = SkipSecretStore();
    std::string fromSecret = skipSecret ? std::string() : LoadUuidFromSecretStore(app);
    std::string fromFile = LoadUuidFromFile(app);

    if (!fromSecret.empty() && IsValidUuid(fromSecret)) {
      SaveUuidToFile(app, fromSecret);
      result = fromSecret;
    } else if (!fromFile.empty()) {
      if (!skipSecret) SaveUuidToSecretStore(app, fromFile);
      result = fromFile;
    } else {
      std::string fresh = GenerateUuid();
      if (!skipSecret) SaveUuidToSecretStore(app, fresh);
      SaveUuidToFile(app, fresh);
      result = fresh;
    }
  } catch (...) {
    result.clear();
  }
  return result.c_str();
}
