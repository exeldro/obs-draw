#include "draw-dock.hpp"
#include "draw-source.h"
#include "name-dialog.hpp"
#include "obs-websocket-api.h"
#include "version.h"
#include <graphics/matrix4.h>
#include <obs-module.h>
#include <QColorDialog>
#include <QFileDialog>
#include <QGuiApplication>
#include <QMainWindow>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <util/platform.h>
#ifdef _WIN32
#include <windows.h>
#endif

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
	if (draw_dock)
		draw_dock->PostLoad();
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

#ifdef _WIN32
#if QT_VERSION < QT_VERSION_CHECK(6, 4, 0)
#define GENERIC_MONITOR_NAME QStringLiteral("Generic PnP Monitor")

struct MonitorData {
	const wchar_t *id;
	MONITORINFOEX info;
	bool found;
};

static BOOL CALLBACK GetMonitorCallback(HMONITOR monitor, HDC, LPRECT, LPARAM param)
{
	MonitorData *data = (MonitorData *)param;

	if (GetMonitorInfoW(monitor, &data->info)) {
		if (wcscmp(data->info.szDevice, data->id) == 0) {
			data->found = true;
			return false;
		}
	}

	return true;
}

QString GetMonitorName(const QString &id)
{
	MonitorData data = {};
	data.id = (const wchar_t *)id.utf16();
	data.info.cbSize = sizeof(data.info);

	EnumDisplayMonitors(nullptr, nullptr, GetMonitorCallback, (LPARAM)&data);
	if (!data.found) {
		return GENERIC_MONITOR_NAME;
	}

	UINT32 numPath, numMode;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPath, &numMode) != ERROR_SUCCESS) {
		return GENERIC_MONITOR_NAME;
	}

	std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPath);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(numMode);

	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPath, paths.data(), &numMode, modes.data(), nullptr) != ERROR_SUCCESS) {
		return GENERIC_MONITOR_NAME;
	}

	DISPLAYCONFIG_TARGET_DEVICE_NAME target;
	bool found = false;

	paths.resize(numPath);
	for (size_t i = 0; i < numPath; ++i) {
		const DISPLAYCONFIG_PATH_INFO &path = paths[i];

		DISPLAYCONFIG_SOURCE_DEVICE_NAME s;
		s.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		s.header.size = sizeof(s);
		s.header.adapterId = path.sourceInfo.adapterId;
		s.header.id = path.sourceInfo.id;

		if (DisplayConfigGetDeviceInfo(&s.header) == ERROR_SUCCESS &&
		    wcscmp(data.info.szDevice, s.viewGdiDeviceName) == 0) {
			target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
			target.header.size = sizeof(target);
			target.header.adapterId = path.sourceInfo.adapterId;
			target.header.id = path.targetInfo.id;
			found = DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS;
			break;
		}
	}

	if (!found) {
		return GENERIC_MONITOR_NAME;
	}

	return QString::fromWCharArray(target.monitorFriendlyDeviceName);
}
#endif
#endif

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

#ifdef _WIN32
bool IsAlwaysOnTop(QWidget *window)
{
	DWORD exStyle = GetWindowLong((HWND)window->winId(), GWL_EXSTYLE);
	return (exStyle & WS_EX_TOPMOST) != 0;
}
#else
bool IsAlwaysOnTop(QWidget *window)
{
	return (window->windowFlags() & Qt::WindowStaysOnTopHint) != 0;
}
#endif

