CMAKE ?= cmake
BUILD_DIR ?= build
BUILD_TYPE ?= Release
QT_PREFIX ?=
PVT_BUILD_QT_GUI ?= OFF
INSTALL_PREFIX ?=
CMAKE_CONFIGURE_ARGS ?=

QT_PREFIX_ARG = $(if $(strip $(QT_PREFIX)),-DCMAKE_PREFIX_PATH="$(QT_PREFIX)",)
INSTALL_PREFIX_ARG = $(if $(strip $(INSTALL_PREFIX)),--prefix "$(INSTALL_PREFIX)",)

.PHONY: all configure run render gui check test debug install clean

all: configure
	$(CMAKE) --build "$(BUILD_DIR)" --config "$(BUILD_TYPE)" --parallel

configure:
	$(CMAKE) -S . -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
		-DPVT_BUILD_CLI=ON \
		-DPVT_BUILD_QT_GUI=$(PVT_BUILD_QT_GUI) \
		$(QT_PREFIX_ARG) $(CMAKE_CONFIGURE_ARGS)

run: all
	@if [ -x "$(BUILD_DIR)/render9" ]; then \
		"$(BUILD_DIR)/render9"; \
	elif [ -x "$(BUILD_DIR)/render9.exe" ]; then \
		"$(BUILD_DIR)/render9.exe"; \
	elif [ -x "$(BUILD_DIR)/$(BUILD_TYPE)/render9" ]; then \
		"$(BUILD_DIR)/$(BUILD_TYPE)/render9"; \
	elif [ -x "$(BUILD_DIR)/$(BUILD_TYPE)/render9.exe" ]; then \
		"$(BUILD_DIR)/$(BUILD_TYPE)/render9.exe"; \
	else \
		echo "Could not locate render9 in $(BUILD_DIR)." >&2; exit 1; \
	fi

render: all
	@if [ -x "$(BUILD_DIR)/render9" ]; then \
		"$(BUILD_DIR)/render9" --render $(ARGS); \
	elif [ -x "$(BUILD_DIR)/render9.exe" ]; then \
		"$(BUILD_DIR)/render9.exe" --render $(ARGS); \
	elif [ -x "$(BUILD_DIR)/$(BUILD_TYPE)/render9" ]; then \
		"$(BUILD_DIR)/$(BUILD_TYPE)/render9" --render $(ARGS); \
	elif [ -x "$(BUILD_DIR)/$(BUILD_TYPE)/render9.exe" ]; then \
		"$(BUILD_DIR)/$(BUILD_TYPE)/render9.exe" --render $(ARGS); \
	else \
		echo "Could not locate render9 in $(BUILD_DIR)." >&2; exit 1; \
	fi

gui:
	$(MAKE) PVT_BUILD_QT_GUI=ON all
	@if [ -d "$(BUILD_DIR)/Procedural Visualizer Tool.app" ]; then \
		open "$(BUILD_DIR)/Procedural Visualizer Tool.app"; \
	elif [ -d "$(BUILD_DIR)/$(BUILD_TYPE)/Procedural Visualizer Tool.app" ]; then \
		open "$(BUILD_DIR)/$(BUILD_TYPE)/Procedural Visualizer Tool.app"; \
	elif [ -x "$(BUILD_DIR)/procedural-visualizer-tool" ]; then \
		"$(BUILD_DIR)/procedural-visualizer-tool"; \
	elif [ -x "$(BUILD_DIR)/$(BUILD_TYPE)/procedural-visualizer-tool" ]; then \
		"$(BUILD_DIR)/$(BUILD_TYPE)/procedural-visualizer-tool"; \
	elif [ -x "$(BUILD_DIR)/procedural-visualizer-tool.exe" ]; then \
		"$(BUILD_DIR)/procedural-visualizer-tool.exe"; \
	elif [ -x "$(BUILD_DIR)/$(BUILD_TYPE)/procedural-visualizer-tool.exe" ]; then \
		"$(BUILD_DIR)/$(BUILD_TYPE)/procedural-visualizer-tool.exe"; \
	elif [ -x "$(BUILD_DIR)/Procedural Visualizer Tool" ]; then \
		"$(BUILD_DIR)/Procedural Visualizer Tool"; \
	else \
		echo "Could not locate the built GUI executable in $(BUILD_DIR)." >&2; exit 1; \
	fi

check test: all
	ctest --test-dir "$(BUILD_DIR)" -C "$(BUILD_TYPE)" --output-on-failure

install: all
	$(CMAKE) --install "$(BUILD_DIR)" --config "$(BUILD_TYPE)" $(INSTALL_PREFIX_ARG)

debug:
	$(MAKE) BUILD_DIR=build-debug BUILD_TYPE=Debug all

clean:
	@case "$(BUILD_DIR)" in \
		""|.|..|/*|*/*) echo "Refusing unsafe BUILD_DIR: $(BUILD_DIR)" >&2; exit 2 ;; \
		build|build-*) ;; \
		*) echo "BUILD_DIR must be 'build' or start with 'build-'." >&2; exit 2 ;; \
	esac
	$(CMAKE) -E remove_directory "$(BUILD_DIR)"
	@if [ "$(BUILD_DIR)" != "build-debug" ]; then \
		$(CMAKE) -E remove_directory build-debug; \
	fi
