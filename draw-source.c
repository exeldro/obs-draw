#include "draw-source.h"
#include "version.h"
#include <graphics/image-file.h>
#include <obs-module.h>
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
#include <util/deque.h>
#define circlebuf_peek_front deque_peek_front
#define circlebuf_peek_back deque_peek_back
#define circlebuf_push_front deque_push_front
#define circlebuf_push_back deque_push_back
#define circlebuf_pop_front deque_pop_front
#define circlebuf_pop_back deque_pop_back
#define circlebuf_init deque_init
#define circlebuf_free deque_free
#else
#include <util/circlebuf.h>
#endif

struct draw_source {
	obs_source_t *source;
	struct vec2 size;

#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
	struct deque undo;
#else
	struct circlebuf undo;
#endif
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
	struct deque redo;
#else
	struct circlebuf redo;
#endif
	uint32_t max_undo;
	gs_texrender_t *render_a;
	gs_texrender_t *render_b;
	bool render_a_active;

	bool show_mouse;
	bool mouse_active;
	uint32_t tool_mode;
	bool shift_down;

	struct vec2 mouse_pos;
	struct vec2 mouse_previous_pos;

	struct vec2 select_from;
	struct vec2 select_to;

	gs_effect_t *draw_effect;
	gs_eparam_t *image_param;
	gs_eparam_t *uv_size_param;
	gs_eparam_t *uv_mouse_param;
	gs_eparam_t *uv_mouse_previous_param;
	gs_eparam_t *draw_cursor_param;
	gs_eparam_t *cursor_color_param;
	gs_eparam_t *cursor_size_param;
	gs_eparam_t *cursor_image_param;
	gs_eparam_t *tool_param;
	gs_eparam_t *tool_color_param;
	gs_eparam_t *tool_size_param;
	gs_eparam_t *tool_mode_param;
	gs_eparam_t *shift_down_param;
	gs_eparam_t *select_from_param;
	gs_eparam_t *select_to_param;

	uint32_t tool;
	struct vec4 tool_color;
	float tool_size;

	struct vec4 cursor_color;
	float cursor_size;
	char *cursor_image_path;
	gs_image_file4_t *cursor_image;
	uint64_t last_tick;
};

const char *ds_get_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("Draw");
}

static void draw_effect(struct draw_source *ds, gs_texture_t *tex, bool mouse)
{
	gs_effect_set_vec2(ds->uv_size_param, &ds->size);
	gs_effect_set_vec2(ds->uv_mouse_param, &ds->mouse_pos);
	gs_effect_set_vec2(ds->uv_mouse_previous_param, &ds->mouse_previous_pos);
	gs_effect_set_vec2(ds->select_from_param, &ds->select_from);
	gs_effect_set_vec2(ds->select_to_param, &ds->select_to);
	gs_effect_set_int(ds->draw_cursor_param, mouse ? (ds->cursor_image ? 2 : 1) : 0);
	gs_effect_set_vec4(ds->cursor_color_param, &ds->cursor_color);
	gs_effect_set_float(ds->cursor_size_param, ds->cursor_size);
	gs_effect_set_texture(ds->cursor_image_param, ds->cursor_image ? ds->cursor_image->image3.image2.image.texture : NULL);
	gs_effect_set_int(ds->tool_param, ds->tool);
	gs_effect_set_vec4(ds->tool_color_param, &ds->tool_color);
	gs_effect_set_float(ds->tool_size_param, ds->tool_size);
	gs_effect_set_int(ds->tool_mode_param, ds->tool_mode);
	gs_effect_set_bool(ds->shift_down_param, ds->shift_down);
	gs_effect_set_texture(ds->image_param, tex);
	while (gs_effect_loop(ds->draw_effect, "Draw"))
		gs_draw_sprite(tex, 0, (uint32_t)ds->size.x, (uint32_t)ds->size.y);
}

