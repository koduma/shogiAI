#include "eval.hpp"
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

static const char* const DISCOVERY_CANDIDATES[] = {
    "src/eval/kpp.bin",
    "eval/kpp.bin",
    "src/eval/nn.bin",
    "eval/nn.bin",
};

constexpr char     KPP_MAGIC[]              = "ShogiAI-KPP-v1";
constexpr uint32_t KPP_VERSION              = 1;
constexpr uint32_t ENDIAN_MARKER            = 0x01020304u;
constexpr uint32_t HEADER_SIZE              = 64u;
constexpr uint32_t STORAGE_KIND_SPARSE_I16  = 1u;
constexpr uint32_t VALUE_TYPE_INT16         = 1u;
constexpr uint32_t MODEL_KING_SQUARE_COUNT  = 81u;
constexpr uint32_t MODEL_PIECE_VALUE_COUNT  = PT_NB;
constexpr uint32_t MODEL_ENTRY_BYTES        = 8u;
constexpr uint32_t MODEL_MATERIAL_BYTES     = MODEL_PIECE_VALUE_COUNT * 4u;
constexpr uint64_t MAX_MODEL_PAYLOAD_BYTES  = 512ull * 1024ull * 1024ull;
constexpr int      BOARD_FEATURE_TYPE_COUNT = 13;
constexpr int      BOARD_FEATURES_PER_COLOR = BOARD_FEATURE_TYPE_COUNT * SQUARE_NB;
constexpr int      BOARD_FEATURE_COUNT      = COLOR_NB * BOARD_FEATURES_PER_COLOR;
constexpr int      HAND_FEATURES_PER_COLOR  = 38;
constexpr int      MODEL_FEATURE_COUNT      = BOARD_FEATURE_COUNT + COLOR_NB * HAND_FEATURES_PER_COLOR;

struct HandFeatureSpec {
    PieceType pt;
    int max_count;
};

constexpr std::array<PieceType, BOARD_FEATURE_TYPE_COUNT> BOARD_FEATURE_TYPES{{
    PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK,
    PROM_PAWN, PROM_LANCE, PROM_KNIGHT, PROM_SILVER, PROM_BISHOP, PROM_ROOK
}};

constexpr std::array<HandFeatureSpec, 7> HAND_FEATURE_SPECS{{
    {PAWN, 18},
    {LANCE, 4},
    {KNIGHT, 4},
    {SILVER, 4},
    {GOLD, 4},
    {BISHOP, 2},
    {ROOK, 2},
}};

struct TableEntry {
    uint64_t key;
    int16_t  value;
};

struct KppModel {
    std::array<int, PT_NB> piece_value{};
    std::vector<TableEntry> entries;
};

struct LoadResult {
    bool success = false;
    bool stop_discovery = false;
    EvalFamily family = EvalFamily::MATERIAL_FALLBACK;
    std::shared_ptr<const KppModel> model;
    std::string status;
};

const std::array<int, PT_NB>& default_piece_values() {
    static const std::array<int, PT_NB> values{{
        0, 100, 300, 300, 500, 600, 800, 1000, 0, 600, 600, 600, 600, 1100, 1300
    }};
    return values;
}

bool        g_loaded_once   = false;
bool        g_explicit_path = false;
std::string g_eval_file_path;
std::string g_status        = "material-only fallback";
EvalFamily  g_family        = EvalFamily::MATERIAL_FALLBACK;
std::shared_ptr<const KppModel> g_model;

uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

int16_t read_i16_le(const uint8_t* p) {
    return static_cast<int16_t>(read_u16_le(p));
}

uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64_le(const uint8_t* p) {
    return static_cast<uint64_t>(read_u32_le(p)) |
           (static_cast<uint64_t>(read_u32_le(p + 4)) << 32);
}

