/*
 * qr-blocker-filter.c - OBS QR Code Blocker Filter Plugin
 *
 * This plugin detects QR codes in a video source (A) and automatically
 * covers them with content from another source (B) during OBS streaming.
 *
 * License: MIT
 */

#include <obs-module.h>
#include <obs-source.h>
#include <obs-hotkey.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>
#include <util/threading.h>
#include <util/platform.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include "quirc.h"

#define PLUGIN_VERSION "v1.0.27"
#define MAX_QR_CODES   16
#define MODULE_NAME    "qr-blocker-filter"
#define MODULE_TEXT    obs_module_text("QR.Blocker.Filter")
#define SOURCE_B_TEXT  obs_module_text("Source.B")
#define PADDING_TEXT   obs_module_text("Padding")
#define DETECT_INTV    obs_module_text("Detect.Interval")
#define FALLBACK_TEXT  obs_module_text("Fallback.Color")

/* Structure for storing QR code bounding box */
struct qr_rect {
	float x;
	float y;
	float w;
	float h;
};

/* Main filter data structure */
struct qr_blocker_filter {
	obs_source_t *context;       /* This filter source */
	obs_source_t *source_b;      /* The overlay source B */
	char *source_b_name;         /* Name of source B for lookup */

	/* Texture rendering for frame capture */
	gs_texrender_t *texrender;
	gs_stagesurf_t *staging;
	uint32_t staging_width;
	uint32_t staging_height;

	/* QR detection results (thread-safe) */
	CRITICAL_SECTION mutex;
	int qr_count;
	struct qr_rect qr_rects[MAX_QR_CODES];

	/* Detection parameters */
	int padding;                 /* Padding around QR code (pixels) */
	int detect_interval;         /* Process every N frames */
	int frame_count;             /* Frame counter */

	/* Fallback overlay color when B source is not available */
	struct vec4 fallback_color;

	/* Detection enabled flag */
	bool enabled;

	/* Overlay source B enabled flag */
	bool source_b_enabled;

	/* Scene item for source B (to control eye icon visibility) */
	obs_sceneitem_t *source_b_sceneitem;

	/* Timer for periodic scene item retry (seconds) */
	float scene_item_retry_timer;

	/* Cached Quirc instance (created once, reused) */
	struct quirc *quirc;
	uint32_t quirc_cache_w;
	uint32_t quirc_cache_h;

	/* Verbose debug logging */
	bool verbose_logging;

	/* Hotkey: smart toggle */
	obs_hotkey_id smart_hotkey_id;
	float disable_timer;
	bool manually_disabled;
	bool manual_toggle_used;

	/* Scene item tracking state (per-instance) */
	bool was_detected;
	float last_warn;

	/* Detection region (0,0,0,0 = full frame) */
	int detect_x;
	int detect_y;
	int detect_w;
	int detect_h;
};

/* ==================== Helper: Overlay Visibility Logic ==================== */

/* Play a distinct sound for hotkey feedback */
static inline void play_hotkey_sound(int type)
{
	switch (type) {
	case 1: /* Force enable — device connect */
		PlaySound("DeviceConnect", NULL, SND_ALIAS | SND_ASYNC);
		break;
	case 2: /* Auto mode (close force) — device disconnect */
		PlaySound("DeviceDisconnect", NULL, SND_ALIAS | SND_ASYNC);
		break;
	case 3: /* Disable 5s — device disconnect */
		PlaySound("DeviceDisconnect", NULL, SND_ALIAS | SND_ASYNC);
		break;
	case 4: /* Timer expired, back to auto — device connect */
		PlaySound("DeviceConnect", NULL, SND_ALIAS | SND_ASYNC);
		break;
	default:
		PlaySound("DeviceConnect", NULL, SND_ALIAS | SND_ASYNC);
		break;
	}
}

/* Should we do QR detection? */
static inline bool should_run_detection(struct qr_blocker_filter *f)
{
	return f->enabled && f->disable_timer <= 0.0f;
}

/* Should the overlay be shown? */
static inline bool should_show_overlay(struct qr_blocker_filter *f, bool qr_detected)
{
	if (!f->enabled) return false;
	if (f->disable_timer > 0.0f) return false;
	if (f->manual_toggle_used && !f->manually_disabled) return true;
	return qr_detected;
}

