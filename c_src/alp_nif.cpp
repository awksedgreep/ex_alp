// ALP NIF — Adaptive Lossless Floating-Point compression for Erlang/Elixir
//
// Exports:
//   nif_alp_encode(timestamps, values) -> {:ok, binary}
//   nif_alp_decode(binary)             -> {:ok, [{int64, float}]}
//
// The binary format:
//   magic "AL" (2 bytes)
//   version (1 byte) = 1
//   point_count (4 bytes, uint32 big-endian)
//   first_timestamp (8 bytes, int64 big-endian)
//   timestamp_deltas (variable, delta-of-delta encoded)
//   alp_encoded_values (variable, ALP compressed)

#include <erl_nif.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <cmath>
#include <algorithm>
#include <climits>
#include <cfloat>

// ALP headers
#include "alp/config.hpp"
#include "alp/constants.hpp"
#include "alp/encoder.hpp"
#include "alp/decoder.hpp"
#include "alp/sampler.hpp"
#include "alp/storer.hpp"
#include "alp/common.hpp"

// --- Timestamp encoding (delta-of-delta, same as gorilla) ---

static void encode_timestamp_deltas(const std::vector<int64_t>& timestamps,
                                     std::vector<uint8_t>& out) {
    size_t n = timestamps.size();
    if (n == 0) return;

    // First timestamp stored in header
    if (n == 1) return;

    // Delta encode
    std::vector<int64_t> deltas(n - 1);
    for (size_t i = 1; i < n; i++) {
        deltas[i - 1] = timestamps[i] - timestamps[i - 1];
    }

    // Delta-of-delta encode
    // First delta stored as varint
    int64_t first_delta = deltas[0];

    // Encode first_delta as 8-byte signed
    for (int b = 7; b >= 0; b--) {
        out.push_back(static_cast<uint8_t>((first_delta >> (b * 8)) & 0xFF));
    }

    // Remaining as delta-of-delta varints
    int64_t prev_delta = first_delta;
    for (size_t i = 1; i < deltas.size(); i++) {
        int64_t dod = deltas[i] - prev_delta;
        prev_delta = deltas[i];

        // Zigzag encode
        uint64_t zigzag = (static_cast<uint64_t>(dod) << 1) ^ (dod >> 63);

        // Varint encode
        while (zigzag >= 0x80) {
            out.push_back(static_cast<uint8_t>(zigzag | 0x80));
            zigzag >>= 7;
        }
        out.push_back(static_cast<uint8_t>(zigzag));
    }
}

static bool decode_timestamp_deltas(const uint8_t* data, size_t len, size_t& pos,
                                     int64_t first_ts, size_t count,
                                     std::vector<int64_t>& timestamps) {
    timestamps.resize(count);
    timestamps[0] = first_ts;
    if (count == 1) return true;

    // Read first delta (8-byte signed big-endian)
    if (pos + 8 > len) return false;
    int64_t first_delta = 0;
    for (int b = 7; b >= 0; b--) {
        first_delta |= static_cast<int64_t>(data[pos++]) << (b * 8);
    }

    timestamps[1] = first_ts + first_delta;
    int64_t prev_delta = first_delta;

    for (size_t i = 2; i < count; i++) {
        // Varint decode
        uint64_t zigzag = 0;
        int shift = 0;
        while (pos < len && shift < 64) {
            uint8_t byte = data[pos++];
            zigzag |= static_cast<uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) break;
            shift += 7;
        }

        // Zigzag decode
        int64_t dod = static_cast<int64_t>((zigzag >> 1) ^ -(zigzag & 1));
        prev_delta += dod;
        timestamps[i] = timestamps[i - 1] + prev_delta;
    }

    return true;
}

// --- ALP value encoding ---

