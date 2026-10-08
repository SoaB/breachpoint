# ==========================================================================================
#  BREACH POINT -- build system
# ------------------------------------------------------------------------------------------
#  Targets
#     make             native debug build  -> build/breachpoint (.exe)
#     make release     native optimised    -> build/breachpoint-release (.exe)
#     make web         WebAssembly build   -> build/web/index.html (+ .js/.wasm/.data)
#     make run         native + run
#     make serve       web build + local http server on :8080
#     make test        headless self-tests (systems only, no GL context)
#     make cfg         regenerate the embedded config blob from assets/cfg/*.cfg
#     make clean
#
#  Toolchain locations (override on the command line if yours differ)
#     RAYLIB_DIR   path to a raylib source checkout
#     EMSDK        path to an activated emsdk
# ==========================================================================================

GAME           := breachpoint
VERSION        := 0.9.0

EMSDK_DIR      ?= $(HOME)/tools/emsdk
EMCC           := $(EMSDK_DIR)/upstream/emscripten/emcc
EMAR           := $(EMSDK_DIR)/upstream/emscripten/emar

BUILD          := build
WEB_OUT        := $(BUILD)/web
NATIVE_OBJ     := $(BUILD)/obj-native
WEB_OBJ        := $(BUILD)/obj-web
TEST_OBJ       := $(BUILD)/obj-test

CC             := gcc
AR             := ar

# ------------------------------------------------------------------------------------------
# OS / Platform Detection & Command Definitions
# ------------------------------------------------------------------------------------------
# ------------------------------------------------------------------------------------------
# OS / Platform Detection & Command Definitions
# ------------------------------------------------------------------------------------------
ifeq ($(OS),Windows_NT)
    # 強制使用 Windows CMD，避免 Make 誤用 Git Bash 執行 cmd 專屬指令
    SHELL      := cmd.exe
    
    # Target Executable Extension
    EXT        := .exe
    
    # Raylib Path & Win32 Libraries
    RAYLIB_DIR ?= $(USERPROFILE)/MingW_Dev_Lib
    NATIVE_LIBS := -lraylib -lopengl32 -lgdi32 -lwinmm -lpthread
    
    # OS commands adaptation (注意 mkdir -p 改用 cmd 相容寫法)
    MKDIR      = if not exist "$(subst /,\,$1)" mkdir "$(subst /,\,$1)"
    RMDIR      = if exist "$(subst /,\,$1)" rmdir /s /q "$(subst /,\,$1)"
    CP         = copy /Y "$(subst /,\,$1)" "$(subst /,\,$2)"
else
    EXT        :=
    RAYLIB_DIR ?= $(HOME)/tools/raylib
    NATIVE_LIBS := -lraylib -lGL -lX11 -lpthread -ldl -lrt -lm
    
    MKDIR      = mkdir -p "$1"
    RMDIR      = rm -rf "$1"
    CP         = cp "$1" "$2"
endif

# ------------------------------------------------------------------------------------------
# Sources
# ------------------------------------------------------------------------------------------
ENGINE_SRC := \
    engine/bp_log.c \
    engine/bp_math.c \
    engine/bp_entity.c \
    engine/bp_config.c \
    engine/bp_assets.c \
    engine/bp_audio.c \
    engine/bp_gfx.c \
    engine/bp_camera.c \
    engine/bp_input.c

GAME_SRC := \
    game/bp_fx.c \
    game/bp_world.c \
    game/bp_hitdetect.c \
    game/bp_weapons.c \
    game/bp_enemies.c \
    game/bp_player.c \
    game/bp_waves.c \
    game/bp_save.c \
    game/bp_ui.c \
    game/bp_hud.c \
    game/bp_menu.c \
    game/bp_game.c

ALL_SRC   := $(ENGINE_SRC) $(GAME_SRC)
MAIN_SRC  := main.c

# ------------------------------------------------------------------------------------------
# Native flags
# ------------------------------------------------------------------------------------------
NATIVE_CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
                 -Wshadow -Wdouble-promotion -Wformat=2 -Wno-unused-function \
                 -fno-strict-aliasing -Iengine -Igame -I. -I$(RAYLIB_DIR)/include \
                 -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33 -DBP_VERSION=\"$(VERSION)\"

NATIVE_LDFLAGS := -L$(RAYLIB_DIR)/lib
RAYLIB_DEPS    := $(RAYLIB_DIR)/lib/libraylib.a

# ------------------------------------------------------------------------------------------
# Web flags
# ------------------------------------------------------------------------------------------
WEB_CFLAGS := -std=c11 -O2 -Wall -Wno-unused-parameter -Wno-missing-field-initializers \
              -Iengine -Igame -I. -I$(RAYLIB_DIR)/include \
              -DPLATFORM_WEB -DGRAPHICS_API_OPENGL_ES2 -DBP_VERSION=\"$(VERSION)\"

WEB_LDFLAGS := -s USE_GLFW=3 -s FULL_ES2=1 -s TOTAL_MEMORY=134217728 -s ALLOW_MEMORY_GROWTH=1 \
               -s INITIAL_MEMORY=64MB -s MAX_WEBGL_VERSION=2 -s MIN_WEBGL_VERSION=1 \
               -s ASYNCIFY=0 -s EXIT_RUNTIME=0 -s WARN_UNIMPLEMENTED_SYSCALLS=0 \
               --shell-file web/shell.html \
               --preload-file assets@/assets \
               -L$(BUILD) -l:libraylib_web.a -lm

