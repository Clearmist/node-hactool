#include <stdio.h>
#include <napi.h>
#include "settings.h"

static void usage(void);

char *finish(hactool_ctx_t *tool_ctx);

char *stop(hactool_ctx_t *tool_ctx, char *text);

char *start(int argc, char **argv, Napi::Env Env);