#ifdef _WIN32
void SetAlwaysOnTop(QWidget *window, bool enable)
{
	HWND hwnd = (HWND)window->winId();
	SetWindowPos(hwnd, enable ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
#else
void SetAlwaysOnTop(QWidget *window, bool enable)
{
	Qt::WindowFlags flags = window->windowFlags();

	if (enable) {
		flags |= Qt::WindowStaysOnTopHint;
	} else {
		flags &= ~Qt::WindowStaysOnTopHint;
	}

	window->setWindowFlags(flags);
	window->show();
}
#endif

DrawDock::DrawDock(QWidget *_parent) : QWidget(_parent), eventFilter(BuildEventFilter()), preview(new OBSQTDisplay(this))
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

	const auto path = obs_module_config_path("config.json");
	config = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);
	if (!config)
		config = obs_data_create();

	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_connect(sh, "source_create", source_create, this);

	toolbar = new QToolBar();
	ml->addWidget(toolbar);

	auto a = toolbar->addAction(QString::fromUtf8(obs_module_text("Config")), [this] {
		if (!draw_source)
			return;
		QMenu menu;

		auto toolMenu = menu.addMenu(QString::fromUtf8(obs_module_text("FavoriteTools")));

		obs_data_array_t *tools = obs_data_get_array(config, "tools");
		auto count = obs_data_array_count(tools);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *ts = obs_data_array_item(tools, i);
			if (!ts)
				continue;
			auto tm = toolMenu->addMenu(CreateToolIcon(ts), QString::fromUtf8(obs_data_get_string(ts, "tool_name")));
			tm->addAction(QString::fromUtf8(obs_module_text("SetToCurrent")), [this, i, ts] {
				if (!draw_source)
					return;
				obs_data_t *gdss = obs_source_get_settings(draw_source);
				obs_data_t *settings = obs_data_get_obj(ts, "settings");
				obs_data_set_int(settings, "tool", obs_data_get_int(gdss, "tool"));
				obs_data_set_string(settings, "tool_image_file", obs_data_get_string(gdss, "tool_image_file"));
				obs_data_set_int(settings, "tool_color", obs_data_get_int(gdss, "tool_color"));
				obs_data_set_double(settings, "tool_size", obs_data_get_double(gdss, "tool_size"));
				obs_data_set_double(settings, "tool_alpha", obs_data_get_double(gdss, "tool_alpha"));
				obs_data_release(settings);
				obs_data_release(gdss);
				auto action = toolbar->actions().at(i + 1);
				action->setIcon(CreateToolIcon(ts));
			});
			tm->addAction(QString::fromUtf8(obs_module_text("Remove")), [this, tools, i, ts] {
				auto action = toolbar->actions().at(i + 1);
				for (auto j = favoriteToolHotkeys.begin(); j != favoriteToolHotkeys.end(); j++) {
					if (j->second.first == action || j->second.second == ts) {
						obs_hotkey_unregister(j->first);
						favoriteToolHotkeys.erase(j);
						break;
					}
				}
				toolbar->removeAction(action);
				obs_data_array_erase(tools, i);
				SaveConfig();
			});
			obs_data_release(ts);
		}
		obs_data_array_release(tools);
		if (count)
			toolMenu->addSeparator();
		toolMenu->addAction(QString::fromUtf8(obs_module_text("AddCurrent")), [this] {
			QAction *tca = nullptr;
			foreach(QAction * action, toolbar->actions())
			{
				if (toolCombo == toolbar->widgetForAction(action)) {
					tca = action;
				}
			}
			if (!tca)
				return;
			std::string name;
			if (!NameDialog::AskForName(this, QString::fromUtf8(obs_module_text("ToolName")), name))
				return;
			if (name.empty())
				return;

			obs_data_array_t *tools = obs_data_get_array(config, "tools");
			if (!tools) {
				tools = obs_data_array_create();
				obs_data_set_array(config, "tools", tools);
			}
			obs_data_t *gdss = obs_source_get_settings(draw_source);
			obs_data_t *tool = obs_data_create();
			obs_data_set_string(tool, "tool_name", name.c_str());
			obs_data_t *settings = obs_data_create();
			obs_data_set_int(settings, "tool", obs_data_get_int(gdss, "tool"));
			obs_data_set_string(settings, "tool_image_file", obs_data_get_string(gdss, "tool_image_file"));
			obs_data_set_int(settings, "tool_color", obs_data_get_int(gdss, "tool_color"));
			obs_data_set_double(settings, "tool_size", obs_data_get_double(gdss, "tool_size"));
			obs_data_set_double(settings, "tool_alpha", obs_data_get_double(gdss, "tool_alpha"));
			obs_data_release(gdss);
			obs_data_set_obj(tool, "settings", settings);
			obs_data_release(settings);
			obs_data_array_push_back(tools, tool);
			obs_data_array_release(tools);
			toolbar->insertAction(tca, AddFavoriteTool(tool));
			obs_data_release(tool);
			SaveConfig();
		});

		obs_data_t *settings = obs_source_get_settings(draw_source);
		auto cursorMenu = menu.addMenu(QString::fromUtf8(obs_module_text("Cursor")));

		auto a = cursorMenu->addAction(QString::fromUtf8(obs_module_text("Show")));
		a->setCheckable(true);

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
			color = QColorDialog::getColor(color, this, QString::fromUtf8(obs_module_text("CursorColor")));
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
			QString fileName = QFileDialog::getOpenFileName(this, QString::fromUtf8(obs_module_text("CursorImage")),
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
		menu.addSeparator();

		menu.addAction(QString::fromUtf8(obs_module_text("Undo")), [this] {
			if (draw_source) {
				proc_handler_t *ph = obs_source_get_proc_handler(draw_source);
				if (!ph)
					return;
				calldata_t d = {};
				proc_handler_call(ph, "undo", &d);
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
					proc_handler_call(ph, "undo", &cd);
					return true;
				},
				nullptr);
		});

		menu.addAction(QString::fromUtf8(obs_module_text("Redo")), [this] {
			if (draw_source) {
				proc_handler_t *ph = obs_source_get_proc_handler(draw_source);
				if (!ph)
					return;
				calldata_t d = {};
				proc_handler_call(ph, "redo", &d);
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
					proc_handler_call(ph, "redo", &cd);
					return true;
				},
				nullptr);
		});
		auto undoMenu = menu.addMenu(QString::fromUtf8(obs_module_text("UndoMax")));
		auto undowa = new QWidgetAction(undoMenu);
		auto maxUndo = new QSpinBox();
		maxUndo->setValue(obs_data_get_int(settings, "max_undo"));
		maxUndo->setRange(0, 1000);
		undowa->setDefaultWidget(maxUndo);
		undoMenu->addAction(undowa);

		connect(maxUndo, &QSpinBox::valueChanged, [this, maxUndo] {
			if (!draw_source)
				return;
			obs_data_t *settings = obs_data_create();
			obs_data_set_int(settings, "max_undo", maxUndo->value());
			obs_source_update(draw_source, settings);
			obs_data_release(settings);
		});

		obs_data_release(settings);

		menu.addSeparator();
		auto d = (QDockWidget *)parent();
		auto action = menu.addAction(QString::fromUtf8(obs_module_text("Fullscreen"))); 
		auto fullMenu = new QMenu();
		action->setMenu(fullMenu);
		QList<QScreen *> screens = QGuiApplication::screens();
		for (int i = 0; i < screens.size(); i++) {
			QScreen *screen = screens[i];
			QRect screenGeometry = screen->geometry();
			qreal ratio = screen->devicePixelRatio();
			QString name = "";
#if defined(_WIN32) && QT_VERSION < QT_VERSION_CHECK(6, 4, 0)
			QTextStream fullname(&name);
			fullname << GetMonitorName(screen->name());
			fullname << " (";
			fullname << (i + 1);
			fullname << ")";
#elif defined(__APPLE__) || defined(_WIN32)
			name = screen->name();
#else
			name = screen->model().simplified();

			if (name.length() > 1 && name.endsWith("-"))
				name.chop(1);
#endif
			name = name.simplified();

			if (name.length() == 0) {
				name = QString("%1 %2")
					       .arg(QString::fromUtf8(obs_frontend_get_locale_string("Display")))
					       .arg(QString::number(i + 1));
			}
			QString str = QString("%1: %2x%3 @ %4,%5")
					      .arg(name, QString::number(screenGeometry.width() * ratio),
						   QString::number(screenGeometry.height() * ratio),
						   QString::number(screenGeometry.x()), QString::number(screenGeometry.y()));

			QAction *a = fullMenu->addAction(str, this, SLOT(OpenFullScreenProjector()));
			a->setProperty("monitor", i);
		}
		action->setCheckable(true);
		action->setChecked(d->parent() == nullptr && config && obs_data_get_bool(config, "fullscreen"));

		action = menu.addAction(QString::fromUtf8(obs_module_text("Dock")), [this] {
			auto dock = (QDockWidget *)parent();
			auto main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			if (!dock->parent()) {
				dock->setParent(main);
				dock->showNormal();
				if (!prevGeometry.isNull()) {
					if (dock->isFloating() != prevFloating)
						dock->setFloating(prevFloating);
					dock->setGeometry(prevGeometry);
					if (!prevFloating)
						main->addDockWidget(prevArea, dock);
				} else {
					if (dock->isFloating())
						dock->setFloating(false);
					dock->resize(860, 530);
					if (main->dockWidgetArea(dock) == Qt::NoDockWidgetArea)
						main->addDockWidget(Qt::LeftDockWidgetArea, dock);
				}
			} else {
				dock->showNormal();
				if (dock->isFloating())
					dock->setFloating(false);
				dock->resize(860, 530);
				if (main->dockWidgetArea(dock) == Qt::NoDockWidgetArea)
					main->addDockWidget(Qt::LeftDockWidgetArea, dock);
			}
			if (config) {
				obs_data_set_bool(config, "fullscreen", false);
				obs_data_set_bool(config, "windowed", false);
			}
		});
		action->setCheckable(true);
		action->setChecked(d->parent() != nullptr);
		action = menu.addAction(QString::fromUtf8(obs_module_text("Windowed")), [this] {
			auto dock = (QDockWidget *)parent();
			if (dock->parent()) {
				prevGeometry = dock->geometry();
				prevFloating = dock->isFloating();
				auto main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
				prevArea = main->dockWidgetArea(dock);
			}
			if (!dock->isFloating())
				dock->setFloating(true);
			if (dock->parent())
				dock->setParent(nullptr);
			dock->showNormal();
			dock->resize(860, 530);
			if (config) {
				obs_data_set_bool(config, "fullscreen", false);
				obs_data_set_bool(config, "windowed", true);
			}
		});
		action->setCheckable(true);
		action->setChecked(d->parent() == nullptr && (!config || obs_data_get_bool(config, "windowed")));
		action = menu.addAction(QString::fromUtf8(obs_module_text("AlwaysOnTop")), [&] {
			auto dock = (QDockWidget *)parent();
			bool aot = !IsAlwaysOnTop(dock);
			SetAlwaysOnTop(dock, aot);
			if (config)
				obs_data_set_bool(config, "always_on_top", aot);

		});
		action->setCheckable(true);
		action->setChecked(IsAlwaysOnTop((QDockWidget *)parent()));

		menu.exec(QCursor::pos());
	});
	toolbar->widgetForAction(a)->setProperty("themeID", "propertiesIconSmall");
	toolbar->widgetForAction(a)->setProperty("class", "icon-gear");

	clearHotkey = obs_hotkey_register_frontend("draw_clear", obs_module_text("DrawClear"), clear_hotkey, this);
	auto hotkeys = obs_data_get_array(config, "clear_hotkey");
	if (hotkeys) {
		obs_hotkey_load(clearHotkey, hotkeys);
		obs_data_array_release(hotkeys);
	}

	obs_data_array_t *tools = obs_data_get_array(config, "tools");
	auto count = obs_data_array_count(tools);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *ts = obs_data_array_item(tools, i);
		if (!ts)
			continue;
		toolbar->addAction(AddFavoriteTool(ts));
		obs_data_release(ts);
	}
	obs_data_array_release(tools);

	toolCombo = new QComboBox;
	toolCombo->setMinimumWidth(60);
	auto demoColor = palette().buttonText().color();
	toolCombo->addItem(CreateToolIcon(demoColor, TOOL_NONE), obs_module_text("None"), QVariant(TOOL_NONE));
	toolCombo->addItem(CreateToolIcon(demoColor, TOOL_PENCIL), obs_module_text("Pencil"), QVariant(TOOL_PENCIL));
	toolCombo->addItem(CreateToolIcon(demoColor, TOOL_BRUSH), obs_module_text("Brush"), QVariant(TOOL_BRUSH));
	toolCombo->addItem(CreateToolIcon(demoColor, TOOL_LINE), obs_module_text("Line"), QVariant(TOOL_LINE));
	toolCombo->addItem(CreateToolIcon(demoColor, TOOL_RECTANGLE_OUTLINE), obs_module_text("RectangleOutline"),
			   QVariant(TOOL_RECTANGLE_OUTLINE));
	toolCombo->addItem(CreateToolIcon(demoColor, TOOL_RECTANGLE_FILL), obs_module_text("RectangleFill"),
			   QVariant(TOOL_RECTANGLE_FILL));
	toolCombo->addItem(CreateToolIcon(demoColor, TOOL_ELLIPSE_OUTLINE), obs_module_text("EllipseOutline"),
			   QVariant(TOOL_ELLIPSE_OUTLINE));
	toolCombo->addItem(CreateToolIcon(demoColor, TOOL_ELLIPSE_FILL), obs_module_text("EllipseFill"),
			   QVariant(TOOL_ELLIPSE_FILL));
	toolCombo->addItem(CreateToolIcon(demoColor, TOOL_SELECT_RECTANGLE), obs_module_text("SelectRectangle"),
			   QVariant(TOOL_SELECT_RECTANGLE));
	toolCombo->addItem(CreateToolIcon(demoColor, TOOL_SELECT_ELLIPSE), obs_module_text("SelectEllipse"),
			   QVariant(TOOL_SELECT_ELLIPSE));
	toolCombo->addItem(CreateToolIcon(demoColor, TOOL_STAMP), obs_module_text("Stamp"), QVariant(TOOL_STAMP));
	toolCombo->addItem(CreateToolIcon(demoColor, TOOL_IMAGE), obs_module_text("Image"), QVariant(TOOL_IMAGE));

	connect(toolCombo, &QComboBox::currentIndexChanged, [this] {
		int tool = toolCombo->currentData().toInt();
		if (tool == TOOL_IMAGE || tool == TOOL_STAMP) {
			colorAction->setVisible(false);
			imageAction->setVisible(true);
		} else {
			imageAction->setVisible(false);
			colorAction->setVisible(true);
		}
		if (!draw_source)
			return;
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
		color = QColorDialog::getColor(color, this, QString::fromUtf8(obs_module_text("ToolColor")));
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
	imageAction = toolbar->addAction(QString::fromUtf8(obs_module_text("ToolImage")), [this] {
		if (!draw_source)
			return;
		obs_data_t *settings = obs_source_get_settings(draw_source);
		const char *path = obs_data_get_string(settings, "tool_image_file");
		obs_data_release(settings);
		QString fileName = QFileDialog::getOpenFileName(this, QString::fromUtf8(obs_module_text("ToolImage")),
								QString::fromUtf8(path), image_filter);
		if (fileName.isEmpty())
			return;
		if (!draw_source)
			return;
		settings = obs_data_create();
		obs_data_set_string(settings, "tool_image_file", fileName.toUtf8().constData());
		obs_source_update(draw_source, settings);
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
				const char *path = (const char *)data;
				obs_data_t *ss = obs_source_get_settings(source);
				if (strcmp(obs_data_get_string(ss, "tool_image_file"), path) != 0) {
					obs_data_set_string(ss, "tool_image_file", path);
					obs_source_update(source, ss);
				}
				obs_data_release(ss);
				return true;
			},
			(void *)fileName.toUtf8().constData());
	});
	imageAction->setVisible(false);
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
	alphaSpin->setValue(50.0);
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
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	connect(eraseCheckbox, &QCheckBox::checkStateChanged, alphaChange);
#else
	connect(eraseCheckbox, &QCheckBox::stateChanged, alphaChange);
