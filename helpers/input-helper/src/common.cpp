#include "common.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>

extern char **environ;

namespace InputHelper {

// ---------------------------------------------------------------- logging
static std::mutex g_logMutex;

void Log(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    fprintf(stderr, "%s\n", buf);
    fflush(stderr);
}

// ---------------------------------------------------------------- lifecycle
static std::atomic<bool> g_shuttingDown{false};
bool IsShuttingDown() { return g_shuttingDown.load(); }
void SetShuttingDown(bool value) { g_shuttingDown.store(value); }

// ---------------------------------------------------------------- time
int64_t NowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void SleepMs(int ms)
{
    if (ms <= 0) {
        return;
    }
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

// ---------------------------------------------------------------- strings
std::string CleanZeroWidthChars(const std::string &text)
{
    static const std::string zwsp = "\xE2\x80\x8B";
    std::string out = text;
    size_t pos = 0;
    while ((pos = out.find(zwsp, pos)) != std::string::npos) {
        out.erase(pos, zwsp.size());
    }
    return out;
}

static size_t Utf8SeqLen(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;  // stray continuation byte counts as one
}

size_t Utf8CodePointCount(const std::string &text)
{
    size_t count = 0;
    for (size_t i = 0; i < text.size();) {
        i += Utf8SeqLen((unsigned char)text[i]);
        count++;
    }
    return count;
}

std::string LimitTextLength(const std::string &text, size_t maxCodePoints, bool keepTail)
{
    std::vector<size_t> starts;
    starts.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        starts.push_back(i);
        i += Utf8SeqLen((unsigned char)text[i]);
    }
    if (starts.size() <= maxCodePoints) {
        return text;
    }
    if (keepTail) {
        return text.substr(starts[starts.size() - maxCodePoints]);
    }
    return text.substr(0, starts[maxCodePoints]);
}

std::string EscapeHtml(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string HtmlToPlainText(const std::string &html)
{
    // Same naive tag stripper as the original: drop <...> and '\r'.
    std::string out;
    bool inTag = false;
    for (char c : html) {
        if (c == '<') {
            inTag = true;
            continue;
        }
        if (c == '>') {
            inTag = false;
            continue;
        }
        if (inTag || c == '\r') {
            continue;
        }
        out += c;
    }
    return out;
}

std::string JsonEscape(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// ---------------------------------------------------------------- JSON
const JsonValue *JsonValue::Get(const std::string &key) const
{
    if (type != Object) return nullptr;
    auto it = o.find(key);
    return it == o.end() ? nullptr : &it->second;
}

std::string JsonValue::GetString(const std::string &key, const std::string &def) const
{
    const JsonValue *v = Get(key);
    return (v && v->type == String) ? v->s : def;
}

bool JsonValue::GetBool(const std::string &key, bool def) const
{
    const JsonValue *v = Get(key);
    return (v && v->type == Bool) ? v->b : def;
}

double JsonValue::GetNumber(const std::string &key, double def) const
{
    const JsonValue *v = Get(key);
    return (v && v->type == Number) ? v->n : def;
}

namespace {
struct JsonParser {
    const std::string &t;
    size_t i = 0;
    int depth = 0;
    explicit JsonParser(const std::string &text) : t(text) {}

    void Ws()
    {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) i++;
    }
    static void AppendUtf8(std::string &s, uint32_t cp)
    {
        if (cp < 0x80) {
            s += (char)cp;
        } else if (cp < 0x800) {
            s += (char)(0xC0 | (cp >> 6));
            s += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += (char)(0xE0 | (cp >> 12));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        } else {
            s += (char)(0xF0 | (cp >> 18));
            s += (char)(0x80 | ((cp >> 12) & 0x3F));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool Hex4(uint32_t &v)
    {
        if (i + 4 > t.size()) return false;
        v = 0;
        for (int k = 0; k < 4; k++) {
            char c = t[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool Str(std::string &out)
    {
        if (i >= t.size() || t[i] != '"') return false;
        i++;
        while (i < t.size()) {
            char c = t[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= t.size()) return false;
            char e = t[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        size_t save = i;
                        i += 2;
                        uint32_t lo;
                        if (Hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            i = save;
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool Val(JsonValue &v)
    {
        if (++depth > 64) return false;
        Ws();
        if (i >= t.size()) return false;
        char c = t[i];
        bool ok = false;
        if (c == '{') {
            v.type = JsonValue::Object;
            i++;
            Ws();
            if (i < t.size() && t[i] == '}') {
                i++;
                ok = true;
            } else {
                while (true) {
                    Ws();
                    std::string key;
                    if (!Str(key)) break;
                    Ws();
                    if (i >= t.size() || t[i] != ':') break;
                    i++;
                    JsonValue child;
                    if (!Val(child)) break;
                    v.o[key] = std::move(child);
                    Ws();
                    if (i < t.size() && t[i] == ',') {
                        i++;
                        continue;
                    }
                    if (i < t.size() && t[i] == '}') {
                        i++;
                        ok = true;
                    }
                    break;
                }
            }
        } else if (c == '[') {
            v.type = JsonValue::Array;
            i++;
            Ws();
            if (i < t.size() && t[i] == ']') {
                i++;
                ok = true;
            } else {
                while (true) {
                    JsonValue child;
                    if (!Val(child)) break;
                    v.a.push_back(std::move(child));
                    Ws();
                    if (i < t.size() && t[i] == ',') {
                        i++;
                        continue;
                    }
                    if (i < t.size() && t[i] == ']') {
                        i++;
                        ok = true;
                    }
                    break;
                }
            }
        } else if (c == '"') {
            v.type = JsonValue::String;
            ok = Str(v.s);
        } else if (t.compare(i, 4, "true") == 0) {
            v.type = JsonValue::Bool;
            v.b = true;
            i += 4;
            ok = true;
        } else if (t.compare(i, 5, "false") == 0) {
            v.type = JsonValue::Bool;
            i += 5;
            ok = true;
        } else if (t.compare(i, 4, "null") == 0) {
            i += 4;
            ok = true;
        } else {
            const char *start = t.c_str() + i;
            char *end = nullptr;
            double d = strtod(start, &end);
            if (end && end != start) {
                v.type = JsonValue::Number;
                v.n = d;
                i += (size_t)(end - start);
                ok = true;
            }
        }
        depth--;
        return ok;
    }
};
}  // namespace

bool JsonParse(const std::string &text, JsonValue &out)
{
    JsonParser p(text);
    out = JsonValue();
    if (!p.Val(out)) return false;
    p.Ws();
    return p.i == text.size();
}

// ---------------------------------------------------------------- processes
static void LibraryAnchor() {}

std::string LibraryDirectory()
{
    Dl_info info;
    if (dladdr((void *)&LibraryAnchor, &info) && info.dli_fname) {
        char resolved[4096];
        const char *path = info.dli_fname;
        if (realpath(path, resolved)) {
            path = resolved;
        }
        std::string p(path);
        size_t slash = p.rfind('/');
        if (slash != std::string::npos) {
            return p.substr(0, slash);
        }
    }
    return ".";
}

std::vector<std::string> SanitizedChildEnvironment()
{
    // Launcher / bundle leftovers that break system binaries (wl-copy etc.).
    static const char *const drop[] = {"DESKTOP_STARTUP_ID", "XDG_ACTIVATION_TOKEN", "GIO_LAUNCHED_DESKTOP_FILE",
                                       "GIO_LAUNCHED_DESKTOP_FILE_PID", "LD_LIBRARY_PATH", "LD_PRELOAD",
                                       "GTK_PATH", "GIO_MODULE_DIR", "QT_PLUGIN_PATH", "APPDIR", nullptr};
    std::vector<std::string> env;
    bool hasPath = false;
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        std::string key(*e, (size_t)(eq - *e));
        bool skip = false;
        for (int k = 0; drop[k]; k++) {
            if (key == drop[k]) {
                skip = true;
                break;
            }
        }
        if (skip) continue;
        if (key == "PATH") {
            if (eq[1] == '\0') continue;
            hasPath = true;
        }
        env.emplace_back(*e);
    }
    if (!hasPath) {
        env.emplace_back("PATH=/usr/local/bin:/usr/bin:/bin");
    }
    return env;
}

static std::string FindInPath(const std::string &name)
{
    if (name.find('/') != std::string::npos) {
        return access(name.c_str(), X_OK) == 0 ? name : std::string();
    }
    const char *path = getenv("PATH");
    std::string p = (path && *path) ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        std::string dir = p.substr(start, end - start);
        if (!dir.empty()) {
            std::string full = dir + "/" + name;
            if (access(full.c_str(), X_OK) == 0) return full;
        }
        start = end + 1;
    }
    return std::string();
}

bool CommandExists(const char *name)
{
    static std::mutex m;
    static std::map<std::string, bool> cache;
    std::lock_guard<std::mutex> lock(m);
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    bool exists = !FindInPath(name).empty();
    cache[name] = exists;
    return exists;
}

SpawnResult RunProcess(const std::vector<std::string> &argv, const std::string &input, int timeoutMs,
                       size_t maxOutput)
{
    SpawnResult result;
    if (argv.empty()) return result;
    std::string exe = FindInPath(argv[0]);
    if (exe.empty()) return result;

    int inPipe[2] = {-1, -1}, outPipe[2] = {-1, -1};
    if (pipe2(inPipe, O_CLOEXEC) < 0) return result;
    if (pipe2(outPipe, O_CLOEXEC) < 0) {
        close(inPipe[0]);
        close(inPipe[1]);
        return result;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, outPipe[1], STDOUT_FILENO);
    {
        // Inherit stderr for diagnostics when usable, otherwise give the child /dev/null
        // (a closed fd 2 would let the child's next socket land on it).
        int flags = fcntl(STDERR_FILENO, F_GETFD);
        if (flags < 0 || (flags & FD_CLOEXEC)) {
            posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        }
    }
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);  // don't leak Electron fds into daemons
#endif
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGCHLD);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGHUP);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::vector<std::string> envStrings = SanitizedChildEnvironment();
    std::vector<char *> envp;
    for (auto &e : envStrings) envp.push_back(const_cast<char *>(e.c_str()));
    envp.push_back(nullptr);
    std::vector<char *> args;
    for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);

    pid_t pid = -1;
    int rc = posix_spawn(&pid, exe.c_str(), &fa, &attr, args.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(inPipe[0]);
    close(outPipe[1]);
    if (rc != 0) {
        Log("[Clipboard] posix_spawnp(%s) failed: %s", argv[0].c_str(), strerror(rc));
        close(inPipe[1]);
        close(outPipe[0]);
        return result;
    }
    result.started = true;

    int64_t deadline = NowMs() + timeoutMs;
    int inFd = inPipe[1];
    int outFd = outPipe[0];
    fcntl(inFd, F_SETFL, fcntl(inFd, F_GETFL) | O_NONBLOCK);
    size_t written = 0;
    if (input.empty()) {
        close(inFd);
        inFd = -1;
    }
    char buf[65536];
    while (inFd >= 0 || outFd >= 0) {
        int64_t left = deadline - NowMs();
        if (left <= 0) break;
        struct pollfd pfds[2];
        int n = 0;
        int inIdx = -1, outIdx = -1;
        if (inFd >= 0) {
            pfds[n] = {inFd, POLLOUT, 0};
            inIdx = n++;
        }
        if (outFd >= 0) {
            pfds[n] = {outFd, POLLIN, 0};
            outIdx = n++;
        }
        int pr = poll(pfds, (nfds_t)n, (int)left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) break;
        if (inIdx >= 0 && pfds[inIdx].revents) {
            ssize_t w = write(inFd, input.data() + written, input.size() - written);
            if (w > 0) {
                written += (size_t)w;
            }
            if ((w < 0 && errno != EAGAIN && errno != EINTR) || written >= input.size() ||
                (pfds[inIdx].revents & (POLLERR | POLLHUP))) {
                if (written < input.size()) {
                    Log("[Clipboard] Short write to clipboard process: %s", strerror(errno));
                }
                close(inFd);
                inFd = -1;
            }
        }
        if (outIdx >= 0 && pfds[outIdx].revents) {
            ssize_t r = read(outFd, buf, sizeof buf);
            if (r > 0) {
                if (result.out.size() + (size_t)r <= maxOutput) result.out.append(buf, (size_t)r);
            } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                close(outFd);
                outFd = -1;
            }
        }
    }
    if (inFd >= 0) close(inFd);
    if (outFd >= 0) close(outFd);

    // Reap (the child exits right after handing over, like wl-copy's parent).
    int status = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            result.exited = true;
            if (WIFEXITED(status)) {
                result.exitCode = WEXITSTATUS(status);
            } else {
                result.signaled = true;
            }
            break;
        }
        if (w < 0 && errno != EINTR) break;
        if (NowMs() >= deadline) {
            Log("[Clipboard] waitpid timed out, terminating pid %d", (int)pid);
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            break;
        }
        SleepMs(2);
    }
    return result;
}

}  // namespace InputHelper
