SHELL := /bin/bash

.PHONY: help all build lint stage clean toolchain toolchain-maruko remote-test verify pre-pr

SOC_BUILD ?= star6e
TOOLCHAIN_URL := https://github.com/openipc/firmware/releases/download/toolchain
TOOLCHAIN_TGZ := toolchain.sigmastar-infinity6e.tgz
TOOLCHAIN_DIR := toolchain/toolchain.sigmastar-infinity6e
CC_BIN := $(TOOLCHAIN_DIR)/bin/arm-openipc-linux-gnueabihf-gcc
TOOLCHAIN_MARUKO_TGZ := toolchain.sigmastar-infinity6c.tgz
TOOLCHAIN_MARUKO_DIR := toolchain/toolchain.sigmastar-infinity6c
CC_MARUKO_BIN := $(TOOLCHAIN_MARUKO_DIR)/bin/arm-openipc-linux-musleabihf-gcc

ifeq ($(SOC_BUILD),maruko)
TOOLCHAIN_TARGET := toolchain-maruko
else ifeq ($(SOC_BUILD),star6e)
TOOLCHAIN_TARGET := toolchain
else
$(error Unsupported SOC_BUILD '$(SOC_BUILD)'; expected 'star6e' or 'maruko')
endif

help:
	@echo "Targets:"
	@echo "  make build       Build standalone binaries (default, SOC_BUILD=star6e)"
	@echo "  make build SOC_BUILD=maruko"
	@echo "  make lint        Fast warning check (-Wall -Werror, compile only)"
	@echo "  make lint SOC_BUILD=maruko"
	@echo "  make stage       Build and stage runtime bundle in star6e-standalone/out"
	@echo "  make clean       Clean standalone build outputs"
	@echo "  make toolchain   Ensure Star6E cross-toolchain is present"
	@echo "  make toolchain-maruko Ensure Maruko cross-toolchain is present"
	@echo "  make remote-test Run remote tester (pass ARGS='...')"
	@echo "  make verify      Build both backends and verify binaries exist"
	@echo "  make pre-pr      Full pre-PR checklist (version, changelog, build)"

all: build

build: $(TOOLCHAIN_TARGET)
	$(MAKE) -C star6e-standalone SOC_BUILD=$(SOC_BUILD) all

lint: $(TOOLCHAIN_TARGET)
	$(MAKE) -C star6e-standalone SOC_BUILD=$(SOC_BUILD) lint

stage: $(TOOLCHAIN_TARGET)
	$(MAKE) -C star6e-standalone SOC_BUILD=$(SOC_BUILD) stage

clean:
	$(MAKE) -C star6e-standalone clean

toolchain:
	@if [ ! -x "$(CC_BIN)" ]; then \
		echo "Fetching $(TOOLCHAIN_TGZ)..."; \
		wget -c -q --show-progress "$(TOOLCHAIN_URL)/$(TOOLCHAIN_TGZ)" -P "$$(pwd)"; \
		mkdir -p "$(TOOLCHAIN_DIR)"; \
		tar -xf "$(TOOLCHAIN_TGZ)" -C "$(TOOLCHAIN_DIR)" --strip-components=1; \
		rm -f "$(TOOLCHAIN_TGZ)"; \
	fi

toolchain-maruko:
	@if [ ! -x "$(CC_MARUKO_BIN)" ]; then \
		echo "Fetching $(TOOLCHAIN_MARUKO_TGZ)..."; \
		wget -c -q --show-progress "$(TOOLCHAIN_URL)/$(TOOLCHAIN_MARUKO_TGZ)" -P "$$(pwd)"; \
		mkdir -p "$(TOOLCHAIN_MARUKO_DIR)"; \
		tar -xf "$(TOOLCHAIN_MARUKO_TGZ)" -C "$(TOOLCHAIN_MARUKO_DIR)" --strip-components=1; \
		rm -f "$(TOOLCHAIN_MARUKO_TGZ)"; \
	fi

remote-test:
	SOC_BUILD=$(SOC_BUILD) ./star6e-standalone/scripts/remote_test.sh $(ARGS)

# ── Verification targets ──────────────────────────────────────────────

STAR6E_BINS := star6e-standalone/out/star6e/venc star6e-standalone/out/star6e/snr_toggle_test star6e-standalone/out/star6e/snr_sequence_probe
MARUKO_BINS := star6e-standalone/out/maruko/venc

verify:
	@echo "=== Building Maruko backend ==="
	$(MAKE) build SOC_BUILD=maruko
	@echo ""
	@echo "=== Verifying Maruko binaries ==="
	@for f in $(MARUKO_BINS); do \
		if [ -x "$$f" ]; then echo "  OK  $$f"; \
		else echo "  FAIL  $$f not found or not executable"; exit 1; fi; \
	done
	@echo ""
	@echo "=== Building Star6E backend ==="
	$(MAKE) build SOC_BUILD=star6e
	@echo ""
	@echo "=== Verifying Star6E binaries ==="
	@for f in $(STAR6E_BINS); do \
		if [ -x "$$f" ]; then echo "  OK  $$f"; \
		else echo "  FAIL  $$f not found or not executable"; exit 1; fi; \
	done
	@echo ""
	@echo "=== Verify passed ==="

pre-pr: verify
	@echo ""
	@echo "=== Pre-PR checks ==="
	@if [ ! -f VERSION ]; then echo "  FAIL  VERSION file missing"; exit 1; fi
	@echo "  VERSION: $$(cat VERSION)"
	@if ! grep -q "$$(cat VERSION)" HISTORY.md; then \
		echo "  WARN   VERSION $$(cat VERSION) not found in HISTORY.md"; \
		echo "         Add a changelog entry before opening a PR."; \
	else \
		echo "  OK  HISTORY.md has entry for $$(cat VERSION)"; \
	fi
	@echo ""
	@echo "=== Pre-PR complete ==="
