#include "draw-dock.hpp"
#include "draw-source.h"
#include "version.h"
#include <graphics/matrix4.h>
#include <obs-module.h>
#include <QColorDialog>
#include <QFileDialog>
#include <QGuiApplication>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <util/platform.h>

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Exeldro");
OBS_MODULE_USE_DEFAULT_LOCALE("draw-dock", "en-US")

static DrawDock *draw_dock = nullptr;

MODULE_EXTERN struct obs_source_info draw_source_info;

bool obs_module_load()
{
	blog(LOG_INFO, "[Draw Dock] loaded version %s", PROJECT_VERSION);
	obs_register_source(&draw_source_info);
	const auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	obs_frontend_push_ui_translation(obs_module_get_string);
	draw_dock = new DrawDock(main_window);
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 0, 0)
	obs_frontend_add_dock_by_id("DrawDock", obs_module_text("DrawDock"), draw_dock);
#else
	const auto dock = new QDockWidget(main_window);
	dock->setObjectName("DrawDock");
	dock->setWindowTitle(QString::fromUtf8(obs_module_text("DrawDock")));
	dock->setWidget(draw_dock);
	dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
	dock->setFloating(true);
	dock->hide();
	obs_frontend_add_dock(dock);
#endif

	obs_frontend_pop_ui_translation();

	return true;
}

void obs_module_post_load(void)
{
	//draw_dock->RegisterObsWebsocket();
}

void obs_module_unload() {}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("DrawDock");
}

static inline QColor color_from_int(long long val)
{
	return QColor(val & 0xff, (val >> 8) & 0xff, (val >> 16) & 0xff, (val >> 24) & 0xff);
}

static inline long long color_to_int(QColor color)
{
	auto shift = [&](unsigned val, int shift) {
		return ((val & 0xff) << shift);
	};

	return shift(color.red(), 0) | shift(color.green(), 8) | shift(color.blue(), 16) | shift(color.alpha(), 24);
}

