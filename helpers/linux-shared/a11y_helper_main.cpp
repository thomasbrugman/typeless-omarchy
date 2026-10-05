// a11y_helper_main.cpp - typeless-atspi-helper entry point (interoperable
// reimplementation of Typeless' linux-shared/a11y_helper_main.cpp).
//
// Single-threaded server:
//   * exits when the parent dies (PR_SET_PDEATHSIG=SIGTERM + getppid() poll),
//   * singleton per user via flock(<runtime>/instance.lock),
//   * listens on <runtime>/helper.sock (runtime = $XDG_RUNTIME_DIR/typeless-atspi
//     or /tmp/typeless-atspi-<uid>), accepts only same-uid peers,
//   * serves length-prefixed JSON requests one at a time, pumping the
//     private AT-SPI GMainContext every 15 ms between requests.
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <vector>

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "a11y_handlers.h"
#include "a11y_ipc.h"
#include "atspi_accessible.h"
#include "linux_apparmor.h"
#include "native_child_guard.h"

namespace {

volatile sig_atomic_t g_stop = 0;
int g_listenFd = -1;
char g_socketPath[512];
char g_pidPath[512];

void OnSignal(int)
{
    g_stop = 1;
}

bool PeerIsSameUser(int fd)
{
    struct ucred cred;
    socklen_t len = sizeof cred;
    cred.pid = 0;
    cred.uid = static_cast<uid_t>(-1);
    cred.gid = static_cast<gid_t>(-1);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return false;
    return cred.uid == ::getuid();
}

// Reads one request, answers it. false = drop the connection.
bool ServeOneRequest(int fd)
{
    using namespace LinuxShared::A11y;
    std::string frame;
    if (!ReadFrame(fd, frame, 2000))
        return false;
    A11yRequest request;
    if (!DecodeRequest(frame, request))
        return false;
    A11yResponse response = HandleRequest(request);
    return WriteFrame(fd, EncodeResponse(response));
}

}  // namespace

int main()
{
    // Children spawned by the original libs inherit the spawning thread's
    // signal mask; make sure SIGTERM (our PDEATHSIG) is deliverable.
    sigset_t all;
    sigemptyset(&all);
    sigprocmask(SIG_SETMASK, &all, nullptr);

    std::signal(SIGTERM, OnSignal);
    std::signal(SIGINT, OnSignal);
    std::signal(SIGPIPE, SIG_IGN);

    if (TypelessNativeChildInstallParentDeathSignal() != 0) {
        std::fwrite("typeless-atspi-helper: parent already gone\n", 1, 43, stderr);
        return 0;
    }

    LinuxShared::NativeChild::TryLeaveAppArmorConfinement();

    if (!LinuxShared::AtspiApi::Load()) {
        std::fwrite("typeless-atspi-helper: failed to load libatspi.so.0\n", 1, 52, stderr);
        return 1;
    }

    int alreadyRunning = 0;
    if (TypelessNativeChildClaimInstanceLock("typeless-atspi", &alreadyRunning) != 0) {
        if (alreadyRunning)
            return 0;  // another helper for this user is serving
        std::fwrite("typeless-atspi-helper: instance lock failed\n", 1, 44, stderr);
        return 1;
    }

    LinuxShared::Atspi::EnsureInitialized();
    LinuxShared::Atspi::EnableSessionAccessibility(false);

    g_listenFd = TypelessNativeChildBindListenSocket("typeless-atspi", "helper.sock", "helper.pid",
                                                     g_socketPath, sizeof g_socketPath, g_pidPath,
                                                     sizeof g_pidPath);
    if (g_listenFd < 0) {
        std::fwrite("typeless-atspi-helper: listen socket failed\n", 1, 44, stderr);
        TypelessNativeChildReleaseListenSocket(g_listenFd, g_socketPath, g_pidPath);
        return 1;
    }

    std::vector<int> clients;
    while (!g_stop && !TypelessNativeChildParentIsGone()) {
        std::vector<struct pollfd> fds;
        fds.push_back({g_listenFd, POLLIN, 0});
        for (int c : clients)
            fds.push_back({c, POLLIN, 0});

        int rc = ::poll(fds.data(), fds.size(), 15);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (rc > 0) {
            std::vector<int> kept;
            kept.reserve(clients.size());
            for (size_t i = 0; i < clients.size(); ++i) {
                int fd = clients[i];
                short re = fds[i + 1].revents;
                bool keep = true;
                if (re & POLLIN)
                    keep = ServeOneRequest(fd);
                else if (re & (POLLERR | POLLHUP | POLLNVAL))
                    keep = false;
                if (keep)
                    kept.push_back(fd);
                else
                    ::close(fd);
            }
            clients.swap(kept);

            if (fds[0].revents & POLLIN) {
                int fd = ::accept4(g_listenFd, nullptr, nullptr, SOCK_CLOEXEC);
                if (fd >= 0) {
                    if (PeerIsSameUser(fd))
                        clients.push_back(fd);
                    else
                        ::close(fd);
                }
            }
        }
        LinuxShared::A11y::PumpApartmentEvents();
        LinuxShared::A11y::DrainCacheUpdates();
    }

    for (int c : clients)
        ::close(c);
    TypelessNativeChildReleaseListenSocket(g_listenFd, g_socketPath, g_pidPath);
    g_listenFd = -1;
    return 0;
}
