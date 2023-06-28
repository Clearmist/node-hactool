#include "settings.h"

char *commandLineOptions();

char *interpolate(char *pattern, ...);

void addLogLine(hactool_ctx_t *tool_ctx, char *text);

void addWarning(hactool_ctx_t *tool_ctx, char *text);