DrawDock::DrawDock(QWidget *parent) : QWidget(parent), eventFilter(BuildEventFilter()), preview(new OBSQTDisplay(this))
{
	auto ml = new QVBoxLayout(this);
	ml->setContentsMargins(0, 0, 0, 0);
	setLayout(ml);

	obs_enter_graphics();

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(1.0f, 1.0f);
	box = gs_render_save();

	obs_leave_graphics();

	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_connect(sh, "source_create", source_create, this);

	toolbar = new QToolBar();
	ml->addWidget(toolbar);

		auto a = toolbar->addAction(QString::fromUtf8(obs_module_text("Config")), [this] {
		if (!draw_source)
			return;
		QMenu menu;

		auto cursorMenu = menu.addMenu(obs_module_text("Cursor"));

		auto a = cursorMenu->addAction(QString::fromUtf8(obs_module_text("Show")));
		a->setCheckable(true);
		obs_data_t *settings = obs_source_get_settings(draw_source);
		a->setChecked(obs_data_get_bool(settings, "show_cursor"));

		connect(a, &QAction::triggered, [this, a] {
			if (!draw_source)
				return;
			obs_data_t *settings = obs_data_create();
			obs_data_set_bool(settings, "show_cursor", a->isChecked());
			obs_source_update(draw_source, settings);
			obs_data_release(settings);
		});
		cursorMenu->addAction(QString::fromUtf8(obs_module_text("Color")), [this] {
			if (!draw_source)
				return;
			obs_data_t *settings = obs_source_get_settings(draw_source);
			QColor color = color_from_int(obs_data_get_int(settings, "cursor_color"));
			obs_data_release(settings);
			const auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			color = QColorDialog::getColor(color, main_window, QString::fromUtf8(obs_module_text("CursorColor")));
			if (!color.isValid())
				return;
			if (!draw_source)
				return;
			settings = obs_data_create();
			obs_data_set_int(settings, "cursor_color", color_to_int(color));
			obs_data_set_string(settings, "cursor_file", "");
			obs_source_update(draw_source, settings);
			obs_data_release(settings);
		});
		cursorMenu->addAction(QString::fromUtf8(obs_module_text("CursorImage")), [this] {
			if (!draw_source)
				return;
			obs_data_t *settings = obs_source_get_settings(draw_source);
			const char *path = obs_data_get_string(settings, "cursor_file");
			obs_data_release(settings);
			const auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			QString fileName = QFileDialog::getOpenFileName(main_window,
									QString::fromUtf8(obs_module_text("CursorImage")),
									QString::fromUtf8(path), image_filter);
			if (fileName.isEmpty())
				return;
			if (!draw_source)
				return;
			settings = obs_data_create();
			obs_data_set_string(settings, "cursor_file", fileName.toUtf8().constData());
			obs_source_update(draw_source, settings);
			obs_data_release(settings);
		});
		auto wa = new QWidgetAction(cursorMenu);
		auto cursorSize = new QDoubleSpinBox();
		cursorSize->setSuffix("px");
		cursorSize->setValue(obs_data_get_double(settings, "cursor_size"));
		cursorSize->setRange(0.0, 1000.0);
		wa->setDefaultWidget(cursorSize);
		cursorMenu->addAction(wa);

		connect(cursorSize, &QDoubleSpinBox::valueChanged, [this, cursorSize] {
			if (!draw_source)
				return;
			obs_data_t *settings = obs_data_create();
			obs_data_set_double(settings, "cursor_size", cursorSize->value());
			obs_source_update(draw_source, settings);
			obs_data_release(settings);
		});

		obs_data_release(settings);
		menu.exec(QCursor::pos());
	});
	toolbar->widgetForAction(a)->setProperty("themeID", "propertiesIconSmall");
	toolbar->widgetForAction(a)->setProperty("class", "icon-gear");

	toolCombo = new QComboBox;
	toolCombo->addItem(obs_module_text("None"), QVariant(TOOL_NONE));
	toolCombo->addItem(obs_module_text("Pencil"), QVariant(TOOL_PENCIL));
	toolCombo->addItem(obs_module_text("Brush"), QVariant(TOOL_BRUSH));
	toolCombo->addItem(obs_module_text("Line"), QVariant(TOOL_LINE));
	toolCombo->addItem(obs_module_text("RectangleOutline"), QVariant(TOOL_RECTANGLE_OUTLINE));
	toolCombo->addItem(obs_module_text("RectangleFill"), QVariant(TOOL_RECTANGLE_FILL));
	toolCombo->addItem(obs_module_text("EllipseOutline"), QVariant(TOOL_ELLIPSE_OUTLINE));
	toolCombo->addItem(obs_module_text("EllipseFill"), QVariant(TOOL_ELLIPSE_FILL));
	connect(toolCombo, &QComboBox::currentIndexChanged, [this] {
		if (!draw_source)
			return;
		int tool = toolCombo->currentData().toInt();
		obs_data_t *settings = obs_source_get_settings(draw_source);
		if (obs_data_get_int(settings, "tool") != tool) {
			obs_data_set_int(settings, "tool", tool);
			obs_source_update(draw_source, settings);
		}
		obs_data_release(settings);
		obs_source_t *scene_source = obs_frontend_get_current_scene();
		if (!scene_source)
			return;
		obs_scene_t *scene = obs_scene_from_source(scene_source);
		obs_source_release(scene_source);
		if (!scene)
			return;

		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *data) {
				auto source = obs_sceneitem_get_source(item);
				if (!source || strcmp(obs_source_get_unversioned_id(source), "draw_source") != 0)
					return true;
				int tool = *((int *)data);
				obs_data_t *ss = obs_source_get_settings(source);
				if (obs_data_get_int(ss, "tool") != tool) {
					obs_data_set_int(ss, "tool", tool);
					obs_source_update(source, ss);
				}
				obs_data_release(ss);
				return true;
			},
			&tool);
	});
	toolbar->addWidget(toolCombo);
	colorAction = toolbar->addAction(QString::fromUtf8(obs_module_text("ToolColor")), [this] {
		if (!draw_source)
			return;
		obs_data_t *settings = obs_source_get_settings(draw_source);
		QColor color = color_from_int(obs_data_get_int(settings, "tool_color"));
		obs_data_release(settings);
		const auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		color = QColorDialog::getColor(color, main_window, QString::fromUtf8(obs_module_text("ToolColor")));
		if (!color.isValid())
			return;
		if (!draw_source)
			return;
		long long longColor = color_to_int(color);
		settings = obs_source_get_settings(draw_source);
		if (obs_data_get_int(settings, "tool_color") != longColor) {
			obs_data_set_int(settings, "tool_color", longColor);
			obs_source_update(draw_source, settings);
		}
		obs_data_release(settings);
		obs_source_t *scene_source = obs_frontend_get_current_scene();
		if (!scene_source)
			return;
		obs_scene_t *scene = obs_scene_from_source(scene_source);
		obs_source_release(scene_source);
		if (!scene)
			return;

		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *data) {
				auto source = obs_sceneitem_get_source(item);
				if (!source || strcmp(obs_source_get_unversioned_id(source), "draw_source") != 0)
					return true;
				long long longColor = *((long long *)data);
				obs_data_t *ss = obs_source_get_settings(source);
				if (obs_data_get_int(ss, "tool_color") != longColor) {
					obs_data_set_int(ss, "tool_color", longColor);
					obs_source_update(source, ss);
				}
				obs_data_release(ss);
				return true;
			},
			&longColor);
	});
	toolSizeSpin = new QDoubleSpinBox;
	toolSizeSpin->setRange(0.0, 1000.0);
	toolSizeSpin->setSuffix("px");
	connect(toolSizeSpin, &QDoubleSpinBox::valueChanged, [this] {
		double size = toolSizeSpin->value();
		if (draw_source) {

			obs_data_t *settings = obs_source_get_settings(draw_source);
			if (abs(obs_data_get_double(settings, "tool_size") - size) > 0.1) {
				obs_data_set_double(settings, "tool_size", size);
				obs_source_update(draw_source, settings);
			}
			obs_data_release(settings);
		}

		obs_source_t *scene_source = obs_frontend_get_current_scene();
		if (!scene_source)
			return;
		obs_scene_t *scene = obs_scene_from_source(scene_source);
		obs_source_release(scene_source);
		if (!scene)
			return;

		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *data) {
				auto source = obs_sceneitem_get_source(item);
				if (!source || strcmp(obs_source_get_unversioned_id(source), "draw_source") != 0)
					return true;
				double size = *((double *)data);
				obs_data_t *ss = obs_source_get_settings(source);
				if (abs(obs_data_get_double(ss, "tool_size") - size) > 0.1) {
					obs_data_set_double(ss, "tool_size", size);
					obs_source_update(source, ss);
				}
				obs_data_release(ss);
				return true;
			},
			&size);
	});

	toolbar->addWidget(toolSizeSpin);

	alphaSpin = new QDoubleSpinBox;
	alphaSpin->setRange(0.0, 100.0);
	alphaSpin->setSuffix("%");
	toolbar->addWidget(alphaSpin);

	eraseCheckbox = new QCheckBox(QString::fromUtf8(obs_module_text("Erase")));
	toolbar->addWidget(eraseCheckbox);

	auto alphaChange = [this] {
		if (!draw_source)
			return;

		double alpha = eraseCheckbox->isChecked() ? -100.0 : alphaSpin->value();
		obs_data_t *settings = obs_source_get_settings(draw_source);
		if (abs(obs_data_get_double(settings, "tool_alpha") - alpha) > 0.1) {
			obs_data_set_double(settings, "tool_alpha", alpha);
			obs_source_update(draw_source, settings);
		}
		obs_data_release(settings);

		obs_source_t *scene_source = obs_frontend_get_current_scene();
		if (!scene_source)
			return;
		obs_scene_t *scene = obs_scene_from_source(scene_source);
		obs_source_release(scene_source);
		if (!scene)
			return;

		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *data) {
				auto source = obs_sceneitem_get_source(item);
				if (!source || strcmp(obs_source_get_unversioned_id(source), "draw_source") != 0)
					return true;
				double alpha = *((double *)data);
				obs_data_t *ss = obs_source_get_settings(source);
				if (abs(obs_data_get_double(ss, "tool_alpha") - alpha) > 0.1) {
					obs_data_set_double(ss, "tool_alpha", alpha);
					obs_source_update(source, ss);
				}
				obs_data_release(ss);
				return true;
			},
			&alpha);
	};

	connect(alphaSpin, &QDoubleSpinBox::valueChanged, alphaChange);
	connect(eraseCheckbox, &QCheckBox::stateChanged, alphaChange);

	toolbar->addSeparator();
	toolbar->addAction(QString::fromUtf8(obs_module_text("Clear")), [this] {
		if (draw_source) {
			proc_handler_t *ph = obs_source_get_proc_handler(draw_source);
			if (!ph)
				return;
			calldata_t d = {};
			proc_handler_call(ph, "clear", &d);
		}
		obs_source_t *scene_source = obs_frontend_get_current_scene();
		if (!scene_source)
			return;
		obs_scene_t *scene = obs_scene_from_source(scene_source);
		obs_source_release(scene_source);
		if (!scene)
			return;

		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *) {
				auto source = obs_sceneitem_get_source(item);
				if (!source || strcmp(obs_source_get_unversioned_id(source), "draw_source") != 0)
					return true;
				proc_handler_t *ph = obs_source_get_proc_handler(source);
				if (!ph)
					return true;
				calldata_t cd = {};
				proc_handler_call(ph, "clear", &cd);
				return true;
			},
			nullptr);
	});

	preview->setObjectName(QStringLiteral("preview"));
	preview->setMinimumSize(QSize(24, 24));
	QSizePolicy sizePolicy1(QSizePolicy::Expanding, QSizePolicy::Expanding);
	sizePolicy1.setHorizontalStretch(0);
	sizePolicy1.setVerticalStretch(0);
	sizePolicy1.setHeightForWidth(preview->sizePolicy().hasHeightForWidth());
	preview->setSizePolicy(sizePolicy1);

	preview->setMouseTracking(true);
	preview->setFocusPolicy(Qt::StrongFocus);
	preview->installEventFilter(eventFilter);

	preview->show();
	connect(preview, &OBSQTDisplay::DisplayCreated,
		[this]() { obs_display_add_draw_callback(preview->GetDisplay(), DrawPreview, this); });

	ml->addWidget(preview);

	obs_frontend_add_event_callback(frontend_event, this);
}

