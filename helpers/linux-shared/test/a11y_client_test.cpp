// a11y_client_test.cpp - smoke test for the A11y client module.
//
//   a11y_client_test reach|focus|input|probe|window|visible|url|surround|recover|
//                    enable|note-own|release|terminate|threads|raw <json>|codec
//
// Set TYPELESS_ATSPI_HELPER=/path/to/typeless-atspi-helper when the helper
// is not next to this executable.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

#include "a11y_client.h"
#include "a11y_ipc.h"
#include "native_child.h"

using namespace LinuxShared;
using namespace LinuxShared::A11y;

static void Print(const char *what, const A11yResponse &r, double ms)
{
    std::printf("%s: id=%llu ok=%d error=%s failReason=\"%s\" generation=%llu (%.1f ms)\n  payload=%s\n",
                what, (unsigned long long)r.id, r.ok ? 1 : 0, A11yErrorName(r.error),
                r.failReason.c_str(), (unsigned long long)r.generation, ms,
                r.payloadJson.c_str());
}

template <typename F>
static double TimeMs(F &&f)
{
    auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

static int Codec()
{
    A11yRequest q;
    q.id = 42;
    q.command = "GetSurroundingText";
    q.windowTitle = "a \"quoted\"\ttitle\\ \x01";
    q.busName = ":1.23";
    q.objectPath = "/org/a11y/atspi/accessible/7";
    q.generation = 9;
    q.reassert = true;
    q.foregroundNote = "lock";
    std::string enc = EncodeRequest(q);
    std::printf("request: %s\n", enc.c_str());
    A11yRequest d;
    bool ok = DecodeRequest(enc, d);
    std::printf("decode ok=%d cmd=%s title=[%s] gen=%llu reassert=%d\n", ok, d.command.c_str(),
                d.windowTitle.c_str(), (unsigned long long)d.generation, d.reassert);
    A11yResponse r;
    r.id = 42;
    r.ok = true;
    r.error = A11yError::kOk;
    r.payloadJson = "{\"a\":{\"error\":\"x}\"},\"b\":[1,2]}";
    std::string er = EncodeResponse(r);
    std::printf("response: %s\n", er.c_str());
    A11yResponse rd;
    DecodeResponse(er, rd);
    std::printf("decoded payload: %s error=%s\n", rd.payloadJson.c_str(), A11yErrorName(rd.error));
    return 0;
}

// Sends a raw frame on a fresh connection and prints the answer.
static int Raw(const char *json)
{
    if (!NativeChild::EnsureReady(NativeChild::Kind::kAtspiHelper)) {
        std::printf("helper not ready\n");
        return 1;
    }
    int fd = NativeChild::Connect(NativeChild::Kind::kAtspiHelper);
    if (fd < 0) {
        std::printf("connect failed\n");
        return 1;
    }
    std::string out;
    bool w = WriteFrame(fd, json);
    bool rd = ReadFrame(fd, out, 3000);
    std::printf("write=%d read=%d answer=%s\n", w, rd, out.c_str());
    ::close(fd);
    return rd ? 0 : 2;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <command>\n", argv[0]);
        return 2;
    }
    std::string cmd = argv[1];
    Client &c = Client::Instance();
    A11yResponse r;
    double ms = 0;
    if (cmd == "codec")
        return Codec();
    if (cmd == "raw")
        return Raw(argc > 2 ? argv[2] : "{}");
    if (cmd == "paths") {
        std::printf("binary=%s\nruntime=%s\nsocket=%s\npid=%s\n",
                    NativeChild::ResolvedBinaryPath(NativeChild::Kind::kAtspiHelper).c_str(),
                    NativeChild::RuntimeDirectory(NativeChild::Kind::kAtspiHelper).c_str(),
                    NativeChild::SocketPath(NativeChild::Kind::kAtspiHelper).c_str(),
                    NativeChild::PidFilePath(NativeChild::Kind::kAtspiHelper).c_str());
        return 0;
    }
    if (cmd == "reach") {
        bool b = false;
        ms = TimeMs([&] { b = c.IsDesktopReachable(); });
        std::printf("IsDesktopReachable=%d (%.1f ms)\n", b, ms);
        return 0;
    }
    if (cmd == "probe") {
        Atspi::FocusedInputCapability cap{};
        ms = TimeMs([&] { cap = c.ProbeFocusedInputCapability(); });
        std::printf("ProbeFocusedInputCapability=%d (%.1f ms)\n", (int)cap, ms);
        return 0;
    }
    if (cmd == "recover") {
        bool b = false;
        ms = TimeMs([&] { b = c.RecoverAfterResume(); });
        std::printf("RecoverAfterResume=%d (%.1f ms)\n", b, ms);
        return 0;
    }
    if (cmd == "enable") {
        bool b = false;
        ms = TimeMs([&] { b = c.EnableSessionA11yStatus(argc > 2 && std::atoi(argv[2]) != 0); });
        std::printf("EnableSessionA11yStatus=%d (%.1f ms)\n", b, ms);
        return 0;
    }
    if (cmd == "note-own") { c.NoteOwnProcessForeground(); std::printf("done\n"); return 0; }
    if (cmd == "release") { c.ReleaseCompositorForegroundLock(); std::printf("done\n"); return 0; }
    if (cmd == "note-pid") {
        c.NoteForegroundProcessId(argc > 2 ? std::atoi(argv[2]) : 0);
        std::printf("done\n");
        return 0;
    }
    int timeout = 1500;
    if (const char *t = std::getenv("A11Y_TIMEOUT_MS"))
        timeout = std::atoi(t);
    if (cmd == "focus") {
        ms = TimeMs([&] { r = c.GetFocusSnapshot(timeout); });
        Print("GetFocusSnapshot", r, ms);
    } else if (cmd == "input") {
        ms = TimeMs([&] { r = c.ReadFocusedInputState(timeout); });
        Print("ReadFocusedInputState", r, ms);
    } else if (cmd == "window") {
        ms = TimeMs([&] { r = c.GetActiveWindowSnapshot(timeout); });
        Print("GetActiveWindowSnapshot", r, ms);
    } else if (cmd == "visible") {
        ms = TimeMs([&] { r = c.GetVisibleText(argc > 2 ? std::atoi(argv[2]) : 2000, timeout); });
        Print("GetVisibleText", r, ms);
    } else if (cmd == "url") {
        ms = TimeMs([&] {
            r = c.FindDocumentUrlInActiveWindow(argc > 2 ? argv[2] : "", 0, timeout);
        });
        Print("FindDocumentUrlInActiveWindow", r, ms);
    } else if (cmd == "surround") {
        A11yResponse f = c.GetFocusSnapshot(timeout);
        FocusKey key;
        JsonReadString(f.payloadJson, "busName", key.busName);
        JsonReadString(f.payloadJson, "objectPath", key.objectPath);
        key.generation = f.generation;
        ms = TimeMs([&] { r = c.GetSurroundingText(200, 200, timeout, key); });
        Print("GetSurroundingText", r, ms);
    } else if (cmd == "fixture") {
        // Lock the foreground to <pid> (as the compositor bridge would) and
        // exercise every read command against it.
        int pid = argc > 2 ? std::atoi(argv[2]) : 0;
        ms = TimeMs([&] { r = c.GetActiveWindowSnapshot(timeout, pid, ForegroundNote::kLock); });
        Print("GetActiveWindowSnapshot(lock)", r, ms);
        ms = TimeMs([&] { r = c.GetFocusSnapshot(timeout); });
        Print("GetFocusSnapshot", r, ms);
        FocusKey key;
        JsonReadString(r.payloadJson, "busName", key.busName);
        JsonReadString(r.payloadJson, "objectPath", key.objectPath);
        key.generation = r.generation;
        ms = TimeMs([&] { r = c.ReadFocusedInputState(timeout); });
        Print("ReadFocusedInputState", r, ms);
        Atspi::FocusedInputCapability cap{};
        ms = TimeMs([&] { cap = c.ProbeFocusedInputCapability(); });
        std::printf("ProbeFocusedInputCapability=%d (%.1f ms)\n", (int)cap, ms);
        ms = TimeMs([&] { r = c.GetSurroundingText(8, 8, timeout, key); });
        Print("GetSurroundingText", r, ms);
        FocusKey stale = key;
        stale.objectPath += "0";
        ms = TimeMs([&] { r = c.GetSurroundingText(8, 8, timeout, stale); });
        Print("GetSurroundingText(stale key)", r, ms);
        ms = TimeMs([&] { r = c.GetVisibleText(500, timeout); });
        Print("GetVisibleText", r, ms);
        ms = TimeMs([&] { r = c.FindDocumentUrlInActiveWindow("", pid, timeout); });
        Print("FindDocumentUrlInActiveWindow", r, ms);
        c.ReleaseCompositorForegroundLock();
    } else if (cmd == "respawn") {
        // Kill the helper behind the client's back; the next call must
        // transparently respawn it (write_failed/read_timeout -> retry).
        std::printf("reach=%d\n", c.IsDesktopReachable());
        for (int round = 0; round < 3; ++round) {
            int hp = 0;
            FILE *f = std::fopen(NativeChild::PidFilePath(NativeChild::Kind::kAtspiHelper).c_str(), "r");
            if (f) { if (std::fscanf(f, "%d", &hp) != 1) hp = 0; std::fclose(f); }
            std::printf("killing helper pid %d with SIGKILL\n", hp);
            if (hp > 0) ::kill(hp, SIGKILL);
            ::usleep(500000);  // > 400 ms spawn throttle
            ms = TimeMs([&] { r = c.ReadFocusedInputState(timeout); });
            Print("after kill", r, ms);
        }
    } else if (cmd == "stall") {
        // SIGSTOP the helper: calls must fail with read_timeout within
        // (timeoutMs + 200) per attempt, then recover after SIGCONT.
        c.IsDesktopReachable();
        int hp = 0;
        FILE *f = std::fopen(NativeChild::PidFilePath(NativeChild::Kind::kAtspiHelper).c_str(), "r");
        if (f) { if (std::fscanf(f, "%d", &hp) != 1) hp = 0; std::fclose(f); }
        ::kill(hp, SIGSTOP);
        ms = TimeMs([&] { r = c.ReadFocusedInputState(100); });
        Print("stopped helper, retry on", r, ms);
        Client::SetRetryAfterReadTimeout(false);
        ms = TimeMs([&] { r = c.ReadFocusedInputState(100); });
        Print("stopped helper, retry off", r, ms);
        ::kill(hp, SIGCONT);
        ::usleep(200000);
        ms = TimeMs([&] { r = c.ReadFocusedInputState(300); });
        Print("after SIGCONT", r, ms);
    } else if (cmd == "malformed") {
        const char *cases[] = {"not json", "{\"id\":5}", "{\"id\":6,\"cmd\":\"NoSuchCommand\"}",
                               "{\"id\":7,\"cmd\":\"IsDesktopReachable\"}"};
        for (const char *j : cases) {
            std::printf("-- %s\n", j);
            Raw(j);
        }
    } else if (cmd == "threads") {
        std::vector<std::thread> ts;
        for (int i = 0; i < 8; ++i) {
            ts.emplace_back([&c, i, timeout] {
                for (int k = 0; k < 10; ++k) {
                    A11yResponse x = (i % 2) ? c.ReadFocusedInputState(timeout)
                                             : c.GetFocusSnapshot(timeout);
                    if (!x.ok)
                        std::printf("thread %d call %d failed: %s %s\n", i, k,
                                    A11yErrorName(x.error), x.failReason.c_str());
                }
            });
        }
        for (auto &t : ts)
            t.join();
        std::printf("threads done\n");
    } else if (cmd == "terminate") {
        c.TerminateHelper();
        std::printf("terminated; respawnStopped=%d\n",
                    NativeChild::RespawnStopped(NativeChild::Kind::kAtspiHelper));
        r = c.ReadFocusedInputState(timeout);
        Print("after terminate", r, 0);
    } else {
        std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
        return 2;
    }
    return 0;
}