#endif

	toolbar->addSeparator();
	toolbar->addAction(QString::fromUtf8(obs_module_text("Clear")), [this] { ClearDraw(); });

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

	QAction *action = new QAction(this);
	action->setShortcut(Qt::Key_Escape);
	addAction(action);
	connect(action, SIGNAL(triggered()), this, SLOT(EscapeTriggered()));

	obs_frontend_add_event_callback(frontend_event, this);
}

DrawDock::~DrawDock()
{
	if (clearHotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(clearHotkey);
	for (auto i = favoriteToolHotkeys.begin(); i != favoriteToolHotkeys.end(); i++) {
		obs_hotkey_unregister(i->first);
	}
	favoriteToolHotkeys.clear();
	DestroyDrawSource();
	delete eventFilter;
	obs_enter_graphics();
	gs_vertexbuffer_destroy(box);
	obs_leave_graphics();
	obs_data_release(config);
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
	auto event_type = event->type();
	const bool mouseUp = event_type == QEvent::MouseButtonRelease;
	if (tabletActive) {
		if (mouseUp)
			tabletActive = false;
		else
			return true;
	}
	if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ControlModifier)) {
		if (!mouseUp) {
			scrollingFromX = event->pos().x();
			scrollingFromY = event->pos().y();
		}
		return true;
	}
	uint32_t clickCount = 1;
	if (event_type == QEvent::MouseButtonDblClick)
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
	if (tabletActive)
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