DrawDock::~DrawDock()
{
	DestroyDrawSource();
	delete eventFilter;
	obs_enter_graphics();
	gs_vertexbuffer_destroy(box);
	obs_leave_graphics();
}

static inline void GetScaleAndCenterPos(int baseCX, int baseCY, int windowCX, int windowCY, int &x, int &y, float &scale)
{
	double windowAspect, baseAspect;
	int newCX, newCY;

	windowAspect = double(windowCX) / double(windowCY);
	baseAspect = double(baseCX) / double(baseCY);

	if (windowAspect > baseAspect) {
		scale = float(windowCY) / float(baseCY);
		newCX = int(double(windowCY) * baseAspect);
		newCY = windowCY;
	} else {
		scale = float(windowCX) / float(baseCX);
		newCX = windowCX;
		newCY = int(float(windowCX) / baseAspect);
	}

	x = windowCX / 2 - newCX / 2;
	y = windowCY / 2 - newCY / 2;
}

void DrawDock::DrawPreview(void *data, uint32_t cx, uint32_t cy)
{
	DrawDock *window = static_cast<DrawDock *>(data);
	if (!window)
		return;

	gs_viewport_push();
	gs_projection_push();

	gs_texture_t *tex = obs_get_main_texture();

	uint32_t sourceCX = gs_texture_get_width(tex);
	if (sourceCX <= 0)
		sourceCX = 1;
	uint32_t sourceCY = gs_texture_get_height(tex);
	if (sourceCY <= 0)
		sourceCY = 1;

	int x, y;
	float scale;

	GetScaleAndCenterPos(sourceCX, sourceCY, cx, cy, x, y, scale);

	auto newCX = scale * float(sourceCX);
	auto newCY = scale * float(sourceCY);

	auto extraCx = (window->zoom - 1.0f) * newCX;
	auto extraCy = (window->zoom - 1.0f) * newCY;
	int newCx = newCX * window->zoom;
	int newCy = newCY * window->zoom;
	x -= extraCx * window->scrollX;
	y -= extraCy * window->scrollY;
	gs_viewport_push();
	gs_projection_push();

	gs_ortho(0.0f, newCx, 0.0f, newCy, -100.0f, 100.0f);
	gs_set_viewport(x, y, newCx, newCy);
	window->DrawBackdrop(newCx, newCy);

	const bool previous = gs_set_linear_srgb(true);

	gs_ortho(0.0f, float(sourceCX), 0.0f, float(sourceCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, newCx, newCy);
	obs_render_main_texture();

	gs_set_linear_srgb(previous);
	gs_projection_pop();
	gs_viewport_pop();
}

bool DrawDock::GetSourceRelativeXY(int mouseX, int mouseY, int &relX, int &relY)
{
	float pixelRatio = devicePixelRatioF();

	int mouseXscaled = (int)roundf(mouseX * pixelRatio);
	int mouseYscaled = (int)roundf(mouseY * pixelRatio);

	QSize size = preview->size() * preview->devicePixelRatioF();

	uint32_t sourceCX = draw_source ? obs_source_get_width(draw_source) : 1;
	if (sourceCX <= 0)
		sourceCX = 1;
	uint32_t sourceCY = draw_source ? obs_source_get_height(draw_source) : 1;
	if (sourceCY <= 0)
		sourceCY = 1;

	int x, y;
	float scale;

	GetScaleAndCenterPos(sourceCX, sourceCY, size.width(), size.height(), x, y, scale);

	auto newCX = scale * float(sourceCX);
	auto newCY = scale * float(sourceCY);

	auto extraCx = (zoom - 1.0f) * newCX;
	auto extraCy = (zoom - 1.0f) * newCY;

	scale *= zoom;

	if (x > 0) {
		relX = int(float(mouseXscaled - x + extraCx * scrollX) / scale);
		relY = int(float(mouseYscaled + extraCy * scrollY) / scale);
	} else {
		relX = int(float(mouseXscaled + extraCx * scrollX) / scale);
		relY = int(float(mouseYscaled - y + extraCy * scrollY) / scale);
	}

	// Confirm mouse is inside the source
	if (relX < 0 || relX > int(sourceCX))
		return false;
	if (relY < 0 || relY > int(sourceCY))
		return false;

	return true;
}
static int TranslateQtKeyboardEventModifiers(QInputEvent *event, bool mouseEvent)
{
	int obsModifiers = INTERACT_NONE;

	if (event->modifiers().testFlag(Qt::ShiftModifier))
		obsModifiers |= INTERACT_SHIFT_KEY;
	if (event->modifiers().testFlag(Qt::AltModifier))
		obsModifiers |= INTERACT_ALT_KEY;
#ifdef __APPLE__
	// Mac: Meta = Control, Control = Command
	if (event->modifiers().testFlag(Qt::ControlModifier))
		obsModifiers |= INTERACT_COMMAND_KEY;
	if (event->modifiers().testFlag(Qt::MetaModifier))
		obsModifiers |= INTERACT_CONTROL_KEY;
#else
	// Handle windows key? Can a browser even trap that key?
	if (event->modifiers().testFlag(Qt::ControlModifier))
		obsModifiers |= INTERACT_CONTROL_KEY;
#endif

	if (!mouseEvent) {
		if (event->modifiers().testFlag(Qt::KeypadModifier))
			obsModifiers |= INTERACT_IS_KEY_PAD;
	}

	return obsModifiers;
}

static int TranslateQtMouseEventModifiers(QMouseEvent *event)
{
	int modifiers = TranslateQtKeyboardEventModifiers(event, true);

	if (event->buttons().testFlag(Qt::LeftButton))
		modifiers |= INTERACT_MOUSE_LEFT;
	if (event->buttons().testFlag(Qt::MiddleButton))
		modifiers |= INTERACT_MOUSE_MIDDLE;
	if (event->buttons().testFlag(Qt::RightButton))
		modifiers |= INTERACT_MOUSE_RIGHT;

	return modifiers;
}

static bool CloseFloat(float a, float b, float epsilon = 0.01)
{
	using std::abs;
	return abs(a - b) <= epsilon;
}

struct click_event {
	int32_t x;
	int32_t y;
	uint32_t modifiers;
	int32_t button;
	bool mouseUp;
	uint32_t clickCount;
	obs_source_t *mouseTarget;
	obs_mouse_event mouseEvent;
};

static bool HandleSceneMouseClickEvent(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	UNUSED_PARAMETER(scene);
	if (!obs_sceneitem_visible(item))
		return true;
	auto source = obs_sceneitem_get_source(item);
	if (!source || strcmp(obs_source_get_unversioned_id(source), "draw_source") != 0)
		return true;

	auto click_event = static_cast<struct click_event *>(data);

	matrix4 transform;
	matrix4 invTransform;
	vec3 transformedPos;
	vec3 pos3;
	vec3 pos3_;

	vec3_set(&pos3, click_event->x, click_event->y, 0.0f);

	obs_sceneitem_get_box_transform(item, &transform);

	matrix4_inv(&invTransform, &transform);
	vec3_transform(&transformedPos, &pos3, &invTransform);
	vec3_transform(&pos3_, &transformedPos, &transform);

	if (CloseFloat(pos3.x, pos3_.x) && CloseFloat(pos3.y, pos3_.y) && transformedPos.x >= 0.0f && transformedPos.x <= 1.0f &&
	    transformedPos.y >= 0.0f && transformedPos.y <= 1.0f) {
		click_event->mouseEvent.x = transformedPos.x * obs_source_get_base_width(source);
		click_event->mouseEvent.y = transformedPos.y * obs_source_get_base_height(source);
		click_event->mouseEvent.modifiers = click_event->modifiers;
		click_event->mouseTarget = source;
		return false;
	}

	return true;
}

bool DrawDock::HandleMouseClickEvent(QMouseEvent *event)
{
	const bool mouseUp = event->type() == QEvent::MouseButtonRelease;
	if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ControlModifier)) {
		if (!mouseUp) {
			scrollingFromX = event->pos().x();
			scrollingFromY = event->pos().y();
		}
		return true;
	}
	uint32_t clickCount = 1;
	if (event->type() == QEvent::MouseButtonDblClick)
		clickCount = 2;

	struct obs_mouse_event mouseEvent = {};

	mouseEvent.modifiers = TranslateQtMouseEventModifiers(event);

	int32_t button = 0;

	switch (event->button()) {
	case Qt::LeftButton:
		button = MOUSE_LEFT;
		break;
	case Qt::MiddleButton:
		button = MOUSE_MIDDLE;
		break;
	case Qt::RightButton:
		button = MOUSE_RIGHT;
		break;
	default:
		blog(LOG_WARNING, "unknown button type %d", event->button());
		return false;
	}

	const bool insideSource = GetSourceRelativeXY(event->pos().x(), event->pos().y(), mouseEvent.x, mouseEvent.y);
	if (!mouseUp && !insideSource)
		return false;

	click_event ce{mouseEvent.x, mouseEvent.y, mouseEvent.modifiers, button, mouseUp, clickCount, nullptr};

	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (scene_source) {
		if (obs_scene_t *scene = obs_scene_from_source(scene_source)) {
			obs_scene_enum_items(scene, HandleSceneMouseClickEvent, &ce);
		}
		obs_source_release(scene_source);
	}
	if (ce.mouseTarget) {
		obs_source_send_mouse_click(ce.mouseTarget, &ce.mouseEvent, button, mouseUp, clickCount);
		if (mouseUp) {
			if (mouse_down_target) {
				if (mouse_down_target == draw_source) {
					obs_source_send_mouse_click(draw_source, &mouseEvent, button, mouseUp, clickCount);
				} else if (mouse_down_target != ce.mouseTarget) {
					obs_source_send_mouse_click(mouse_down_target, &mouseEvent, button, mouseUp, clickCount);
				}
				mouse_down_target = nullptr;
			}
		} else {
			mouse_down_target = ce.mouseTarget;
		}
	} else if (draw_source) {
		obs_source_send_mouse_click(draw_source, &mouseEvent, button, mouseUp, clickCount);
		if (mouseUp) {
			if (mouse_down_target && mouse_down_target != draw_source) {
				obs_source_send_mouse_click(mouse_down_target, &mouseEvent, button, mouseUp, clickCount);
			}
			mouse_down_target = nullptr;
		} else {
			mouse_down_target = draw_source;
		}
	} else if (mouseUp && mouse_down_target) {
		obs_source_send_mouse_click(mouse_down_target, &mouseEvent, button, mouseUp, clickCount);
		mouse_down_target = nullptr;
	} else {
		mouse_down_target = nullptr;
	}

	return true;
}