static void detect_qr_codes(struct qr_blocker_filter *filter,
			    const uint8_t *gray_data,
			    uint32_t width, uint32_t height,
			    int offset_x, int offset_y)
{
	const uint8_t *input = gray_data;

	/* Scale up to min dimension of ~512px for reliable Quirc detection */
	float scale = 512.0f / (width < height ? width : height);
	if (scale < 2.0f) scale = 2.0f;
	uint32_t sw = (uint32_t)(width * scale);
	uint32_t sh = (uint32_t)(height * scale);
	uint8_t *scaled = bmalloc(sw * sh);
	for (uint32_t y = 0; y < sh; y++) {
		uint32_t sy = (uint32_t)(y / scale);
		if (sy >= height) sy = height - 1;
		for (uint32_t x = 0; x < sw; x++) {
			uint32_t sx = (uint32_t)(x / scale);
			if (sx >= width) sx = width - 1;
			scaled[y * sw + x] = input[sy * width + sx];
		}
	}

	if (filter->verbose_logging)
		blog(LOG_INFO, "[QR Blocker] detect: %dx%d -> %dx%d (scale=%.1f)",
		     width, height, sw, sh, scale);

	/* Resize cached Quirc only when dimensions change */
	struct quirc *qr = filter->quirc;
	if (qr && (sw != filter->quirc_cache_w || sh != filter->quirc_cache_h)) {
		if (quirc_resize(qr, (int)sw, (int)sh) < 0) {
			blog(LOG_WARNING, "[QR Blocker] quirc_resize failed");
			bfree(scaled);
			return;
		}
		filter->quirc_cache_w = sw;
		filter->quirc_cache_h = sh;
	}

	int best_count = 0;
	int best_corners[MAX_QR_CODES][4][2];
	struct quirc_code best_code;

	/* Try normal pass first, only try inverted if nothing found */
	for (int pass = 0; pass < 2 && best_count == 0; pass++) {
		if (!qr) continue;

		uint8_t *qr_image = quirc_begin(qr, NULL, NULL);
		if (pass == 0) {
			memcpy(qr_image, scaled, sw * sh);
		} else {
			for (uint32_t i = 0; i < sw * sh; i++)
				qr_image[i] = 255 - scaled[i];
		}
		quirc_end(qr);

		int n = quirc_count(qr);
		if (n > 0) {
			best_count = n > MAX_QR_CODES ? MAX_QR_CODES : n;
			for (int i = 0; i < best_count; i++) {
				quirc_extract(qr, i, &best_code);
				for (int j = 0; j < 4; j++) {
					best_corners[i][j][0] = best_code.corners[j].x;
					best_corners[i][j][1] = best_code.corners[j].y;
				}
			}
		}
	}

	blog(LOG_INFO, "[QR Blocker] quirc_count=%d (%dx%d)",
	     best_count, width, height);

	EnterCriticalSection(&filter->mutex);
	filter->qr_count = 0;

	for (int i = 0; i < best_count && i < MAX_QR_CODES; i++) {
		int min_x = (int)sw, min_y = (int)sh;
		int max_x = 0, max_y = 0;

		for (int j = 0; j < 4; j++) {
			int cx = best_corners[i][j][0];
			int cy = best_corners[i][j][1];

			if (cx < min_x) min_x = cx;
			if (cy < min_y) min_y = cy;
			if (cx > max_x) max_x = cx;
			if (cy > max_y) max_y = cy;
		}

		int pad = filter->padding;
		int ox = (int)(min_x / scale);
		int oy = (int)(min_y / scale);
		int ow = (int)((max_x - min_x) / scale);
		int oh = (int)((max_y - min_y) / scale);

		min_x = (ox > pad) ? (ox - pad) : 0;
		min_y = (oy > pad) ? (oy - pad) : 0;
		max_x = (ox + ow + pad < (int)width)  ? (ox + ow + pad) : (int)width;
		max_y = (oy + oh + pad < (int)height) ? (oy + oh + pad) : (int)height;

		filter->qr_rects[i].x = (float)(min_x + offset_x);
		filter->qr_rects[i].y = (float)(min_y + offset_y);
		filter->qr_rects[i].w = (float)(max_x - min_x);
		filter->qr_rects[i].h = (float)(max_y - min_y);
		filter->qr_count++;
	}

	LeaveCriticalSection(&filter->mutex);
	bfree(scaled);
}

