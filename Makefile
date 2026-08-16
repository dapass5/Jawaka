SHELL := /bin/bash

CC ?= cc
CSTD := -std=c11
CWARN := -Wall -Wextra -Wpedantic -Wno-unused-parameter
BUILD ?= build
CFLAGS_PLATFORM ?=
LDFLAGS_PLATFORM ?=
PLATFORM ?= mac
MLP1_TOOLCHAIN_IMAGE ?= ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:local
MLP1_BUILD_PROFILE ?= release
WORKSPACE_ROOT ?= $(abspath ..)

ifeq ($(PLATFORM),mlp1)
MLP1_FLAGS_MK ?= $(firstword $(wildcard /opt/mlp1-toolchain/umrk/mlp1-build-flags.mk $(WORKSPACE_ROOT)/mlp1-toolchain/flags/mlp1-build-flags.mk ../mlp1-toolchain/flags/mlp1-build-flags.mk))
ifneq ($(MLP1_FLAGS_MK),)
include $(MLP1_FLAGS_MK)
else
UMRK_MLP1_PROFILE_CFLAGS ?= -O2 -mcpu=cortex-a55 -mtune=cortex-a55 -ffunction-sections -fdata-sections -DNDEBUG
UMRK_MLP1_PROFILE_LDFLAGS ?= -Wl,--gc-sections
endif
CDEBUG ?= $(UMRK_MLP1_PROFILE_CFLAGS)
LDFLAGS_PLATFORM += $(UMRK_MLP1_PROFILE_LDFLAGS)
else
CDEBUG ?= -g -O0
endif

ifeq ($(PLATFORM),mlp1)
CFLAGS_PLATFORM += -DPLATFORM_MLP1
endif

DEFAULT_CATASTROPHE_DIR := $(if $(wildcard ../Catastrophe/include/catastrophe.h),$(abspath ../Catastrophe),third_party/catastrophe)
CATASTROPHE_DIR ?= $(DEFAULT_CATASTROPHE_DIR)
CATASTROPHE_INCLUDE := $(CATASTROPHE_DIR)/include
CATASTROPHE_HEADER := $(CATASTROPHE_INCLUDE)/catastrophe.h
CATASTROPHE_RES := $(CATASTROPHE_DIR)/res

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf SDL2_image)
SDL_LDFLAGS := $(shell pkg-config --libs sdl2 SDL2_ttf SDL2_image)
WAYLAND_CFLAGS := $(shell pkg-config --cflags wayland-client 2>/dev/null)
WAYLAND_LDFLAGS := $(shell pkg-config --libs wayland-client 2>/dev/null)
WAYLAND_PROTOCOLS_DIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)
WAYLAND_SCANNER ?= wayland-scanner
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LDFLAGS := $(shell pkg-config --libs libcurl 2>/dev/null)
# The Mac lane links the system libcurl when pkg-config has no entry; the
# mlp1 branch below still demands a pkg-config hit from the toolchain.
ifneq ($(PLATFORM),mlp1)
ifeq ($(strip $(CURL_LDFLAGS)),)
CURL_LDFLAGS := -lcurl
endif
endif

# ScreenScraper developer credentials. Prefer the current checkout's ignored
# .env.local, then the primary checkout's copy when building from a linked Git
# worktree. Release-candidate builds use temporary worktrees, and silently
# dropping the credentials there produces a launcher whose artwork picker can
# enumerate local games but can never enqueue a scrape.
JAWAKA_GIT_COMMON_DIR := $(shell git -C "$(CURDIR)" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)
SCREENSCRAPER_ENV_FILE ?= $(firstword $(wildcard \
	$(CURDIR)/.env.local \
	$(JAWAKA_GIT_COMMON_DIR)/../.env.local))
ifneq ($(strip $(SCREENSCRAPER_ENV_FILE)),)
-include $(SCREENSCRAPER_ENV_FILE)
endif

# Export names into the MLP1 Docker build with `docker -e NAME`; values never
# appear in the echoed command line. Direct builds without credentials remain
# supported unless their caller explicitly requires the feature.
export SCREENSCRAPER_DEV_ID
export SCREENSCRAPER_DEV_PASSWORD
export SCREENSCRAPER_DEBUG_PASSWORD

SCREENSCRAPER_AVAILABLE := 0
ifneq ($(strip $(SCREENSCRAPER_DEV_ID)),)
ifneq ($(strip $(SCREENSCRAPER_DEV_PASSWORD)),)
SCREENSCRAPER_AVAILABLE := 1
endif
endif

SCREENSCRAPER_REQUIRED ?= 0
ifneq ($(filter 1 yes true,$(SCREENSCRAPER_REQUIRED)),)
ifeq ($(SCREENSCRAPER_AVAILABLE),0)
$(error ScreenScraper credentials are required; create .env.local or set SCREENSCRAPER_DEV_ID and SCREENSCRAPER_DEV_PASSWORD)
endif
endif

SCRAPE_CREDENTIALS_HEADER := $(BUILD)/generated/screenscraper_credentials.h