static void copy_to_undo(struct draw_source *ds)
{
	obs_enter_graphics();
	while (ds->redo.size) {
		gs_texrender_t *old;
		circlebuf_pop_front(&ds->redo, &old, sizeof(old));
		gs_texrender_destroy(old);
	}
	gs_texrender_t *texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (gs_texrender_begin(texrender, (uint32_t)ds->size.x, (uint32_t)ds->size.y)) {
		gs_texture_t *tex = gs_texrender_get_texture(ds->render_a_active ? ds->render_a : ds->render_b);
		gs_blend_state_push();
		gs_reset_blend_state();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		gs_ortho(0.0f, ds->size.x, 0.0f, ds->size.y, -100.0f, 100.0f);
		if (tex)
			draw_effect(ds, tex, false);
		gs_blend_state_pop();
		gs_texrender_end(texrender);
		circlebuf_push_back(&ds->undo, &texrender, sizeof(texrender));
		if (ds->undo.size > sizeof(texrender) * ds->max_undo) {
			circlebuf_pop_front(&ds->undo, &texrender, sizeof(texrender));
			gs_texrender_destroy(texrender);
		}
	}
	obs_leave_graphics();
}

void clear(struct draw_source *ds)
{
	copy_to_undo(ds);
	obs_enter_graphics();
	gs_texrender_reset(ds->render_a_active ? ds->render_b : ds->render_a);
	if (gs_texrender_begin(ds->render_a_active ? ds->render_b : ds->render_a, (uint32_t)ds->size.x, (uint32_t)ds->size.y)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_texrender_end(ds->render_a_active ? ds->render_b : ds->render_a);
		ds->render_a_active = !ds->render_a_active;
	}
	obs_leave_graphics();
}

void clear_proc_handler(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct draw_source *context = data;
	clear(context);
}

static void apply_tool(struct draw_source *ds);

void draw_proc_handler(void *param, calldata_t *cd)
{
	struct draw_source *context = param;
	obs_data_t *data = calldata_ptr(cd, "data");

	if (obs_data_has_user_value(data, "tool"))
		context->tool = (uint32_t)obs_data_get_int(data, "tool");
	if (obs_data_has_user_value(data, "from_x"))
		context->mouse_previous_pos.x = (float)obs_data_get_double(data, "from_x");
	if (obs_data_has_user_value(data, "from_y"))
		context->mouse_previous_pos.y = (float)obs_data_get_double(data, "from_y");
	if (obs_data_has_user_value(data, "to_x"))
		context->mouse_pos.x = (float)obs_data_get_double(data, "to_x");
	if (obs_data_has_user_value(data, "to_y"))
		context->mouse_pos.y = (float)obs_data_get_double(data, "to_y");
	if (obs_data_has_user_value(data, "tool_color")) {
		vec4_from_rgba(&context->tool_color, (uint32_t)obs_data_get_int(data, "tool_color"));
		if (context->tool_color.w == 0.0f)
			context->tool_color.w = 1.0f;
	}
	if (obs_data_has_user_value(data, "tool_alpha"))
		context->tool_color.w = (float)obs_data_get_double(data, "tool_alpha") / 100.0f;
	if (obs_data_has_user_value(data, "tool_size"))
		context->tool_size = (float)obs_data_get_double(data, "tool_size");
	context->tool_mode = TOOL_DOWN;
	apply_tool(context);
	context->tool_mode = TOOL_UP;
	context->mouse_previous_pos = context->mouse_pos;
}

void undo(struct draw_source *ds)
{
	if (!ds->undo.size)
		return;

	gs_texrender_t *texrender;
	circlebuf_pop_back(&ds->undo, &texrender, sizeof(texrender));

	if (ds->render_a_active) {
		gs_texrender_t *old = ds->render_a;
		ds->render_a = texrender;
		circlebuf_push_back(&ds->redo, &old, sizeof(old));
	} else {
		gs_texrender_t *old = ds->render_b;
		ds->render_b = texrender;
		circlebuf_push_back(&ds->redo, &old, sizeof(old));
	}
}

void undo_proc_handler(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct draw_source *ds = data;
	undo(ds);
}

void redo(struct draw_source *ds)
{
	if (!ds->redo.size)
		return;

	gs_texrender_t *texrender = NULL;
	circlebuf_pop_back(&ds->redo, &texrender, sizeof(texrender));

	if (ds->render_a_active) {
		gs_texrender_t *old = ds->render_a;
		ds->render_a = texrender;
		circlebuf_push_back(&ds->undo, &old, sizeof(old));
	} else {
		gs_texrender_t *old = ds->render_b;
		ds->render_b = texrender;
		circlebuf_push_back(&ds->undo, &old, sizeof(old));
	}
}

void redo_proc_handler(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct draw_source *ds = data;
	redo(ds);
}