struct move_event {
	int32_t x;
	int32_t y;
	uint32_t modifiers;
	bool mouseLeave;
	obs_source_t *mouseTarget;
	obs_mouse_event mouseEvent;
};

static bool HandleSceneMouseMoveEvent(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	UNUSED_PARAMETER(scene);
	if (!obs_sceneitem_visible(item))
		return true;
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source || strcmp(obs_source_get_unversioned_id(source), "draw_source") != 0)
		return true;

	auto move_event = static_cast<struct move_event *>(data);

	matrix4 transform{};
	matrix4 invTransform{};
	vec3 transformedPos{};
	vec3 pos3{};
	vec3 pos3_{};

	vec3_set(&pos3, move_event->x, move_event->y, 0.0f);

	obs_sceneitem_get_box_transform(item, &transform);

	matrix4_inv(&invTransform, &transform);
	vec3_transform(&transformedPos, &pos3, &invTransform);
	vec3_transform(&pos3_, &transformedPos, &transform);

	if (CloseFloat(pos3.x, pos3_.x) && CloseFloat(pos3.y, pos3_.y) && transformedPos.x >= 0.0f && transformedPos.x <= 1.0f &&
	    transformedPos.y >= 0.0f && transformedPos.y <= 1.0f) {
		move_event->mouseEvent.x = transformedPos.x * obs_source_get_base_width(source);
		move_event->mouseEvent.y = transformedPos.y * obs_source_get_base_height(source);
		move_event->mouseEvent.modifiers = move_event->modifiers;
		move_event->mouseTarget = source;
		return false;
	}

	obs_mouse_event mouseEvent;
	mouseEvent.x = transformedPos.x * obs_source_get_base_width(source);
	mouseEvent.y = transformedPos.y * obs_source_get_base_height(source);
	mouseEvent.modifiers = move_event->modifiers;
	obs_source_send_mouse_move(source, &mouseEvent, true);

	return true;
}