uint32_t fnv1a32(const uint8_t* data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

int board_feature_type_index(PieceType pt) {
    for (int i = 0; i < static_cast<int>(BOARD_FEATURE_TYPES.size()); ++i) {
        if (BOARD_FEATURE_TYPES[i] == pt) return i;
    }
    return -1;
}

int hand_feature_index(Color c, PieceType pt, int ordinal) {
    int offset = BOARD_FEATURE_COUNT + static_cast<int>(c) * HAND_FEATURES_PER_COLOR;
    for (const HandFeatureSpec& spec : HAND_FEATURE_SPECS) {
        if (spec.pt == pt) {
            if (ordinal < 0 || ordinal >= spec.max_count) return -1;
            return offset + ordinal;
        }
        offset += spec.max_count;
    }
    return -1;
}

int board_feature_index(Piece p, Square sq) {
    if (p == NO_PIECE) return -1;
    const PieceType pt = type_of(p);
    if (pt == KING) return -1;
    const int type_index = board_feature_type_index(pt);
    if (type_index < 0) return -1;
    return static_cast<int>(color_of(p)) * BOARD_FEATURES_PER_COLOR +
           type_index * SQUARE_NB + sq;
}

uint64_t make_entry_key(Square king_sq, int feature_a, int feature_b) {
    const uint32_t a = static_cast<uint32_t>(std::min(feature_a, feature_b));
    const uint32_t b = static_cast<uint32_t>(std::max(feature_a, feature_b));
    return (static_cast<uint64_t>(static_cast<uint32_t>(king_sq)) << 32) |
           (static_cast<uint64_t>(a) << 16) |
           static_cast<uint64_t>(b);
}

void collect_features(const Board& board, std::vector<int>& features) {
    features.clear();
    features.reserve(40);

    for (int sq = 0; sq < SQUARE_NB; ++sq) {
        const int index = board_feature_index(board.piece_at(sq), sq);
        if (index >= 0) features.push_back(index);
    }

    for (int color = BLACK; color <= WHITE; ++color) {
        const Color c = static_cast<Color>(color);
        for (const HandFeatureSpec& spec : HAND_FEATURE_SPECS) {
            const int count = std::min(board.hand(c, spec.pt), spec.max_count);
            for (int ordinal = 0; ordinal < count; ++ordinal) {
                const int index = hand_feature_index(c, spec.pt, ordinal);
                if (index >= 0) features.push_back(index);
            }
        }
    }
}

int lookup_table_value(const KppModel& model, Square king_sq, int feature_a, int feature_b) {
    const uint64_t key = make_entry_key(king_sq, feature_a, feature_b);
    const auto it = std::lower_bound(
        model.entries.begin(), model.entries.end(), key,
        [](const TableEntry& entry, uint64_t value) {
            return entry.key < value;
        });
    return (it != model.entries.end() && it->key == key) ? static_cast<int>(it->value) : 0;
}

int kpp_term_for_side(const KppModel& model, Square king_sq, const std::vector<int>& features) {
    int64_t acc = 0;
    for (size_t i = 0; i < features.size(); ++i) {
        for (size_t j = i + 1; j < features.size(); ++j) {
            acc += lookup_table_value(model, king_sq, features[i], features[j]);
        }
    }

    if (acc > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
    if (acc < std::numeric_limits<int>::min()) return std::numeric_limits<int>::min();
    return static_cast<int>(acc);
}

int material_score(const Board& board, Color us, const std::array<int, PT_NB>& piece_value) {
    int64_t score = 0;

    for (int sq = 0; sq < SQUARE_NB; ++sq) {
        const Piece p = board.piece_at(sq);
        if (p == NO_PIECE) continue;
        const int value = piece_value[type_of(p)];
        score += (color_of(p) == us) ? value : -value;
    }

    for (int pt = PAWN; pt <= ROOK; ++pt) {
        const int value = piece_value[pt];
        score += static_cast<int64_t>(board.hand(us,  static_cast<PieceType>(pt))) * value;
        score -= static_cast<int64_t>(board.hand(~us, static_cast<PieceType>(pt))) * value;
    }

    if (score > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
    if (score < std::numeric_limits<int>::min()) return std::numeric_limits<int>::min();
    return static_cast<int>(score);
}

bool path_looks_like_nnue(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    const std::string name = (pos == std::string::npos) ? path : path.substr(pos + 1);
    return name == "nn.bin" || name == "nnue.bin";
}

bool file_declares_nnue_manifest(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;

    char buf[256]{};
    ifs.read(buf, sizeof(buf));
    const std::string text(buf, static_cast<size_t>(ifs.gcount()));
    return text.find("model_type=nnue") != std::string::npos ||
           text.find("model_type = nnue") != std::string::npos;
}

bool load_kpp_model_binary(const std::string& path,
                           std::shared_ptr<const KppModel>& model_out,
                           std::string& error) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        error = "file not found";
        return false;
    }

    const std::streamoff file_size_stream = ifs.tellg();
    if (file_size_stream < static_cast<std::streamoff>(HEADER_SIZE)) {
        error = "file too small";
        return false;
    }
    const uint64_t file_size = static_cast<uint64_t>(file_size_stream);
    if (file_size > HEADER_SIZE + MAX_MODEL_PAYLOAD_BYTES) {
        error = "model exceeds maximum supported payload size";
        return false;
    }

    ifs.seekg(0, std::ios::beg);
    std::array<uint8_t, HEADER_SIZE> header{};
    if (!ifs.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()))) {
        error = "failed to read header";
        return false;
    }

    const size_t magic_length = sizeof(KPP_MAGIC) - 1;
    if (!std::equal(header.begin(), header.begin() + static_cast<std::ptrdiff_t>(magic_length), KPP_MAGIC) ||
        !std::all_of(header.begin() + static_cast<std::ptrdiff_t>(magic_length), header.begin() + 16,
                     [](uint8_t byte) { return byte == 0; })) {
        error = "magic/version mismatch";
        return false;
    }

    const uint32_t version           = read_u32_le(header.data() + 16);
    const uint32_t endian_marker     = read_u32_le(header.data() + 20);
    const uint32_t header_size       = read_u32_le(header.data() + 24);
    const uint32_t king_square_count = read_u32_le(header.data() + 28);
    const uint32_t feature_count     = read_u32_le(header.data() + 32);
    const uint32_t piece_value_count = read_u32_le(header.data() + 36);
    const uint32_t entry_count       = read_u32_le(header.data() + 40);
    const uint32_t storage_kind      = read_u32_le(header.data() + 44);
    const uint32_t value_type        = read_u32_le(header.data() + 48);
    const uint64_t payload_bytes     = read_u64_le(header.data() + 52);
    const uint32_t payload_checksum  = read_u32_le(header.data() + 60);

    if (version != KPP_VERSION) {
        error = "unsupported version";
        return false;
    }
    if (endian_marker != ENDIAN_MARKER) {
        error = "unsupported endianness marker";
        return false;
    }
    if (header_size != HEADER_SIZE) {
        error = "unexpected header size";
        return false;
    }
    if (king_square_count != MODEL_KING_SQUARE_COUNT) {
        error = "king square count mismatch";
        return false;
    }
    if (feature_count != MODEL_FEATURE_COUNT) {
        error = "feature count mismatch";
        return false;
    }
    if (piece_value_count != MODEL_PIECE_VALUE_COUNT) {
        error = "piece value count mismatch";
        return false;
    }
    if (storage_kind != STORAGE_KIND_SPARSE_I16) {
        error = "unsupported storage kind";
        return false;
    }
    if (value_type != VALUE_TYPE_INT16) {
        error = "unsupported table element type";
        return false;
    }
    if (payload_bytes > MAX_MODEL_PAYLOAD_BYTES) {
        error = "payload too large";
        return false;
    }

    const uint64_t expected_payload_bytes =
        static_cast<uint64_t>(MODEL_MATERIAL_BYTES) + static_cast<uint64_t>(entry_count) * MODEL_ENTRY_BYTES;
    if (payload_bytes != expected_payload_bytes) {
        error = "payload length mismatch";
        return false;
    }
    if (file_size != static_cast<uint64_t>(HEADER_SIZE) + payload_bytes) {
        error = "truncated or oversized payload";
        return false;
    }

    std::vector<uint8_t> payload;
    try {
        payload.resize(static_cast<size_t>(payload_bytes));
    } catch (const std::bad_alloc&) {
        error = "allocation failure while reading model";
        return false;
    }

    if (!ifs.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()))) {
        error = "failed to read payload";
        return false;
    }
    if (fnv1a32(payload.data(), payload.size()) != payload_checksum) {
        error = "payload checksum mismatch";
        return false;
    }

    try {
        auto model = std::make_shared<KppModel>();
        model->piece_value = default_piece_values();
        for (int i = 0; i < PT_NB; ++i) {
            model->piece_value[i] = static_cast<int>(read_u32_le(payload.data() + i * 4));
        }
        model->piece_value[NO_PT] = 0;
        model->piece_value[KING]  = 0;

        model->entries.reserve(entry_count);
        uint64_t previous_key = 0;
        const uint8_t* entry_ptr = payload.data() + MODEL_MATERIAL_BYTES;
        for (uint32_t i = 0; i < entry_count; ++i, entry_ptr += MODEL_ENTRY_BYTES) {
            const uint16_t king_sq   = read_u16_le(entry_ptr + 0);
            const uint16_t feature_a = read_u16_le(entry_ptr + 2);
            const uint16_t feature_b = read_u16_le(entry_ptr + 4);
            const int16_t  value     = read_i16_le(entry_ptr + 6);

            if (king_sq >= MODEL_KING_SQUARE_COUNT) {
                error = "entry king square out of range";
                return false;
            }
            if (feature_a >= MODEL_FEATURE_COUNT || feature_b >= MODEL_FEATURE_COUNT) {
                error = "entry feature index out of range";
                return false;
            }
            if (feature_a >= feature_b) {
                error = "entry feature pair must be strictly ordered";
                return false;
            }

            const uint64_t key = make_entry_key(static_cast<Square>(king_sq), feature_a, feature_b);
            if (!model->entries.empty() && key <= previous_key) {
                error = "entries must be strictly sorted and unique";
                return false;
            }
            previous_key = key;
            model->entries.push_back(TableEntry{key, value});
        }

        model_out = std::static_pointer_cast<const KppModel>(model);
    } catch (const std::bad_alloc&) {
        error = "allocation failure while building model";
        return false;
    }
    return true;
}