static void *ds_create(obs_data_t *settings, obs_source_t *source)
{
	struct draw_source *context = bzalloc(sizeof(struct draw_source));
	context->source = source;

	context->max_undo = 5;
	context->size.x = (float)obs_data_get_int(settings, "width");
	context->size.y = (float)obs_data_get_int(settings, "height");
	vec4_from_rgba_srgb(&context->cursor_color, 0xFFFFFF00);
	context->cursor_size = 10;

	context->show_mouse = true;

	char *effect_path = obs_module_file("effects/draw.effect");
	obs_enter_graphics();
	context->draw_effect = gs_effect_create_from_file(effect_path, NULL);
	if (context->draw_effect) {
		context->image_param = gs_effect_get_param_by_name(context->draw_effect, "image");
		context->uv_size_param = gs_effect_get_param_by_name(context->draw_effect, "uv_size");
		context->uv_mouse_param = gs_effect_get_param_by_name(context->draw_effect, "uv_mouse");
		context->uv_mouse_previous_param = gs_effect_get_param_by_name(context->draw_effect, "uv_mouse_previous");
		context->select_from_param = gs_effect_get_param_by_name(context->draw_effect, "select_from");
		context->select_to_param = gs_effect_get_param_by_name(context->draw_effect, "select_to");
		context->draw_cursor_param = gs_effect_get_param_by_name(context->draw_effect, "draw_cursor");
		context->cursor_color_param = gs_effect_get_param_by_name(context->draw_effect, "cursor_color");
		context->cursor_size_param = gs_effect_get_param_by_name(context->draw_effect, "cursor_size");
		context->cursor_image_param = gs_effect_get_param_by_name(context->draw_effect, "cursor_image");
		context->tool_param = gs_effect_get_param_by_name(context->draw_effect, "tool");
		context->tool_color_param = gs_effect_get_param_by_name(context->draw_effect, "tool_color");
		context->tool_size_param = gs_effect_get_param_by_name(context->draw_effect, "tool_size");
		context->tool_mode_param = gs_effect_get_param_by_name(context->draw_effect, "tool_mode");
		context->shift_down_param = gs_effect_get_param_by_name(context->draw_effect, "shift_down");
	}
	obs_leave_graphics();
	bfree(effect_path);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void clear()", clear_proc_handler, context);
	proc_handler_add(ph, "void draw(in ptr data)", draw_proc_handler, context);
	proc_handler_add(ph, "void undo()", undo_proc_handler, context);
	proc_handler_add(ph, "void redo()", redo_proc_handler, context);

	obs_source_update(source, NULL);
	return context;
}

static void ds_destroy(void *data)
{
	struct draw_source *context = data;
	bool graphics = false;
	if (context->undo.size) {
		graphics = true;
		obs_enter_graphics();
	}
	while (context->undo.size) {
		gs_texrender_t *texrender;
		circlebuf_pop_front(&context->undo, &texrender, sizeof(texrender));
		gs_texrender_destroy(texrender);
	}
	circlebuf_free(&context->undo);
	while (context->redo.size) {
		gs_texrender_t *texrender;
		circlebuf_pop_front(&context->redo, &texrender, sizeof(texrender));
		gs_texrender_destroy(texrender);
	}
	circlebuf_free(&context->redo);
	if (context->render_a) {
		if (!graphics) {
			graphics = true;
			obs_enter_graphics();
		}
		gs_texrender_destroy(context->render_a);
	}
	if (context->render_b) {
		if (!graphics) {
			graphics = true;
			obs_enter_graphics();
		}
		gs_texrender_destroy(context->render_b);
	}
	if (context->cursor_image) {
		if (!graphics) {
			graphics = true;
			obs_enter_graphics();
		}
		gs_image_file4_free(context->cursor_image);
		bfree(context->cursor_image);
	}
	if (graphics)
		obs_leave_graphics();
	if (context->cursor_image_path)
		bfree(context->cursor_image_path);
	bfree(context);
}

static uint32_t ds_get_width(void *data)
{
	struct draw_source *context = data;
	return (uint32_t)context->size.x;
}

static uint32_t ds_get_height(void *data)
{
	struct draw_source *context = data;
	return (uint32_t)context->size.y;
}

static void ds_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct draw_source *ds = data;
	if (!ds->render_a && !ds->render_b)
		return;
	if (!ds->draw_effect)
		return;

	gs_texture_t *tex = gs_texrender_get_texture(ds->render_a_active ? ds->render_a : ds->render_b);
	if (tex) {
		draw_effect(ds, tex, ds->mouse_active && ds->show_mouse);
	}
}

