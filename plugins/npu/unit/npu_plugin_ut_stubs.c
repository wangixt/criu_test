#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <unistd.h>
#include "cr_options.h"

/* CRIU's xmalloc/xfree helpers: use libc equivalents in unit tests. */
void *xmalloc(size_t size)
{
	return malloc(size);
}

void *xzalloc(size_t size)
{
	return calloc(1, size);
}

void *xrealloc(void *p, size_t size)
{
	return realloc(p, size);
}

char *xstrdup(const char *s)
{
	return s ? strdup(s) : NULL;
}

void xfree(const void *p)
{
	free((void *)p);
}

/*
 * Failure injection for strdup() in unit tests.
 * Some branches in npu_plugin.c are only reachable when strdup() fails.
 */
char *__real_strdup(const char *s);
char *__wrap_strdup(const char *s)
{
	const char *e = getenv("NPU_UT_FAIL_STRDUP");

	if (e && e[0] == '1') {
		errno = ENOMEM;
		return NULL;
	}

	return __real_strdup(s);
}

/*
 * Make HwHiAiUser lookup deterministic in unit tests.
 *
 * scan_npu_devices() calls getpwnam_r("HwHiAiUser", ...).  On most hosts
 * this user doesn't exist, and scan_npu_devices() returns early, leaving an
 * empty device list.  Wrap getpwnam_r() so tests can simulate:
 *   NPU_UT_HW_USER_MODE=found    (default): return current uid/gid
 *   NPU_UT_HW_USER_MODE=notfound: (*result = NULL, return 0)
 *   NPU_UT_HW_USER_MODE=fail:    return EIO
 */
int __real_getpwnam_r(const char *name, struct passwd *pwd, char *buf,
		      size_t buflen, struct passwd **result);
int __wrap_getpwnam_r(const char *name, struct passwd *pwd, char *buf,
		      size_t buflen, struct passwd **result)
{
	const char *mode = getenv("NPU_UT_HW_USER_MODE");

	if (name && strcmp(name, "HwHiAiUser") == 0) {
		if (mode && strcmp(mode, "fail") == 0)
			return EIO;
		if (mode && strcmp(mode, "notfound") == 0) {
			*result = NULL;
			return 0;
		}

		/* default: found */
		if (!pwd || !buf || buflen < 32) {
			*result = NULL;
			return ERANGE;
		}

		memset(pwd, 0, sizeof(*pwd));
		snprintf(buf, buflen, "%s", "HwHiAiUser");
		pwd->pw_name = buf;
		pwd->pw_uid = getuid();
		pwd->pw_gid = getgid();
		*result = pwd;
		return 0;
	}

	return __real_getpwnam_r(name, pwd, buf, buflen, result);
}

/*
 * Used when opts.stream == 1.  Unit tests don't exercise streaming mode,
 * but we provide the symbol to satisfy the linker.
 */
int img_streamer_open(const char *path, int mode)
{
	(void)path;
	(void)mode;
	errno = ENOTSUP;
	return -1;
}

/*
 * Stub for CRIU's logging backend.  With the #ifdef NPU_PLUGIN_UT block
 * removed from npu_plugin.c, the standard pr_* macros (from log.h) are
 * used, which call print_on_level().  We provide it here so the UT binary
 * does not depend on the full CRIU logging subsystem.
 */
void print_on_level(unsigned int loglevel, const char *format, ...)
{
	static const char *const prefixes[] = {
		"",      /* LOG_MSG   (0) */
		"ERR: ", /* LOG_ERROR (1) */
		"WARN: ",/* LOG_WARN  (2) */
		"INFO: ",/* LOG_INFO  (3) */
		"DBG: ", /* LOG_DEBUG (4) */
	};
	va_list ap;

	if (loglevel < sizeof(prefixes) / sizeof(prefixes[0]))
		fprintf(stderr, "%s", prefixes[loglevel]);
	else
		fprintf(stderr, "[L%u] ", loglevel);

	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
}

unsigned int log_get_loglevel(void)
{
	return 4; /* LOG_DEBUG: show everything in unit tests */
}

/*
 * Stub for criu_get_image_dir() / npu_ut_set_img_dirfd().
 *
 * npu_plugin.c calls criu_get_image_dir() to obtain the image directory fd.
 * In unit tests we control this fd via npu_ut_set_img_dirfd().
 */
static int ut_img_dirfd = -1;

void npu_ut_set_img_dirfd(int fd)
{
	ut_img_dirfd = fd;
}

int criu_get_image_dir(void)
{
	return ut_img_dirfd;
}

/*
 * Stub for cr_system().
 *
 * In unit tests cr_system() does NOT execute any external commands.
 * The return value is controlled via the NPU_UT_CR_SYSTEM_RET env var:
 *   - set to a valid number string: return that number
 *   - not set or not a valid number: return 0 (success)
 */
int cr_system(int in, int out, int err, char *cmd, char *const argv[],
	      unsigned flags)
{
	const char *ret_str = getenv("NPU_UT_CR_SYSTEM_RET");
	(void)in;
	(void)out;
	(void)err;
	(void)cmd;
	(void)argv;
	(void)flags;

	if (ret_str) {
		char *end;
		long val = strtol(ret_str, &end, 10);

		if (end != ret_str && *end == '\0')
			return (int)val;
	}
	return 0;
}