/* Convert BGRA frame to grayscale and run QR detection */
static void process_frame_for_qr(struct qr_blocker_filter *filter,
				 uint8_t *data, uint32_t frame_w,
				 uint32_t frame_h, uint32_t linesize)
{
	uint32_t region_x = 0, region_y = 0;
	uint32_t region_w = frame_w, region_h = frame_h;

	if (filter->detect_w > 0 && filter->detect_h > 0) {
		region_x = (uint32_t)filter->detect_x;
		region_y = (uint32_t)filter->detect_y;
		region_w = (uint32_t)filter->detect_w;
		region_h = (uint32_t)filter->detect_h;
		if (region_x + region_w > frame_w)
			region_w = frame_w - region_x;
		if (region_y + region_h > frame_h)
			region_h = frame_h - region_y;
	}

	if (filter->verbose_logging)
		blog(LOG_INFO, "[QR Blocker] process_frame: Full %dx%d, Region %dx%d at %d,%d",
		     frame_w, frame_h, region_w, region_h, region_x, region_y);

	uint8_t *gray = bmalloc(region_w * region_h);
	if (!gray)
		return;

	uint8_t min_val = 255, max_val = 0;

	for (uint32_t y = 0; y < region_h; y++) {
		const uint8_t *row = data + (region_y + y) * linesize + region_x * 4;
		for (uint32_t x = 0; x < region_w; x++) {
			uint32_t idx = x * 4;
			uint8_t b = row[idx];
			uint8_t g = row[idx + 1];
			uint8_t r = row[idx + 2];
			uint8_t v = (uint8_t)(
				(77 * r + 150 * g + 29 * b) >> 8);
			gray[y * region_w + x] = v;
			if (v < min_val) min_val = v;
			if (v > max_val) max_val = v;
		}
	}

	/* Normalize contrast in-place if range is narrow */
	int range = max_val - min_val;
	if (range > 0 && range < 200) {
		for (uint32_t i = 0; i < region_w * region_h; i++)
			gray[i] = (uint8_t)((gray[i] - min_val) * 255 / range);
	}

	detect_qr_codes(filter, gray, region_w, region_h,
			(int)region_x, (int)region_y);

	bfree(gray);
}

/* ==================== OBS Filter Callbacks ==================== */

/* Callback for finding scene containing source A */
struct find_scene_cb_data {
	obs_source_t *target;
	obs_scene_t *found;
};

static bool find_scene_enum_cb(void *param, obs_source_t *scene_source)
{
	struct find_scene_cb_data *data = param;
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (!scene)
		return true;

	if (obs_scene_find_source(scene, obs_source_get_name(data->target))) {
		data->found = scene;
		return false;
	}
	return true;
}

/* Find the scene item for source B in the same scene as source A */
static void lookup_source_b_sceneitem(struct qr_blocker_filter *filter)
{
	filter->source_b_sceneitem = NULL;
	if (!filter->source_b_name)
		return;

	obs_source_t *parent = obs_filter_get_parent(filter->context);
	if (!parent)
		return;

	struct find_scene_cb_data data = {
		.target = parent,
		.found = NULL,
	};
	obs_enum_scenes(find_scene_enum_cb, &data);

	if (data.found) {
		filter->source_b_sceneitem =
			obs_scene_find_source(data.found, filter->source_b_name);
		if (filter->source_b_sceneitem)
			blog(LOG_INFO, "[QR Blocker] Scene item B found: %s",
			     filter->source_b_name);
		else
			blog(LOG_WARNING, "[QR Blocker] Source B '%s' not in scene",
			     filter->source_b_name);
	} else {
		blog(LOG_WARNING, "[QR Blocker] Parent source not found in any scene");
	}
}

static const char *filter_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return MODULE_TEXT;
}

