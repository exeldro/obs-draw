#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define TOOL_NONE 0
#define TOOL_PENCIL 1
#define TOOL_BRUSH 2
#define TOOL_LINE 3
#define TOOL_RECTANGLE_OUTLINE 4
#define TOOL_RECTANGLE_FILL 5
#define TOOL_ELLIPSE_OUTLINE 6
#define TOOL_ELLIPSE_FILL 7
#define TOOL_SELECT_RECTANGLE 8
#define TOOL_SELECT_ELLIPSE 9
#define TOOL_STAMP 10
#define TOOL_IMAGE 11

#define TOOL_UP 0
#define TOOL_DOWN 1
#define TOOL_DRAG 2

extern const char *image_filter;

#ifdef __cplusplus
}
#endif