bool DrawDock::HandleTabletEvent(QTabletEvent *event)
{
	if (!event)
		return false;

	auto pressure = event->pressure();

	auto event_type = event->type();
	if (event_type == QEvent::TabletPress)
		tabletActive = true;
	else if (event_type == QEvent::TabletRelease)
		tabletActive = false;
	else if (pressure <= 0.0 && tabletActive)
		tabletActive = false;
	else if (pressure > 0.0 && !tabletActive)
		pressure = 0.0;

	int posx;
	int posy;
	GetSourceRelativeXY(event->position().x(), event->position().y(), posx, posy);
	click_event ce{posx, posy, 0, 0, pressure <= 0.0, 1, nullptr};

	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (scene_source) {
		if (obs_scene_t *scene = obs_scene_from_source(scene_source)) {
			obs_scene_enum_items(scene, HandleSceneMouseClickEvent, &ce);
		}
		obs_source_release(scene_source);
	}
	if (ce.mouseTarget) {
		auto ph = obs_source_get_proc_handler(ce.mouseTarget);
		if (ph) {
			struct calldata cd;
			calldata_init(&cd);
			calldata_set_int(&cd, "posx", ce.mouseEvent.x);
			calldata_set_int(&cd, "posy", ce.mouseEvent.y);
			calldata_set_float(&cd, "pressure", pressure);
			proc_handler_call(ph, "tablet", &cd);
			calldata_free(&cd);
		}
		if (pressure <= 0.0) {
			if (mouse_down_target) {
				if (mouse_down_target == draw_source) {
					ph = obs_source_get_proc_handler(draw_source);
					if (ph) {
						struct calldata cd;
						calldata_init(&cd);
						calldata_set_int(&cd, "posx", posx);
						calldata_set_int(&cd, "posy", posy);
						calldata_set_float(&cd, "pressure", pressure);
						proc_handler_call(ph, "tablet", &cd);
						calldata_free(&cd);
					}
				} else if (mouse_down_target != ce.mouseTarget) {
					ph = obs_source_get_proc_handler(mouse_down_target);
					if (ph) {
						struct calldata cd;
						calldata_init(&cd);
						calldata_set_int(&cd, "posx", posx);
						calldata_set_int(&cd, "posy", posy);
						calldata_set_float(&cd, "pressure", pressure);
						proc_handler_call(ph, "tablet", &cd);
						calldata_free(&cd);
					}
				}
				mouse_down_target = nullptr;
			}
		} else {
			mouse_down_target = ce.mouseTarget;
		}

	} else if (draw_source) {
		auto ph = obs_source_get_proc_handler(draw_source);
		if (ph) {
			struct calldata cd;
			calldata_init(&cd);
			calldata_set_int(&cd, "posx", posx);
			calldata_set_int(&cd, "posy", posy);
			calldata_set_float(&cd, "pressure", pressure);
			proc_handler_call(ph, "tablet", &cd);
			calldata_free(&cd);
		}
		if (pressure <= 0.0) {
			if (mouse_down_target && mouse_down_target != draw_source) {
				ph = obs_source_get_proc_handler(mouse_down_target);
				if (ph) {
					struct calldata cd;
					calldata_init(&cd);
					calldata_set_int(&cd, "posx", posx);
					calldata_set_int(&cd, "posy", posy);
					calldata_set_float(&cd, "pressure", pressure);
					proc_handler_call(ph, "tablet", &cd);
					calldata_free(&cd);
				}
			}
			mouse_down_target = nullptr;
		} else {
			mouse_down_target = draw_source;
		}
	} else if (pressure <= 0.0 && mouse_down_target) {
		auto ph = obs_source_get_proc_handler(mouse_down_target);
		if (ph) {
			struct calldata cd;
			calldata_init(&cd);
			calldata_set_int(&cd, "posx", posx);
			calldata_set_int(&cd, "posy", posy);
			calldata_set_float(&cd, "pressure", pressure);
			proc_handler_call(ph, "tablet", &cd);
			calldata_free(&cd);
		}
		mouse_down_target = nullptr;
	} else {
		mouse_down_target = nullptr;
	}

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
		case QEvent::TabletPress:
		case QEvent::TabletRelease:
		case QEvent::TabletMove:
		case QEvent::TabletEnterProximity:
		case QEvent::TabletLeaveProximity:
			return this->HandleTabletEvent(static_cast<QTabletEvent *>(event));
		default:
			return false;
		}
	});
}