/* Smart hotkey callback: adapts behavior based on current state */
static void smart_toggle_cb(void *data, obs_hotkey_id id,
			    obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct qr_blocker_filter *filter = data;

	if (!filter || !pressed)
		return;

	/* During 130s timer: cancel and go back to auto mode */
	if (filter->disable_timer > 0.0f) {
		filter->disable_timer = 0.0f;
		filter->manual_toggle_used = false;
		filter->manually_disabled = false;
		play_hotkey_sound(4);
		blog(LOG_INFO, "[QR Blocker] Smart: Cancel 130s timer → AUTO");
		return;
	}

	/* Check current QR detection state */
	bool qr_present = false;
	EnterCriticalSection(&filter->mutex);
	qr_present = filter->qr_count > 0;
	LeaveCriticalSection(&filter->mutex);

	if (filter->manual_toggle_used && !filter->manually_disabled) {
		/* Force On state */
		if (qr_present) {
			/* Force On + QR present → Disable 130s */
			filter->disable_timer = 130.0f;
			EnterCriticalSection(&filter->mutex);
			filter->qr_count = 0;
			LeaveCriticalSection(&filter->mutex);
			filter->manual_toggle_used = false;
			filter->manually_disabled = false;
			play_hotkey_sound(3);
			blog(LOG_INFO, "[QR Blocker] Smart: FORCE ON + QR → DISABLE 130s");
		} else {
			/* Force On + no QR → Auto mode */
			filter->manual_toggle_used = false;
			filter->manually_disabled = false;
			play_hotkey_sound(2);
			blog(LOG_INFO, "[QR Blocker] Smart: FORCE ON → AUTO");
		}
	} else if (qr_present) {
		/* Auto + QR present → Disable 130s */
		filter->disable_timer = 130.0f;
		EnterCriticalSection(&filter->mutex);
		filter->qr_count = 0;
		LeaveCriticalSection(&filter->mutex);
		play_hotkey_sound(3);
		blog(LOG_INFO, "[QR Blocker] Smart: AUTO + QR → DISABLE 130s");
	} else {
		/* Auto + no QR → Force On */
		filter->manual_toggle_used = true;
		filter->manually_disabled = false;
		filter->disable_timer = 0.0f;
		play_hotkey_sound(1);
		blog(LOG_INFO, "[QR Blocker] Smart: AUTO + no QR → FORCE ON");
	}
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct qr_blocker_filter *filter = bzalloc(sizeof(*filter));
	if (!filter)
		return NULL;

	filter->context = source;
	filter->enabled = true;
	filter->padding = 10;
	filter->detect_interval = 4;
	filter->detect_x = 0;
	filter->detect_y = 0;
	filter->detect_w = 250;
	filter->detect_h = 250;
	filter->frame_count = 0;
	filter->qr_count = 0;
	filter->staging = NULL;
	filter->staging_width = 0;
	filter->staging_height = 0;
	filter->texrender = NULL;
	filter->source_b = NULL;
	filter->source_b_name = NULL;
	filter->source_b_sceneitem = NULL;
	filter->scene_item_retry_timer = 0.0f;
	filter->quirc = quirc_new();
	filter->quirc_cache_w = 0;
	filter->quirc_cache_h = 0;
	filter->verbose_logging = false;
	filter->disable_timer = 0.0f;
	filter->manually_disabled = false;
	filter->manual_toggle_used = false;
	filter->was_detected = false;
	filter->last_warn = -999.0f;

	filter->smart_hotkey_id = OBS_INVALID_HOTKEY_ID;

	vec4_set(&filter->fallback_color, 0.0f, 0.0f, 0.0f, 1.0f);
	InitializeCriticalSection(&filter->mutex);

	obs_source_update(source, settings);

	blog(LOG_INFO, "[QR Blocker] Filter created");
	return filter;
}

static void filter_destroy(void *data)
{
	struct qr_blocker_filter *filter = data;

	if (!filter)
		return;

	if (filter->smart_hotkey_id != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(filter->smart_hotkey_id);
		filter->smart_hotkey_id = OBS_INVALID_HOTKEY_ID;
	}

	EnterCriticalSection(&filter->mutex);

	if (filter->source_b) {
		obs_source_release(filter->source_b);
		filter->source_b = NULL;
	}

	bfree(filter->source_b_name);
	filter->source_b_name = NULL;

	if (filter->texrender) {
		gs_texrender_destroy(filter->texrender);
		filter->texrender = NULL;
	}

	if (filter->staging) {
		gs_stagesurface_destroy(filter->staging);
		filter->staging = NULL;
	}

	if (filter->quirc) {
		quirc_destroy(filter->quirc);
		filter->quirc = NULL;
	}

	LeaveCriticalSection(&filter->mutex);
	DeleteCriticalSection(&filter->mutex);

	bfree(filter);
	blog(LOG_INFO, "[QR Blocker] Filter destroyed");
}

