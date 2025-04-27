#pragma once
#include "qt-display.hpp"
#include <obs-frontend-api.h>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QToolBar>

typedef std::function<bool(QObject *, QEvent *)> EventFilterFunc;

class OBSEventFilter : public QObject {
	Q_OBJECT
public:
	OBSEventFilter(EventFilterFunc filter_) : filter(filter_) {}

protected:
	bool eventFilter(QObject *obj, QEvent *event) { return filter(obj, event); }

public:
	EventFilterFunc filter;
};

class DrawDock : public QWidget {
	Q_OBJECT
private:
	OBSEventFilter *eventFilter;
	OBSQTDisplay *preview;
	obs_source_t *draw_source = nullptr;
	gs_vertbuffer_t *box = nullptr;

	obs_source_t *mouse_down_target = nullptr;

	QToolBar *toolbar;
	QComboBox *drawCombo;
	QComboBox *toolCombo;
	QAction *colorAction;
	QAction *imageAction;
	QDoubleSpinBox *toolSizeSpin;
	QDoubleSpinBox *alphaSpin;
	QCheckBox *eraseCheckbox;

	obs_data_t *config;
	std::map<obs_hotkey_id, std::pair<QAction *, obs_data_t *>> favoriteToolHotkeys;
	obs_hotkey_id clearHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_pair_id showHideHotkey = OBS_INVALID_HOTKEY_PAIR_ID;

	float zoom = 1.0f;
	float scrollX = 0.5f;
	float scrollY = 0.5f;
	int scrollingFromX = 0;
	int scrollingFromY = 0;

	bool tabletActive = false;

	QRect prevGeometry;
	bool prevFloating;
	Qt::DockWidgetArea prevArea;

	void *vendor;

	bool GetSourceRelativeXY(int mouseX, int mouseY, int &x, int &y);

	bool HandleMouseClickEvent(QMouseEvent *event);
	bool HandleMouseMoveEvent(QMouseEvent *event);
	bool HandleMouseWheelEvent(QWheelEvent *event);
	bool HandleFocusEvent(QFocusEvent *event);
	bool HandleKeyEvent(QKeyEvent *event);
	bool HandleTabletEvent(QTabletEvent *event);
	OBSEventFilter *BuildEventFilter();

	void DrawBackdrop(float cx, float cy);

	void CreateDrawSource(obs_source_t *source = nullptr);
	void DestroyDrawSource();

	void SaveConfig();

	void ClearDraw();

	QAction *AddFavoriteTool(obs_data_t *settings = nullptr);
	void ApplyFavoriteTool(obs_data_t *settings = nullptr);
	QIcon CreateToolIcon(obs_data_t *settings);
	QIcon CreateToolIcon(QColor toolColor, uint32_t tool, double alpha = 100.0, double toolSize = 20.0,
			     const char *image = nullptr);

	static void DrawPreview(void *data, uint32_t cx, uint32_t cy);
	static void frontend_event(enum obs_frontend_event event, void *data);
	static void draw_source_update(void *data, calldata_t *cd);
	static void draw_source_destroy(void *data, calldata_t *cd);
	static void source_create(void *data, calldata_t *cd);
	static void clear_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	static bool show_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed);
	static bool hide_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed);
	static void favorite_tool_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	static void vendor_request_version(obs_data_t *request_data, obs_data_t *response_data, void *);
	static void vendor_request_clear(obs_data_t *request_data, obs_data_t *response_data, void *);
	static void vendor_request_draw(obs_data_t *request_data, obs_data_t *response_data, void *);

private slots:
	void DrawSourceUpdate();
	void SceneChanged();
	void OpenFullScreenProjector();
	void EscapeTriggered();

public:
	void PostLoad();
	void FinishedLoad();
	DrawDock(QWidget *parent = nullptr);
	~DrawDock();
};
