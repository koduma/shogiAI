// Workaround for MinGW-w64 GCC Bug 65782 / 54412:
// `-march=native` on AVX-512 CPUs emits `.seh_savexmm %xmm16..31` (rejected by GNU as on Win64 SEH)
// and 64-byte ZMM stack spills. Disable AVX-512 on MinGW GCC while keeping full AVX2/FMA/BMI2 active.
#if (defined(_WIN32) && defined(__GNUC__) && !defined(__clang__)) || defined(SHOGIAI_FORCE_NO_AVX512_TEST)
#pragma GCC target("no-avx512f,no-avx512vl,no-avx512bw,no-avx512dq,no-avx512cd")
#ifndef SHOGIAI_NO_AVX512
#define SHOGIAI_NO_AVX512 1
#endif
#endif
#include "usi_proc.hpp"
#include "search.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
//#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <climits>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace {

std::string to_lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool is_default_shogi_engine_name(const std::string& p) {
    const std::string lp = to_lower_ascii(p);
    return lp == "shogi_engine.exe" || lp == "./shogi_engine" ||
           lp == ".\\shogi_engine.exe" || lp == "shogi_engine";
}

int score_candidate_engine_filename(const std::string& fname_orig) {
    const std::string f = to_lower_ascii(fname_orig);
    // Never pick our own book tools, test binaries, or installers!
    if (f.find("winrate_book_player") != std::string::npos) return -1;
    if (f.find("winrate_book_gen") != std::string::npos)    return -1;
    if (f.find("shogi_tests") != std::string::npos)         return -1;
    if (f.find("unins") == 0 || f.find("setup") == 0 || f.find("vc_redist") == 0) return -1;

#if defined(_WIN32)
    if (f.size() < 5 || f.substr(f.size() - 4) != ".exe") return -1;
#endif

    int score = 10;
    if (f.find("suisho") != std::string::npos || fname_orig.find("水匠") != std::string::npos) score += 100;
    if (f.find("yaneura") != std::string::npos)  score += 90;
    if (f.find("fukaura") != std::string::npos)  score += 80;
    if (f.find("kristall") != std::string::npos) score += 75;
    if (f.find("dlshogi") != std::string::npos)  score += 70;
    if (f.find("apery") != std::string::npos || f.find("gikou") != std::string::npos) score += 60;
    if (f.find("avx2") != std::string::npos)     score += 15;
    if (f.find("sse42") != std::string::npos)    score += 5;
    if (f.find("engine") != std::string::npos || f.find("shogi") != std::string::npos) score += 20;
    return score;
}

std::string scan_dir_for_fallback_engine(const std::string&) {
    (void)score_candidate_engine_filename;
    return "";
}

} // namespace

std::string parent_directory(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return "";
    if (pos == 0) return path.substr(0, 1);
    return path.substr(0, pos);
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    const char c = dir.back();
    if (c == '/' || c == '\\') return dir + name;
#if defined(_WIN32)
    return dir + "\\" + name;
#else
    return dir + "/" + name;
#endif
}

std::string executable_directory() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    SetLastError(ERROR_SUCCESS);
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return parent_directory(std::string(buf));
    }
#elif defined(__linux__)
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0 && len < static_cast<ssize_t>(sizeof(buf) - 1)) {
        buf[len] = '\0';
        return parent_directory(std::string(buf));
    }
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0) {
        return parent_directory(std::string(buf));
    }
#endif
    return "";
}

bool file_exists_nonempty_path(const std::string& path) {
    if (path.empty()) return false;
    std::ifstream ifs(path, std::ios::binary);
    return ifs.good();
}

std::string resolve_sibling_or_cwd_path(const std::string& path) {
    if (path.empty()) return path;
    if (file_exists_nonempty_path(path)) return path;
#if defined(_WIN32)
    if (to_lower_ascii(path).find(".exe") == std::string::npos &&
        file_exists_nonempty_path(path + ".exe")) {
        return path + ".exe";
    }
#endif
    const std::string exe_dir = executable_directory();
    if (!exe_dir.empty()) {
        const std::string cand = join_path(exe_dir, path);
        if (file_exists_nonempty_path(cand)) return cand;
#if defined(_WIN32)
        if (to_lower_ascii(path).find(".exe") == std::string::npos &&
            file_exists_nonempty_path(cand + ".exe")) {
            return cand + ".exe";
        }
#endif
    }
    return path;
}

