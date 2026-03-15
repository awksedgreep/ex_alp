defmodule ExAlpTest do
  use ExUnit.Case

  test "encode and decode round-trip" do
    points = for i <- 0..99 do
      {1_700_000_000 + i * 15, Float.round(45.0 + :math.sin(i / 10) * 15, 2)}
    end

    {:ok, blob} = ExAlp.compress(points)
    {:ok, decoded} = ExAlp.decompress(blob)

    assert length(decoded) == 100
    assert decoded == points
  end

  test "compression ratio on gauge data" do
    points = for i <- 0..999 do
      {1_700_000_000 + i * 15, Float.round(45.0 + :math.sin(i / 50) * 15, 2)}
    end

    {:ok, blob} = ExAlp.compress(points)
    bytes_per_point = byte_size(blob) / 1000

    assert bytes_per_point < 4.0
  end

  test "empty list" do
    {:ok, blob} = ExAlp.compress([])
    {:ok, decoded} = ExAlp.decompress(blob)
    assert decoded == []
  end

  test "single point" do
    points = [{1_700_000_000, 42.5}]
    {:ok, blob} = ExAlp.compress(points)
    {:ok, decoded} = ExAlp.decompress(blob)
    assert decoded == points
  end

  test "zstd container compression" do
    points = for i <- 0..999 do
      {1_700_000_000 + i * 15, Float.round(45.0 + :math.sin(i / 50) * 15, 2)}
    end

    {:ok, blob} = ExAlp.compress(points, compression: :zstd)
    {:ok, decoded} = ExAlp.decompress(blob, compression: :zstd)

    assert decoded == points

    {:ok, raw} = ExAlp.compress(points)
    assert byte_size(blob) < byte_size(raw)
  end
end
