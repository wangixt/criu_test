#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>
#include "criu-plugin.h"
#include "../criu-npu.pb-c.h"
#include "cr_options.h"
struct cr_options opts;
#ifndef ND_TYPE__LOOPBACK
#define ND_TYPE__LOOPBACK 1
#endif
/* Match path used by npu_plugin_update_vmamap(); can be overridden via -DNPU_DEVICE="..." */
#ifndef NPU_DEVICE
#define NPU_DEVICE "/dev/davinci_manager"
#endif
/*
 * Avoid including <sys/mount.h> because CRIU's include paths can shadow
 * system headers and pull in compel-generated headers in a standalone build.
 */
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif
extern int mount(const char *source, const char *target, const char *fstype,
                 unsigned long flags, const void *data);
extern int umount2(const char *target, int flags);
/* From npu_plugin.c (built with -DNPU_PLUGIN_UT) */
extern int write_fp(FILE *fp, const void *buf, const size_t buf_len);
extern int read_fp(FILE *fp, void *buf, const size_t buf_len);
extern FILE *open_img_file(char *path, bool write, size_t *size);
extern int write_img_file(char *path, const void *buf, const size_t buf_len);
extern int npu_plugin_init(int stage);
extern void npu_plugin_fini(int stage, int ret);
extern int npu_plugin_dump_file(int fd, int id);
extern int npu_plugin_restore_file(int id);
extern int npu_plugin_handle_device_vma(int fd, const struct stat *st_buf);
extern int npu_plugin_update_vmamap(const char *in_path, const uint64_t addr,
                                    const uint64_t old_offset,
                                    uint64_t *new_offset, int *updated_fd);
extern int npu_plugin_update_inetsk(uint32_t id, uint32_t family,
                                    uint32_t state, uint32_t *src_ip,
                                    uint32_t *dst_ip);
extern int npu_plugin_resume_devices_late(int target_pid);
extern void npu_ut_set_img_dirfd(int fd);
#define UT_FAIL(...)                          \
        do {                                  \
                fprintf(stderr, __VA_ARGS__); \
                fprintf(stderr, "\n");        \
                return 1;                     \
        } while (0)
#define UT_ASSERT(expr)                                                   \
        do {                                                              \
                if (!(expr))                                              \
                        UT_FAIL("ASSERT failed: %s (%s:%d)", #expr,        \
                                __FILE__, __LINE__);                      \
        } while (0)
static int mk_tmpdir(char *out, size_t outsz, const char *tag)
{
        char tmpl[PATH_MAX];
        snprintf(tmpl, sizeof(tmpl), "/tmp/%s.XXXXXX", tag);
        if (!mkdtemp(tmpl))
                return -1;
        snprintf(out, outsz, "%s", tmpl);
        return 0;
}
static int mount_tmpfs(const char *dir)
{
        if (mount("tmpfs", dir, "tmpfs", 0, "mode=755") < 0)
                return -1;
        return 0;
}
static void umount_best_effort(const char *dir)
{
        (void)umount2(dir, MNT_DETACH);
}
static int make_chrdev(const char *path, int maj, int min, mode_t mode,
                       uid_t uid, gid_t gid);
/*
 * scan_npu_devices() in the plugin always scans "/dev". To keep unit tests
 * hermetic and avoid scanning/modifying the real host /dev, run tests inside
 * a private mount namespace and mount a tmpfs on /dev, then recreate the few
 * device nodes the tests rely on.
 *
 * This requires root/CAP_SYS_ADMIN (typical for CRIU UT runs).
 * The Makefile wraps execution with 'unshare --mount --propagation private'
 * to guarantee host isolation even if the inner MS_PRIVATE call below
 * does not take effect on this system's kernel configuration.
 */
