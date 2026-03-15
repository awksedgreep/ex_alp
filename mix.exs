defmodule ExAlp.MixProject do
  use Mix.Project

  @version "0.1.3"
  @source_url "https://github.com/awksedgreep/ex_alp"

  def project do
    [
      app: :ex_alp,
      version: @version,
      elixir: "~> 1.18",
      start_permanent: Mix.env() == :prod,
      compilers: [:elixir_make] ++ Mix.compilers(),
      make_env: fn ->
        erts_include_dir =
          Path.join([
            to_string(:code.root_dir()),
            "erts-#{:erlang.system_info(:version)}",
            "include"
          ])

        %{"ERTS_INCLUDE_DIR" => erts_include_dir}
      end,
      make_clean: ["clean"],
      description: description(),
      package: package(),
      docs: docs(),
      deps: deps(),
      source_url: @source_url
    ]
  end

  def application do
    [
      extra_applications: [:logger]
    ]
  end

  defp description do
    "ALP (Adaptive Lossless floating-Point) compression for Elixir. " <>
      "2.5x better compression and 2x faster than Gorilla encoding for time series data. " <>
      "NIF wrapper around the SIGMOD 2024 reference implementation."
  end

  defp package do
    [
      name: "ex_alp",
      licenses: ["MIT"],
      links: %{
        "GitHub" => @source_url
      },
      files: ~w(lib c_src Makefile mix.exs README.md LICENSE),
      maintainers: ["Mark Cotner"]
    ]
  end

  defp docs do
    [
      main: "readme",
      extras: ["README.md"],
      source_url: @source_url,
      source_ref: "v#{@version}"
    ]
  end

  defp deps do
    [
      {:elixir_make, "~> 0.9", runtime: false},
      {:ezstd, "~> 1.2", optional: true},
      {:ex_openzl, "~> 0.4", optional: true},
      {:ex_doc, "~> 0.34", only: :dev, runtime: false}
    ]
  end
end
