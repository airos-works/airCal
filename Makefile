# airCal: native Haiku / air/OS build.
#   make              builds build-haiku/airCal
#   make package      builds artifacts/aircal-<version>-<arch>.hpkg (on Haiku)
#   make check        runs the core tests (here, on Haiku)
#   make check-host   runs the core tests on Linux (needs ICU's dev files)
# tools/build-cross.sh runs this Makefile with an air/OS cross SDK from Linux.
.DEFAULT_GOAL := all
CXX ?= g++
BUILD ?= build-haiku
# BAboutWindow lives in the private shared headers (haiku_devel ships them);
# '=' is relative to the (cross) sysroot.
PRIVATE_HEADERS ?= =/boot/system/develop/headers/private
CPPFLAGS += -Isrc -Ivendor $(APP_CPPFLAGS)
HAIKU_CPPFLAGS = -I$(PRIVATE_HEADERS)/shared -I$(PRIVATE_HEADERS)/interface
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-multichar -Wno-unused-parameter \
	-Wno-missing-field-initializers -Wno-sign-compare
RC ?= rc
XRES ?= xres
MIMESET ?= mimeset

# core: portable model, iCalendar, recurrence, parsing, storage.
# sync: the providers (Google, CalDAV, Todoist) over the Http interface.
# net: libcurl transport. ui/platform: the Be API application.
CORE_SRC = $(wildcard src/core/*.cpp) $(wildcard src/sync/*.cpp)
NET_SRC = $(wildcard src/net/*.cpp)
APP_SRC = $(wildcard src/ui/*.cpp) $(wildcard src/platform/*.cpp)
CORE_OBJ = $(CORE_SRC:%.cpp=$(BUILD)/%.o)
NET_OBJ = $(NET_SRC:%.cpp=$(BUILD)/%.o)
APP_OBJ = $(APP_SRC:%.cpp=$(BUILD)/%.o)
TEST_SRC = $(wildcard tests/*.cpp)
TEST_OBJ = $(TEST_SRC:%.cpp=$(BUILD)/%.o)

ICU_LIBS = -licui18n -licuuc
CORE_LIBS = $(ICU_LIBS) -lpthread
LIBS = -lbe -ltracker -ltranslation -llocalestub -lcurl -lnetwork $(ICU_LIBS)

.PHONY: all package check check-host integration-host icon clean

all: $(BUILD)/airCal

$(BUILD)/airCal.rsrc: resources/airCal.rdef $(wildcard resources/*.hvif)
	mkdir -p $(BUILD)
	$(RC) -I resources -o $@ resources/airCal.rdef

$(BUILD)/airCal: $(APP_OBJ) $(CORE_OBJ) $(NET_OBJ) $(BUILD)/airCal.rsrc
	$(CXX) $(APP_LDFLAGS) -o $@.new $(APP_OBJ) $(CORE_OBJ) $(NET_OBJ) $(LIBS)
	$(XRES) -o $@.new $(BUILD)/airCal.rsrc
	$(MIMESET) -f $@.new
	mv $@.new $@

$(BUILD)/src/ui/%.o: src/ui/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(HAIKU_CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/aircal_tests: $(TEST_OBJ) $(CORE_OBJ)
	$(CXX) -o $@ $(TEST_OBJ) $(CORE_OBJ) $(CORE_LIBS)

check: $(BUILD)/aircal_tests
	$(BUILD)/aircal_tests $(FILTER)

check-host:
	$(MAKE) BUILD=build-host CXX=g++ CXXFLAGS="-O1 -g -fsanitize=address,undefined" \
		CORE_LIBS="$(CORE_LIBS) -fsanitize=address,undefined" check

# End-to-end sync against tools/fake-cloud.py and Radicale (both running).
INTEGRATION_SRC = $(wildcard tests/integration/*.cpp)
$(BUILD)/aircal_integration: $(INTEGRATION_SRC:%.cpp=$(BUILD)/%.o) $(CORE_OBJ)
	$(CXX) -o $@ $^ $(CORE_LIBS)

integration-host:
	$(MAKE) BUILD=build-host CXX=g++ CXXFLAGS="-O1 -g -fsanitize=address,undefined" \
		CORE_LIBS="$(CORE_LIBS) -fsanitize=address,undefined" build-host/aircal_integration
	build-host/aircal_integration $(ONLY)

package: all
	bash tools/package-haiku.sh $(BUILD)

clean:
	rm -rf build-haiku build-host build-x86_64 build-arm64

-include $(CORE_OBJ:.o=.d) $(NET_OBJ:.o=.d) $(APP_OBJ:.o=.d) $(TEST_OBJ:.o=.d)