void DrawDock::frontend_event(enum obs_frontend_event event, void *data)
{
	DrawDock *window = static_cast<DrawDock *>(data);
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		window->FinishedLoad();
		window->CreateDrawSource();
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
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

	obs_data_t *gds = obs_save_source(source);
	if (gds) {
		obs_data_set_obj(config, "global_draw_source", gds);
		obs_data_release(gds);
	}
	SaveConfig();

	for (uint32_t i = 0; i < MAX_CHANNELS; i++) {
		obs_source_t *s = obs_get_output_source(i);
		if (s == source) {
			obs_set_output_source(i, nullptr);
		}
		obs_source_release(s);
	}

	obs_source_release(source);
}

void DrawDock::SaveConfig()
{
	char *path = obs_module_config_path("config.json");
	if (!path)
		return;
	ensure_directory(path);

	obs_data_array_t *clearHotkeyData = obs_hotkey_save(clearHotkey);
	if (clearHotkeyData) {
		obs_data_set_array(config, "clear_hotkey", clearHotkeyData);
		obs_data_array_release(clearHotkeyData);
	}

	obs_data_array_t *tools = obs_data_get_array(config, "tools");
	size_t count = obs_data_array_count(tools);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *ts = obs_data_array_item(tools, i);
		if (!ts)
			continue;

		for (auto j = favoriteToolHotkeys.begin(); j != favoriteToolHotkeys.end(); j++) {
			if (j->second.second == ts) {
				obs_data_array_t *hotkeys = obs_hotkey_save(j->first);
				obs_data_set_array(ts, "hotkeys", hotkeys);
				obs_data_array_release(hotkeys);
			}
		}
		obs_data_release(ts);
	}
	obs_data_array_release(tools);

	if (obs_data_get_bool(config, "windowed")) {
		auto dock = (QDockWidget *)parent();
		obs_data_set_string(config, "window_geometry", dock->saveGeometry().toBase64().constData());
	}

	if (obs_data_save_json_safe(config, path, "tmp", "bak")) {
		blog(LOG_INFO, "[Draw Dock] Saved settings");
	} else {
		blog(LOG_ERROR, "[Draw Dock] Failed saving settings");
	}
	bfree(path);
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