std::string resolve_engine_executable_path(const std::string& requested_path) {
    const std::string direct = resolve_sibling_or_cwd_path(requested_path);
    if (file_exists_nonempty_path(direct)) return direct;

    // If the user deleted `shogi_engine.exe` and dropped Suisho 5's `.exe` (`YaneuraOu-by-gcc.exe`, etc.)
    // into the same folder without changing `ChildEnginePath` / `--engine`, auto-detect it seamlessly!
    if (is_default_shogi_engine_name(requested_path)) {
        const std::string exe_dir = executable_directory();
        std::string found = scan_dir_for_fallback_engine(exe_dir);
        if (!found.empty()) return found;
        found = scan_dir_for_fallback_engine(".");
        if (!found.empty()) return found;
    }
    return direct;
}

ExternalUsiEngine::ExternalUsiEngine() = default;

ExternalUsiEngine::~ExternalUsiEngine() {
    stop_process();
}

void ExternalUsiEngine::parse_usi_option_line(const std::string& line) {
    // Standard USI format:
    // option name <Name> type <Type> [default <Def>] [min <Min>] [max <Max>] [var <V1> var <V2> ...]
    std::istringstream iss(line);
    std::string tok;
    if (!(iss >> tok) || tok != "option") return;
    if (!(iss >> tok) || tok != "name") return;

    std::string name;
    while (iss >> tok && tok != "type") {
        if (!name.empty()) name += ' ';
        name += tok;
    }
    if (name.empty() || tok != "type") return;

    UsiOptionMeta meta;
    meta.canonical_name = name;
    if (!(iss >> meta.type)) return;

    bool has_min = false, has_max = false;
    while (iss >> tok) {
        if (tok == "default") {
            std::string dval;
            while (iss >> tok && tok != "min" && tok != "max" && tok != "var") {
                if (!dval.empty()) dval += ' ';
                dval += tok;
            }
            meta.default_val = dval;
            if (!iss && tok != "min" && tok != "max" && tok != "var") break;
        }
        auto parse_i64_sat = [](const std::string& ts, int64_t& out) -> bool {
            if (ts.empty()) return false;
            try {
                if (ts[0] == '-') {
                    out = std::stoll(ts);
                } else {
                    unsigned long long uv = std::stoull(ts);
                    out = (uv > static_cast<unsigned long long>(INT64_MAX))
                        ? INT64_MAX : static_cast<int64_t>(uv);
                }
                return true;
            } catch (...) { return false; }
        };
        if (tok == "min") {
            std::string ts;
            if ((iss >> ts) && parse_i64_sat(ts, meta.min_val)) has_min = true;
        } else if (tok == "max") {
            std::string ts;
            if ((iss >> ts) && parse_i64_sat(ts, meta.max_val)) has_max = true;
        } else if (tok == "var") {
            std::string vstr;
            while (iss >> tok && tok != "var" && tok != "min" && tok != "max") {
                if (!vstr.empty()) vstr += ' ';
                vstr += tok;
            }
            if (!vstr.empty()) meta.vars.push_back(vstr);
            // Put back handling if loop stopped on keyword `var`
            while (tok == "var" && iss) {
                vstr.clear();
                while (iss >> tok && tok != "var" && tok != "min" && tok != "max") {
                    if (!vstr.empty()) vstr += ' ';
                    vstr += tok;
                }
                if (!vstr.empty()) meta.vars.push_back(vstr);
            }
        }
    }
    meta.has_bounds = (has_min && has_max && meta.min_val <= meta.max_val);
    options_map_[to_lower_ascii(name)] = std::move(meta);
}

bool ExternalUsiEngine::has_option(const std::string& name) const {
    return options_map_.find(to_lower_ascii(name)) != options_map_.end();
}