CFLAGS_COMMON := $(CSTD) $(CWARN) $(CDEBUG) $(CFLAGS_PLATFORM) -I. -Iinternal -Ithird_party/cjson
CFLAGS_DAEMON := $(CFLAGS_COMMON)
CFLAGS_UI := $(CFLAGS_COMMON) -I$(CATASTROPHE_INCLUDE) -Ithird_party/miniz $(SDL_CFLAGS) $(CURL_CFLAGS)
LDLIBS_COMMON := $(LDFLAGS_PLATFORM) -lsqlite3
LDLIBS_DAEMON := $(LDLIBS_COMMON)
LDLIBS_UI := $(LDLIBS_COMMON) $(SDL_LDFLAGS) $(CURL_LDFLAGS) -lm -lpthread
ifeq ($(shell uname -s),Darwin)
LDLIBS_UI += -lobjc
endif
ifeq ($(PLATFORM),mlp1)
ifeq ($(strip $(CURL_LDFLAGS)),)
$(error PLATFORM=mlp1 requires libcurl in the toolchain; rebuild mlp1-toolchain)
endif
CFLAGS_DAEMON += -DJW_UPDATE_USE_LIBCURL=1 $(CURL_CFLAGS)
LDLIBS_DAEMON += $(CURL_LDFLAGS)
# MLP1 dismisses the stock boot transition from jawakad's platform backend by
# dlopen-ing libloong_sdk.so at runtime, which needs libdl.
LDLIBS_DAEMON += -ldl
PLATFORM_BACKEND_SRC := internal/platform/device_mlp1.c
PLATFORM_ID_SRC := internal/platform/platform_id_mlp1.c
INPUT_PROXY_SRC := internal/platform/input_proxy_mlp1.c
INPUT_ROSTER_SRC := internal/platform/input_roster_mlp1.c
EXTERNAL_INPUT_SRC := internal/platform/external_input_monitor_mlp1.c
BLUETOOTH_SRC := internal/platform/bluetooth_mlp1.c
WIFI_SRC := internal/platform/wifi_mlp1.c
OSD_BACKEND_SRC := cmd/jawaka-osd/osd_wayland.c $(BUILD)/generated/xdg-shell-protocol.c
OSD_DEPS := $(BUILD)/generated/xdg-shell-client-protocol.h
OSD_CFLAGS := $(CFLAGS_COMMON) $(WAYLAND_CFLAGS) -I$(BUILD)/generated
OSD_LDLIBS := $(LDLIBS_COMMON) $(WAYLAND_LDFLAGS)
else
PLATFORM_BACKEND_SRC := internal/platform/device_mock.c
PLATFORM_ID_SRC := internal/platform/platform_id_mock.c
INPUT_PROXY_SRC := internal/platform/input_proxy_mock.c
INPUT_ROSTER_SRC := internal/platform/input_roster_mock.c
EXTERNAL_INPUT_SRC := internal/platform/external_input_monitor_mock.c
BLUETOOTH_SRC := internal/platform/bluetooth_unsupported.c
WIFI_SRC := internal/platform/wifi_unsupported.c
OSD_BACKEND_SRC := cmd/jawaka-osd/osd_sdl.c
OSD_DEPS :=
OSD_CFLAGS := $(CFLAGS_UI)
OSD_LDLIBS := $(LDLIBS_UI)
endif
PLATFORM_COMMON_SRC := internal/platform/platform_common.c
LEAF_VERSION_SRC := internal/platform/leaf_version.c
PAKRAT_CATALOG_SRC := internal/store/pakrat_catalog.c
PAKRAT_STATE_LOGIC_SRC := internal/store/pakrat_state_logic.c

# ScreenScraper scrape engine (daemon-side; curl + vendored stb/miniz/md5).
SCRAPE_SRCS := \
	internal/scrape/ss_client.c \
	internal/scrape/scrape_catalog.c \
	internal/scrape/scrape_md5.c \
	internal/scrape/scrape_systems.c \
	internal/scrape/scrape_worker.c \
	third_party/md5/md5.c \
	third_party/miniz/miniz.c \
	third_party/miniz/miniz_tdef.c \
	third_party/miniz/miniz_tinfl.c \
	third_party/miniz/miniz_zip.c
SCRAPE_CFLAGS := -include $(SCRAPE_CREDENTIALS_HEADER) $(CURL_CFLAGS) \
	-Ithird_party/stb -Ithird_party/miniz -Ithird_party/md5

CFLAGS_DAEMON += $(SCRAPE_CFLAGS)
LDLIBS_DAEMON += $(CURL_LDFLAGS) -lpthread -lm

DAEMON_SRCS := \
	cmd/jawakad/main.c \
	internal/core/log.c \
	internal/ipc/ipc.c \
	internal/ipc/ipc_stream.c \
	internal/ipc/ipc_client.c \
	internal/ipc/ctl1.c \
	internal/ipc/life1.c \
	internal/launcher/active_game.c \
	internal/launcher/standalone_policy.c \
	$(PLATFORM_COMMON_SRC) \
	internal/platform/device.c \
	$(BLUETOOTH_SRC) \
	$(WIFI_SRC) \
	$(PLATFORM_BACKEND_SRC) \
	$(PLATFORM_ID_SRC) \
	$(INPUT_PROXY_SRC) \
	$(INPUT_ROSTER_SRC) \
	$(EXTERNAL_INPUT_SRC) \
	internal/platform/calibration.c \
	$(LEAF_VERSION_SRC) \
	internal/platform/paths.c \
	internal/platform/raofflineproxy.c \
	internal/power/suspend_inhibit.c \
	internal/retroarch/catalog.c \
	internal/retroarch/command.c \
	internal/retroarch/legacy_migration.c \
	internal/retroarch/states.c \
	internal/storage/sources.c \
	internal/store/catalog_source.c \
	internal/store/managed_apps.c \
	internal/store/pakrat_recovery.c \
	internal/store/pakrat_txn.c \
	internal/update/update.c \
	internal/update/sha256.c \
	internal/db/db.c \
	internal/focus/focus.c \
	internal/db/relocation.c \
	internal/settings/appearance.c \
	internal/i18n/i18n.c \
	internal/settings/theme_resolve.c \
internal/discovery/discovery.c \
	$(SCRAPE_SRCS) \
	internal/services/manifest.c \
	internal/services/ownership.c \
	internal/services/lease.c \
	internal/services/stop.c \
	internal/services/reservation.c \
	internal/services/backoff.c \
	internal/services/dup_ids.c \
	internal/services/unverified_stop.c \
	internal/services/control_state.c \
	internal/services/legacy_ssh_migration.c \
	internal/services/log_redact.c \
	internal/services/launch.c \
	internal/services/supervisor.c \
	third_party/cjson/cJSON.c

RETROARCH_CTL_SRCS := \
	cmd/jawaka-retroarchctl/main.c \
	internal/retroarch/command.c

RETROARCH_RUNNER_SRCS := \
	cmd/jawaka-retroarch-runner/main.c \
	internal/core/log.c \
	$(PLATFORM_ID_SRC) \
	internal/platform/paths.c \
	internal/retroarch/catalog.c \
	third_party/cjson/cJSON.c

UPDATE_RUNNER_SRCS := \
	cmd/jawaka-update-runner/main.c \
	internal/update/sha256.c \
	third_party/cjson/cJSON.c

PLATFORM_CTL_SRCS := \
	cmd/jawaka-platformctl/main.c \
	internal/core/log.c \
	internal/ipc/ipc.c \
	internal/ipc/ipc_client.c \
	internal/platform/platform_common.c \
	internal/i18n/i18n.c \
	$(PLATFORM_ID_SRC) \
	internal/platform/paths.c \
	internal/retroarch/catalog.c \
	third_party/cjson/cJSON.c

OSD_SRCS := \
	cmd/jawaka-osd/main.c \
	cmd/jawaka-osd/game_launch.c \
	$(OSD_BACKEND_SRC) \
	internal/core/log.c \
	internal/ipc/ipc.c \
	$(PLATFORM_ID_SRC) \
	internal/platform/paths.c \
	internal/retroarch/catalog.c \
	third_party/cjson/cJSON.c

SCAN_SMOKE_SRCS := \
	cmd/jawaka-scan-smoke/main.c \
	$(PLATFORM_ID_SRC) \
	internal/db/db.c \
	internal/db/relocation.c \
	internal/discovery/discovery.c \
	internal/retroarch/catalog.c \
	internal/storage/sources.c \
	third_party/cjson/cJSON.c