LoadResult try_load_from_path(const std::string& path, bool is_auto) {
    LoadResult result;
    std::ifstream probe(path, std::ios::binary);
    if (!probe) {
        if (!is_auto) {
            result.status = "material-only fallback (file not found: " + path + ")";
        }
        return result;
    }

    if (path_looks_like_nnue(path) || file_declares_nnue_manifest(path)) {
        const std::string tag = is_auto ? "auto-discovered" : "explicit";
        result.stop_discovery = true;
        result.family = EvalFamily::NNUE_UNSUPPORTED;
        result.status = "nnue unsupported (source: " + path + " [" + tag + "]); using material-only fallback";
        return result;
    }

    std::shared_ptr<const KppModel> model;
    std::string error;
    if (load_kpp_model_binary(path, model, error)) {
        const std::string tag = is_auto ? "auto-discovered" : "explicit";
        result.success = true;
        result.stop_discovery = true;
        result.family = EvalFamily::KPP_TABLE;
        result.model = std::move(model);
        result.status = "kpp table: loaded from " + path + " [" + tag + "]";
        return result;
    }

    if (!is_auto) {
        result.status = "material-only fallback (invalid ShogiAI-KPP-v1 file: " + path + "; " + error + ")";
    } else {
        result.status = "material-only fallback (ignored invalid auto-discovery candidate: " + path + "; " + error + ")";
    }
    return result;
}