bool DrawDock::HandleMouseMoveEvent(QMouseEvent *event)
{
	if (!event)
		return false;
	if (event->buttons() == Qt::LeftButton && event->modifiers().testFlag(Qt::ControlModifier)) {

		QSize size = preview->size() * preview->devicePixelRatioF();
		scrollX -= float(event->pos().x() - scrollingFromX) / size.width();
		scrollY -= float(event->pos().y() - scrollingFromY) / size.height();
		if (scrollX < 0.0f)
			scrollX = 0.0;
		if (scrollX > 1.0f)
			scrollX = 1.0f;
		if (scrollY < 0.0f)
			scrollY = 0.0;
		if (scrollY > 1.0f)
			scrollY = 1.0f;
		scrollingFromX = event->pos().x();
		scrollingFromY = event->pos().y();
	}

	struct obs_mouse_event mouseEvent = {};

	bool mouseLeave = event->type() == QEvent::Leave;

	if (!mouseLeave) {
		mouseEvent.modifiers = TranslateQtMouseEventModifiers(event);
		mouseLeave = !GetSourceRelativeXY(event->pos().x(), event->pos().y(), mouseEvent.x, mouseEvent.y);
	}

	move_event ce{mouseEvent.x, mouseEvent.y, mouseEvent.modifiers, mouseLeave, nullptr};

	if (!mouseLeave) {
		obs_source_t *scene_source = obs_frontend_get_current_scene();
		if (scene_source) {
			if (obs_scene_t *scene = obs_scene_from_source(scene_source)) {
				obs_scene_enum_items(scene, HandleSceneMouseMoveEvent, &ce);
			}
			obs_source_release(scene_source);
		}
		if (ce.mouseTarget)
			obs_source_send_mouse_move(ce.mouseTarget, &ce.mouseEvent, false);
	}

	if (draw_source)
		obs_source_send_mouse_move(draw_source, &mouseEvent,
					   mouseLeave || (ce.mouseTarget && mouse_down_target != draw_source));

	return true;
}