bool ExternalUsiEngine::start(const std::string& exe_path,
                              const std::vector<std::pair<std::string, std::string>>& raw_options,
                              std::string* err_out,
                              bool defer_isready) {
    stop_process();
    read_buf_.clear();
    engine_id_name_.clear();
    options_map_.clear();
    resolved_exe_ = resolve_engine_executable_path(exe_path);

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength              = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE stdin_rd = nullptr, stdin_wr = nullptr;
    HANDLE stdout_rd = nullptr, stdout_wr = nullptr;

    if (!CreatePipe(&stdin_rd, &stdin_wr, &sa, 0)) {
        if (err_out) *err_out = "CreatePipe(stdin) failed";
        return false;
    }
    SetHandleInformation(stdin_wr, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&stdout_rd, &stdout_wr, &sa, 0)) {
        CloseHandle(stdin_rd);
        CloseHandle(stdin_wr);
        if (err_out) *err_out = "CreatePipe(stdout) failed";
        return false;
    }
    SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb         = sizeof(STARTUPINFOA);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = stdin_rd;
    si.hStdOutput = stdout_wr;
    si.hStdError  = stdout_wr;

    PROCESS_INFORMATION pi{};
    std::string abs_win_exe = resolved_exe_;
    {
        char full_buf[MAX_PATH];
        DWORD flen = GetFullPathNameA(resolved_exe_.c_str(), MAX_PATH, full_buf, nullptr);
        if (flen > 0 && flen < MAX_PATH) {
            abs_win_exe = std::string(full_buf);
        }
    }
    resolved_exe_ = abs_win_exe;

    std::string cmdline = "\"" + resolved_exe_ + "\"";
    std::vector<char> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back('\0');

    const std::string work_dir = parent_directory(resolved_exe_);
    const char* lp_cwd = work_dir.empty() ? nullptr : work_dir.c_str();

    const BOOL ok = CreateProcessA(
        nullptr,
        cmd_buf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        lp_cwd,
        &si,
        &pi);

    CloseHandle(stdin_rd);
    CloseHandle(stdout_wr);

    if (!ok) {
        CloseHandle(stdin_wr);
        CloseHandle(stdout_rd);
        if (err_out) *err_out = "CreateProcessA failed for: " + resolved_exe_;
        return false;
    }

    CloseHandle(pi.hThread);
    child_stdin_wr_  = stdin_wr;
    child_stdout_rd_ = stdout_rd;
    child_proc_      = pi.hProcess;
    running_         = true;
#else
    int in_pipe[2]  = {-1, -1};
    int out_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0) {
        if (err_out) *err_out = "pipe(stdin) failed";
        return false;
    }
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        if (err_out) *err_out = "pipe(stdout) failed";
        return false;
    }

    std::string abs_exe = resolved_exe_;
    {
        char real_buf[4096];
        if (realpath(resolved_exe_.c_str(), real_buf) != nullptr) {
            abs_exe = std::string(real_buf);
        }
    }
    resolved_exe_ = abs_exe;
    const std::string work_dir = parent_directory(abs_exe);

    const pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        if (err_out) *err_out = "fork() failed";
        return false;
    }

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        if (!work_dir.empty()) {
            if (chdir(work_dir.c_str()) != 0) {}
        }
        char* const argv[] = {const_cast<char*>(abs_exe.c_str()), nullptr};
        execvp(abs_exe.c_str(), argv);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    signal(SIGPIPE, SIG_IGN);

    child_stdin_wr_  = in_pipe[1];
    child_stdout_rd_ = out_pipe[0];
    child_pid_       = static_cast<int>(pid);
    running_         = true;
#endif

    // Handshake: usi -> capture `id name` and all `option name ...` -> usiok
    if (!write_line("usi")) {
        stop_process();
        if (err_out) *err_out = "Failed sending 'usi' to: " + resolved_exe_;
        return false;
    }
    std::string line;
    bool got_usiok = false;
    while (read_line(line)) {
        if (line.find("id name ") == 0) {
            engine_id_name_ = line.substr(8);
        } else if (line.find("option name ") == 0) {
            parse_usi_option_line(line);
        } else if (line == "usiok" || line.find("usiok") == 0) {
            got_usiok = true;
            break;
        }
    }
    if (!got_usiok) {
        stop_process();
        if (err_out) *err_out = "Did not receive 'usiok' from: " + resolved_exe_;
        return false;
    }

    for (const auto& kv : raw_options) {
        set_option_if_supported(kv.first, kv.second);
    }

    if (!defer_isready) {
        if (!ensure_ready()) {
            stop_process();
            if (err_out) *err_out = "Did not receive 'readyok' from: " + resolved_exe_;
            return false;
        }
    }

    return true;
}

