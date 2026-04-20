# Build CTranslate2 as a static library for librime-predict.
#
# Static linking ensures the plugin (.dylib/.so) is self-contained:
# users don't need to install CTranslate2 separately.
#
# Usage:
#   make -f deps.mk              # build with default settings
#   make -f deps.mk clean        # remove build artifacts

project_root = $(CURDIR)
deps_dir = $(project_root)/deps

ifndef NOPARALLEL
NPROC := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
export MAKEFLAGS += -j$(NPROC)
endif

build ?= build
prefix ?= $(project_root)

OS_NAME = $(shell uname)

predict_deps = ctranslate2

.PHONY: all clean clean-dist $(predict_deps)

all: $(predict_deps)

clean:
	rm -rf $(deps_dir)/CTranslate2/$(build) || true

clean-dist:
	rm -f $(prefix)/lib/libctranslate2* || true
	rm -f $(prefix)/lib/libcpu_features* || true
	rm -rf $(prefix)/include/ctranslate2 || true

ctranslate2:
	cd $(deps_dir)/CTranslate2; \
	cmake . -B$(build) \
	-DBUILD_SHARED_LIBS:BOOL=OFF \
	-DBUILD_CLI:BOOL=OFF \
	-DBUILD_TESTS:BOOL=OFF \
	-DWITH_CUDA:BOOL=OFF \
	-DWITH_MKL:BOOL=OFF \
	-DWITH_OPENBLAS:BOOL=OFF \
	-DWITH_RUY:BOOL=OFF \
	-DWITH_ACCELERATE:BOOL=$(if $(filter Darwin,$(OS_NAME)),ON,OFF) \
	-DOPENMP_RUNTIME:STRING="NONE" \
	-DCMAKE_BUILD_TYPE:STRING="Release" \
	-DCMAKE_INSTALL_PREFIX:PATH="$(prefix)" \
	-DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON \
	&& cmake --build $(build) \
	&& cmake --build $(build) --target install
	@# cpu_features is built only on x86_64; copy it to prefix if present.
	@if [ -f "$(deps_dir)/CTranslate2/$(build)/third_party/cpu_features/libcpu_features.a" ]; then \
		cp "$(deps_dir)/CTranslate2/$(build)/third_party/cpu_features/libcpu_features.a" "$(prefix)/lib/"; \
		echo "Installed cpu_features.a to $(prefix)/lib/"; \
	fi