bool DrawDock::HandleMouseWheelEvent(QWheelEvent *event)
{
	struct obs_mouse_event mouseEvent = {};

	mouseEvent.modifiers = TranslateQtKeyboardEventModifiers(event, true);

	int xDelta = 0;
	int yDelta = 0;

	const QPoint angleDelta = event->angleDelta();
	if (!event->pixelDelta().isNull()) {
		if (angleDelta.x())
			xDelta = event->pixelDelta().x();
		else
			yDelta = event->pixelDelta().y();
	} else {
		if (angleDelta.x())
			xDelta = angleDelta.x();
		else
			yDelta = angleDelta.y();
	}

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
	const QPointF position = event->position();
	const int x = position.x();
	const int y = position.y();
#else
	const int x = event->pos().x();
	const int y = event->pos().y();
#endif

	const bool insideSource = GetSourceRelativeXY(x, y, mouseEvent.x, mouseEvent.y);
	if ((QGuiApplication::keyboardModifiers() & Qt::ControlModifier) && yDelta != 0) {
		const auto factor = 1.0f + (0.0008f * yDelta);

		zoom *= factor;
		if (zoom < 1.0f)
			zoom = 1.0f;
		if (zoom > 100.0f)
			zoom = 100.0f;

	} else if (insideSource && draw_source) {
		obs_source_send_mouse_wheel(draw_source, &mouseEvent, xDelta, yDelta);
	}

	return true;
}

bool DrawDock::HandleFocusEvent(QFocusEvent *event)
{
	bool focus = event->type() == QEvent::FocusIn;

	if (draw_source)
		obs_source_send_focus(draw_source, focus);

	return true;
}