void DrawDock::source_create(void *data, calldata_t *cd)
{
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
	auto toolColor = obs_data_get_int(settings, "tool_color");
	QColor color = color_from_int(toolColor);
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

	if (tool == TOOL_STAMP || tool == TOOL_IMAGE)
		imageAction->setIcon(CreateToolIcon(color, tool, alpha, size, obs_data_get_string(settings, "tool_image_file")));

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

QAction *DrawDock::AddFavoriteTool(obs_data_t *tool)
{
	auto toolName = obs_data_get_string(tool, "tool_name");
	obs_data_t *settings = obs_data_get_obj(tool, "settings");
	auto action = new QAction(CreateToolIcon(tool), QString::fromUtf8(toolName));
	connect(action, &QAction::triggered, [this, settings] { ApplyFavoriteTool(settings); });
	obs_data_release(settings);
	std::string hotKeyName = "DrawDockFavoriteTool.";
	hotKeyName += toolName;
	std::string hotKeyDescription = obs_module_text("DrawFavoriteTool");
	hotKeyDescription += " ";
	hotKeyDescription += toolName;
	auto hotkeyId = obs_hotkey_register_frontend(hotKeyName.c_str(), hotKeyDescription.c_str(), favorite_tool_hotkey, this);
	auto hotkeys = obs_data_get_array(tool, "hotkeys");
	if (hotkeys) {
		obs_hotkey_load(hotkeyId, hotkeys);
		obs_data_array_release(hotkeys);
	}
	favoriteToolHotkeys.emplace(hotkeyId, std::pair<QAction *, obs_data_t *>(action, tool));
	return action;
}

void DrawDock::clear_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(hotkey);
	UNUSED_PARAMETER(id);
	if (!pressed)
		return;

	DrawDock *window = static_cast<DrawDock *>(data);
	window->ClearDraw();
}

void DrawDock::favorite_tool_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;

	DrawDock *window = static_cast<DrawDock *>(data);

	auto i = window->favoriteToolHotkeys.find(id);
	if (i == window->favoriteToolHotkeys.end())
		return;

	obs_data_t *settings = obs_data_get_obj(i->second.second, "settings");
	window->ApplyFavoriteTool(settings);
	obs_data_release(settings);
}

void DrawDock::ApplyFavoriteTool(obs_data_t *settings)
{
	if (draw_source)
		obs_source_update(draw_source, settings);
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
			obs_data_t *settings = (obs_data_t *)data;
			obs_source_update(source, settings);
			return true;
		},
		settings);
}

