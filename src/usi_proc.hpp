#pragma once
#include "types.hpp"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct UsiEvalResult {
    bool        ok             = false;
    Move        best_move      = MOVE_NONE;
    std::string best_usi       = "resign";
    bool        is_declare_win = false; // `bestmove win` (CSA Nyugyoku declaration in Suisho / YaneuraOu)
    bool        is_resign      = false; // `bestmove resign`
    int         score_stm_cp   = 0;     // side-to-move POV centipawns (mates mapped to +-30000)
    bool        is_mate        = false;
    int         mate_plies     = 0;
    int         depth          = 0;
    uint64_t    nodes          = 0;
};

struct UsiOptionMeta {
    std::string              canonical_name;
    std::string              type;        // "spin", "check", "string", "combo", "button", "filename"
    std::string              default_val;
    int64_t                  min_val    = 0;
    int64_t                  max_val    = 0;
    bool                     has_bounds = false;
    std::vector<std::string> vars;
};

// Path utilities shared by generator & proxy player
std::string executable_directory();
std::string join_path(const std::string& dir, const std::string& name);
std::string parent_directory(const std::string& path);
bool        file_exists_nonempty_path(const std::string& path);
std::string resolve_sibling_or_cwd_path(const std::string& path);
std::string resolve_engine_executable_path(const std::string& requested_path);

// Manages a persistent external USI engine process (`shogi_engine.exe`, Suisho 5 / YaneuraOu, etc.)
// over anonymous stdin/stdout pipes on both Windows (Win32 CreateProcessA) and POSIX.
// Automatically inspects the child engine's `option name ...` capabilities during `usi -> usiok`
// so `shogi_engine.exe` can be swapped directly with Suisho 5 (`YaneuraOu*.exe`) with zero manual setup.
class ExternalUsiEngine {
public:
    ExternalUsiEngine();
    ~ExternalUsiEngine();

    ExternalUsiEngine(const ExternalUsiEngine&) = delete;
    ExternalUsiEngine& operator=(const ExternalUsiEngine&) = delete;

    bool start(const std::string& exe_path,
               const std::vector<std::pair<std::string, std::string>>& raw_options = {},
               std::string* err_out = nullptr,
               bool defer_isready = false);

    bool is_running() const { return running_; }
    const std::string& exe_path() const { return resolved_exe_; }
    const std::string& engine_id_name() const { return engine_id_name_; }

    bool has_option(const std::string& name) const;
    bool set_option(const std::string& name, const std::string& value);
    bool set_option_if_supported(const std::string& name, const std::string& value);

    // Cross-engine semantic configuration helpers (work identically for `shogi_engine.exe` and Suisho 5 / YaneuraOu)
    void configure_threads(int threads);
    void configure_hash_mb(int hash_mb);
    void configure_node_limit(uint64_t nodes);
    void configure_own_book(bool enable_own_book);
    void configure_zero_overhead_local_pipe();

    bool ensure_ready();
    bool send_usinewgame();
    bool send_raw_line(const std::string& cmd_line);

    // Runs `position ...` + `go ...`, optionally forwarding every `info ...` line live,
    // and blocks until `bestmove ...` is returned (or pipe closes).
    // If `hard_timeout_ms > 0`, automatically sends `"stop\n"` if `hard_timeout_ms` elapses.
    UsiEvalResult run_search(const std::string& position_cmd,
                             const std::string& go_cmd,
                             const std::function<void(const std::string&)>& on_info_line = {},
                             int hard_timeout_ms = 0);

    // Thread-safe asynchronous `"stop\n"` write (can be called from main USI thread or watchdog timer
    // while worker thread is inside `run_search()`).
    void send_stop();

    void stop_process();

private:
    bool write_line(const std::string& line);
    bool read_line(std::string& out_line);
    void parse_usi_option_line(const std::string& line);

    bool        running_ = false;
    std::string resolved_exe_;
    std::string engine_id_name_;
    std::string read_buf_;
    std::mutex  write_mtx_;
    std::unordered_map<std::string, UsiOptionMeta> options_map_;

#if defined(_WIN32)
    void* child_stdin_wr_  = nullptr;
    void* child_stdout_rd_ = nullptr;
    void* child_proc_      = nullptr;
#else
    int   child_stdin_wr_  = -1;
    int   child_stdout_rd_ = -1;
    int   child_pid_       = -1;
#endif
};