static void filter_update(void *data, obs_data_t *settings)
{
	struct qr_blocker_filter *filter = data;

	if (!filter)
		return;

	EnterCriticalSection(&filter->mutex);

	filter->padding = (int)obs_data_get_int(settings, "padding");
	filter->detect_interval =
		(int)obs_data_get_int(settings, "detect_interval");
	if (filter->detect_interval < 1)
		filter->detect_interval = 1;

	filter->detect_x = (int)obs_data_get_int(settings, "detect_x");
	filter->detect_y = (int)obs_data_get_int(settings, "detect_y");
	filter->detect_w = (int)obs_data_get_int(settings, "detect_w");
	filter->detect_h = (int)obs_data_get_int(settings, "detect_h");
	if (filter->detect_x < 0) filter->detect_x = 0;
	if (filter->detect_y < 0) filter->detect_y = 0;
	if (filter->detect_w < 0) filter->detect_w = 0;
	if (filter->detect_h < 0) filter->detect_h = 0;

	/* Get fallback color (stored as uint32_t 0xAABBGGRR) */
	uint32_t color = (uint32_t)obs_data_get_int(settings, "fallback_color");
	float alpha = (float)((color >> 24) & 0xFF) / 255.0f;
	float blue  = (float)((color >> 16) & 0xFF) / 255.0f;
	float green = (float)((color >> 8)  & 0xFF) / 255.0f;
	float red   = (float)( color        & 0xFF) / 255.0f;
	vec4_set(&filter->fallback_color, red, green, blue, alpha);

	/* Update source B reference */
	const char *name = obs_data_get_string(settings, "source_b");
	bool name_changed = false;

	if (!filter->source_b_name && name && strlen(name) > 0)
		name_changed = true;
	else if (filter->source_b_name && (!name || strcmp(filter->source_b_name, name) != 0))
		name_changed = true;
	else if (!filter->source_b_name && (!name || strlen(name) == 0))
		name_changed = false; /* Both empty, no change */

	if (name_changed) {
		bfree(filter->source_b_name);
		filter->source_b_name = (name && strlen(name) > 0) ?
			bstrdup(name) : NULL;

		/* Release old source B */
		if (filter->source_b) {
			obs_source_release(filter->source_b);
			filter->source_b = NULL;
		}

		/* Acquire new source B */
		if (filter->source_b_name) {
			filter->source_b =
				obs_get_source_by_name(filter->source_b_name);
			if (filter->source_b) {
				blog(LOG_INFO,
				     "[QR Blocker] Source B set to: %s",
				     filter->source_b_name);
			} else {
				blog(LOG_WARNING,
				     "[QR Blocker] Source B '%s' not found",
				     filter->source_b_name);
			}
		}
	}

	/* Re-lookup scene item if source B changed */
	if (name_changed) {
		lookup_source_b_sceneitem(filter);
		filter->scene_item_retry_timer = 0.0f;
	}

	filter->source_b_enabled = obs_data_get_bool(settings, "source_b_enabled");

	filter->verbose_logging = obs_data_get_bool(settings, "verbose_logging");

	LeaveCriticalSection(&filter->mutex);
}

static void filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "padding", 10);
	obs_data_set_default_bool(settings, "source_b_enabled", true);
	obs_data_set_default_int(settings, "detect_interval", 4);
	obs_data_set_default_int(settings, "fallback_color", 0xFF000000);
	obs_data_set_default_int(settings, "detect_x", 0);
	obs_data_set_default_int(settings, "detect_y", 0);
	obs_data_set_default_int(settings, "detect_w", 250);
	obs_data_set_default_int(settings, "detect_h", 250);
	obs_data_set_default_bool(settings, "verbose_logging", false);
}

/* Helper struct for source enumeration */
struct enum_data {
	obs_source_t *filter_source;
	obs_property_t *list;
};

static bool enum_source_cb(void *param, obs_source_t *source)
{
	struct enum_data *data = param;

	/* Skip the filter source itself to avoid circular reference */
	if (source == data->filter_source)
		return true;

	/* Skip filter-type sources (only show regular sources) */
	uint32_t flags = obs_source_get_output_flags(source);
	if (flags & OBS_SOURCE_CAP_DISABLED)
		return true;

	const char *name = obs_source_get_name(source);
	if (name && strlen(name) > 0)
		obs_property_list_add_string(data->list, name, name);

	return true;
}

