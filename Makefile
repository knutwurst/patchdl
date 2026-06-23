PS5_HOST ?= ps5
PS5_PORT ?= 9021
PATCHDL_HTTP_PORT ?= 12880
ETAHEN_SOURCE_DIR ?= ../Source Code

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

PYTHON ?= python3
BIN := patchdl-ps5.elf

SRCS := src/main.c src/patchdl_assets.c src/patchdl_websrv.c

WEB_ASSETS := web/index.html web/styles.css web/app.js
GEN_SRCS := $(patsubst web/%,gen/web/%.c,$(WEB_ASSETS))

CFLAGS := -g -O2 -Wall -Werror -Isrc -I"$(ETAHEN_SOURCE_DIR)/include" -DPATCHDL_HTTP_PORT=$(PATCHDL_HTTP_PORT)
LDADD := -L"$(ETAHEN_SOURCE_DIR)/lib" -lmicrohttpd -lkernel_sys

all: $(BIN)

gen/web:
	mkdir -p gen/web

gen/web/%.c: web/% scripts/gen_asset_module.py | gen/web
	$(PYTHON) scripts/gen_asset_module.py --path $* $< > $@

$(BIN): $(SRCS) $(GEN_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDADD)

test: $(BIN)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

clean:
	rm -rf $(BIN) gen

.PHONY: all clean test
