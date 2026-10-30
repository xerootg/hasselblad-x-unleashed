/* log.h — Simple leveled logging: stderr + file (truncate when rolling) */
#ifndef PRN_LOG_H
#define PRN_LOG_H

enum prn_log_level { PRN_LOG_DEBUG = 0, PRN_LOG_INFO, PRN_LOG_WARN, PRN_LOG_ERROR };

/* file==NULL writes only to stderr; output messages at or above the specified level */
void prn_log_init(const char *file, int level);
void prn_log_close(void);
void prn_log_raw(int level, const char *fmt, ...);

#define LOGD(...) prn_log_raw(PRN_LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) prn_log_raw(PRN_LOG_INFO,  __VA_ARGS__)
#define LOGW(...) prn_log_raw(PRN_LOG_WARN,  __VA_ARGS__)
#define LOGE(...) prn_log_raw(PRN_LOG_ERROR, __VA_ARGS__)

/* Map level name ↔ numeric value; returns -1 on failure */
int prn_log_level_parse(const char *name);

/* Recursively create directories (0 on success or if already present); provided by log.c,
 * reused by config.c */
int prn_mkdir_p(const char *path);

#endif
