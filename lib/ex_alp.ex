defmodule ExAlp do
  @moduledoc """
  ALP (Adaptive Lossless floating-Point) compression for time series data.

  Compresses `[{timestamp, value}]` lists using ALP encoding for values
  and delta-of-delta encoding for timestamps, with optional zstd or openzl
  container compression.

  ## Usage

      points = [{1700000000, 45.2}, {1700000015, 45.8}, {1700000030, 46.1}]

      {:ok, blob} = ExAlp.compress(points)
      {:ok, ^points} = ExAlp.decompress(blob)

      # With zstd container compression
      {:ok, blob} = ExAlp.compress(points, compression: :zstd)
      {:ok, ^points} = ExAlp.decompress(blob, compression: :zstd)
  """

  @doc """
  Compress a list of `{timestamp, value}` tuples.

  ## Options

    * `:compression` - Container compression: `:none` (default), `:zstd`, or `:openzl`
    * `:compression_level` - Level for container compression (default: 2)
  """
  def compress(points, opts \\ []) when is_list(points) do
    compression = Keyword.get(opts, :compression, :none)
    level = Keyword.get(opts, :compression_level, 2)

    case ExAlp.Native.encode(points) do
      {:ok, blob} ->
        case compression do
          :none -> {:ok, blob}
          :zstd -> {:ok, :ezstd.compress(blob, level)}
          :openzl -> {:ok, ExOpenzl.compress(blob, level)}
        end

      error ->
        error
    end
  end

  @doc """
  Decompress a binary back into `[{timestamp, value}]` tuples.

  ## Options

    * `:compression` - Container compression used: `:none` (default), `:zstd`, or `:openzl`
  """
  def decompress(blob, opts \\ []) when is_binary(blob) do
    compression = Keyword.get(opts, :compression, :none)

    raw =
      case compression do
        :none -> blob
        :zstd -> :ezstd.decompress(blob)
        :openzl -> ExOpenzl.decompress(blob)
      end

    ExAlp.Native.decode(raw)
  end
end
