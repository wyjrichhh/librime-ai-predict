# librime-ai-predict — only builds CTranslate2 (librime itself builds the plugin).
#
# Usage:
#   make deps    # build CTranslate2 static library into ./include and ./lib
#
# Then build librime from its source tree with this directory under plugins/, e.g.:
#   cp -R /path/to/librime-ai-predict /path/to/librime/plugins/
#   cd /path/to/librime && make deps && make

project_root ?= $(CURDIR)

OS_NAME = $(shell uname)

# macOS has no nproc; avoid empty -j which breaks make.
ifndef NOPARALLEL
NPROC := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
export MAKEFLAGS += -j$(NPROC)
endif

ifeq ($(OS_NAME),Darwin)
export SDKROOT ?= $(shell xcrun --sdk macosx --show-sdk-path)
export MACOSX_DEPLOYMENT_TARGET ?= 10.15
endif

.PHONY: deps deps/%

deps:
	git submodule update --init deps/CTranslate2
	cd deps/CTranslate2 && git submodule update --init third_party/spdlog third_party/cpu_features
	$(MAKE) -f deps.mk

deps/%:
	$(MAKE) -f deps.mk $(@:deps/%=%)