static void apply_tool(struct draw_source *ds)
{
	obs_enter_graphics();
	gs_texture_t *tex = gs_texrender_get_texture(ds->render_a_active ? ds->render_a : ds->render_b);
	if (tex) {
		gs_texrender_reset(ds->render_a_active ? ds->render_b : ds->render_a);
		if (gs_texrender_begin(ds->render_a_active ? ds->render_b : ds->render_a, (uint32_t)ds->size.x,
				       (uint32_t)ds->size.y)) {
			gs_blend_state_push();
			gs_reset_blend_state();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

			gs_ortho(0.0f, ds->size.x, 0.0f, ds->size.y, -100.0f, 100.0f);
			draw_effect(ds, tex, false);
			gs_blend_state_pop();
			gs_texrender_end(ds->render_a_active ? ds->render_b : ds->render_a);
		}
		ds->render_a_active = !ds->render_a_active;
	}
	obs_leave_graphics();
}

static bool draw_on_mouse_move(uint32_t tool)
{
	return tool == TOOL_PENCIL || tool == TOOL_BRUSH;
}

static void ds_mouse_move(void *data, const struct obs_mouse_event *event, bool mouse_leave)
{
	struct draw_source *ds = data;
	//if (context->pen_down && (context->mouse_x != event->x || context->mouse_y != event->y)) {
	//}
	if (draw_on_mouse_move(ds->tool)) {
		ds->mouse_previous_pos = ds->mouse_pos;
	}
	ds->mouse_pos.x = (float)event->x;
	ds->mouse_pos.y = (float)event->y;
	ds->mouse_active = !mouse_leave;
	ds->shift_down = ((event->modifiers & INTERACT_SHIFT_KEY) == INTERACT_SHIFT_KEY);

	if (ds->mouse_active && ds->tool_mode != TOOL_UP && draw_on_mouse_move(ds->tool)) {
		apply_tool(ds);
	}

	//if (mouse_leave)
	//    context->tool_down = false;
}

void ds_mouse_click(void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up, uint32_t click_count)
{
	UNUSED_PARAMETER(click_count);
	struct draw_source *context = data;

	context->mouse_pos.x = (float)event->x;
	context->mouse_pos.y = (float)event->y;
	context->shift_down = ((event->modifiers & INTERACT_SHIFT_KEY) == INTERACT_SHIFT_KEY);
	bool draw = draw_on_mouse_move(context->tool);
	if (draw) {
		context->mouse_previous_pos.x = -1.0f;
		context->mouse_previous_pos.y = -1.0f;
	}
	if (!mouse_up && draw)
		copy_to_undo(context);

	if (!mouse_up && type == 0) {
		context->tool_mode = TOOL_DOWN;
		if (context->tool == TOOL_SELECT_RECTANGLE || context->tool == TOOL_SELECT_ELLIPSE) {
			if (context->mouse_pos.x > fminf(context->select_from.x, context->select_to.x) &&
			    context->mouse_pos.x < fmaxf(context->select_from.x, context->select_to.x) &&
			    context->mouse_pos.y > fminf(context->select_from.y, context->select_to.y) &&
			    context->mouse_pos.y < fmaxf(context->select_from.y, context->select_to.y)) {
				context->tool_mode = TOOL_DRAG;
			}
		}
		if (draw)
			apply_tool(context);
	} else if (context->tool_mode == TOOL_DOWN) {
		if (!draw && type == 0) {
			if (context->tool == TOOL_SELECT_RECTANGLE || context->tool == TOOL_SELECT_ELLIPSE) {
				context->select_from = context->mouse_previous_pos;
				context->select_to = context->mouse_pos;
			} else {
				copy_to_undo(context);
				apply_tool(context);
			}
		}
		context->tool_mode = TOOL_UP;
	} else if (context->tool_mode == TOOL_DRAG) {
		copy_to_undo(context);
		apply_tool(context);
		context->select_from.x += context->mouse_pos.x - context->mouse_previous_pos.x;
		context->select_from.y += context->mouse_pos.y - context->mouse_previous_pos.y;
		context->select_to.x += context->mouse_pos.x - context->mouse_previous_pos.x;
		context->select_to.y += context->mouse_pos.y - context->mouse_previous_pos.y;
		context->tool_mode = TOOL_UP;
	}
	if (!draw) {
		context->mouse_previous_pos = context->mouse_pos;
	}
}

