# ExAlp

ALP (Adaptive Lossless floating-Point) compression for Elixir. Wraps the [SIGMOD 2024 reference implementation](https://github.com/cwida/ALP) as a NIF.

Compresses `{timestamp, value}` time series data using ALP encoding for values and delta-of-delta encoding for timestamps. Optional zstd container compression.

## Performance

Benchmarked against [gorilla_stream](https://hex.pm/packages/gorilla_stream) on 10K gauge data points (CPU-like oscillating values):

| Metric | ExAlp + zstd 2 | Gorilla + zstd 2 |
|--------|---------------|-----------------|
| Compression | **1.63 B/pt** | 4.08 B/pt |
| Encode speed | **28M pts/s** | 12.7M pts/s |
| Decode speed | **18.7M pts/s** | 9.2M pts/s |

2.5x better compression, 2.2x faster encode, 2x faster decode. Lossless.

## Usage

```elixir
points = [{1700000000, 45.2}, {1700000015, 45.8}, {1700000030, 46.1}]

# ALP only
{:ok, blob} = ExAlp.compress(points)
{:ok, ^points} = ExAlp.decompress(blob)

# ALP + zstd container compression
{:ok, blob} = ExAlp.compress(points, compression: :zstd)
{:ok, ^points} = ExAlp.decompress(blob, compression: :zstd)

# ALP + zstd with custom level
{:ok, blob} = ExAlp.compress(points, compression: :zstd, compression_level: 9)
```

## Installation

```elixir
{:ex_alp, "~> 0.1"}
```

Requires a C++17 compiler. The NIF compiles automatically via `elixir_make`.

Optional container compression:

```elixir
{:ezstd, "~> 1.2"}      # for compression: :zstd
{:ex_openzl, "~> 0.4"}  # for compression: :openzl
```

## How ALP Works

Unlike Gorilla (XOR-based), ALP converts floating-point values to integers by multiplying by a power of 10, then bit-packs the integers using Frame-of-Reference encoding. Values that can't be encoded exactly are stored as exceptions.

For example, `45.23` × 100 = `4523` (integer). A block of similar values like `[4523, 4518, 4531, ...]` has a small range that packs into few bits per value.

Timestamps use delta-of-delta encoding with varint compression (same approach as Gorilla).

## License

MIT
