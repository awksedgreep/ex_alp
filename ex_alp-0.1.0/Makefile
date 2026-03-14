PRIV_DIR = priv
NIF_SO = $(PRIV_DIR)/alp_nif.so

ERTS_INCLUDE_DIR ?= $(shell erl -noshell -eval 'io:format("~s/erts-~s/include", [code:root_dir(), erlang:system_info(version)])' -s init stop)

CXXFLAGS = -O3 -std=c++17 -fPIC -shared -I$(ERTS_INCLUDE_DIR) -Ic_src
LDFLAGS = -shared

ifeq ($(shell uname -s),Darwin)
	LDFLAGS += -undefined dynamic_lookup -flat_namespace
endif

all: $(NIF_SO)

$(NIF_SO): c_src/alp_nif.cpp
	@mkdir -p $(PRIV_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -rf $(PRIV_DIR)/alp_nif.so
