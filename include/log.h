#pragma once
#include <stdarg.h>
#include <time.h>
#include <stdio.h>

enum log_level { LOG_DEBUG=0, LOG_INFO=1, LOG_WARN=2, LOG_ERR=3 };
extern int g_log_level;

void log_at(enum log_level lvl, const char *fmt, ...);

#define LOGD(...) do { if (g_log_level <= LOG_DEBUG) log_at(LOG_DEBUG, __VA_ARGS__); } while (0)
#define LOGI(...) do { if (g_log_level <= LOG_INFO)  log_at(LOG_INFO,  __VA_ARGS__); } while (0)
#define LOGW(...) do { if (g_log_level <= LOG_WARN)  log_at(LOG_WARN,  __VA_ARGS__); } while (0)
#define LOGE(...) do { if (g_log_level <= LOG_ERR)   log_at(LOG_ERR,   __VA_ARGS__); } while (0)
