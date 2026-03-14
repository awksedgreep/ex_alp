defmodule ExAlp.MixProject do
  use Mix.Project

  @version "0.1.0"

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
      description: "ALP (Adaptive Lossless floating-Point) compression for Elixir with optional zstd/openzl container compression.",
      deps: deps()
    ]
  end

  def application do
    [
      extra_applications: [:logger]
    ]
  end

  defp deps do
    [
      {:elixir_make, "~> 0.9", runtime: false},
      {:ezstd, "~> 1.2", optional: true},
      {:ex_openzl, "~> 0.4", optional: true}
    ]
  end
end