QIcon DrawDock::CreateToolIcon(QColor toolColor, uint32_t tool, double alpha, double toolSize, const char *image)
{
	auto pixmap = QPixmap(256, 256);
	if (alpha >= 0.0) {
		pixmap.fill(QColor(0, 0, 0, 0));
		toolColor.setAlphaF(alpha / 100.0);
	} else {
		pixmap.fill(toolColor);
		toolColor = palette().button().color();
	}

	if (tool == TOOL_PENCIL) {
		auto painter = QPainter(&pixmap);
		painter.setPen(QPen(toolColor, toolSize, Qt::SolidLine, Qt::RoundCap));
		QPainterPath path;
		path.moveTo(4 + toolSize / 2.0, 4 + toolSize / 2.0);
		path.cubicTo(64, 4 + toolSize / 2.0, 128, 64, 128, 128);
		path.cubicTo(128, 192, 252.0 - toolSize / 2.0, 192, 252.0 - toolSize / 2.0, 252.0 - toolSize / 2.0);
		painter.drawPath(path);
	} else if (tool == TOOL_BRUSH) {
		auto painter = QPainter(&pixmap);
		QPainterPath path;
		path.moveTo(4 + toolSize / 2.0, 4 + toolSize / 2.0);
		path.cubicTo(64, 4 + toolSize / 2.0, 128, 64, 128, 128);
		path.cubicTo(128, 192, 252.0 - toolSize / 2.0, 192, 252.0 - toolSize / 2.0, 252.0 - toolSize / 2.0);
		for (auto step = toolSize; step > 0.0; step -= 1.0) {
			auto c = toolColor;
			c.setAlphaF(toolColor.alphaF() / toolSize);
			painter.setPen(QPen(c, toolSize - step, Qt::SolidLine, Qt::RoundCap));
			painter.drawPath(path);
		}
	} else if (tool == TOOL_LINE) {
		auto painter = QPainter(&pixmap);
		painter.setPen(QPen(toolColor, toolSize, Qt::SolidLine, Qt::RoundCap));
		painter.drawLine(128, toolSize / 2.0, 128, 256.0 - toolSize / 2.0);
	} else if (tool == TOOL_RECTANGLE_OUTLINE) {
		auto painter = QPainter(&pixmap);
		painter.setPen(QPen(toolColor, toolSize));
		painter.drawRect(QRect(toolSize / 2.0, toolSize / 2.0, 256.0 - toolSize, 256.0 - toolSize));
	} else if (tool == TOOL_RECTANGLE_FILL) {
		auto painter = QPainter(&pixmap);
		painter.fillRect(QRect(4, 4, 248, 248), toolColor);
	} else if (tool == TOOL_ELLIPSE_OUTLINE) {
		auto painter = QPainter(&pixmap);
		painter.setPen(QPen(toolColor, toolSize));
		painter.drawEllipse(QRect(toolSize / 2.0, toolSize / 2.0, 256.0 - toolSize, 256.0 - toolSize));
	} else if (tool == TOOL_ELLIPSE_FILL) {
		auto painter = QPainter(&pixmap);
		painter.setPen(QPen(toolColor, 120));
		painter.drawEllipse(QRect(68, 68, 120, 120));
	} else if (tool == TOOL_SELECT_RECTANGLE) {
		auto painter = QPainter(&pixmap);
		painter.setPen(QPen(toolColor, toolSize, Qt::DotLine));
		painter.drawRect(QRect(toolSize / 2.0, toolSize / 2.0, 256.0 - toolSize, 256.0 - toolSize));
	} else if (tool == TOOL_SELECT_ELLIPSE) {
		auto painter = QPainter(&pixmap);
		painter.setPen(QPen(toolColor, toolSize, Qt::DotLine));
		painter.drawEllipse(QRect(toolSize / 2.0, toolSize / 2.0, 256.0 - toolSize, 256.0 - toolSize));
	} else if (tool == TOOL_STAMP || tool == TOOL_IMAGE) {
		if (image && strlen(image)) {
			pixmap = QPixmap(QString::fromUtf8(image));
		}
	}

	return QIcon(pixmap);
}

QIcon DrawDock::CreateToolIcon(obs_data_t *ts)
{
	obs_data_t *settings = obs_data_get_obj(ts, "settings");
	auto toolColor = color_from_int(obs_data_get_int(settings, "tool_color"));
	auto tool = (uint32_t)obs_data_get_int(settings, "tool");
	auto alpha = obs_data_get_double(settings, "tool_alpha");
	auto toolSize = obs_data_get_double(settings, "tool_size") * 2.0;
	auto toolImage = obs_data_get_string(settings, "tool_image_file");
	obs_data_release(settings);
	return CreateToolIcon(toolColor, tool, alpha, toolSize, toolImage);
}

void DrawDock::PostLoad()
{
	vendor = obs_websocket_register_vendor("draw");
	if (!vendor)
		return;
	obs_websocket_vendor_register_request(vendor, "version", vendor_request_version, nullptr);
	obs_websocket_vendor_register_request(vendor, "clear", vendor_request_clear, nullptr);
	obs_websocket_vendor_register_request(vendor, "draw", vendor_request_draw, nullptr);
}