SCRAPE_SMOKE_SRCS := \
	cmd/jawaka-scrape-smoke/main.c \
	$(SCRAPE_SRCS) \
	internal/core/log.c \
	internal/db/db.c \
	internal/db/relocation.c \
	$(PLATFORM_ID_SRC) \
	internal/retroarch/catalog.c \
	internal/storage/sources.c \
	third_party/cjson/cJSON.c

PAKRAT_SMOKE_SRCS := \
	cmd/jawaka-pakrat-smoke/main.c \
	$(LEAF_VERSION_SRC) \
	$(PAKRAT_CATALOG_SRC) \
	$(PAKRAT_STATE_LOGIC_SRC) \
	internal/store/pakrat.c \
	internal/store/pakrat_recovery.c \
	internal/store/pakrat_state.c \
	internal/store/pakrat_txn.c \
	internal/ipc/ipc.c \
	$(PLATFORM_ID_SRC) \
	internal/db/db.c \
	internal/db/relocation.c \
	internal/discovery/discovery.c \
	internal/retroarch/catalog.c \
	internal/storage/sources.c \
	internal/store/catalog_source.c \
	internal/store/managed_apps.c \
	internal/services/manifest.c \
	internal/update/sha256.c \
	third_party/cjson/cJSON.c \
	third_party/miniz/miniz.c \
	third_party/miniz/miniz_tdef.c \
	third_party/miniz/miniz_tinfl.c \
	third_party/miniz/miniz_zip.c

CATALOG_SMOKE_SRCS := \
	cmd/jawaka-catalog-smoke/main.c \
	$(PLATFORM_ID_SRC) \
	internal/retroarch/catalog.c \
	third_party/cjson/cJSON.c

I18N_TEST_SRCS := \
	cmd/jawaka-i18n-test/main.c \
	internal/i18n/i18n.c \
	internal/core/log.c

CORE_OVERRIDE_SMOKE_SRCS := \
	cmd/jawaka-core-override-smoke/main.c \
	$(PLATFORM_ID_SRC) \
	internal/db/db.c \
	internal/db/relocation.c \
	internal/retroarch/catalog.c \
	third_party/cjson/cJSON.c

UPDATE_SMOKE_SRCS := \
	cmd/jawaka-update-smoke/main.c \
	$(LEAF_VERSION_SRC) \
	internal/update/update.c \
	internal/update/sha256.c \
	third_party/cjson/cJSON.c

INHIBIT_CTL_SRCS := \
	cmd/jawaka-inhibitctl/main.c \
	internal/ipc/ipc.c \
	third_party/cjson/cJSON.c

UI_SRCS := \
	internal/core/log.c \
	internal/ipc/ipc.c \
	internal/ipc/ipc_client.c \
	$(PLATFORM_COMMON_SRC) \
	$(PLATFORM_ID_SRC) \
	internal/platform/cat_services.c \
	$(LEAF_VERSION_SRC) \
	internal/platform/paths.c \
	$(BLUETOOTH_SRC) \
	$(WIFI_SRC) \
	internal/retroarch/catalog.c \
	internal/retroarch/states.c \
	internal/storage/sources.c \
	internal/store/catalog_source.c \
	internal/store/managed_apps.c \
	$(PAKRAT_CATALOG_SRC) \
	$(PAKRAT_STATE_LOGIC_SRC) \
	internal/store/pakrat.c \
	internal/store/pakrat_recovery.c \
	internal/store/pakrat_state.c \
	internal/store/pakrat_txn.c \
	internal/update/sha256.c \
	internal/discovery/discovery.c \
	internal/db/db.c \
	internal/focus/focus.c \
	internal/db/relocation.c \
	internal/launcher/console_colors.c \
	internal/launcher/coverflow.c \
	internal/launcher/focus_screen.c \
	internal/launcher/game_switcher.c \
	internal/launcher/system_names.c \
	internal/scrape/scrape_catalog.c \
	internal/settings/appearance.c \
	internal/i18n/i18n.c \
	internal/settings/settings.c \
	internal/settings/theme_resolve.c \
	internal/services/manifest.c \
	third_party/cjson/cJSON.c \
	third_party/miniz/miniz.c \
	third_party/miniz/miniz_tdef.c \
	third_party/miniz/miniz_tinfl.c \
	third_party/miniz/miniz_zip.c

ALL_BINS := \
	$(BUILD)/bin/jawakad \
	$(BUILD)/bin/jawaka-launcher \
	$(BUILD)/bin/jawaka-menu \
	$(BUILD)/bin/jawaka-osd \
	$(BUILD)/bin/jawaka-retroarchctl \
	$(BUILD)/bin/jawaka-retroarch-runner \
	$(BUILD)/bin/jawaka-update-runner \
	$(BUILD)/bin/jawaka-platformctl \
	$(BUILD)/bin/jawaka-inhibitctl

ifeq ($(PLATFORM),mlp1)
ALL_BINS += $(BUILD)/bin/jawaka-ledd
endif
ifeq ($(PLATFORM),mlp1)
ALL_OUTPUTS := $(ALL_BINS) $(BUILD)/build-manifest.json
else
ALL_OUTPUTS := $(ALL_BINS)
endif

.PHONY: all jawakad jawaka-launcher jawaka-menu jawaka-osd jawaka-retroarchctl jawaka-retroarch-runner jawaka-update-runner jawaka-platformctl jawaka-ledd jawaka-scan-smoke jawaka-scrape-smoke jawaka-pakrat-smoke jawaka-catalog-smoke jawaka-core-override-smoke jawaka-i18n-test i18n-pot i18n-check jawaka-update-smoke jawaka-inhibitctl leaf-version-test pakrat-catalog-test pakrat-state-logic-test pakrat-txn-test storage-sources-test source-paths-v2-smoke service-manifest-test ownership-test lease-test stop-test reservation-test backoff-test dup-ids-test unverified-stop-test control-state-test legacy-ssh-migration-test log-redact-test launch-test supervisor-test service-fixtures service-fixture-test ctl1-test life1-test ipc-stream-test wire-fixture-test osd-game-launch-test life1-subscriber-ipc-smoke life1-game-ipc-smoke life1-game-wait-ipc-smoke life1-game-check-ipc-smoke life1-game-fallback-ipc-smoke life1-game-unmanaged-ipc-smoke life1-game-override-ipc-smoke life1-app-noevent-ipc-smoke active-game-recovery-ipc-smoke active-game-test writer-group-test service-client-test focus-test schema-v6-test relocation-test relocation-ipc-smoke package-quiesce-ipc-smoke power-transition-ipc-smoke imported-title-test imported-title-ipc-smoke settings-status-test states-core-test legacy-migration-test retroarch-command-test retroarch-config-test retroarch-recording-path-test catalog-folder-test standalone-policy-test suspend-inhibit-test suspend-inhibit-ipc-smoke update-local-manifest-smoke pakrat-state-smoke pakrat-history-smoke pakrat-recovery-smoke pakrat-service-mutation-smoke mockgen run-daemon run-daemon-interactive run-daemon-only run-launcher run-menu run-interactive clean help tg5040 tg5050 my355 mlp1 mlp1-pakrat-smoke mlp1-inhibit-smoke mlp1-adb-smoke mlp1-adb-service-fixture-smoke mlp1-adb-pakrat-recovery-smoke mlp1-adb-service-mutation-smoke mlp1-adb-life1-smoke mlp1-adb-input-capture mlp1-adb-ra-command-smoke phase3-fixture-scan-smoke phase3-core-choice-smoke check-catastrophe check-sdl FORCE