static obs_properties_t *filter_properties(void *data)
{
	struct qr_blocker_filter *filter = data;
	obs_properties_t *props = obs_properties_create();

	/* Source B selection */
	obs_property_t *source_list = obs_properties_add_list(
		props, "source_b", obs_module_text("Source.B"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(source_list, "", "");

	if (filter) {
		struct enum_data ed = {
			.filter_source = filter->context,
			.list = source_list,
		};
		obs_enum_sources(enum_source_cb, &ed);
	}

	obs_properties_add_bool(props, "source_b_enabled",
		obs_module_text("Source.B.Enable"));

	/* Detection parameters */
	obs_properties_add_int(props, "padding",
		obs_module_text("Padding"), 0, 100, 1);
	obs_properties_add_int(props, "detect_interval",
		obs_module_text("Detect.Interval"), 1, 60, 1);

	/* Detection region */
	obs_properties_add_int(props, "detect_x",
		obs_module_text("Detect.X"), 0, 10000, 1);
	obs_properties_add_int(props, "detect_y",
		obs_module_text("Detect.Y"), 0, 10000, 1);
	obs_properties_add_int(props, "detect_w",
		obs_module_text("Detect.Width"), 0, 10000, 1);
	obs_properties_add_int(props, "detect_h",
		obs_module_text("Detect.Height"), 0, 10000, 1);

	/* Fallback overlay color (when B source is not set) */
	obs_properties_add_color_alpha(props, "fallback_color",
		obs_module_text("Fallback.Color"));

	/* Debug options */
	obs_properties_add_bool(props, "verbose_logging",
		obs_module_text("Verbose.Logging"));

	return props;
}

static void filter_video_render(void *data, gs_effect_t *effect)
{
	struct qr_blocker_filter *filter = data;

	if (!filter)
		return;

	obs_source_t *parent = obs_filter_get_parent(filter->context);
	if (!parent)
		return;

	uint32_t width = obs_source_get_base_width(parent);
	uint32_t height = obs_source_get_base_height(parent);

	if (width == 0 || height == 0 || !should_run_detection(filter)) {
		obs_source_video_render(parent);
		return;
	}

	if (filter->verbose_logging)
		blog(LOG_INFO, "[QR Blocker] render: frame=%d, %dx%d, interval=%d",
		     filter->frame_count, width, height, filter->detect_interval);

	/* ====== Render parent and capture to texrender (standard OBS pipeline) ====== */
	UNUSED_PARAMETER(effect);

	/* Create or recreate texrender as needed */
	if (!filter->texrender)
		filter->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);

	/* Render parent source to texrender */
	gs_texrender_reset(filter->texrender);
	if (gs_texrender_begin(filter->texrender, width, height)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height,
			 -100.0f, 100.0f);
		obs_source_video_render(parent);
		gs_texrender_end(filter->texrender);
	}

	/* Draw parent source directly to output */
	obs_source_video_render(parent);

	/* ====== QR detection (every N frames) from texrender ====== */
	gs_texture_t *detect_tex = gs_texrender_get_texture(
		filter->texrender);

	filter->frame_count++;
	if (detect_tex && filter->frame_count % filter->detect_interval == 0) {
		/* When already blocking, only check every 60 frames (~1s) */
		bool already_blocking = filter->qr_count > 0;
		if (already_blocking && filter->frame_count % 60 != 0)
			goto skip_detect;

		if (!filter->staging ||
		    filter->staging_width != width ||
		    filter->staging_height != height) {
			if (filter->staging)
				gs_stagesurface_destroy(filter->staging);
			filter->staging = gs_stagesurface_create(
				width, height, GS_BGRA);
			filter->staging_width = width;
			filter->staging_height = height;
		}

		gs_stage_texture(filter->staging, detect_tex);

		uint8_t *map_data = NULL;
		uint32_t map_linesize = 0;
		if (gs_stagesurface_map(filter->staging,
					&map_data, &map_linesize)) {
			process_frame_for_qr(filter, map_data,
					     width, height,
					     map_linesize);
			gs_stagesurface_unmap(filter->staging);
		}
	}

skip_detect:

	/* ====== Step 3.5: Draw detection region box (only in debug/verbose mode) ====== */
	if (filter->verbose_logging && filter->detect_w > 0 && filter->detect_h > 0) {
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		if (solid) {
			int rx = filter->detect_x;
			int ry = filter->detect_y;
			int rw = filter->detect_w;
			int rh = filter->detect_h;
			int bw = 3;

			gs_eparam_t *color_param =
				gs_effect_get_param_by_name(solid, "color");
			if (!color_param)
				goto skip_box;

			/* Draw semi-transparent fill */
			struct vec4 fill;
			vec4_set(&fill, 0.0f, 1.0f, 0.0f, 0.08f);
			gs_effect_set_vec4(color_param, &fill);
			gs_matrix_push();
			gs_matrix_translate3f((float)rx, (float)ry, 0.0f);
			gs_matrix_scale3f((float)rw, (float)rh, 1.0f);
			while (gs_effect_loop(solid, "Solid"))
				gs_draw_sprite(NULL, 0, 1, 1);
			gs_matrix_pop();

			/* Draw border */
			struct vec4 border;
			vec4_set(&border, 0.0f, 1.0f, 0.0f, 0.8f);
			gs_effect_set_vec4(color_param, &border);

			gs_matrix_push();
			while (gs_effect_loop(solid, "Solid")) {
				gs_matrix_push();
				gs_matrix_translate3f((float)rx, (float)ry, 0.0f);
				gs_matrix_scale3f((float)rw, (float)bw, 1.0f);
				gs_draw_sprite(NULL, 0, 1, 1);
				gs_matrix_pop();

				gs_matrix_push();
				gs_matrix_translate3f((float)rx, (float)(ry + rh - bw), 0.0f);
				gs_matrix_scale3f((float)rw, (float)bw, 1.0f);
				gs_draw_sprite(NULL, 0, 1, 1);
				gs_matrix_pop();

				gs_matrix_push();
				gs_matrix_translate3f((float)rx, (float)(ry + bw), 0.0f);
				gs_matrix_scale3f((float)bw, (float)(rh - bw * 2), 1.0f);
				gs_draw_sprite(NULL, 0, 1, 1);
				gs_matrix_pop();

				gs_matrix_push();
				gs_matrix_translate3f((float)(rx + rw - bw), (float)(ry + bw), 0.0f);
				gs_matrix_scale3f((float)bw, (float)(rh - bw * 2), 1.0f);
				gs_draw_sprite(NULL, 0, 1, 1);
				gs_matrix_pop();
			}
			gs_matrix_pop();
		}
skip_box:;
	}

	/* ====== Step 4: Draw overlay over QR code areas ONLY ====== */
	EnterCriticalSection(&filter->mutex);

	int count = filter->qr_count;
	if (filter->verbose_logging)
		blog(LOG_INFO, "[QR Blocker] Step4: qr_count=%d", count);
	if (count > 0) {
		for (int i = 0; i < count; i++) {
			int rx = (int)filter->qr_rects[i].x;
			int ry = (int)filter->qr_rects[i].y;
			int rw = (int)filter->qr_rects[i].w;
			int rh = (int)filter->qr_rects[i].h;

			if (rx < 0) { rw += rx; rx = 0; }
			if (ry < 0) { rh += ry; ry = 0; }
			if (rx + rw > (int)width)  rw = (int)width - rx;
			if (ry + rh > (int)height) rh = (int)height - ry;
			if (rw <= 0 || rh <= 0) continue;

			gs_effect_t *solid = obs_get_base_effect(
				OBS_EFFECT_SOLID);
			if (solid) {
				gs_eparam_t *color_param =
					gs_effect_get_param_by_name(
						solid, "color");
				if (color_param)
					gs_effect_set_vec4(
						color_param,
						&filter->fallback_color);

				gs_matrix_push();
				gs_matrix_translate3f((float)rx, (float)ry, 0.0f);
				gs_matrix_scale3f((float)rw, (float)rh, 1.0f);

				while (gs_effect_loop(solid, "Solid"))
					gs_draw_sprite(NULL, 0, 1, 1);

				gs_matrix_pop();
			}
		}
	}

	LeaveCriticalSection(&filter->mutex);
}