void DrawDock::FinishedLoad()
{
	auto imageIcon = static_cast<QMainWindow *>(obs_frontend_get_main_window())->property("imageIcon").value<QIcon>();
	imageAction->setIcon(imageIcon);
	toolCombo->setItemIcon(TOOL_STAMP, imageIcon);
	toolCombo->setItemIcon(TOOL_IMAGE, imageIcon);
	auto dock = (QDockWidget *)parent();
	if (obs_data_get_bool(config, "fullscreen")) {
		dock->setFloating(true);
		dock->setParent(nullptr);
		dock->setGeometry(QRect(obs_data_get_int(config, "fullscreen_left"), obs_data_get_int(config, "fullscreen_top"),
					obs_data_get_int(config, "fullscreen_width"),
					obs_data_get_int(config, "fullscreen_height")));
		dock->showFullScreen();
	} else if (obs_data_get_bool(config, "windowed")) {
		dock->setFloating(true);
		dock->setParent(nullptr);
		dock->showNormal();

		const char * geom = obs_data_get_string(config, "window_geometry");
		if (geom && strlen(geom)) {
			QByteArray ba = QByteArray::fromBase64(QByteArray(geom));
			dock->restoreGeometry(ba);
		}
	}
	if (obs_data_get_bool(config, "always_on_top"))
		SetAlwaysOnTop(dock, true);
}

void DrawDock::vendor_request_version(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	UNUSED_PARAMETER(request_data);
	obs_data_set_string(response_data, "version", PROJECT_VERSION);
	obs_data_set_bool(response_data, "success", true);
}

void DrawDock::vendor_request_clear(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	auto source_name = obs_data_get_string(request_data, "source");
	obs_source_t *source = nullptr;
	if (!source_name || !strlen(source_name)) {
		if (draw_dock && draw_dock->draw_source) {
			source = obs_source_get_ref(draw_dock->draw_source);
		}
	} else {
		source = obs_get_source_by_name(source_name);
	}
	if (!source) {
		obs_data_set_string(response_data, "error", "'source' not found");
		obs_data_set_bool(response_data, "success", false);
		return;
	}
	if (strcmp(obs_source_get_unversioned_id(source), "draw_source") != 0) {
		obs_source_release(source);
		obs_data_set_string(response_data, "error", "'source' not a draw source");
		obs_data_set_bool(response_data, "success", false);
		return;
	}

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	obs_source_release(source);
	if (!ph) {
		obs_data_set_bool(response_data, "success", false);
		return;
	}
	calldata_t d = {};
	obs_data_set_bool(response_data, "success", proc_handler_call(ph, "clear", &d));
}

void DrawDock::vendor_request_draw(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	auto source_name = obs_data_get_string(request_data, "source");
	obs_source_t *source = nullptr;
	if (!source_name || !strlen(source_name)) {
		if (draw_dock && draw_dock->draw_source) {
			source = obs_source_get_ref(draw_dock->draw_source);
		}
	} else {
		source = obs_get_source_by_name(source_name);
	}
	if (!source) {
		obs_data_set_string(response_data, "error", "'source' not found");
		obs_data_set_bool(response_data, "success", false);
		return;
	}
	if (strcmp(obs_source_get_unversioned_id(source), "draw_source") != 0) {
		obs_source_release(source);
		obs_data_set_string(response_data, "error", "'source' not a draw source");
		obs_data_set_bool(response_data, "success", false);
		return;
	}

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	obs_source_release(source);
	if (!ph) {
		obs_data_set_bool(response_data, "success", false);
		return;
	}
	calldata_t d = {};
	calldata_init(&d);
	calldata_set_ptr(&d, "data", request_data);
	obs_data_set_bool(response_data, "success", proc_handler_call(ph, "draw", &d));
	calldata_free(&d);
}

void DrawDock::ClearDraw()
{
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
}

void DrawDock::OpenFullScreenProjector()
{
	int monitor = sender()->property("monitor").toInt();
	auto screen = QGuiApplication::screens()[monitor];
	auto dock = (QDockWidget *)parent();
	if (dock->parent()) {
		prevGeometry = dock->geometry();
		prevFloating = dock->isFloating();
		auto main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		prevArea = main->dockWidgetArea(dock);
	}
	dock->setFloating(true);
	auto geometry = screen->geometry();
	dock->setGeometry(geometry);
	dock->setParent(nullptr);
	dock->showFullScreen();
	if (config) {
		obs_data_set_bool(config, "windowed", false);
		obs_data_set_bool(config, "fullscreen", true);
		obs_data_set_int(config, "fullscreen_left", geometry.left());
		obs_data_set_int(config, "fullscreen_top", geometry.top());
		obs_data_set_int(config, "fullscreen_width", geometry.width());
		obs_data_set_int(config, "fullscreen_height", geometry.height());
	}
}

void DrawDock::EscapeTriggered()
{
	auto dock = (QDockWidget *)parent();
	if (!dock->isFullScreen())
		return;

	if (config)
		obs_data_set_bool(config, "fullscreen", false);
	auto main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	dock->setParent(main);
	dock->showNormal();
	if (!prevGeometry.isNull()) {
		if (dock->isFloating() != prevFloating)
			dock->setFloating(prevFloating);
		dock->setGeometry(prevGeometry);
		if (!prevFloating)
			main->addDockWidget(prevArea, dock);
	} else {
		if (!dock->isFloating())
			dock->setFloating(true);
		dock->resize(860, 530);
	}
}