all: $(ALL_OUTPUTS)

jawakad: $(BUILD)/bin/jawakad
jawaka-launcher: $(BUILD)/bin/jawaka-launcher
jawaka-menu: $(BUILD)/bin/jawaka-menu
jawaka-osd: $(BUILD)/bin/jawaka-osd
jawaka-retroarchctl: $(BUILD)/bin/jawaka-retroarchctl
jawaka-retroarch-runner: $(BUILD)/bin/jawaka-retroarch-runner
jawaka-update-runner: $(BUILD)/bin/jawaka-update-runner
jawaka-platformctl: $(BUILD)/bin/jawaka-platformctl
ifeq ($(PLATFORM),mlp1)
jawaka-ledd: $(BUILD)/bin/jawaka-ledd
else
jawaka-ledd:
	@echo "jawaka-ledd is MLP1-only; build with PLATFORM=mlp1." >&2
	@exit 1
endif
jawaka-scan-smoke: $(BUILD)/bin/jawaka-scan-smoke
jawaka-scrape-smoke: $(BUILD)/bin/jawaka-scrape-smoke
jawaka-pakrat-smoke: $(BUILD)/bin/jawaka-pakrat-smoke
jawaka-catalog-smoke: $(BUILD)/bin/jawaka-catalog-smoke
jawaka-core-override-smoke: $(BUILD)/bin/jawaka-core-override-smoke
jawaka-i18n-test: $(BUILD)/bin/jawaka-i18n-test

# Regenerate the canonical key list from the sources. Commit the result --
# CI diffs it, so a UI-string change without a regenerated .pot fails there.
i18n-pot:
	python3 tools/i18n-extract.py

