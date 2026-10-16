#pragma once
#include <stdio.h>
#define LOGE(fmt, ...) fprintf(stderr, "[%s] " fmt "\n", LOG_TAG __VA_OPT__(, ) __VA_ARGS__)
#define LOGW LOGE
#define LOGI LOGE
#define LOGD(...) ((void)0)