bool DrawDock::HandleKeyEvent(QKeyEvent *event)
{
	struct obs_key_event keyEvent;

	QByteArray text = event->text().toUtf8();
	keyEvent.modifiers = TranslateQtKeyboardEventModifiers(event, false);
	keyEvent.text = text.data();
	keyEvent.native_modifiers = event->nativeModifiers();
	keyEvent.native_scancode = event->nativeScanCode();
	keyEvent.native_vkey = event->nativeVirtualKey();

	bool keyUp = event->type() == QEvent::KeyRelease;

	if (event->key() == Qt::Key_Shift) {
		if (!keyUp) {
			keyEvent.modifiers |= INTERACT_SHIFT_KEY;
		} else if ((keyEvent.modifiers & INTERACT_SHIFT_KEY) == INTERACT_SHIFT_KEY) {
			keyEvent.modifiers -= INTERACT_SHIFT_KEY;
		}
	}

	if (draw_source)
		obs_source_send_key_click(draw_source, &keyEvent, keyUp);

	return true;
}

OBSEventFilter *DrawDock::BuildEventFilter()
{
	return new OBSEventFilter([this](QObject *obj, QEvent *event) {
		UNUSED_PARAMETER(obj);

		switch (event->type()) {
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonRelease:
		case QEvent::MouseButtonDblClick:
			return this->HandleMouseClickEvent(static_cast<QMouseEvent *>(event));
		case QEvent::MouseMove:
		case QEvent::Enter:
		case QEvent::Leave:
			return this->HandleMouseMoveEvent(static_cast<QMouseEvent *>(event));

		case QEvent::Wheel:
			return this->HandleMouseWheelEvent(static_cast<QWheelEvent *>(event));
		case QEvent::FocusIn:
		case QEvent::FocusOut:
			return this->HandleFocusEvent(static_cast<QFocusEvent *>(event));
		case QEvent::KeyPress:
		case QEvent::KeyRelease:
			return this->HandleKeyEvent(static_cast<QKeyEvent *>(event));
		default:
			return false;
		}
	});
}

void DrawDock::frontend_event(enum obs_frontend_event event, void *data)
{
	DrawDock *window = static_cast<DrawDock *>(data);
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
		window->CreateDrawSource();
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP || event == OBS_FRONTEND_EVENT_EXIT ||
		   event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING) {
		window->DestroyDrawSource();
	} else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED || event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED ||
		   event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED || event == OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED) {
		QMetaObject::invokeMethod(window, "SceneChanged", Qt::QueuedConnection);
	}
}

void DrawDock::CreateDrawSource(obs_source_t *new_source)
{
	bool set_output = true;
	for (uint32_t i = MAX_CHANNELS - 1; i > 0; i--) {
		obs_source_t *source = obs_get_output_source(i);
		if (!source)
			continue;
		if (strcmp(obs_source_get_unversioned_id(source), "draw_source") == 0) {
			obs_source_release(draw_source);
			if (draw_source != source) {
				draw_source = source;
			}
			set_output = false;
			break;
		}
		obs_source_release(source);
	}
	if (draw_source) {
		signal_handler_t *sh = obs_source_get_signal_handler(draw_source);
		signal_handler_disconnect(sh, "update", draw_source_update, this);
		signal_handler_disconnect(sh, "destroy", draw_source_destroy, this);
	} else if (new_source) {
		draw_source = obs_source_get_ref(new_source);
	} else {
		draw_source = obs_get_source_by_name("Global Draw Source");
	}

	if (draw_source && strcmp(obs_source_get_unversioned_id(draw_source), "draw_source") != 0) {
		obs_source_release(draw_source);
		draw_source = nullptr;
		return;
	}

	const auto path = obs_module_config_path("config.json");
	obs_data_t *config = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);

	obs_source_t *scene = obs_frontend_get_current_scene();
	obs_data_t *settings = config ? obs_data_get_obj(config, "global_draw_source") : nullptr;
	if (settings && obs_data_has_user_value(settings, "settings")) {
		if (!draw_source)
			draw_source = obs_load_source(settings);
		if (draw_source) {
			obs_data_release(settings);
			settings = obs_source_get_settings(draw_source);
		}
	}
	obs_data_release(config);
	if (!settings) {
		settings = obs_data_create();
		obs_data_set_int(settings, "tool", 1);
		obs_data_set_double(settings, "tool_alpha", 50.0);
		if (!scene) {
			obs_data_set_int(settings, "width", 1920);
			obs_data_set_int(settings, "height", 1080);
		}
	}
	if (scene) {
		obs_data_set_int(settings, "width", obs_source_get_base_width(scene));
		obs_data_set_int(settings, "height", obs_source_get_base_height(scene));
		obs_source_release(scene);
	}
	if (!draw_source) {
		draw_source = obs_source_create("draw_source", "Global Draw Source", settings, nullptr);
	} else {
		obs_source_update(draw_source, settings);
	}
	obs_data_release(settings);

	signal_handler_t *sh = obs_source_get_signal_handler(draw_source);
	signal_handler_connect(sh, "update", draw_source_update, this);
	signal_handler_connect(sh, "destroy", draw_source_destroy, this);
	if (set_output) {
		for (uint32_t i = MAX_CHANNELS - 1; i > 0; i--) {
			obs_source_t *source = obs_get_output_source(i);
			if (source) {
				obs_source_release(source);
				continue;
			}
			obs_set_output_source(i, draw_source);
			return;
		}
	}
}