void ds_key_click(void *data, const struct obs_key_event *event, bool key_up)
{
	UNUSED_PARAMETER(key_up);
	struct draw_source *context = data;
	context->shift_down = ((event->modifiers & INTERACT_SHIFT_KEY) == INTERACT_SHIFT_KEY);

	if (!key_up && ((event->modifiers & INTERACT_CONTROL_KEY) == INTERACT_CONTROL_KEY)) {
		if (event->native_vkey == 'Z' || event->native_vkey == 'z') {
			undo(context);
		} else if (event->native_vkey == 'Y' || event->native_vkey == 'y') {
			redo(context);
		}
	}
}

static void ds_update(void *data, obs_data_t *settings)
{
	struct draw_source *context = data;
	context->max_undo = (uint32_t)obs_data_get_int(settings, "max_undo");
	context->size.x = (float)obs_data_get_int(settings, "width");
	context->size.y = (float)obs_data_get_int(settings, "height");
	context->tool = (uint32_t)obs_data_get_int(settings, "tool");
	context->show_mouse = obs_data_get_bool(settings, "show_cursor");
	context->cursor_size = (float)obs_data_get_double(settings, "cursor_size");
	vec4_from_rgba(&context->cursor_color, (uint32_t)obs_data_get_int(settings, "cursor_color"));
	context->cursor_color.w = 1.0f;
	vec4_from_rgba(&context->tool_color, (uint32_t)obs_data_get_int(settings, "tool_color"));
	context->tool_color.w = (float)obs_data_get_double(settings, "tool_alpha") / 100.0f;
	context->tool_size = (float)obs_data_get_double(settings, "tool_size");

	if (!context->render_a || !context->render_b) {
		obs_enter_graphics();
		context->render_a = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (gs_texrender_begin(context->render_a, (uint32_t)context->size.x, (uint32_t)context->size.y)) {
			struct vec4 clear_color;
			vec4_zero(&clear_color);
			gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
			gs_texrender_end(context->render_a);
		}
		context->render_b = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (gs_texrender_begin(context->render_b, (uint32_t)context->size.x, (uint32_t)context->size.y)) {
			struct vec4 clear_color;
			vec4_zero(&clear_color);
			gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
			gs_texrender_end(context->render_b);
		}
		obs_leave_graphics();
	} else {
		//gs_texrender_reset(context->render);
	}

	const char *cursor_image_path = obs_data_get_string(settings, "cursor_file");
	if (strlen(cursor_image_path) > 0) {
		if (!context->cursor_image_path || strcmp(cursor_image_path, context->cursor_image_path) != 0) {
			if (context->cursor_image_path)
				bfree(context->cursor_image_path);
			context->cursor_image_path = bstrdup(cursor_image_path);
			if (!context->cursor_image) {
				context->cursor_image = bzalloc(sizeof(gs_image_file4_t));
			} else {
				obs_enter_graphics();
				gs_image_file4_free(context->cursor_image);
				obs_leave_graphics();
			}
			gs_image_file4_init(context->cursor_image, cursor_image_path, GS_IMAGE_ALPHA_PREMULTIPLY_SRGB);
			// : GS_IMAGE_ALPHA_PREMULTIPLY);

			obs_enter_graphics();
			gs_image_file4_init_texture(context->cursor_image);
			obs_leave_graphics();
		}
	} else if (context->cursor_image) {
		obs_enter_graphics();
		gs_image_file4_free(context->cursor_image);
		obs_leave_graphics();
		bfree(context->cursor_image);
		context->cursor_image = NULL;
		if (context->cursor_image_path) {
			bfree(context->cursor_image_path);
			context->cursor_image_path = NULL;
		}
	}
}

static bool clear_property_button(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct draw_source *ds = data;
	clear(ds);
	return false;
}

const char *image_filter =
#ifdef _WIN32
	"All formats (*.bmp *.tga *.png *.jpeg *.jpg *.jxr *.gif *.psd *.webp);;"
#else
	"All formats (*.bmp *.tga *.png *.jpeg *.jpg *.gif *.psd *.webp);;"
#endif
	"BMP Files (*.bmp);;"
	"Targa Files (*.tga);;"
	"PNG Files (*.png);;"
	"JPEG Files (*.jpeg *.jpg);;"
