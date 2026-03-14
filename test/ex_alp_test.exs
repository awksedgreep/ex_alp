defmodule ExAlpTest do
  use ExUnit.Case
  doctest ExAlp

  test "greets the world" do
    assert ExAlp.hello() == :world
  end
end
