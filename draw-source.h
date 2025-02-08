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

extern const char *image_filter;

#ifdef __cplusplus
}
#endif