void try_load_eval_file_once() {
    if (g_loaded_once) return;
    g_loaded_once = true;
    g_model.reset();
    g_family = EvalFamily::MATERIAL_FALLBACK;
    g_status = "material-only fallback";

    if (g_explicit_path && !g_eval_file_path.empty()) {
        const LoadResult result = try_load_from_path(g_eval_file_path, /*is_auto=*/false);
        g_model = result.model;
        g_family = result.family;
        g_status = result.status.empty() ? "material-only fallback" : result.status;
        return;
    }

    if (const char* env_path = std::getenv("SHOGIAI_EVAL_FILE")) {
        if (*env_path) {
            const LoadResult result = try_load_from_path(std::string(env_path), /*is_auto=*/false);
            g_model = result.model;
            g_family = result.family;
            g_status = result.status.empty() ? "material-only fallback" : result.status;
            return;
        }
    }

    std::string auto_error_status;
    for (const char* candidate : DISCOVERY_CANDIDATES) {
        const LoadResult result = try_load_from_path(candidate, /*is_auto=*/true);
        if (result.success || result.stop_discovery) {
            g_model = result.model;
            g_family = result.family;
            g_status = result.status;
            return;
        }
        if (!result.status.empty()) {
            auto_error_status = result.status;
        }
    }

    if (!auto_error_status.empty()) {
        g_status = auto_error_status;
    } else {
        g_status = "material-only fallback (no compatible ShogiAI-KPP-v1 file found; auto-discovery checked: src/eval/kpp.bin, eval/kpp.bin, src/eval/nn.bin, eval/nn.bin)";
    }
}

} // namespace

void set_eval_file_path(const std::string& path) {
    g_eval_file_path = path;
    g_explicit_path  = !path.empty();
    g_loaded_once    = false;
    g_status         = "material-only fallback";
    g_family         = EvalFamily::MATERIAL_FALLBACK;
    g_model.reset();
}

std::string eval_status_message() {
    try_load_eval_file_once();
    return g_status;
}

EvalFamily get_eval_family() {
    try_load_eval_file_once();
    return g_family;
}

int evaluate(const Board& board) {
    try_load_eval_file_once();

    const Color us = board.side_to_move();
    const auto& piece_value = g_model ? g_model->piece_value : default_piece_values();
    int score = material_score(board, us, piece_value);

    if (g_model && g_family == EvalFamily::KPP_TABLE) {
        std::vector<int> features;
        collect_features(board, features);
        score += kpp_term_for_side(*g_model, board.king_sq(us),  features);
        score -= kpp_term_for_side(*g_model, board.king_sq(~us), features);
    }

    return score;
}