static bool alp_encode_values(const std::vector<double>& values,
                               std::vector<uint8_t>& out) {
    size_t n = values.size();
    if (n == 0) return true;

    alp::state<double> stt;
    stt.vector_size = alp::config::VECTOR_SIZE;

    // Sample to find best exponent/factor — use up to 8K values spread across input
    size_t max_sample = std::min(n, static_cast<size_t>(8192));
    size_t stride = std::max(n / max_sample, static_cast<size_t>(1));
    std::vector<double> sample;
    sample.reserve(max_sample);
    for (size_t i = 0; i < n && sample.size() < max_sample; i += stride) {
        sample.push_back(values[i]);
    }
    stt.sampled_values_n = sample.size();
    alp::encoder<double>::find_top_k_combinations(sample.data(), stt);

    if (stt.best_k_combinations.empty()) {
        // Fallback: store raw
        out.push_back(0xFF); // marker: raw mode
        for (double v : values) {
            uint64_t bits;
            memcpy(&bits, &v, 8);
            for (int b = 7; b >= 0; b--) {
                out.push_back(static_cast<uint8_t>((bits >> (b * 8)) & 0xFF));
            }
        }
        return true;
    }

    stt.exp = stt.best_k_combinations[0].first;
    stt.fac = stt.best_k_combinations[0].second;

    // Encode values
    std::vector<int64_t> encoded(n);
    std::vector<uint32_t> exception_positions;
    std::vector<double> exceptions;

    for (size_t i = 0; i < n; i++) {
        // Guard against double-to-int64 overflow (UB on ARM)
        double v = values[i];
        if (std::isnan(v) || std::isinf(v) ||
            v > 9.2e18 || v < -9.2e18) {
            // Cannot ALP-encode — store as exception
            exception_positions.push_back(static_cast<uint32_t>(i));
            exceptions.push_back(v);
            encoded[i] = 0;
            continue;
        }

        int64_t enc = alp::encoder<double>::encode_value(v, stt.fac, stt.exp);
        volatile double decoded = alp::decoder<double>::decode_value(enc, stt.fac, stt.exp);
        volatile double original = v;

        if (decoded != original) {
            exception_positions.push_back(static_cast<uint32_t>(i));
            exceptions.push_back(v);
            encoded[i] = enc;
        } else {
            encoded[i] = enc;
        }
    }

    // Find min/max for frame-of-reference
    int64_t min_val = *std::min_element(encoded.begin(), encoded.end());
    int64_t max_val = *std::max_element(encoded.begin(), encoded.end());
    uint64_t range = static_cast<uint64_t>(max_val - min_val);
    uint8_t bit_width = (range == 0) ? 0 : (64 - __builtin_clzll(range));

    // Write header
    out.push_back(0x01); // marker: ALP mode
    out.push_back(stt.exp);
    out.push_back(stt.fac);
    out.push_back(bit_width);

    // Write base (min) as 8 bytes big-endian
    for (int b = 7; b >= 0; b--) {
        out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(min_val) >> (b * 8)) & 0xFF));
    }

    // Bit-pack encoded values (frame-of-reference)
    if (bit_width > 0 && bit_width < 64) {
        // Pack values into bytes
        uint64_t buffer = 0;
        int bits_in_buffer = 0;
        uint64_t mask = (1ULL << bit_width) - 1;

        for (size_t i = 0; i < n; i++) {
            uint64_t delta = static_cast<uint64_t>(encoded[i] - min_val);
            buffer = (buffer << bit_width) | (delta & mask);
            bits_in_buffer += bit_width;

            while (bits_in_buffer >= 8) {
                bits_in_buffer -= 8;
                out.push_back(static_cast<uint8_t>((buffer >> bits_in_buffer) & 0xFF));
            }
        }

        // Flush remaining bits
        if (bits_in_buffer > 0) {
            out.push_back(static_cast<uint8_t>((buffer << (8 - bits_in_buffer)) & 0xFF));
        }
    } else if (bit_width >= 64) {
        // Full 8 bytes per value — no bit-packing possible
        for (size_t i = 0; i < n; i++) {
            uint64_t delta = static_cast<uint64_t>(encoded[i] - min_val);
            for (int b = 7; b >= 0; b--) {
                out.push_back(static_cast<uint8_t>((delta >> (b * 8)) & 0xFF));
            }
        }
    }

    // Write exceptions (4-byte count, 4-byte positions)
    uint32_t exc_count = static_cast<uint32_t>(exceptions.size());
    out.push_back(static_cast<uint8_t>((exc_count >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((exc_count >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((exc_count >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(exc_count & 0xFF));

    for (size_t i = 0; i < exc_count; i++) {
        // Position (4 bytes)
        uint32_t p = exception_positions[i];
        out.push_back(static_cast<uint8_t>((p >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((p >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((p >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(p & 0xFF));

        // Exact double value
        uint64_t bits;
        memcpy(&bits, &exceptions[i], 8);
        for (int b = 7; b >= 0; b--) {
            out.push_back(static_cast<uint8_t>((bits >> (b * 8)) & 0xFF));
        }
    }

    return true;
}

static bool alp_decode_values(const uint8_t* data, size_t len, size_t& pos,
                               size_t count, std::vector<double>& values) {
    values.resize(count);
    if (count == 0) return true;
    if (pos >= len) return false;

    uint8_t mode = data[pos++];

    if (mode == 0xFF) {
        // Raw mode
        for (size_t i = 0; i < count; i++) {
            if (pos + 8 > len) return false;
            uint64_t bits = 0;
            for (int b = 7; b >= 0; b--) {
                bits |= static_cast<uint64_t>(data[pos++]) << (b * 8);
            }
            memcpy(&values[i], &bits, 8);
        }
        return true;
    }

    // ALP mode
    if (pos + 3 > len) return false;
    uint8_t exp = data[pos++];
    uint8_t fac = data[pos++];
    uint8_t bit_width = data[pos++];

    // Read base
    if (pos + 8 > len) return false;
    int64_t min_val = 0;
    for (int b = 7; b >= 0; b--) {
        min_val |= static_cast<int64_t>(data[pos++]) << (b * 8);
    }

    // Bit-unpack
    std::vector<int64_t> encoded(count);
    if (bit_width > 0 && bit_width < 64) {
        uint64_t buffer = 0;
        int bits_in_buffer = 0;
        uint64_t mask = (1ULL << bit_width) - 1;

        for (size_t i = 0; i < count; i++) {
            while (bits_in_buffer < bit_width && pos < len) {
                buffer = (buffer << 8) | data[pos++];
                bits_in_buffer += 8;
            }
            bits_in_buffer -= bit_width;
            uint64_t delta = (buffer >> bits_in_buffer) & mask;
            encoded[i] = min_val + static_cast<int64_t>(delta);
        }
    } else if (bit_width >= 64) {
        for (size_t i = 0; i < count; i++) {
            if (pos + 8 > len) return false;
            uint64_t delta = 0;
            for (int b = 7; b >= 0; b--) {
                delta |= static_cast<uint64_t>(data[pos++]) << (b * 8);
            }
            encoded[i] = min_val + static_cast<int64_t>(delta);
        }
    } else {
        for (size_t i = 0; i < count; i++) {
            encoded[i] = min_val;
        }
    }

    // Decode values
    for (size_t i = 0; i < count; i++) {
        values[i] = alp::decoder<double>::decode_value(encoded[i], fac, exp);
    }

    // Read and apply exceptions (4-byte count, 4-byte positions)
    if (pos + 4 > len) return false;
    uint32_t exc_count = (static_cast<uint32_t>(data[pos]) << 24) |
                         (static_cast<uint32_t>(data[pos+1]) << 16) |
                         (static_cast<uint32_t>(data[pos+2]) << 8) |
                         static_cast<uint32_t>(data[pos+3]);
    pos += 4;

    for (size_t i = 0; i < exc_count; i++) {
        if (pos + 12 > len) return false;
        uint32_t exc_pos = (static_cast<uint32_t>(data[pos]) << 24) |
                           (static_cast<uint32_t>(data[pos+1]) << 16) |
                           (static_cast<uint32_t>(data[pos+2]) << 8) |
                           static_cast<uint32_t>(data[pos+3]);
        pos += 4;

        uint64_t bits = 0;
        for (int b = 7; b >= 0; b--) {
            bits |= static_cast<uint64_t>(data[pos++]) << (b * 8);
        }
        double exact;
        memcpy(&exact, &bits, 8);

        if (exc_pos < count) {
            values[exc_pos] = exact;
        }
    }

    return true;
}

// --- NIF functions ---

static ERL_NIF_TERM
nif_alp_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
  try {
    if (argc != 1) return enif_make_badarg(env);

    // Parse list of {timestamp, value} tuples
    unsigned list_len;
    if (!enif_get_list_length(env, argv[0], &list_len)) return enif_make_badarg(env);

    // Empty list — return minimal header
    if (list_len == 0) {
        uint8_t empty[] = {'A', 'L', 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        ERL_NIF_TERM bin;
        uint8_t* buf = enif_make_new_binary(env, sizeof(empty), &bin);
        memcpy(buf, empty, sizeof(empty));
        return enif_make_tuple2(env, enif_make_atom(env, "ok"), bin);
    }

    std::vector<int64_t> timestamps(list_len);
    std::vector<double> values(list_len);

    ERL_NIF_TERM head, tail = argv[0];
    for (unsigned i = 0; i < list_len; i++) {
        if (!enif_get_list_cell(env, tail, &head, &tail)) return enif_make_badarg(env);

        const ERL_NIF_TERM* tuple;
        int arity;
        if (!enif_get_tuple(env, head, &arity, &tuple) || arity != 2) return enif_make_badarg(env);

        ErlNifSInt64 ts;
        double val;
        if (!enif_get_int64(env, tuple[0], &ts)) return enif_make_badarg(env);
        if (!enif_get_double(env, tuple[1], &val)) return enif_make_badarg(env);

        timestamps[i] = ts;
        values[i] = val;
    }

    // Build output
    std::vector<uint8_t> out;
    out.reserve(list_len * 4); // estimate

    // Magic + version + count
    out.push_back('A');
    out.push_back('L');
    out.push_back(1); // version

    // Point count (4 bytes big-endian)
    uint32_t count = static_cast<uint32_t>(list_len);
    out.push_back(static_cast<uint8_t>((count >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((count >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((count >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(count & 0xFF));

    // First timestamp (8 bytes big-endian)
    int64_t first_ts = timestamps.empty() ? 0 : timestamps[0];
    for (int b = 7; b >= 0; b--) {
        out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(first_ts) >> (b * 8)) & 0xFF));
    }

    // Timestamp deltas
    encode_timestamp_deltas(timestamps, out);

    // ALP encoded values
    if (!alp_encode_values(values, out)) {
        return enif_make_tuple2(env,
            enif_make_atom(env, "error"),
            enif_make_string(env, "ALP encoding failed", ERL_NIF_LATIN1));
    }

    // Return binary
    ERL_NIF_TERM bin;
    uint8_t* buf = enif_make_new_binary(env, out.size(), &bin);
    memcpy(buf, out.data(), out.size());

    return enif_make_tuple2(env, enif_make_atom(env, "ok"), bin);
  } catch (const std::exception& e) {
    return enif_make_tuple2(env,
        enif_make_atom(env, "error"),
        enif_make_string(env, e.what(), ERL_NIF_LATIN1));
  } catch (...) {
    return enif_make_tuple2(env,
        enif_make_atom(env, "error"),
        enif_make_string(env, "unknown C++ exception", ERL_NIF_LATIN1));
  }
}

static ERL_NIF_TERM
nif_alp_decode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
  try {
    if (argc != 1) return enif_make_badarg(env);

    ErlNifBinary bin;
    if (!enif_inspect_binary(env, argv[0], &bin)) return enif_make_badarg(env);

    const uint8_t* data = bin.data;
    size_t len = bin.size;
    size_t pos = 0;

    // Check magic + version
    if (len < 15) return enif_make_tuple2(env, enif_make_atom(env, "error"),
        enif_make_string(env, "binary too short", ERL_NIF_LATIN1));

    if (data[0] != 'A' || data[1] != 'L' || data[2] != 1) {
        return enif_make_tuple2(env, enif_make_atom(env, "error"),
            enif_make_string(env, "invalid magic or version", ERL_NIF_LATIN1));
    }
    pos = 3;

    // Point count — sanity check to prevent huge allocations
    uint32_t count = (static_cast<uint32_t>(data[pos]) << 24) |
                     (static_cast<uint32_t>(data[pos+1]) << 16) |
                     (static_cast<uint32_t>(data[pos+2]) << 8) |
                     static_cast<uint32_t>(data[pos+3]);
    pos += 4;

    if (count > 100000000) {  // 100M points max per decode call
        return enif_make_tuple2(env, enif_make_atom(env, "error"),
            enif_make_string(env, "count exceeds maximum", ERL_NIF_LATIN1));
    }

    // First timestamp
    int64_t first_ts = 0;
    for (int b = 7; b >= 0; b--) {
        first_ts |= static_cast<int64_t>(data[pos++]) << (b * 8);
    }

    // Decode timestamps
    std::vector<int64_t> timestamps;
    if (!decode_timestamp_deltas(data, len, pos, first_ts, count, timestamps)) {
        return enif_make_tuple2(env, enif_make_atom(env, "error"),
            enif_make_string(env, "timestamp decode failed", ERL_NIF_LATIN1));
    }

    // Decode values
    std::vector<double> values;
    if (!alp_decode_values(data, len, pos, count, values)) {
        return enif_make_tuple2(env, enif_make_atom(env, "error"),
            enif_make_string(env, "value decode failed", ERL_NIF_LATIN1));
    }

    // Build list of tuples
    ERL_NIF_TERM list = enif_make_list(env, 0);
    for (int i = static_cast<int>(count) - 1; i >= 0; i--) {
        ERL_NIF_TERM tuple = enif_make_tuple2(env,
            enif_make_int64(env, timestamps[i]),
            enif_make_double(env, values[i]));
        list = enif_make_list_cell(env, tuple, list);
    }

    return enif_make_tuple2(env, enif_make_atom(env, "ok"), list);
  } catch (const std::exception& e) {
    return enif_make_tuple2(env,
        enif_make_atom(env, "error"),
        enif_make_string(env, e.what(), ERL_NIF_LATIN1));
  } catch (...) {
    return enif_make_tuple2(env,
        enif_make_atom(env, "error"),
        enif_make_string(env, "unknown C++ exception", ERL_NIF_LATIN1));
  }
}

// --- NIF registration ---

static ErlNifFunc nif_funcs[] = {
    {"nif_encode", 1, nif_alp_encode, ERL_NIF_DIRTY_JOB_CPU_BOUND},
    {"nif_decode", 1, nif_alp_decode, ERL_NIF_DIRTY_JOB_CPU_BOUND}
};

static int load(ErlNifEnv*, void**, ERL_NIF_TERM) { return 0; }

ERL_NIF_INIT(Elixir.ExAlp.Native, nif_funcs, load, NULL, NULL, NULL)