static void filter_video_tick(void *data, float seconds)
{
	struct qr_blocker_filter *filter = data;

	if (!filter)
		return;

	/* Deferred hotkey registration (parent may not be available in create) */
	if (filter->smart_hotkey_id == OBS_INVALID_HOTKEY_ID) {
		obs_source_t *parent = obs_filter_get_parent(filter->context);
		if (parent) {
			filter->smart_hotkey_id = obs_hotkey_register_source(
				parent,
				"qr_blocker_filter.smart",
				obs_module_text("Smart.Hotkey"),
				smart_toggle_cb, filter);
		}
	}

	/* Auto-re-enable after timer hotkey expires */
	if (filter->disable_timer > 0.0f) {
		filter->disable_timer -= seconds;
		if (filter->disable_timer <= 0.0f) {
			filter->disable_timer = 0.0f;
			filter->manually_disabled = true;
			play_hotkey_sound(4);
			blog(LOG_INFO, "[QR Blocker] Timer expired, back to auto mode");
		}
	}

	/* Verify source B is still valid */
	EnterCriticalSection(&filter->mutex);
	if (filter->source_b_name && !filter->source_b) {
		filter->source_b =
			obs_get_source_by_name(filter->source_b_name);
	}

	/* Retry scene item lookup every 60 seconds if still missing */
	if (filter->source_b && !filter->source_b_sceneitem) {
		filter->scene_item_retry_timer += seconds;
		if (filter->scene_item_retry_timer >= 60.0f) {
			filter->scene_item_retry_timer = 0.0f;
			lookup_source_b_sceneitem(filter);
		}
	}

	bool detected = filter->qr_count > 0;
	LeaveCriticalSection(&filter->mutex);

	/* Control source B scene item visibility */
	if (filter->source_b_enabled && filter->source_b_sceneitem) {
		bool show = should_show_overlay(filter, detected);
		bool item_visible = obs_sceneitem_visible(filter->source_b_sceneitem);
		if (show != filter->was_detected) {
			filter->was_detected = show;
			blog(LOG_INFO, "[QR Blocker] Scene item B: %s (prev=%d, item=%d)",
			     show ? "SHOW" : "HIDE", !show, item_visible);
		}
		obs_sceneitem_set_visible(filter->source_b_sceneitem, show);
	} else if (filter->source_b_enabled && filter->source_b) {
		if (filter->scene_item_retry_timer - filter->last_warn > 60.0f ||
		    filter->last_warn < 0.0f) {
			filter->last_warn = filter->scene_item_retry_timer;
			blog(LOG_WARNING, "[QR Blocker] Scene item B not found: %s",
			     filter->source_b_name ? filter->source_b_name : "(null)");
		}
	}
}

