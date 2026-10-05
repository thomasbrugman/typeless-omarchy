// linux_apparmor.h - tiny AppArmor helpers used by typeless-atspi-helper
// (interoperable reimplementation of Typeless' linux-shared/linux_apparmor.h).
#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace LinuxShared {
namespace NativeChild {

// Contents of /proc/self/attr/current with trailing whitespace removed
// ("" when unreadable, e.g. no LSM).
inline std::string ReadAppArmorCurrentLabel()
{
    char buf[256];
    buf[0] = '\0';
    FILE *f = std::fopen("/proc/self/attr/current", "re");
    if (f == nullptr)
        return std::string();
    if (std::fgets(buf, sizeof buf, f) != nullptr) {
        size_t n = std::strlen(buf);
        while (n > 0) {
            unsigned char c = static_cast<unsigned char>(buf[n - 1]);
            if (c > ' ' || !(c == ' ' || c == '\t' || c == '\n' || c == '\r'))
                break;
            buf[--n] = '\0';
        }
    } else {
        buf[0] = '\0';
    }
    std::fclose(f);
    return std::string(buf);
}

// "unconfined" or "unconfined (...)"-style labels.
inline bool AppArmorLabelIsDbusUnconfined(const std::string &label)
{
    if (label == "unconfined")
        return true;
    return label.size() >= 12 && label.compare(0, 11, "unconfined ") == 0;
}

// The original helper, when running confined, writes "unconfined" to
// /proc/self/attr/current (a no-op/EINVAL on systems without AppArmor).
inline void TryLeaveAppArmorConfinement()
{
    if (AppArmorLabelIsDbusUnconfined(ReadAppArmorCurrentLabel()))
        return;
    int fd = ::open("/proc/self/attr/current", O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    ssize_t n = ::write(fd, "unconfined", 10);
    ::close(fd);
    if (n == 10)
        (void)ReadAppArmorCurrentLabel();
}

}  // namespace NativeChild
}  // namespace LinuxShared
