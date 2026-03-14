defmodule ExAlp.Native do
  @moduledoc false
  @on_load :load_nif

  def load_nif do
    path = :filename.join(:code.priv_dir(:ex_alp), ~c"alp_nif")

    case :erlang.load_nif(path, 0) do
      :ok -> :ok
      {:error, {:reload, _}} -> :ok
      {:error, reason} -> {:error, reason}
    end
  end

  @doc "Encode a list of {timestamp, value} tuples into an ALP-compressed binary."
  def nif_encode(_points), do: :erlang.nif_error(:nif_not_loaded)

  @doc "Decode an ALP-compressed binary back into a list of {timestamp, value} tuples."
  def nif_decode(_binary), do: :erlang.nif_error(:nif_not_loaded)

  def encode(points), do: nif_encode(points)
  def decode(binary), do: nif_decode(binary)
end