static void filter_activate(void *data)
{
	struct qr_blocker_filter *filter = data;
	if (!filter)
		return;
	if (filter->disable_timer <= 0.0f) {
		filter->enabled = true;
		filter->manually_disabled = false;
		filter->manual_toggle_used = false;
	}
}

static void filter_deactivate(void *data)
{
	struct qr_blocker_filter *filter = data;
	if (filter)
		filter->enabled = false;
}

/* ==================== OBS Module Entry Point ==================== */

static struct obs_source_info qr_blocker_filter_info = {
	.id             = "qr_blocker_filter",
	.type           = OBS_SOURCE_TYPE_FILTER,
	.output_flags   = OBS_SOURCE_VIDEO,
	.get_name       = filter_get_name,
	.create         = filter_create,
	.destroy        = filter_destroy,
	.update         = filter_update,
	.get_defaults   = filter_defaults,
	.get_properties = filter_properties,
	.video_render   = filter_video_render,
	.video_tick     = filter_video_tick,
	.activate       = filter_activate,
	.deactivate     = filter_deactivate,
};

/* Required OBS module declarations */
OBS_DECLARE_MODULE()

/* Use OBS default locale handler */
OBS_MODULE_USE_DEFAULT_LOCALE("qr-blocker-filter", "zh-CN")

bool obs_module_load(void)
{
	obs_register_source(&qr_blocker_filter_info);
	blog(LOG_INFO, "[QR Blocker] Plugin loaded successfully (" PLUGIN_VERSION ")");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[QR Blocker] Plugin unloaded");
}

const char *obs_module_description(void)
{
	return "Detects QR codes in video and overlays content to block them";
}

const char *obs_module_name(void)
{
	return "QR Blocker Filter";
}

const char *obs_module_author(void)
{
	return "OBS Plugin Developer";
}