# ------------------------------------------------------------------------------------------
# Default Targets
# ------------------------------------------------------------------------------------------
.PHONY: all release web run serve test cfg clean raylib raylib-web help

all: $(BUILD)/$(GAME)$(EXT)

help:
	@echo make             native debug build
	@echo make release     native optimised build
	@echo make web         WebAssembly build into $(WEB_OUT)
	@echo make run         build + run native
	@echo make serve       build web + serve on http://localhost:8080
	@echo make test        headless systems self-test
	@echo make cfg         regenerate engine/bp_config_data.h from assets/cfg
	@echo make clean       remove all build output

# ------------------------------------------------------------------------------------------
# raylib: native static lib
# ------------------------------------------------------------------------------------------
$(RAYLIB_DEPS):
	@echo >> building raylib (native)
	$(MAKE) -C $(RAYLIB_DIR)/include PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC -j4

raylib: $(RAYLIB_DEPS)

# raylib for emscripten -- built once into build/libraylib_web.a
$(BUILD)/libraylib_web.a:
	@echo >> building raylib (web/wasm)
	$(call MKDIR,$(BUILD))
	cd $(RAYLIB_DIR)/src && $(MAKE) PLATFORM=PLATFORM_WEB GRAPHICS=GRAPHICS_API_OPENGL_ES2 \
	    EMSCRIPTEN_PATH=$(EMSDK_DIR)/upstream/emscripten RAYLIB_LIBTYPE=STATIC \
	    RAYLIB_RELEASE_PATH=$(abspath $(BUILD)) -j4
	$(call CP,$(BUILD)/libraylib.a,$(BUILD)/libraylib_web.a)

raylib-web: $(BUILD)/libraylib_web.a

# ------------------------------------------------------------------------------------------
# native objects
# ------------------------------------------------------------------------------------------
$(NATIVE_OBJ)/%.o: %.c
	$(call MKDIR,$(dir $@))
	$(CC) $(NATIVE_CFLAGS) -c $< -o $@

NATIVE_OBJS := $(patsubst %.c,$(NATIVE_OBJ)/%.o,$(ALL_SRC) $(MAIN_SRC))

$(BUILD)/$(GAME)$(EXT): $(NATIVE_OBJS) $(RAYLIB_DEPS)
	$(call MKDIR,$(BUILD))
	$(CC) $(NATIVE_OBJS) -o $@ $(NATIVE_LDFLAGS) $(NATIVE_LIBS)
	@echo >> built $@

release: NATIVE_CFLAGS := $(subst -O2 -g,-O3 -DNDEBUG,$(NATIVE_CFLAGS))
release: $(BUILD)/$(GAME)-release$(EXT)

$(BUILD)/$(GAME)-release$(EXT): $(NATIVE_OBJS) $(RAYLIB_DEPS)
	$(call MKDIR,$(BUILD))
	$(CC) $(NATIVE_OBJS) -o $@ $(NATIVE_LDFLAGS) $(NATIVE_LIBS)
	@echo >> built $@

run: $(BUILD)/$(GAME)$(EXT)
	.$(BUILD)/$(GAME)$(EXT)

# ------------------------------------------------------------------------------------------
# web objects
# ------------------------------------------------------------------------------------------
$(WEB_OBJ)/%.o: %.c
	$(call MKDIR,$(dir $@))
	$(EMCC) $(WEB_CFLAGS) -c $< -o $@

WEB_OBJS := $(patsubst %.c,$(WEB_OBJ)/%.o,$(ALL_SRC) $(MAIN_SRC))

web: $(WEB_OUT)/index.html

$(WEB_OUT)/index.html: $(WEB_OBJS) $(BUILD)/libraylib_web.a
	$(call MKDIR,$(WEB_OUT))
	$(EMCC) $(WEB_OBJS) -o $(WEB_OUT)/$(GAME).html $(WEB_LDFLAGS)
	-$(call CP,web/shell.html,$(WEB_OUT)/shell.html)
	-$(call CP,$(WEB_OUT)/$(GAME).html,$(WEB_OUT)/index.html)
	-$(call CP,$(WEB_OUT)/$(GAME).js,$(WEB_OUT)/game.js)
	@echo >> web build complete

serve: web
	@echo >> serving $(WEB_OUT) on http://0.0.0.0:8080
	cd $(WEB_OUT) && python3 -m http.server 8080 --bind 0.0.0.0

# ------------------------------------------------------------------------------------------
# config blob generation
# ------------------------------------------------------------------------------------------
cfg: engine/bp_config_data.h

engine/bp_config_data.h: $(wildcard assets/cfg/*.cfg) tools/gen_cfg.py
	python3 tools/gen_cfg.py assets/cfg engine/bp_config_data.h
	@echo >> regenerated engine/bp_config_data.h

# ------------------------------------------------------------------------------------------
# headless self-test
# ------------------------------------------------------------------------------------------
TEST_SRC := tests/test_systems.c engine/bp_log.c engine/bp_math.c engine/bp_entity.c engine/bp_config.c
test:
	$(call MKDIR,$(BUILD))
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Iengine -Igame -I. \
	    -I$(RAYLIB_DIR)/src -DBP_HEADLESS_TEST -o $(BUILD)/test_systems$(EXT) $(TEST_SRC) -lm
	.$(BUILD)/test_systems$(EXT)

clean:
	$(call RMDIR,$(BUILD))
	@echo >> cleaned