#ifndef MS_REC
#define MS_REC 16384
#endif
#ifndef MS_PRIVATE
#define MS_PRIVATE 262144
#endif
static int devns_entered;
static int recreate_basic_dev_nodes(uid_t uid, gid_t gid)
{
        /* null(1,3) zero(1,5) full(1,7) */
        (void)unlink("/dev/null");
        (void)unlink("/dev/zero");
        (void)unlink("/dev/full");
        if (make_chrdev("/dev/null", 1, 3, 0666, uid, gid) < 0)
                return -1;
        if (make_chrdev("/dev/zero", 1, 5, 0666, uid, gid) < 0)
                return -1;
        if (make_chrdev("/dev/full", 1, 7, 0666, uid, gid) < 0)
                return -1;
        return 0;
}
static int mount_test_devfs(void)
{
        uid_t uid = getuid();
        gid_t gid = getgid();
        /* (Re)mount tmpfs on /dev */
        (void)umount2("/dev", MNT_DETACH);
        (void)mkdir("/dev", 0755);
        if (mount_tmpfs("/dev") < 0)
                return -1;
        return recreate_basic_dev_nodes(uid, gid);
}
static int enter_private_dev_namespace(void)
{
        if (devns_entered)
                return 0;
        if (unshare(CLONE_NEWNS) < 0) {
                perror("unshare(CLONE_NEWNS)");
                return -1;
        }
        if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
                perror("mount --make-rprivate /");
                return -1;
        }
        devns_entered = 1;
        return mount_test_devfs();
}
static int get_hw_user(uid_t *uid, gid_t *gid)
{
        struct passwd *pwd = getpwnam("HwHiAiUser");
        if (!pwd)
                return -1;
        *uid = pwd->pw_uid;
        *gid = pwd->pw_gid;
        return 0;
}
static int make_chrdev(const char *path, int maj, int min, mode_t mode,
                       uid_t uid, gid_t gid)
{
        if (mknod(path, S_IFCHR | mode, makedev(maj, min)) < 0)
                return -1;
        (void)chown(path, uid, gid);
        (void)chmod(path, mode);
        return 0;
}
static int write_img_bytes(const char *imgdir, int id, const void *data,
                           size_t data_len, size_t data_write_len)
{
        char path[PATH_MAX];
        int fd;
        snprintf(path, sizeof(path), "%s/davinci-dev-%d.img", imgdir, id);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0)
                return -1;
        /* Header is size_t payload length */
        if (write(fd, &data_len, sizeof(data_len)) != (ssize_t)sizeof(data_len)) {
                close(fd);
                return -1;
        }
        if (data_write_len) {
                if (write(fd, data, data_write_len) != (ssize_t)data_write_len) {
                        close(fd);
                        return -1;
                }
        }
        close(fd);
        return 0;
}
static int write_dfd_img(const char *imgdir, int id, int32_t org_fd,
                         int32_t flag, const char *fname,
                         bool truncate_body, bool force_unpack_fail)
{
        CriuDfd cd;
        uint8_t *buf = NULL;
        size_t len = 0;
        size_t write_len = 0;
        /*
         * To reliably hit "unpack == NULL" across both protobuf-c and our
         * minimal UT serializer, write an invalid protobuf-like stream:
         * a single varint continuation byte (0x80) without termination.
         */
        if (force_unpack_fail) {
                uint8_t bad = 0x80;
                return write_img_bytes(imgdir, id, &bad, 1, 1);
        }
        criu_dfd__init(&cd);
        cd.org_fd = org_fd;
        cd.flag = flag;
        cd.file_name = (char *)fname;
        len = criu_dfd__get_packed_size(&cd);
        buf = malloc(len);
        if (!buf)
                return -1;
        (void)criu_dfd__pack(&cd, buf);
        write_len = len;
        if (truncate_body && write_len)
                write_len--;
        int ret = write_img_bytes(imgdir, id, buf, len, write_len);
        free(buf);
        return ret;
}
static int run_in_child(int (*fn)(void))
{
        pid_t pid = fork();
        int st;
        if (pid < 0)
                return 1;
        if (pid == 0)
                exit(fn() ? 1 : 0);
        if (waitpid(pid, &st, 0) < 0)
                return 1;
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
                return 1;
        return 0;
}
static int ut_scan_opendir_fail(void)
{
        /*
         * Force scan_npu_devices() to hit opendir("/dev") failure path by
         * temporarily removing /dev in our private mount namespace.
         */
        UT_ASSERT(enter_private_dev_namespace() == 0);
        setenv("NPU_UT_HW_USER_MODE", "notfound", 1);
        unsetenv("SNAPSHOT_LINK_REMAP_DST");
        unsetenv("SNAPSHOT_LINK_REMAP_SRC");
        UT_ASSERT(npu_plugin_init(CR_PLUGIN_STAGE__RESTORE) == 0);
        setenv("NPU_UT_HW_USER_MODE", "found", 1);
        umount_best_effort("/dev");
        (void)rmdir("/dev");
        unsetenv("SNAPSHOT_LINK_REMAP_DST");
        unsetenv("SNAPSHOT_LINK_REMAP_SRC");
        (void)npu_plugin_init(CR_PLUGIN_STAGE__RESTORE);
        /* Restore a usable /dev for following tests */
        UT_ASSERT(mount_test_devfs() == 0);
        return 0;
}
static int ut_fp_io_errors(void)
{
        FILE *fp;
        char b[8];
        int ret;
        fp = fopen("/dev/full", "w");
        if (fp) {
                /* Make the error immediate, avoid stdio buffering surprises */
                setvbuf(fp, NULL, _IONBF, 0);
                ret = write_fp(fp, "abcd", 4);
                UT_ASSERT(ret == -EIO);
                fclose(fp);
        }
        fp = fopen("/dev/null", "r");
        if (fp) {
                (void)read_fp(fp, b, sizeof(b));
                fclose(fp);
        }
        return 0;
}
static int ut_open_img_file_error_paths(void)
{
        size_t sz = 123;
        FILE *fp;
        int dirfd;
        /* opts.stream -> img_streamer_open() -> fd < 0 */
        opts.stream = 1;
        fp = open_img_file("does-not-matter", true, &sz);
        UT_ASSERT(fp == NULL);
        opts.stream = 0;
        /* openat/criu_get_image_dir error: invalid dirfd */
        npu_ut_set_img_dirfd(-1);
        fp = open_img_file("x.img", true, &sz);
        UT_ASSERT(fp == NULL);
        /* write_fp/read_fp error through /dev/full and /dev/null */
        dirfd = open("/dev", O_RDONLY | O_DIRECTORY);
        UT_ASSERT(dirfd >= 0);
        npu_ut_set_img_dirfd(dirfd);
        fp = open_img_file("full", true, &sz);
        /*
         * /dev/full is a best-effort way to hit write error paths, but stdio
         * buffering may delay the error until fflush/fclose. Don't assert here.
         */
        if (fp)
                fclose(fp);
        fp = open_img_file("null", false, &sz);
        UT_ASSERT(fp == NULL);
        close(dirfd);
        return 0;
}
static int ut_write_img_file_open_fail_errno(void)
{
        char dummy = 0;
        int ret;
        /* NULL path branches */
        ret = write_img_file(NULL, &dummy, sizeof(dummy));
        UT_ASSERT(ret == -EINVAL);
        UT_ASSERT(open_img_file(NULL, true, (size_t *)&dummy) == NULL);
        npu_ut_set_img_dirfd(-1);
        errno = 0;
        ret = write_img_file("x.img", &dummy, sizeof(dummy));
        UT_ASSERT(ret < 0);
        return 0;
}
static int ut_misc_hook_funcs(void)
{
        struct stat st = { 0 };
        uint64_t new_off = 0;
        int ufd = -1;
        int ret;
        /* Cover trivial hook implementations */
        ret = npu_plugin_handle_device_vma(-1, &st);
        UT_ASSERT(ret == 0);
        ret = npu_plugin_resume_devices_late(getpid());
        UT_ASSERT(ret == 0);
        /* Cover UPDATE_VMA_MAP happy path and no-match path in the parent */
        unsetenv("NPU_TEST_FORCE_VMA");
        ret = npu_plugin_update_vmamap("/not/matching", 0, 0, &new_off, &ufd);
        UT_ASSERT(ret == 0);
        unsetenv("SNAPSHOT_LINK_REMAP_DST");
        unsetenv("SNAPSHOT_LINK_REMAP_SRC");
        setenv("NPU_UT_HW_USER_MODE", "found", 1);
        UT_ASSERT(npu_plugin_init(CR_PLUGIN_STAGE__RESTORE) == 0);
        ret = npu_plugin_update_vmamap(NULL, 0, 0, &new_off, &ufd);
        UT_ASSERT(ret == 0);
        ret = npu_plugin_update_vmamap(NPU_DEVICE, 0, 0, &new_off,
                                       &ufd);
        UT_ASSERT(ret == 1);
        UT_ASSERT(ufd >= 0);
        close(ufd);
        return 0;
}
static int ut_link_remap_branches(void)
{
        uid_t uid = 0;
        gid_t gid = 0;
        int r;
        unsetenv("SNAPSHOT_LINK_REMAP_DST");
        unsetenv("SNAPSHOT_LINK_REMAP_SRC");
        unsetenv("NPU_UT_CR_SYSTEM_RET");
        UT_ASSERT(enter_private_dev_namespace() == 0);
        if (get_hw_user(&uid, &gid) < 0) {
                uid = getuid();
                gid = getgid();
        }
        (void)unlink("/dev/davinci_manager");
        UT_ASSERT(make_chrdev("/dev/davinci_manager", 1, 7, 0660, uid, gid) == 0);
        /* Cover init restore (without env vars) */
        UT_ASSERT(npu_plugin_init(CR_PLUGIN_STAGE__RESTORE) == 0);
        /* Cover init restore link remap (with env vars) + cr_system stub */
        setenv("SNAPSHOT_LINK_REMAP_DST", "/tmp/npu-ut.dst.tar", 1);
        setenv("SNAPSHOT_LINK_REMAP_SRC", "/tmp/npu-ut-src/thing", 1);
        setenv("NPU_UT_CR_SYSTEM_RET", "1", 1);
        r = npu_plugin_init(CR_PLUGIN_STAGE__RESTORE);
        UT_ASSERT(r == 1);
        unsetenv("NPU_UT_CR_SYSTEM_RET");
        /* Cover the default-success path in UT cr_system() */
        setenv("NPU_UT_CR_SYSTEM_RET", "bad", 1);
        UT_ASSERT(npu_plugin_init(CR_PLUGIN_STAGE__RESTORE) == 0);
        unsetenv("NPU_UT_CR_SYSTEM_RET");
        /* Cover bname==NULL branch */
        setenv("SNAPSHOT_LINK_REMAP_SRC", "noslash", 1);
        r = npu_plugin_init(CR_PLUGIN_STAGE__RESTORE);
        UT_ASSERT(r == -EINVAL);
        /* Cover fini restore stage logging */
        npu_plugin_fini(CR_PLUGIN_STAGE__RESTORE, 0);
        /* Cover fini dump link remap */
        setenv("SNAPSHOT_LINK_REMAP_DST", "/tmp/npu-ut.dst.tar", 1);
        setenv("SNAPSHOT_LINK_REMAP_SRC", "/tmp/npu-ut-src/thing", 1);
        setenv("NPU_UT_CR_SYSTEM_RET", "1", 1);
        npu_plugin_fini(CR_PLUGIN_STAGE__DUMP, 0);
        unsetenv("NPU_UT_CR_SYSTEM_RET");
        /* Cover fini dump success path (free(tmpsrc) + final "finished" log) */
        setenv("SNAPSHOT_LINK_REMAP_DST", "/tmp/npu-ut.dst.tar", 1);
        setenv("SNAPSHOT_LINK_REMAP_SRC", "/tmp/npu-ut-src/thing", 1);
        unsetenv("NPU_UT_CR_SYSTEM_RET"); /* cr_system() stub -> 0 */
        npu_plugin_fini(CR_PLUGIN_STAGE__DUMP, 0);
        /* Cover early return when dst/src env vars are missing */
        unsetenv("SNAPSHOT_LINK_REMAP_DST");
        unsetenv("SNAPSHOT_LINK_REMAP_SRC");
        npu_plugin_fini(CR_PLUGIN_STAGE__DUMP, 0);
        unsetenv("SNAPSHOT_LINK_REMAP_DST");
        unsetenv("SNAPSHOT_LINK_REMAP_SRC");
        return 0;
}
static int ut_update_inetsk_more_branches(void)
{
        uint32_t src[4] = { 0 }, dst[4] = { 0 };
        int r;
        unsetenv("INETSK_LOCAL_IPV4_KEY");
        /* src_ip == NULL */
        r = npu_plugin_update_inetsk(1, AF_INET, 0, NULL, dst);
        UT_ASSERT(r == 0);
        /* AF_INET skip 127.0.0.1 */
        src[0] = 0x7f000001;
        r = npu_plugin_update_inetsk(1, AF_INET, 0, src, dst);
        UT_ASSERT(r == 0);
        /* AF_INET6 skip ::1 */
        src[0] = 0;
        src[1] = 0;
        src[2] = 0;
        src[3] = 1;
        r = npu_plugin_update_inetsk(1, AF_INET6, 0, src, dst);
        UT_ASSERT(r == 0);
        /* env missing -> 0 */
        src[0] = 0x01020304;
        r = npu_plugin_update_inetsk(1, AF_INET, 0, src, dst);
        UT_ASSERT(r == 0);
        /* invalid env -> handle_ipv4 error */
        setenv("INETSK_LOCAL_IPV4_KEY", "not_an_ip", 1);
        src[0] = 0x01020304;
        r = npu_plugin_update_inetsk(2, AF_INET, 0, src, dst);
        UT_ASSERT(r < 0);
        /* invalid env -> ipv4_to_ipv6_mapped inet_pton fail path */
        src[0] = 0x11111111;
        src[1] = 0x22222222;
        src[2] = 0x33333333;
        src[3] = 0x44444444;
        r = npu_plugin_update_inetsk(3, AF_INET6, 0, src, dst);
        UT_ASSERT(r < 0);
        /* loopback branch for dst updates */
        setenv("INETSK_LOCAL_IPV4_KEY", "10.9.8.7", 1);
        src[0] = 0x01020304;
        memset(dst, 0, sizeof(dst));
        r = npu_plugin_update_inetsk(4, AF_INET, ND_TYPE__LOOPBACK, src, dst);
        UT_ASSERT(r == 0);
        UT_ASSERT(dst[0] != 0);
        memset(src, 0x12, sizeof(src));
        memset(dst, 0, sizeof(dst));
        r = npu_plugin_update_inetsk(5, AF_INET6, ND_TYPE__LOOPBACK, src, dst);
        UT_ASSERT(r == 0);
        return 0;
}
static int ut_update_vmamap_memfd_create_fail(void)
{
        struct rlimit old, lim;
        int fds[256];
        int i, n = 0;
        uint64_t new_off = 0;
        int ufd = -1;
        int ret;
        npu_plugin_fini(CR_PLUGIN_STAGE__RESTORE, 0);
        getrlimit(RLIMIT_NOFILE, &old);
        lim = old;
        lim.rlim_cur = 32;
        setrlimit(RLIMIT_NOFILE, &lim);
        for (i = 0; i < 256; i++) {
                int fd = open("/dev/null", O_RDONLY);
                if (fd < 0)
                        break;
                fds[n++] = fd;
        }
        unsetenv("SNAPSHOT_LINK_REMAP_DST");
        unsetenv("SNAPSHOT_LINK_REMAP_SRC");
        setenv("NPU_UT_HW_USER_MODE", "found", 1);
        /* Try to init memfd under FD pressure (expected to fail internally). */
        (void)npu_plugin_init(CR_PLUGIN_STAGE__RESTORE);
        ret = npu_plugin_update_vmamap(NPU_DEVICE, 0, 0, &new_off,
                                       &ufd);
        UT_ASSERT(ret == -ENOTSUP);
        for (i = 0; i < n; i++)
                close(fds[i]);
        setrlimit(RLIMIT_NOFILE, &old);
        return 0;
}
static int ut_update_vmamap_dup_fail(void)
{
        struct rlimit old, lim;
        int fds[256];
        int i, n = 0;
        uint64_t new_off = 0;
        int ufd = -1;
        int ret;
        npu_plugin_fini(CR_PLUGIN_STAGE__RESTORE, 0);
        unsetenv("SNAPSHOT_LINK_REMAP_DST");
        unsetenv("SNAPSHOT_LINK_REMAP_SRC");
        setenv("NPU_UT_HW_USER_MODE", "found", 1);
        UT_ASSERT(npu_plugin_init(CR_PLUGIN_STAGE__RESTORE) == 0);
        /* First, create cached memfd */
        ret = npu_plugin_update_vmamap(NPU_DEVICE, 0, 0, &new_off,
                                       &ufd);
        UT_ASSERT(ret == 1);
        close(ufd);
        getrlimit(RLIMIT_NOFILE, &old);
        lim = old;
        lim.rlim_cur = 32;
        setrlimit(RLIMIT_NOFILE, &lim);
        for (i = 0; i < 256; i++) {
                int fd = open("/dev/null", O_RDONLY);
                if (fd < 0)
                        break;
                fds[n++] = fd;
        }
        ret = npu_plugin_update_vmamap(NPU_DEVICE, 0, 0, &new_off,
                                       &ufd);
        UT_ASSERT(ret == -1);
        for (i = 0; i < n; i++)
                close(fds[i]);
        setrlimit(RLIMIT_NOFILE, &old);
        return 0;
}
static int ut_recreate_and_restore_paths(void)
{
        char root[PATH_MAX], imgdir[PATH_MAX];
        uid_t uid = 0;
        gid_t gid = 0;
        int imgfd = -1;
        int fd;
        int ret;
        if (mk_tmpdir(root, sizeof(root), "npu-ut") < 0)
                return 1;
        snprintf(imgdir, sizeof(imgdir), "%s/img", root);
        UT_ASSERT(mkdir(imgdir, 0755) == 0);
        UT_ASSERT(enter_private_dev_namespace() == 0);
        /* Ensure scan_npu_devices will accept our node even if HwHiAiUser exists */
        if (get_hw_user(&uid, &gid) < 0) {
                uid = getuid();
                gid = getgid();
        }
        (void)unlink("/dev/davinci_manager");
        UT_ASSERT(make_chrdev("/dev/davinci_manager", 1, 5, 0660, uid, gid) == 0);
        unsetenv("SNAPSHOT_LINK_REMAP_DST");
        unsetenv("SNAPSHOT_LINK_REMAP_SRC");
        (void)npu_plugin_init(CR_PLUGIN_STAGE__RESTORE);
        /* Run a second time to cover list cleanup paths in scan_npu_devices(). */
        (void)npu_plugin_init(CR_PLUGIN_STAGE__RESTORE);
        /* Create mismatch to exercise recreate path */
        UT_ASSERT(unlink("/dev/davinci_manager") == 0);
        UT_ASSERT(make_chrdev("/dev/davinci_manager", 1, 7, 0660, uid, gid) == 0);
        imgfd = open(imgdir, O_RDONLY | O_DIRECTORY);
        UT_ASSERT(imgfd >= 0);
        npu_ut_set_img_dirfd(imgfd);
        /* Missing image -> -EINVAL (after recreate) */
        ret = npu_plugin_restore_file(999);
        UT_ASSERT(ret == -EINVAL || ret == -ENODEV);
        /* Truncated body -> read_fp error path */
        UT_ASSERT(write_dfd_img(imgdir, 1, 10, O_RDONLY, "/dev/zero", true,
                                false) == 0);
        ret = npu_plugin_restore_file(1);
        UT_ASSERT(ret < 0);
        /* Force unpack fail */
        UT_ASSERT(write_dfd_img(imgdir, 2, 10, O_RDONLY, "/dev/zero", false,
                                true) == 0);
        ret = npu_plugin_restore_file(2);
        UT_ASSERT(ret < 0);
        /* Empty file_name -> -ENOTSUP */
        UT_ASSERT(write_dfd_img(imgdir, 3, 10, O_RDONLY, "", false, false) ==
                  0);
        ret = npu_plugin_restore_file(3);
        UT_ASSERT(ret == -ENOTSUP);
        /* Non-existent path -> open fail branch */
        UT_ASSERT(write_dfd_img(imgdir, 4, 10, O_RDONLY, "/no/such/file", false,
                                false) == 0);
        ret = npu_plugin_restore_file(4);
        UT_ASSERT(ret < 0);
        /* Success restore */
        UT_ASSERT(write_dfd_img(imgdir, 5, 10, O_RDONLY, "/dev/zero", false,
                                false) == 0);
        ret = npu_plugin_restore_file(5);
        UT_ASSERT(ret >= 0);
        close(ret);
        /* stat(d->name) fail -> -ENODEV */
        UT_ASSERT(unlink("/dev/davinci_manager") == 0);
        ret = npu_plugin_restore_file(6);
        UT_ASSERT(ret == -ENODEV);
        /* dump_file fstat error */
        ret = npu_plugin_dump_file(-1, 1);
        UT_ASSERT(ret < 0);
        fd = open("/dev/zero", O_RDONLY);
        if (fd >= 0) {
                /* Force write_img_file() to fail */
                npu_ut_set_img_dirfd(-1);
                ret = npu_plugin_dump_file(fd, 123);
                UT_ASSERT(ret != 0);
                npu_ut_set_img_dirfd(imgfd);
                /* exercise dump success path */
                (void)npu_plugin_dump_file(fd, 7);
                close(fd);
        }
        close(imgfd);
        return 0;
}
int main(void)
{
        int total = 0, passed = 0;
        /* Ensure plugin scan/recreate logic uses an isolated /dev */
        if (enter_private_dev_namespace() != 0) {
                fprintf(stderr, "Failed to enter private mount namespace or mount /dev\n");
                return 1;
        }
#define RUN_TEST(name, fn)                                                   \
        do {                                                                 \
                int __rc;                                                    \
                total++;                                                    \
                __rc = (fn);                                                \
                if (__rc == 0) {                                            \
                        passed++;                                           \
                } else {                                                     \
                        fprintf(stderr, "TEST FAIL: %s\n", (name));         \
                        fprintf(stderr, "SUMMARY: %d/%d passed\n", passed,  \
                                total);                                     \
                        return 1;                                          \
                }                                                            \
        } while (0)
        RUN_TEST("scan_opendir_fail", ut_scan_opendir_fail());
        RUN_TEST("fp_io_errors", ut_fp_io_errors());
        RUN_TEST("open_img_file_error_paths", ut_open_img_file_error_paths());
        RUN_TEST("write_img_file_open_fail_errno", ut_write_img_file_open_fail_errno());
        RUN_TEST("link_remap_branches", ut_link_remap_branches());
        RUN_TEST("update_inetsk_more_branches", ut_update_inetsk_more_branches());
        RUN_TEST("update_vmamap_memfd_create_fail", run_in_child(ut_update_vmamap_memfd_create_fail));
        RUN_TEST("update_vmamap_dup_fail", run_in_child(ut_update_vmamap_dup_fail));
        RUN_TEST("misc_hook_funcs", ut_misc_hook_funcs());
        RUN_TEST("recreate_and_restore_paths", ut_recreate_and_restore_paths());
#undef RUN_TEST
        fprintf(stderr, "PASS (%d/%d)\n", passed, total);
        return 0;
}
