#include <obs-module.h>
#include "draw-source.h"

struct draw_source {
	obs_source_t *source;
	struct vec2 size;

	gs_texrender_t *render_a;
	gs_texrender_t *render_b;
	bool render_a_active;

	bool show_mouse;
	bool mouse_active;
	bool tool_down;
	bool shift_down;

	struct vec2 mouse_pos;
	struct vec2 mouse_previous_pos;

	gs_effect_t *draw_effect;
	gs_eparam_t *image_param;
	gs_eparam_t *uv_size_param;
	gs_eparam_t *uv_mouse_param;
	gs_eparam_t *uv_mouse_previous_param;
	gs_eparam_t *draw_mouse_param;
	gs_eparam_t *mouse_color_param;
	gs_eparam_t *mouse_size_param;
	gs_eparam_t *tool_param;
	gs_eparam_t *tool_color_param;
	gs_eparam_t *tool_size_param;
	gs_eparam_t *tool_down_param;
	gs_eparam_t *shift_down_param;

	uint32_t tool;
	struct vec4 tool_color;
	float tool_size;

	struct vec4 mouse_color;
	float mouse_size;
};

const char *ds_get_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("Draw");
}

void clear(struct draw_source *ds)
{
	obs_enter_graphics();
	gs_texrender_reset(ds->render_a_active ? ds->render_a : ds->render_b);
	if (gs_texrender_begin(ds->render_a_active ? ds->render_a : ds->render_b, (uint32_t)ds->size.x, (uint32_t)ds->size.y)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_texrender_end(ds->render_a_active ? ds->render_a : ds->render_b);
	}
	obs_leave_graphics();
}

void clear_proc_handler(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct draw_source *context = data;
	clear(context);
}

static void *ds_create(obs_data_t *settings, obs_source_t *source)
{
	struct draw_source *context = bzalloc(sizeof(struct draw_source));
	context->source = source;

	context->size.x = (float)obs_data_get_int(settings, "width");
	context->size.y = (float)obs_data_get_int(settings, "height");
	vec4_from_rgba_srgb(&context->mouse_color, 0xFFFFFF00);
	context->mouse_size = 10;

	context->show_mouse = true;

	char *effect_path = obs_module_file("effects/draw.effect");
	obs_enter_graphics();
	context->draw_effect = gs_effect_create_from_file(effect_path, NULL);
	if (context->draw_effect) {
		context->image_param = gs_effect_get_param_by_name(context->draw_effect, "image");
		context->uv_size_param = gs_effect_get_param_by_name(context->draw_effect, "uv_size");
		context->uv_mouse_param = gs_effect_get_param_by_name(context->draw_effect, "uv_mouse");
		context->uv_mouse_previous_param = gs_effect_get_param_by_name(context->draw_effect, "uv_mouse_previous");
		context->draw_mouse_param = gs_effect_get_param_by_name(context->draw_effect, "draw_mouse");
		context->mouse_color_param = gs_effect_get_param_by_name(context->draw_effect, "mouse_color");
		context->mouse_size_param = gs_effect_get_param_by_name(context->draw_effect, "mouse_size");
		context->tool_param = gs_effect_get_param_by_name(context->draw_effect, "tool");
		context->tool_color_param = gs_effect_get_param_by_name(context->draw_effect, "tool_color");
		context->tool_size_param = gs_effect_get_param_by_name(context->draw_effect, "tool_size");
		context->tool_down_param = gs_effect_get_param_by_name(context->draw_effect, "tool_down");
		context->shift_down_param = gs_effect_get_param_by_name(context->draw_effect, "shift_down");
	}
	obs_leave_graphics();
	bfree(effect_path);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void clear()", clear_proc_handler, context);

	obs_source_update(source, NULL);
	return context;
}