#ifdef _WIN32
	"JXR Files (*.jxr);;"
#endif
	"GIF Files (*.gif);;"
	"PSD Files (*.psd);;"
	"WebP Files (*.webp);;"
	"All Files (*.*)";

static obs_properties_t *ds_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_int(props, "width", obs_module_text("Width"), 10, 10000, 1);
	obs_properties_add_int(props, "height", obs_module_text("Height"), 10, 10000, 1);
	obs_property_t *p =
		obs_properties_add_list(props, "tool", obs_module_text("Tool"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("None"), TOOL_NONE);
	obs_property_list_add_int(p, obs_module_text("Pencil"), TOOL_PENCIL);
	obs_property_list_add_int(p, obs_module_text("Brush"), TOOL_BRUSH);
	obs_property_list_add_int(p, obs_module_text("Line"), TOOL_LINE);
	obs_property_list_add_int(p, obs_module_text("RectangleOutline"), TOOL_RECTANGLE_OUTLINE);
	obs_property_list_add_int(p, obs_module_text("RectangleFill"), TOOL_RECTANGLE_FILL);
	obs_property_list_add_int(p, obs_module_text("EllipseOutline"), TOOL_ELLIPSE_OUTLINE);
	obs_property_list_add_int(p, obs_module_text("EllipseFill"), TOOL_ELLIPSE_FILL);

	obs_properties_add_color(props, "tool_color", obs_module_text("ToolColor"));
	p = obs_properties_add_float_slider(props, "tool_alpha", obs_module_text("ToolAlpha"), -100.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_float_slider(props, "tool_size", obs_module_text("ToolSize"), 0.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "px");

	obs_properties_add_color(props, "cursor_color", obs_module_text("CursorColor"));
	p = obs_properties_add_float_slider(props, "cursor_size", obs_module_text("CursorSize"), 0.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "px");
	obs_properties_add_path(props, "cursor_file", obs_module_text("CursorFile"), OBS_PATH_FILE, image_filter, NULL);

	obs_properties_add_int(props, "max_undo", obs_module_text("UndoMax"), 1, 10000, 1);

	obs_properties_add_button2(props, "clear", obs_module_text("Clear"), clear_property_button, data);

	obs_properties_add_text(props, "plugin_info",
				"<a href=\"https://obsproject.com/forum/resources/draw.2081/\">Draw</a> (" PROJECT_VERSION
				") by <a href=\"https://www.exeldro.com\">Exeldro</a>",
				OBS_TEXT_INFO);

	return props;
}

static void ds_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 200);
	obs_data_set_default_int(settings, "height", 200);
	obs_data_set_default_double(settings, "tool_size", 10.0);
	obs_data_set_default_int(settings, "cursor_color", 0xFFFFFF00);
	obs_data_set_default_int(settings, "tool_color", 0xFF0000FF);
	obs_data_set_default_double(settings, "tool_alpha", 100.0);
	obs_data_set_default_bool(settings, "show_cursor", true);
	obs_data_set_default_double(settings, "cursor_size", 10);
	obs_data_set_default_int(settings, "max_undo", 5);
}

static void ds_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct draw_source *ds = data;

	uint64_t frame_time = obs_get_video_frame_time();

	if (ds->last_tick && ds->cursor_image && ds->cursor_image->image3.image2.image.is_animated_gif) {
		uint64_t elapsed = frame_time - ds->last_tick;
		if (gs_image_file4_tick(ds->cursor_image, elapsed)) {
			obs_enter_graphics();
			gs_image_file4_update_texture(ds->cursor_image);
			obs_leave_graphics();
		}
	}
	ds->last_tick = frame_time;
}

struct obs_source_info draw_source_info = {
	.id = "draw_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB | OBS_SOURCE_INTERACTION | OBS_SOURCE_CUSTOM_DRAW |
			OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = ds_get_name,
	.create = ds_create,
	.destroy = ds_destroy,
	.get_width = ds_get_width,
	.get_height = ds_get_height,
	.icon_type = OBS_ICON_TYPE_COLOR,
	.video_render = ds_video_render,
	.mouse_move = ds_mouse_move,
	.mouse_click = ds_mouse_click,
	.key_click = ds_key_click,
	.update = ds_update,
	.get_properties = ds_get_properties,
	.get_defaults = ds_get_defaults,
	.video_tick = ds_video_tick,
};
