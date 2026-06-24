TEAMTALK_ROOT ?= $(PWD)/..
CONFIGTYPE ?= release
SHELL := /bin/bash


OMVLL_LIB ?= $(PWD)/../omvll-ndk.so
OMVLL_CONFIG ?= $(PWD)/../omvll_config.py

teamtalk-env:
	@if [ -d "$(TEAMTALK_ROOT)" ]; then \
		echo "Using TEAMTALK_ROOT: $(TEAMTALK_ROOT)"; \
	else \
		echo "Error: TEAMTALK_ROOT not found"; exit 1; \
	fi

genlib:
	mkdir -p $(BUILDDIR)
	cd $(BUILDDIR) && cmake $(CMAKE_EXTRA) \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		-DCMAKE_BUILD_TYPE=$(CONFIGTYPE) \
		../../

buildlib: genlib
	cmake --build $(BUILDDIR) $(CMAKE_BUILD_EXTRA)

android-build:
	@if [ -z "$(ANDROID_NDK_HOME)" ]; then echo "Error: ANDROID_NDK_HOME is not set"; exit 1; fi; \
	$(MAKE) CONFIGTYPE=$(CONFIGTYPE) \
	CMAKE_EXTRA="$(CMAKE_EXTRA) -DCMAKE_TOOLCHAIN_FILE=$(ANDROID_NDK_HOME)/build/cmake/android.toolchain.cmake \
	-DCMAKE_CXX_FLAGS='-fpass-plugin=$(OMVLL_LIB) -DOMVLL_CONFIG=$(OMVLL_CONFIG) -fvisibility=hidden -O3 -flto' \
	-DANDROID_ABI=$(ANDROID_ABI) \
	-DANDROID_PLATFORM=android-21 \
	-DBUILD_TEAMTALK_LIBRARY_LIB=ON \
	-DBUILD_TEAMTALK_CLIENTS=OFF \
	-DBUILD_TEAMTALK_SERVERS=OFF \
	-DTOOLCHAIN_ZLIB=OFF" \
	buildlib

android-arm64-v8a: teamtalk-env
	$(MAKE) ANDROID_ABI=arm64-v8a BUILDDIR=build-$(CONFIGTYPE)-$@ android-build

android-armeabi-v7a: teamtalk-env
	$(MAKE) ANDROID_ABI=armeabi-v7a BUILDDIR=build-$(CONFIGTYPE)-$@ android-build