static void ds_destroy(void *data)
{
	struct draw_source *context = data;
	if (context->render_a || context->render_b) {
		obs_enter_graphics();
		gs_texrender_destroy(context->render_a);
		gs_texrender_destroy(context->render_b);
		obs_leave_graphics();
	}
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

static void draw_effect(struct draw_source *ds, gs_texture_t *tex, bool mouse)
{
	gs_effect_set_vec2(ds->uv_size_param, &ds->size);
	gs_effect_set_vec2(ds->uv_mouse_param, &ds->mouse_pos);
	gs_effect_set_vec2(ds->uv_mouse_previous_param, &ds->mouse_previous_pos);
	gs_effect_set_bool(ds->draw_mouse_param, mouse);
	gs_effect_set_vec4(ds->mouse_color_param, &ds->mouse_color);
	gs_effect_set_float(ds->mouse_size_param, ds->mouse_size);
	gs_effect_set_int(ds->tool_param, ds->tool);
	gs_effect_set_vec4(ds->tool_color_param, &ds->tool_color);
	gs_effect_set_float(ds->tool_size_param, ds->tool_size);
	gs_effect_set_bool(ds->tool_down_param, ds->tool_down);
	gs_effect_set_bool(ds->shift_down_param, ds->shift_down);
	gs_effect_set_texture(ds->image_param, tex);
	while (gs_effect_loop(ds->draw_effect, "Draw"))
		gs_draw_sprite(tex, 0, (uint32_t)ds->size.x, (uint32_t)ds->size.y);
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
			//if (ds->tool == TOOL_ERASER) {
			//	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_SRCALPHA,
			//				   GS_BLEND_INVSRCALPHA);
			//} else {
			//	//gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			//	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_SRCALPHA,
			//				   GS_BLEND_INVSRCALPHA);
			//}
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

	if (ds->mouse_active && ds->tool_down && draw_on_mouse_move(ds->tool)) {
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

	if (!mouse_up && type == 0) {
		context->tool_down = true;
		if (draw)
			apply_tool(context);
	} else if (context->tool_down) {
		if (!draw && type == 0)
			apply_tool(context);
		context->tool_down = false;
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
}

static void ds_update(void *data, obs_data_t *settings)
{
	struct draw_source *context = data;
	context->size.x = (float)obs_data_get_int(settings, "width");
	context->size.y = (float)obs_data_get_int(settings, "height");
	context->tool = (uint32_t)obs_data_get_int(settings, "tool");
	context->show_mouse = obs_data_get_bool(settings, "show_cursor");
	context->mouse_size = (float)obs_data_get_double(settings, "cursor_size");
	vec4_from_rgba(&context->mouse_color, (uint32_t)obs_data_get_int(settings, "mouse_color"));
	context->mouse_color.w = 1.0f;
	vec4_from_rgba(&context->tool_color, (uint32_t)obs_data_get_int(settings, "tool_color"));
	context->tool_color.w = (float)obs_data_get_double(settings, "tool_alpha") / 100.0f;
	context->tool_size = (float)obs_data_get_double(settings, "tool_size");

	if (!context->render_a || !context->render_b) {
		obs_enter_graphics();
		context->render_a = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (gs_texrender_begin(context->render_a, (uint32_t)context->size.x, (uint32_t)context->size.y))
			gs_texrender_end(context->render_a);
		context->render_b = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (gs_texrender_begin(context->render_b, (uint32_t)context->size.x, (uint32_t)context->size.y))
			gs_texrender_end(context->render_b);
		obs_leave_graphics();
	} else {
		//gs_texrender_reset(context->render);
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

	obs_properties_add_color(props, "mouse_color", obs_module_text("CursorColor"));
	p = obs_properties_add_float_slider(props, "cursor_size", obs_module_text("CursorSize"), 0.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "px");
	obs_properties_add_color(props, "tool_color", obs_module_text("ToolColor"));
	p = obs_properties_add_float_slider(props, "tool_alpha", obs_module_text("ToolAlpha"), -100.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_float_slider(props, "tool_size", obs_module_text("ToolSize"), 0.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "px");

	obs_properties_add_button2(props, "clear", obs_module_text("Clear"), clear_property_button, data);

	return props;
}

static void ds_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 200);
	obs_data_set_default_int(settings, "height", 200);
	obs_data_set_default_double(settings, "tool_size", 10.0);
	obs_data_set_default_int(settings, "mouse_color", 0xFFFFFF00);
	obs_data_set_default_int(settings, "tool_color", 0xFF0000FF);
	obs_data_set_default_double(settings, "tool_alpha", 100.0);
	obs_data_set_default_bool(settings, "show_cursor", true);
	obs_data_set_default_double(settings, "cursor_size", 10);
}

struct obs_source_info draw_source_info = {
	.id = "draw_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB | OBS_SOURCE_INTERACTION | OBS_SOURCE_CUSTOM_DRAW,
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
};
