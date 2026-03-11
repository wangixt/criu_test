#include <errno.h>
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