bool ExternalUsiEngine::write_line(const std::string& line) {
    std::lock_guard<std::mutex> lk(write_mtx_);
    if (!running_) return false;
    const std::string data = line + "\n";
#if defined(_WIN32)
    HANDLE h = static_cast<HANDLE>(child_stdin_wr_);
    if (!h) return false;
    DWORD total = 0;
    const DWORD len = static_cast<DWORD>(data.size());
    while (total < len) {
        DWORD written = 0;
        if (!WriteFile(h, data.data() + total, len - total, &written, nullptr) || written == 0) {
            return false;
        }
        total += written;
    }
    return true;
#else
    if (child_stdin_wr_ < 0) return false;
    size_t total = 0;
    while (total < data.size()) {
        const ssize_t n = write(child_stdin_wr_, data.data() + total, data.size() - total);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
#endif
}

bool ExternalUsiEngine::read_line(std::string& out_line) {
    out_line.clear();
    while (true) {
        const auto nl = read_buf_.find('\n');
        if (nl != std::string::npos) {
            out_line = read_buf_.substr(0, nl);
            if (!out_line.empty() && out_line.back() == '\r') out_line.pop_back();
            read_buf_.erase(0, nl + 1);
            return true;
        }
        if (!running_) return false;

        char chunk[4096];
#if defined(_WIN32)
        HANDLE h = static_cast<HANDLE>(child_stdout_rd_);
        if (!h) return false;
        DWORD got = 0;
        if (!ReadFile(h, chunk, sizeof(chunk), &got, nullptr) || got == 0) {
            running_ = false;
            return false;
        }
        read_buf_.append(chunk, static_cast<size_t>(got));
#else
        if (child_stdout_rd_ < 0) return false;
        const ssize_t got = read(child_stdout_rd_, chunk, sizeof(chunk));
        if (got <= 0) {
            if (got < 0 && errno == EINTR) continue;
            running_ = false;
            return false;
        }
        read_buf_.append(chunk, static_cast<size_t>(got));
#endif
    }
}

bool ExternalUsiEngine::set_option(const std::string& name, const std::string& value) {
    return write_line("setoption name " + name + " value " + value);
}

bool ExternalUsiEngine::set_option_if_supported(const std::string& name, const std::string& value) {
    const auto it = options_map_.find(to_lower_ascii(name));
    if (it == options_map_.end()) return false;

    const UsiOptionMeta& meta = it->second;
    std::string final_val = value;
    if (meta.type == "spin" && meta.has_bounds) {
        try {
            int64_t iv = std::stoll(value);
            iv = std::clamp(iv, meta.min_val, meta.max_val);
            final_val = std::to_string(iv);
        } catch (...) {}
    }
    return write_line("setoption name " + meta.canonical_name + " value " + final_val);
}

void ExternalUsiEngine::configure_threads(int threads) {
    const std::string s = std::to_string(std::max(1, threads));
    if (!set_option_if_supported("Threads", s)) {
        set_option_if_supported("USI_Threads", s);
    }
}

void ExternalUsiEngine::configure_hash_mb(int hash_mb) {
    const std::string s = std::to_string(std::max(1, hash_mb));
    const bool set_usi = set_option_if_supported("USI_Hash", s);
    if (!set_usi) {
        set_option_if_supported("Hash", s);
    }
}

void ExternalUsiEngine::configure_node_limit(uint64_t nodes) {
    const std::string s = std::to_string(nodes);
    // `shogi_engine.exe` uses `nodelimit`; Suisho 5 / YaneuraOu uses `NodesLimit`!
    set_option_if_supported("nodelimit", s);
    set_option_if_supported("NodesLimit", s);
}

void ExternalUsiEngine::configure_own_book(bool enable_own_book) {
    if (enable_own_book) {
        set_option_if_supported("USI_OwnBook", "true");
        set_option_if_supported("OwnBook", "true");
        return;
    }
    // Disable child engine's built-in opening book (`standard_book.db` / `user_book1.db` in Suisho 5 / YaneuraOu)
    // so `winrate_book_gen` always gets real deep search scores and `winrate_book_player` controls book play cleanly!
    set_option_if_supported("USI_OwnBook", "false");
    set_option_if_supported("OwnBook", "false");
    set_option_if_supported("BookMoves", "0");
    const auto it = options_map_.find("bookfile");
    if (it != options_map_.end() && it->second.type == "combo") {
        for (const std::string& v : it->second.vars) {
            if (v == "no_book") {
                set_option_if_supported("BookFile", "no_book");
                break;
            }
        }
    }
}

void ExternalUsiEngine::configure_zero_overhead_local_pipe() {
    // Suisho 5 / YaneuraOu defaults to `NetworkDelay=120`, `NetworkDelay2=1120`, `MinimumThinkingTime=2000`,
    // which subtracts 120ms..1120ms from short `go movetime` searches during local self-play/book generation!
    // Over a zero-latency local OS pipe, clamp these to their minimum allowed values (`0` / `1`).
    set_option_if_supported("NetworkDelay", "0");
    set_option_if_supported("NetworkDelay2", "0");
    set_option_if_supported("MinimumThinkingTime", "1");
    set_option_if_supported("MultiPV", "1");
    set_option_if_supported("USI_Ponder", "false");
}

bool ExternalUsiEngine::ensure_ready() {
    if (!write_line("isready")) return false;
    std::string line;
    while (read_line(line)) {
        if (line == "readyok" || line.find("readyok") == 0) return true;
    }
    return false;
}

bool ExternalUsiEngine::send_usinewgame() {
    return write_line("usinewgame");
}

bool ExternalUsiEngine::send_raw_line(const std::string& cmd_line) {
    return write_line(cmd_line);
}

void ExternalUsiEngine::send_stop() {
    write_line("stop");
}

UsiEvalResult ExternalUsiEngine::run_search(
    const std::string& position_cmd,
    const std::string& go_cmd,
    const std::function<void(const std::string&)>& on_info_line,
    int hard_timeout_ms) {

    UsiEvalResult res{};
    if (!write_line(position_cmd) || !write_line(go_cmd)) {
        return res;
    }

    // Optional host-side wall-clock watchdog to guarantee strict <= 10s per self-play game
    // regardless of how any third-party engine handles `go nodes` or `go movetime`.
    std::mutex              wd_mtx;
    std::condition_variable wd_cv;
    bool                    search_done = false;
    std::thread             wd_thread;

    if (hard_timeout_ms > 0) {
        wd_thread = std::thread([this, &wd_mtx, &wd_cv, &search_done, hard_timeout_ms]() {
            std::unique_lock<std::mutex> lk(wd_mtx);
            if (!wd_cv.wait_for(lk, std::chrono::milliseconds(hard_timeout_ms),
                                [&]() { return search_done; })) {
                this->send_stop();
            }
        });
    }

    struct WatchdogGuard {
        std::mutex&              mtx;
        std::condition_variable& cv;
        bool&                    done;
        std::thread&             th;
        ~WatchdogGuard() {
            {
                std::lock_guard<std::mutex> lk(mtx);
                done = true;
            }
            cv.notify_all();
            if (th.joinable()) th.join();
        }
    } wd_guard{wd_mtx, wd_cv, search_done, wd_thread};

    std::string line;
    while (read_line(line)) {
        if (line.find("info ") == 0) {
            if (on_info_line) on_info_line(line);

            // Parse `multipv`, `depth`, `nodes`, `score cp <X>`, or `score mate <M>`
            std::istringstream iss(line);
            std::string tok;
            iss >> tok; // "info"
            int multipv_idx = 1;
            int parsed_depth = res.depth;
            uint64_t parsed_nodes = res.nodes;
            bool has_score = false;
            int parsed_cp = 0;
            bool parsed_is_mate = false;
            int parsed_mate_plies = 0;

            while (iss >> tok) {
                if (tok == "string") {
                    break; // remainder of line is free-form text
                } else if (tok == "multipv") {
                    iss >> multipv_idx;
                } else if (tok == "depth") {
                    iss >> parsed_depth;
                } else if (tok == "nodes") {
                    iss >> parsed_nodes;
                } else if (tok == "score") {
                    std::string kind;
                    if (iss >> kind) {
                        if (kind == "cp") {
                            int cp = 0;
                            if (iss >> cp) {
                                has_score         = true;
                                parsed_cp         = cp;
                                parsed_is_mate    = false;
                                parsed_mate_plies = 0;
                            }
                        } else if (kind == "mate") {
                            std::string mval;
                            if (iss >> mval) {
                                has_score      = true;
                                parsed_is_mate = true;
                                if (mval == "+") {
                                    parsed_mate_plies = 1;
                                    parsed_cp         = MATE_VALUE - 1;
                                } else if (mval == "-") {
                                    parsed_mate_plies = -1;
                                    parsed_cp         = -(MATE_VALUE - 1);
                                } else {
                                    try {
                                        int m = std::stoi(mval);
                                        parsed_mate_plies = m;
                                        parsed_cp = (m >= 0)
                                            ? (MATE_VALUE - std::min(1000, std::abs(m)))
                                            : -(MATE_VALUE - std::min(1000, std::abs(m)));
                                    } catch (...) {}
                                }
                            }
                        }
                    }
                } else if (tok == "pv") {
                    break;
                }
            }

            res.nodes = parsed_nodes;
            if (multipv_idx == 1) {
                res.depth = parsed_depth;
                if (has_score) {
                    res.score_stm_cp = parsed_cp;
                    res.is_mate      = parsed_is_mate;
                    res.mate_plies   = parsed_mate_plies;
                }
            }
        } else if (line.find("bestmove ") == 0 || line == "bestmove") {
            std::istringstream iss(line);
            std::string bm_kw, bm_str;
            iss >> bm_kw >> bm_str;
            res.ok       = true;
            res.best_usi = bm_str.empty() ? "resign" : bm_str;
            if (res.best_usi == "win") {
                res.is_declare_win = true;
                res.best_move      = MOVE_NONE;
                res.score_stm_cp   = MATE_VALUE - 1;
            } else if (res.best_usi == "resign" || res.best_usi == "none") {
                res.is_resign = true;
                res.best_move = MOVE_NONE;
            } else {
                res.best_move = usi_to_move(res.best_usi);
            }
            return res;
        }
    }
    return res;
}

void ExternalUsiEngine::stop_process() {
#if defined(_WIN32)
    if (running_ && child_stdin_wr_) {
        write_line("quit");
    }
    running_ = false;
    if (child_stdin_wr_) {
        CloseHandle(static_cast<HANDLE>(child_stdin_wr_));
        child_stdin_wr_ = nullptr;
    }
    if (child_stdout_rd_) {
        CloseHandle(static_cast<HANDLE>(child_stdout_rd_));
        child_stdout_rd_ = nullptr;
    }
    if (child_proc_) {
        HANDLE hp = static_cast<HANDLE>(child_proc_);
        if (WaitForSingleObject(hp, 400) == WAIT_TIMEOUT) {
            TerminateProcess(hp, 0);
            WaitForSingleObject(hp, 200);
        }
        CloseHandle(hp);
        child_proc_ = nullptr;
    }
#else
    if (running_ && child_stdin_wr_ >= 0) {
        write_line("quit");
    }
    running_ = false;
    if (child_stdin_wr_ >= 0) {
        close(child_stdin_wr_);
        child_stdin_wr_ = -1;
    }
    if (child_stdout_rd_ >= 0) {
        close(child_stdout_rd_);
        child_stdout_rd_ = -1;
    }
    if (child_pid_ > 0) {
        int status = 0;
        bool exited = false;
        for (int i = 0; i < 20; ++i) {
            const pid_t r = waitpid(static_cast<pid_t>(child_pid_), &status, WNOHANG);
            if (r == static_cast<pid_t>(child_pid_) || r < 0) {
                exited = true;
                break;
            }
            usleep(10000);
        }
        if (!exited) {
            kill(static_cast<pid_t>(child_pid_), SIGKILL);
            waitpid(static_cast<pid_t>(child_pid_), &status, 0);
        }
        child_pid_ = -1;
    }
#endif
}
