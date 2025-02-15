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
	QDoubleSpinBox *toolSizeSpin;
	QDoubleSpinBox *alphaSpin;
	QCheckBox *eraseCheckbox;

	obs_data_t *config;
	std::map<obs_hotkey_id, std::pair<QAction *, obs_data_t *>> favoriteToolHotkeys;

	float zoom = 1.0f;
	float scrollX = 0.5f;
	float scrollY = 0.5f;
	int scrollingFromX = 0;
	int scrollingFromY = 0;

	bool GetSourceRelativeXY(int mouseX, int mouseY, int &x, int &y);

	bool HandleMouseClickEvent(QMouseEvent *event);
	bool HandleMouseMoveEvent(QMouseEvent *event);
	bool HandleMouseWheelEvent(QWheelEvent *event);
	bool HandleFocusEvent(QFocusEvent *event);
	bool HandleKeyEvent(QKeyEvent *event);
	OBSEventFilter *BuildEventFilter();

	void DrawBackdrop(float cx, float cy);

	void CreateDrawSource(obs_source_t *source = nullptr);
	void DestroyDrawSource();

	void SaveConfig();

	QAction *AddFavoriteTool(obs_data_t *settings = nullptr);
	void ApplyFavoriteTool(obs_data_t *settings = nullptr);
	QIcon CreateToolIcon(obs_data_t *settings);
	QIcon CreateToolIcon(QColor toolColor, uint32_t tool, double alpha = 100.0, double toolSize = 20.0);

	static void DrawPreview(void *data, uint32_t cx, uint32_t cy);
	static void frontend_event(enum obs_frontend_event event, void *data);
	static void draw_source_update(void *data, calldata_t *cd);
	static void draw_source_destroy(void *data, calldata_t *cd);
	static void source_create(void *data, calldata_t *cd);
	static void favorite_tool_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);

private slots:
	void DrawSourceUpdate();
	void SceneChanged();

public:
	DrawDock(QWidget *parent = nullptr);
	~DrawDock();
};
