#include "log.h"
#include <pthread.h>

int g_log_level = LOG_INFO;

void log_at(enum log_level lvl, const char *fmt, ...)
{
	static const char *names[] = { "DBG", "INF", "WRN", "ERR" };
	struct timespec tv;
	clock_gettime(CLOCK_REALTIME, &tv);
	struct tm tm;
	localtime_r(&tv.tv_sec, &tm);
	char ts[32];
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
	FILE *out = (lvl >= LOG_WARN) ? stderr : stdout;
	fprintf(out, "%s.%03ld [%s] ", ts, tv.tv_nsec / 1000000, names[lvl]);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(out, fmt, ap);
	va_end(ap);
	fputc('\n', out);
	fflush(out);
}