static void ensure_directory(char *path)
{
#ifdef _WIN32
	char *backslash = strrchr(path, '\\');
	if (backslash)
		*backslash = '/';
#endif

	char *slash = strrchr(path, '/');
	if (slash) {
		*slash = 0;
		os_mkdirs(path);
		*slash = '/';
	}

#ifdef _WIN32
	if (backslash)
		*backslash = '\\';
#endif
}

void DrawDock::DestroyDrawSource()
{
	if (!draw_source)
		return;

	auto source = obs_source_get_ref(draw_source);
	if (!source) {
		draw_source = nullptr;
		return;
	}
	obs_source_release(draw_source);
	draw_source = nullptr;

	signal_handler_t *sh = obs_source_get_signal_handler(source);
	signal_handler_disconnect(sh, "update", draw_source_update, this);
	signal_handler_disconnect(sh, "destroy", draw_source_destroy, this);

	char *path = obs_module_config_path("config.json");
	if (!path)
		return;
	ensure_directory(path);
	obs_data_t *config = obs_data_create();
	obs_data_t *gds = obs_save_source(source);
	if (gds) {
		obs_data_set_obj(config, "global_draw_source", gds);
		obs_data_release(gds);
	}
	if (obs_data_save_json_safe(config, path, "tmp", "bak")) {
		blog(LOG_INFO, "[Draw Dock] Saved settings");
	} else {
		blog(LOG_ERROR, "[Draw Dock] Failed saving settings");
	}
	obs_data_release(config);
	bfree(path);

	for (uint32_t i = 0; i < MAX_CHANNELS; i++) {
		obs_source_t *s = obs_get_output_source(i);
		if (s == source) {
			obs_set_output_source(i, nullptr);
		}
		obs_source_release(s);
	}

	obs_source_release(source);
}

void DrawDock::draw_source_update(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	DrawDock *window = static_cast<DrawDock *>(data);
	if (!window)
		return;

	QMetaObject::invokeMethod(window, "DrawSourceUpdate", Qt::QueuedConnection);
}

void DrawDock::draw_source_destroy(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	DrawDock *window = static_cast<DrawDock *>(data);
	if (!window)
		return;

	window->draw_source = nullptr;
}

void DrawDock::source_create(void* data, calldata_t* cd) {
	DrawDock *window = static_cast<DrawDock *>(data);
	if (!window)
		return;

	obs_source_t *source = (obs_source_t *)calldata_ptr(cd, "source");
	if (!source)
		return;
	if (source == window->draw_source)
		return;
	if (strcmp(obs_source_get_unversioned_id(source), "draw_source") != 0)
		return;
	if (strcmp(obs_source_get_name(source), "Global Draw Source") != 0)
		return;
	window->CreateDrawSource(source);
}

void DrawDock::DrawSourceUpdate()
{
	if (!draw_source)
		return;
	obs_data_t *settings = obs_source_get_settings(draw_source);
	if (!settings)
		return;

	int tool = (int)obs_data_get_int(settings, "tool");
	if (toolCombo->currentIndex() != tool)
		toolCombo->setCurrentIndex(tool);

	QColor color = color_from_int(obs_data_get_int(settings, "tool_color"));
	auto w = toolbar->widgetForAction(colorAction);
	QString s("background: " + color.name() + ";");
	if (w->styleSheet() != s) {
		w->setStyleSheet(s);

		QPixmap pixmap(100, 100);
		pixmap.fill(color);
		QIcon colorIcon(pixmap);
		colorAction->setIcon(colorIcon);
	}

	auto size = obs_data_get_double(settings, "tool_size");
	if (abs(toolSizeSpin->value() - size) > 0.1)
		toolSizeSpin->setValue(size);

	auto alpha = obs_data_get_double(settings, "tool_alpha");
	auto erase = alpha < 0.0;
	if (eraseCheckbox->isChecked() != erase)
		eraseCheckbox->setChecked(erase);
	if (alpha >= 0.0 && abs(alphaSpin->value() - alpha) > 0.1)
		alphaSpin->setValue(alpha);

	obs_data_release(settings);
}

void DrawDock::DrawBackdrop(float cx, float cy)
{
	if (!box)
		return;

	GS_DEBUG_MARKER_BEGIN(GS_DEBUG_COLOR_DEFAULT, "DrawBackdrop");

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	vec4 colorVal;
	vec4_set(&colorVal, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(color, &colorVal);

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_scale3f(float(cx), float(cy), 1.0f);

	gs_load_vertexbuffer(box);
	gs_draw(GS_TRISTRIP, 0, 0);

	gs_matrix_pop();
	gs_technique_end_pass(tech);
	gs_technique_end(tech);

	gs_load_vertexbuffer(nullptr);

	GS_DEBUG_MARKER_END();
}

void DrawDock::SceneChanged()
{
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (!scene_source)
		return;
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	obs_source_release(scene_source);
	if (!scene)
		return;

	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *data) {
			auto source = obs_sceneitem_get_source(item);
			if (!source || strcmp(obs_source_get_unversioned_id(source), "draw_source") != 0)
				return true;
			DrawDock *window = static_cast<DrawDock *>(data);
			if (!window)
				return true;

			return true;
		},
		this);
}