# What CI runs: the committed .pot must match the code, and any committed
# translation must parse, carry no orphan keys, and keep its printf
# conversions compatible (i18n-compile.py enforces that last one).
i18n-check:
	python3 tools/i18n-extract.py --check --po $(wildcard i18n/*.po)
	@for po in $(wildcard i18n/*.po); do \
		python3 tools/i18n-compile.py $$po -o /tmp/i18n-check.jwi || exit 1; \
	done
jawaka-update-smoke: $(BUILD)/bin/jawaka-update-smoke
jawaka-inhibitctl: $(BUILD)/bin/jawaka-inhibitctl

leaf-version-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/leaf-version-test \
		internal/platform/leaf_version_test.c $(LEAF_VERSION_SRC) \
		third_party/cjson/cJSON.c
	$(BUILD)/bin/leaf-version-test

pakrat-catalog-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/pakrat-catalog-test \
		internal/store/pakrat_catalog_test.c $(PAKRAT_CATALOG_SRC) \
		$(LEAF_VERSION_SRC) third_party/cjson/cJSON.c
	$(BUILD)/bin/pakrat-catalog-test

pakrat-state-logic-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/pakrat-state-logic-test \
		internal/store/pakrat_state_logic_test.c \
		$(PAKRAT_STATE_LOGIC_SRC) $(LEAF_VERSION_SRC) \
		third_party/cjson/cJSON.c
	$(BUILD)/bin/pakrat-state-logic-test

pakrat-txn-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -DJW_ENABLE_FAULT_INJECTION=1 \
		-o $(BUILD)/bin/pakrat-txn-test \
		internal/store/pakrat_txn_test.c internal/store/pakrat_txn.c \
		internal/store/pakrat_recovery.c internal/services/manifest.c \
		internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		third_party/cjson/cJSON.c $(LDLIBS_COMMON)
	$(BUILD)/bin/pakrat-txn-test

storage-sources-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/storage-sources-test \
		internal/storage/sources_test.c internal/storage/sources.c
	$(BUILD)/bin/storage-sources-test

source-paths-v2-smoke:
	BUILD="$(BUILD)" scripts/source-paths-v2-smoke.sh

schema-v6-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/schema-v6-test \
		internal/db/schema_v6_test.c internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		$(LDLIBS_COMMON)
	$(BUILD)/bin/schema-v6-test

relocation-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/relocation-test \
		internal/db/relocation_test.c internal/db/db.c internal/db/relocation.c \
		internal/storage/sources.c $(LDLIBS_COMMON)
	$(BUILD)/bin/relocation-test

relocation-ipc-smoke:
	scripts/relocation-ipc-smoke.sh

package-quiesce-ipc-smoke:
	scripts/package-quiesce-ipc-smoke.sh

power-transition-ipc-smoke:
	scripts/power-transition-ipc-smoke.sh

service-manifest-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/service-manifest-test \
		internal/services/manifest_test.c internal/services/manifest.c \
		third_party/cjson/cJSON.c
	$(BUILD)/bin/service-manifest-test

$(BUILD)/bin/ownership-test: internal/services/ownership_test.c \
		internal/services/ownership.c internal/services/ownership.h | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -DJW_SVC_OWNERSHIP_TESTING -o $(BUILD)/bin/ownership-test \
		internal/services/ownership_test.c internal/services/ownership.c -lpthread

ownership-test: $(BUILD)/bin/ownership-test
	$(BUILD)/bin/ownership-test

lease-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/lease-test \
		internal/services/lease_test.c internal/services/lease.c
	$(BUILD)/bin/lease-test

stop-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/stop-test \
		internal/services/stop_test.c internal/services/stop.c
	$(BUILD)/bin/stop-test

osd-game-launch-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/osd-game-launch-test \
		cmd/jawaka-osd/game_launch_test.c cmd/jawaka-osd/game_launch.c \
		third_party/cjson/cJSON.c
	$(BUILD)/bin/osd-game-launch-test

reservation-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/reservation-test \
		internal/services/reservation_test.c internal/services/reservation.c \
		third_party/cjson/cJSON.c
	$(BUILD)/bin/reservation-test

backoff-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/backoff-test \
		internal/services/backoff_test.c internal/services/backoff.c
	$(BUILD)/bin/backoff-test

dup-ids-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/dup-ids-test \
		internal/services/dup_ids_test.c internal/services/dup_ids.c
	$(BUILD)/bin/dup-ids-test

unverified-stop-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/unverified-stop-test \
		internal/services/unverified_stop_test.c internal/services/unverified_stop.c
	$(BUILD)/bin/unverified-stop-test

control-state-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/control-state-test \
		internal/services/control_state_test.c internal/services/control_state.c \
		-lsqlite3
	$(BUILD)/bin/control-state-test

legacy-ssh-migration-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/legacy-ssh-migration-test \
		internal/services/legacy_ssh_migration_test.c \
		internal/services/legacy_ssh_migration.c \
		internal/services/control_state.c -lsqlite3
	$(BUILD)/bin/legacy-ssh-migration-test

log-redact-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/log-redact-test \
		internal/services/log_redact_test.c internal/services/log_redact.c
	$(BUILD)/bin/log-redact-test

launch-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/launch-test \
		internal/services/launch_test.c internal/services/launch.c \
		third_party/cjson/cJSON.c -lpthread
	$(BUILD)/bin/launch-test

supervisor-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/supervisor-test \
		internal/services/supervisor_test.c internal/services/supervisor.c \
		internal/services/manifest.c internal/services/lease.c \
		internal/services/launch.c internal/services/ownership.c \
		internal/services/stop.c internal/services/reservation.c \
		internal/services/backoff.c internal/services/dup_ids.c \
		internal/services/control_state.c \
		internal/services/legacy_ssh_migration.c \
		internal/core/log.c \
		third_party/cjson/cJSON.c -lsqlite3 -lpthread
	$(BUILD)/bin/supervisor-test

SERVICE_FIXTURE_ROOT := $(BUILD)/service-fixtures
SERVICE_FIXTURE_INVALID := $(WORKSPACE_ROOT)/umrk-workspace/contracts/leaf-services/manifests/invalid
SERVICE_FIXTURE_TEST_ROOT ?= $(abspath $(SERVICE_FIXTURE_ROOT))

$(BUILD)/bin/service-fixture: internal/services/fixtures/fixture_service.c | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $<

service-fixtures: $(BUILD)/bin/service-fixture
	python3 internal/services/fixtures/materialize.py \
		--templates internal/services/fixtures/paks \
		--binary $(BUILD)/bin/service-fixture \
		--canonical-invalid $(SERVICE_FIXTURE_INVALID) \
		--output $(SERVICE_FIXTURE_ROOT)

$(BUILD)/bin/service-fixture-test: service-fixtures \
		internal/services/fixtures/fixture_paks_test.c | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) \
		-DJW_TEST_SERVICE_FIXTURES_ROOT=\"$(SERVICE_FIXTURE_TEST_ROOT)\" \
		-o $(BUILD)/bin/service-fixture-test \
		internal/services/fixtures/fixture_paks_test.c \
		internal/services/supervisor.c internal/services/manifest.c \
		internal/services/lease.c internal/services/launch.c \
		internal/services/ownership.c internal/services/stop.c \
		internal/services/reservation.c internal/services/backoff.c \
		internal/services/dup_ids.c internal/services/control_state.c \
		internal/services/legacy_ssh_migration.c \
		internal/core/log.c \
		third_party/cjson/cJSON.c -lsqlite3 -lpthread

service-fixture-test: $(BUILD)/bin/service-fixture-test
	$(BUILD)/bin/service-fixture-test

ctl1-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/ctl1-test \
		internal/ipc/ctl1_test.c internal/ipc/ctl1.c \
		third_party/cjson/cJSON.c -lm
	$(BUILD)/bin/ctl1-test

life1-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/life1-test \
		internal/ipc/life1_test.c internal/ipc/life1.c \
		third_party/cjson/cJSON.c -lm
	$(BUILD)/bin/life1-test

wire-fixture-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) \
		-DJW_TEST_WIRE_FIXTURES_ROOT=\"$(WORKSPACE_ROOT)/umrk-workspace/contracts/leaf-services/wire-fixtures\" \
		-o $(BUILD)/bin/wire-fixture-test \
		internal/ipc/wire_fixture_test.c third_party/cjson/cJSON.c -lm
	$(BUILD)/bin/wire-fixture-test

active-game-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/active-game-test \
		internal/launcher/active_game_test.c \
		internal/launcher/active_game.c third_party/cjson/cJSON.c
	$(BUILD)/bin/active-game-test

writer-group-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/writer-group-test \
		internal/launcher/writer_group_test.c internal/services/ownership.c
	$(BUILD)/bin/writer-group-test

ipc-stream-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/ipc-stream-test \
		internal/ipc/ipc_stream_test.c internal/ipc/ipc_stream.c \
		internal/ipc/ipc.c
	$(BUILD)/bin/ipc-stream-test

$(BUILD)/bin/life1-fixture-service: internal/services/fixtures/life1_fixture_service.c \
		internal/ipc/ipc.c | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $^

$(BUILD)/bin/game-writer-fixture: internal/services/fixtures/game_writer_fixture.c | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $<

life1-subscriber-ipc-smoke: $(BUILD)/bin/life1-fixture-service
	scripts/life1-subscriber-ipc-smoke.sh

life1-game-ipc-smoke: $(BUILD)/bin/life1-fixture-service $(BUILD)/bin/game-writer-fixture
	scripts/life1-game-ipc-smoke.sh

life1-game-fallback-ipc-smoke: $(BUILD)/bin/life1-fixture-service $(BUILD)/bin/game-writer-fixture
	UMRK_LIFE1_SMOKE_SCENARIO=game-malformed scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-drop scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-timeout scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-stalled scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-never-subscribe scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-late-subscribe scripts/life1-game-ipc-smoke.sh

life1-game-wait-ipc-smoke: $(BUILD)/bin/life1-fixture-service $(BUILD)/bin/game-writer-fixture
	UMRK_LIFE1_SMOKE_SCENARIO=game-waiting scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-wait-expiry scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-start-now scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-slow-ready scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-reconnect scripts/life1-game-ipc-smoke.sh

life1-game-check-ipc-smoke: $(BUILD)/bin/life1-fixture-service $(BUILD)/bin/game-writer-fixture
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-current scripts/life1-game-check-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-wait scripts/life1-game-check-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-play scripts/life1-game-check-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-cancel scripts/life1-game-check-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-expiry scripts/life1-game-check-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-timeout scripts/life1-game-check-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-malformed scripts/life1-game-check-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-unsafe-card scripts/life1-game-check-ipc-smoke.sh

life1-game-unmanaged-ipc-smoke: $(BUILD)/bin/life1-fixture-service $(BUILD)/bin/game-writer-fixture
	UMRK_LIFE1_SMOKE_SCENARIO=game-disabled scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-no-pak scripts/life1-game-ipc-smoke.sh

life1-game-override-ipc-smoke: $(BUILD)/bin/life1-fixture-service $(BUILD)/bin/game-writer-fixture
	scripts/life1-game-override-ipc-smoke.sh

raofflineproxy-bridge-ipc-smoke: jawakad jawaka-platformctl
	scripts/raofflineproxy-bridge-ipc-smoke.sh

.PHONY: raofflineproxy-bridge-ipc-smoke raofflineproxy-bridge-test

life1-app-noevent-ipc-smoke: $(BUILD)/bin/life1-fixture-service
	scripts/life1-app-noevent-ipc-smoke.sh

active-game-recovery-ipc-smoke:
	scripts/active-game-recovery-ipc-smoke.sh

service-client-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/service-client-test \
		internal/ipc/service_client_test.c internal/ipc/ipc.c \
		internal/ipc/ipc_client.c internal/ipc/ctl1.c \
		internal/platform/platform_common.c internal/i18n/i18n.c \
		internal/core/log.c third_party/cjson/cJSON.c -lm
	$(BUILD)/bin/service-client-test

imported-title-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/imported-title-test \
		internal/db/imported_title_test.c internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		$(LDLIBS_COMMON)
	$(BUILD)/bin/imported-title-test

settings-status-test: | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $(BUILD)/bin/settings-status-test \
		internal/settings/settings_status_test.c $(UI_SRCS) $(LDLIBS_UI)
	$(BUILD)/bin/settings-status-test

pinyin-search-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/pinyin-search-test \
		internal/db/pinyin_search_test.c internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		$(LDLIBS_COMMON)
	$(BUILD)/bin/pinyin-search-test

imported-title-ipc-smoke:
	scripts/imported-title-ipc-smoke.sh

focus-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/focus-test \
		internal/focus/focus_test.c internal/focus/focus.c \
		internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		internal/update/sha256.c $(LDLIBS_COMMON)
	$(BUILD)/bin/focus-test

states-core-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/states-core-test \
		internal/retroarch/states_core_test.c internal/retroarch/states.c
	$(BUILD)/bin/states-core-test

legacy-migration-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/legacy-migration-test \
		internal/retroarch/legacy_migration_test.c \
		internal/retroarch/legacy_migration.c internal/retroarch/catalog.c \
		internal/platform/platform_id_mock.c third_party/cjson/cJSON.c
	$(BUILD)/bin/legacy-migration-test

retroarch-command-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/retroarch-command-test \
		internal/retroarch/command_test.c internal/retroarch/command.c
	$(BUILD)/bin/retroarch-command-test

retroarch-config-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/retroarch-config-test \
		internal/platform/paths_config_test.c internal/platform/paths.c \
		internal/platform/platform_id_mock.c internal/retroarch/catalog.c \
		internal/core/log.c third_party/cjson/cJSON.c
	$(BUILD)/bin/retroarch-config-test

raofflineproxy-bridge-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/raofflineproxy-bridge-test \
		internal/platform/raofflineproxy_bridge_test.c internal/platform/paths.c \
		internal/platform/raofflineproxy.c \
		internal/platform/platform_id_mock.c internal/retroarch/catalog.c \
		internal/core/log.c third_party/cjson/cJSON.c -lpthread
	$(BUILD)/bin/raofflineproxy-bridge-test

retroarch-recording-path-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -DPLATFORM_MLP1 -o $(BUILD)/bin/retroarch-recording-path-test \
		internal/platform/paths_config_test.c internal/platform/paths.c \
		internal/platform/platform_id_mock.c internal/retroarch/catalog.c \
		internal/core/log.c third_party/cjson/cJSON.c
	$(BUILD)/bin/retroarch-recording-path-test

catalog-folder-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/catalog-folder-test \
		internal/retroarch/catalog_folder_test.c internal/retroarch/catalog.c \
		internal/platform/platform_id_mock.c third_party/cjson/cJSON.c
	$(BUILD)/bin/catalog-folder-test

standalone-policy-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/standalone-policy-test \
		internal/launcher/standalone_policy_test.c \
		internal/launcher/standalone_policy.c
	$(BUILD)/bin/standalone-policy-test

suspend-inhibit-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/suspend-inhibit-test \
		internal/power/suspend_inhibit_test.c internal/power/suspend_inhibit.c
	$(BUILD)/bin/suspend-inhibit-test

suspend-inhibit-ipc-smoke:
	scripts/suspend-inhibit-ipc-smoke.sh

update-local-manifest-smoke:
	@scripts/update-local-manifest-smoke.sh

$(BUILD)/bin:
	@mkdir -p $(BUILD)/bin

check-catastrophe:
	@test -f "$(CATASTROPHE_HEADER)" || \
		( echo "Catastrophe headers not found. Set CATASTROPHE_DIR=/path/to/Catastrophe and retry." && exit 1 )

check-sdl:
	@pkg-config --exists sdl2 SDL2_ttf SDL2_image 2>/dev/null || \
		( echo "SDL2 libraries not found. Install with: brew install sdl2 sdl2_ttf sdl2_image" && exit 1 )

$(BUILD)/bin/jawakad: $(DAEMON_SRCS) $(SCRAPE_CREDENTIALS_HEADER) | $(BUILD)/bin
	@echo "  CC      $@"
	@$(CC) $(CFLAGS_DAEMON) -o $@ $(DAEMON_SRCS) $(LDLIBS_DAEMON)

$(BUILD)/bin/jawaka-launcher: cmd/jawaka-launcher/main.c $(UI_SRCS) $(CATASTROPHE_HEADER) | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $@ cmd/jawaka-launcher/main.c $(UI_SRCS) $(LDLIBS_UI)

$(BUILD)/bin/jawaka-menu: cmd/jawaka-menu/main.c $(UI_SRCS) $(CATASTROPHE_HEADER) | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $@ cmd/jawaka-menu/main.c $(UI_SRCS) $(LDLIBS_UI)

$(BUILD)/generated:
	@mkdir -p $(BUILD)/generated

# Keep credential changes in make's dependency graph without exposing values in
# compiler command lines. FORCE reruns this small recipe; preserving the header
# mtime when its content is unchanged keeps incremental daemon builds fast.
$(SCRAPE_CREDENTIALS_HEADER): FORCE | $(BUILD)/generated
	@umask 077; tmp="$@.tmp"; \
		{ \
			printf '#define SCREENSCRAPER_DEV_ID "%s"\n' "$$SCREENSCRAPER_DEV_ID"; \
			printf '#define SCREENSCRAPER_DEV_PASSWORD "%s"\n' "$$SCREENSCRAPER_DEV_PASSWORD"; \
			printf '#define SCREENSCRAPER_DEBUG_PASSWORD "%s"\n' "$$SCREENSCRAPER_DEBUG_PASSWORD"; \
		} > "$$tmp"; \
		if test -f "$@" && cmp -s "$$tmp" "$@"; then \
			rm -f "$$tmp"; \
		else \
			mv -f "$$tmp" "$@"; \
		fi

$(BUILD)/generated/xdg-shell-client-protocol.h: | $(BUILD)/generated
	@test -n "$(WAYLAND_PROTOCOLS_DIR)" || { echo "wayland-protocols pkg-config data dir missing" >&2; exit 1; }
	$(WAYLAND_SCANNER) client-header "$(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml" $@

$(BUILD)/generated/xdg-shell-protocol.c: $(BUILD)/generated/xdg-shell-client-protocol.h
	@test -n "$(WAYLAND_PROTOCOLS_DIR)" || { echo "wayland-protocols pkg-config data dir missing" >&2; exit 1; }
	$(WAYLAND_SCANNER) private-code "$(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml" $@

$(BUILD)/bin/jawaka-osd: $(OSD_SRCS) $(OSD_DEPS) $(CATASTROPHE_HEADER) | $(BUILD)/bin
	$(CC) $(OSD_CFLAGS) -o $@ $(OSD_SRCS) $(OSD_LDLIBS)

$(BUILD)/bin/jawaka-retroarchctl: $(RETROARCH_CTL_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(RETROARCH_CTL_SRCS)

$(BUILD)/bin/jawaka-retroarch-runner: $(RETROARCH_RUNNER_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(RETROARCH_RUNNER_SRCS)

$(BUILD)/bin/jawaka-update-runner: $(UPDATE_RUNNER_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(UPDATE_RUNNER_SRCS)

$(BUILD)/bin/jawaka-platformctl: $(PLATFORM_CTL_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(PLATFORM_CTL_SRCS) $(LDLIBS_COMMON)

$(BUILD)/bin/jawaka-scan-smoke: $(SCAN_SMOKE_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(SCAN_SMOKE_SRCS) $(LDLIBS_COMMON)

$(BUILD)/bin/jawaka-scrape-smoke: $(SCRAPE_SMOKE_SRCS) $(SCRAPE_CREDENTIALS_HEADER) | $(BUILD)/bin
	@echo "  CC      $@"
	@$(CC) $(CFLAGS_COMMON) $(SCRAPE_CFLAGS) -o $@ $(SCRAPE_SMOKE_SRCS) $(LDLIBS_COMMON) $(CURL_LDFLAGS) -lpthread -lm

$(BUILD)/bin/jawaka-pakrat-smoke: $(PAKRAT_SMOKE_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -DJW_ENABLE_FAULT_INJECTION=1 $(CURL_CFLAGS) -Ithird_party/miniz -o $@ $(PAKRAT_SMOKE_SRCS) $(LDLIBS_COMMON) $(CURL_LDFLAGS) -lm

$(BUILD)/bin/jawaka-catalog-smoke: $(CATALOG_SMOKE_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(CATALOG_SMOKE_SRCS)

$(BUILD)/bin/jawaka-i18n-test: $(I18N_TEST_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(I18N_TEST_SRCS) $(LDLIBS_COMMON)

$(BUILD)/bin/jawaka-core-override-smoke: $(CORE_OVERRIDE_SMOKE_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(CORE_OVERRIDE_SMOKE_SRCS) $(LDLIBS_COMMON)

$(BUILD)/bin/jawaka-update-smoke: $(UPDATE_SMOKE_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) $(CURL_CFLAGS) -o $@ $(UPDATE_SMOKE_SRCS) $(LDLIBS_COMMON) $(CURL_LDFLAGS) -lpthread

$(BUILD)/bin/jawaka-inhibitctl: $(INHIBIT_CTL_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(INHIBIT_CTL_SRCS)

ifeq ($(PLATFORM),mlp1)
$(BUILD)/bin/jawaka-ledd: cmd/jawaka-ledd/main.c | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ cmd/jawaka-ledd/main.c

$(BUILD)/build-manifest.json: $(ALL_BINS) FORCE
	@mkdir -p "$(BUILD)"
	@{ \
		printf '{\n'; \
		printf '  "platform": "mlp1",\n'; \
		printf '  "target_soc": "%s",\n' "$(UMRK_MLP1_TARGET_SOC)"; \
		printf '  "target_cpu": "%s",\n' "$(UMRK_MLP1_TARGET_CPU)"; \
		printf '  "build_profile": "%s",\n' "$(MLP1_BUILD_PROFILE)"; \
		printf '  "cflags": "%s",\n' "$(CDEBUG)"; \
		printf '  "ldflags": "%s",\n' "$(LDFLAGS_PLATFORM)"; \
		printf '  "features": {"screenscraper": %s},\n' "$(if $(filter 1,$(SCREENSCRAPER_AVAILABLE)),true,false)"; \
		printf '  "binaries": ["jawakad", "jawaka-launcher", "jawaka-menu", "jawaka-osd", "jawaka-retroarchctl", "jawaka-retroarch-runner", "jawaka-update-runner", "jawaka-platformctl", "jawaka-inhibitctl", "jawaka-ledd"],\n'; \
		printf '  "exceptions": []\n'; \
		printf '}\n'; \
	} > "$@"

endif

FORCE:

phase3-fixture-scan-smoke:
	scripts/phase3-fixture-scan-smoke.sh

phase3-core-choice-smoke:
	scripts/phase3-core-choice-smoke.sh

pakrat-state-smoke:
	scripts/pakrat-state-smoke.sh

pakrat-history-smoke:
	scripts/pakrat-history-smoke.sh

pakrat-recovery-smoke:
	scripts/pakrat-recovery-smoke.sh

pakrat-service-mutation-smoke:
	BUILD="$(BUILD)" bash scripts/pakrat-service-mutation-smoke.sh

mockgen:
	bash scripts/mockgen.sh

run-daemon: $(BUILD)/bin/jawakad mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_AUTODEMO="$${JAWAKA_AUTODEMO:-1}" \
	JAWAKA_AUTODEMO_DELAY_MS="$${JAWAKA_AUTODEMO_DELAY_MS:-1200}" \
	JAWAKA_THEME="$${JAWAKA_THEME:-Jawaka-Tabs}" \
	$(BUILD)/bin/jawakad

run-daemon-interactive: $(BUILD)/bin/jawakad mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_AUTODEMO=0 \
	JAWAKA_THEME="$${JAWAKA_THEME:-Jawaka-Tabs}" \
	$(BUILD)/bin/jawakad

run-daemon-only: $(BUILD)/bin/jawakad mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_THEME="$${JAWAKA_THEME:-Jawaka-Tabs}" \
	$(BUILD)/bin/jawakad --daemon-only

run-launcher: $(BUILD)/bin/jawaka-launcher mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_THEME="$${JAWAKA_THEME:-Jawaka-Tabs}" \
	$(BUILD)/bin/jawaka-launcher

run-interactive: run-daemon-interactive

run-menu: $(BUILD)/bin/jawaka-menu mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_THEME="$${JAWAKA_THEME:-Jawaka-Tabs}" \
	$(BUILD)/bin/jawaka-menu

clean:
	rm -rf $(BUILD)

tg5040 tg5050 my355:
	$(MAKE) -C ports/$@ all

mlp1:
	docker run --rm \
		-e MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		-e SCREENSCRAPER_REQUIRED="$(SCREENSCRAPER_REQUIRED)" \
		-e SCREENSCRAPER_DEV_ID \
		-e SCREENSCRAPER_DEV_PASSWORD \
		-e SCREENSCRAPER_DEBUG_PASSWORD \
		-v "$(WORKSPACE_ROOT)":/workspace \
		-v "$(CURDIR)":/workspace/Jawaka \
		-v "$(abspath $(CATASTROPHE_DIR))":/workspace/Catastrophe \
		-w /workspace/Jawaka \
		"$(MLP1_TOOLCHAIN_IMAGE)" \
		make -f ports/mlp1/Makefile all

mlp1-pakrat-smoke:
	docker run --rm \
		-e MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		-v "$(WORKSPACE_ROOT)":/workspace \
		-w /workspace/Jawaka \
		"$(MLP1_TOOLCHAIN_IMAGE)" \
		make -f ports/mlp1/Makefile pakrat-smoke

mlp1-inhibit-smoke:
	docker run --rm \
		-e MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		-v "$(WORKSPACE_ROOT)":/workspace \
		-w /workspace/Jawaka \
		"$(MLP1_TOOLCHAIN_IMAGE)" \
		make -f ports/mlp1/Makefile inhibit-smoke

mlp1-adb-smoke:
	scripts/adb-mlp1-smoke.sh

mlp1-adb-pakrat-recovery-smoke:
	scripts/adb-mlp1-pakrat-recovery-smoke.sh

mlp1-adb-service-mutation-smoke:
	scripts/adb-mlp1-service-mutation-smoke.sh

mlp1-adb-service-fixture-smoke:
	scripts/adb-mlp1-service-fixture-smoke.sh

mlp1-adb-life1-smoke:
	scripts/adb-mlp1-life1-smoke.sh

mlp1-adb-inhibit-smoke:
	scripts/adb-mlp1-suspend-inhibit-smoke.sh

mlp1-adb-input-capture:
	scripts/adb-mlp1-input-capture.sh

mlp1-adb-ra-command-smoke:
	scripts/adb-mlp1-retroarch-command-smoke.sh

help:
	@echo ""
	@echo "Jawaka build targets"
	@echo "===================="
	@echo "  make               Build jawakad, jawaka-launcher, jawaka-menu"
	@echo "  make mockgen       Create/update the mock SD-card tree"
	@echo "  make run-daemon              Run the daemon-driven phase-0/1 demo (auto-transitions)"
	@echo "  make run-daemon-interactive  Run daemon without auto-demo (stays open for testing)"
	@echo "  make run-daemon-only         Run jawakad without spawning launcher/menu"
	@echo "  make run-launcher            Run jawaka-launcher directly (requires daemon)"
	@echo "  make run-interactive         Alias for run-daemon-interactive"
	@echo "  make run-menu                Run jawaka-menu directly"
	@echo "  make jawaka-osd              Build the daemon-owned brightness OSD"
	@echo "  make jawaka-platformctl      Build platform status/control helper"
	@echo "  make jawaka-inhibitctl       Build suspend-inhibitor diagnostic helper"
	@echo "  make suspend-inhibit-test suspend-inhibit-ipc-smoke  Run native lease/power tests"
	@echo "  make jawaka-retroarch-runner Build RetroArch app/config runner"
	@echo "  make jawaka-update-runner    Build OTA install handoff runner"
	@echo "  make jawaka-pakrat-smoke     Build local Pak Rat install/uninstall smoke helper"
	@echo "  make jawaka-catalog-smoke    Build metadata/core-choice smoke helper"
	@echo "  make standalone-policy-test  Validate standalone DRM/input classification"
	@echo "  make update-local-manifest-smoke  Validate developer artifact.url handling"
	@echo "  make pakrat-state-smoke      Exercise Pak Rat stale + managed-state safeguards"
	@echo "  make pakrat-txn-test         Exercise service-pak mutation metadata and uninstall"
	@echo "  make pakrat-recovery-smoke   Exercise Pak Rat promote-transaction crash recovery"
	@echo "  make pakrat-service-mutation-smoke  Exercise TXN-1 through jawakad and Pak Rat"
	@echo "  make clean         Remove build artifacts"
	@echo "  make tg5040        Placeholder cross-compile target"
	@echo "  make tg5050        Placeholder cross-compile target"
	@echo "  make my355         Placeholder cross-compile target"
	@echo "  make mlp1          Cross-compile for Miniloong Pocket 1"
	@echo "  make mlp1-pakrat-smoke  Cross-compile local Pak Rat smoke helper for MLP1"
	@echo "  make mlp1-adb-inhibit-smoke  Run the RTC-woken MLP1 suspend/reap smoke"
	@echo "  make mlp1-adb-smoke  Build, push to /tmp, and run an ADB UI smoke"
	@echo "  make mlp1-adb-pakrat-recovery-smoke  Run P1 recovery matrix on a real FAT card"
	@echo "  make mlp1-adb-service-mutation-smoke Run B4a service mutation matrix on both FAT cards"
	@echo "  make mlp1-adb-service-fixture-smoke  Run A2 service fixtures on an attached MLP1"
	@echo "  make mlp1-adb-life1-smoke  Run A3b LIFE-1 coordination on an attached MLP1"
	@echo "  make mlp1-adb-input-capture  Record Loong Gamepad evtest labels over ADB"
	@echo "  make mlp1-adb-ra-command-smoke  Run RetroArch command feature smoke over ADB"
	@echo "  make phase3-fixture-scan-smoke  Run metadata-aware scan fixture checks"
	@echo "  make phase3-core-choice-smoke  Run core picker path-core fixture checks"
	@echo ""
	@echo "Environment variables"
	@echo "====================="
	@echo "  JAWAKA_SDCARD_ROOT            Path to SD-card root (default: ./mock-sdcard)"
	@echo "  JAWAKA_AUTODEMO=1             Auto-transition launcher→menu→shutdown (default in run-daemon)"
	@echo "  JAWAKA_AUTODEMO_DELAY_MS=N    Delay before auto-demo action fires, ms (default: 1200)"
	@echo "  JAWAKA_THEME=Jawaka-Tabs      Launcher theme: Jawaka-Tabs, Jawaka-Vertical, Jawaka-Horizontal"
	@echo ""
	@echo "Catastrophe include root: $(CATASTROPHE_INCLUDE)"
	@echo ""
