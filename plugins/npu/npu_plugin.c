#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <linux/limits.h>
#include <linux/capability.h>

#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <pwd.h>

#include "criu-plugin.h"
#include "plugin.h"
#include "criu-npu.pb-c.h"
#include "images/netdev.pb-c.h"

#include "xmalloc.h"
#include "criu-log.h"
#include "files.h"
#include "pstree.h"
#include "util.h"

#include "common/list.h"

#include "img-streamer.h"
#include "image.h"
#include "cr_options.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifdef LOG_PREFIX
#undef LOG_PREFIX
#endif
#define LOG_PREFIX "npu_plugin: "

struct npu_device_info {
	char *name;
	int major;
	int minor;
	mode_t n_mode;
	uid_t n_uid;
	gid_t n_gid;
};

struct npu_device_node {
	struct npu_device_info info;
	struct list_head list;
};

static LIST_HEAD(npu_device_list);

static int scan_npu_devices(void)
{
	DIR *dir;
	struct dirent *dent;
	struct stat st;
	char path[PATH_MAX];
	int dir_len;
	struct passwd pwd_buf;
	struct passwd *pwd_result;
	char buf[1024];
	uid_t target_uid;
	int result = 0;
	pr_info("Scanning /dev for NPU devices...\n");

	int ret = getpwnam_r("HwHiAiUser", &pwd_buf, buf, sizeof(buf), &pwd_result);
	if (ret == 0) {
		if (pwd_result == NULL) {
			pr_info("HwHiAiUser not found, continuing without UID filtering");
			return 0;
		}
		target_uid = pwd_result->pw_uid;
		pr_info("Found HwHiAiUser with UID: %d", target_uid);
	} else {
		pr_err("getpwnam_r failed with error %d\n", ret);
		return -1;
	}

	if (!(dir = opendir("/dev"))) {
		pr_perror("Failed to open /dev directory");
		return -1;
	}

	struct npu_device_node *device, *tmp;
	list_for_each_entry_safe(device, tmp, &npu_device_list, list) {
		list_del(&device->list);
		free(device->info.name);
		free(device);
	}

	while ((dent = readdir(dir))) {
		if (dent->d_type != DT_CHR) continue;

		dir_len = snprintf(path, sizeof(path), "/dev/%s", dent->d_name);
		if (dir_len < 0) {
			pr_err("Encoding error in filename: %s", dent->d_name);
			continue;
		}
		if ((size_t)(dir_len) >= sizeof(path)) {
			pr_warn("Path too long: /dev/%s (truncated)", dent->d_name);
			// The path has been truncated, but processing continues for safety
			path[sizeof(path) - 1] = '\0';
		}

		if (stat(path, &st) < 0) {
			pr_perror("stat failed for %s", path);
			return -1;
		}
		
		if (target_uid != (uid_t)-1 && st.st_uid != target_uid) {
			continue;
		}

		// create new device node
		struct npu_device_node *node = xmalloc(sizeof(*node));
		if (!node) {
			pr_err("Failed to allocate memory for device node");
			result = -1;
			continue;
		}
		node->info.name = strdup(path);
		node->info.major = major(st.st_rdev);
		node->info.minor = minor(st.st_rdev);
		node->info.n_mode = st.st_mode;
		node->info.n_uid = st.st_uid;
		node->info.n_gid = st.st_gid;
		pr_info("%-20s: Maj=%d Min=%d Mode=%04o UID=%d\n",
			node->info.name,
			node->info.major,
			node->info.minor,
			node->info.n_mode & 07777,
			node->info.n_uid);

		list_add_tail(&node->list, &npu_device_list);
		
		pr_debug("Found NPU device: %s (Maj:%d Min:%d UID:%d)",
				path, major(st.st_rdev), minor(st.st_rdev), st.st_uid);
	}
	closedir(dir);
	return result;
}

int write_fp(FILE *fp, const void *buf, const size_t buf_len)
{
	size_t len_write;

	len_write = fwrite(buf, 1, buf_len, fp);
	if (len_write != buf_len) {
		pr_perror("Unable to write file (wrote:%ld buf_len:%ld)", len_write, buf_len);
		return -EIO;
	}
	return 0;
}

int read_fp(FILE *fp, void *buf, const size_t buf_len)
{
	size_t len_read;

	len_read = fread(buf, 1, buf_len, fp);
	if (len_read != buf_len) {
		pr_perror("Unable to read file (read:%ld buf_len:%ld)", len_read, buf_len);
		return -EIO;
	}
	return 0;
}

FILE *open_img_file(char *path, bool write, size_t *size)
{
	FILE *fp = NULL;
	int fd, ret;
	if (!path) {
		pr_err("open_img_file: NULL path pointer");
		return NULL;
	}

	if (opts.stream)
		fd = img_streamer_open(path, write ? O_DUMP : O_RSTR);
	else
		fd = openat(criu_get_image_dir(), path, write ? (O_WRONLY | O_CREAT) : O_RDONLY, 0600);

	if (fd < 0) {
		pr_perror("%s: Failed to open for %s", path, write ? "write" : "read");
		return NULL;
	}

	fp = fdopen(fd, write ? "w" : "r");
	if (!fp) {
		pr_perror("%s: Failed get pointer for %s", path, write ? "write" : "read");
		close(fd);
		return NULL;
	}

	if (write)
		ret = write_fp(fp, size, sizeof(*size));
	else
		ret = read_fp(fp, size, sizeof(*size));

	if (ret) {
		pr_perror("%s:Failed to access file size", path);
		fclose(fp);
		return NULL;
	}

	pr_debug("%s:Opened file for %s with size:%ld\n", path, write ? "write" : "read", *size);
	return fp;
}

int write_img_file(char *path, const void *buf, const size_t buf_len)
{
	int ret;
	FILE *fp;
	size_t len = buf_len;
	if (!path) {
		pr_err("write_img_file: NULL path pointer");
		return -EINVAL;
	}

	fp = open_img_file(path, true, &len);
	if (!fp)
		return -errno;

	ret = write_fp(fp, buf, buf_len);
	fclose(fp); /* this will also close fd */
	return ret;
}

#define LINK_REMAP_DST "SNAPSHOT_LINK_REMAP_DST"
#define LINK_REMAP_SRC "SNAPSHOT_LINK_REMAP_SRC"

int npu_plugin_init(int stage)
{
	pr_info("initialized: %s (Davinci NPU)\n", CR_PLUGIN_DESC.name);

	if (stage == CR_PLUGIN_STAGE__RESTORE) {

		int scan_result = scan_npu_devices();
		if (scan_result != 0) {
			pr_err("Failed to scan NPU devices: %d\n", scan_result);
			return -ENODEV;
		}
		char *dst = getenv(LINK_REMAP_DST);
		char *src = getenv(LINK_REMAP_SRC);
		if (dst == NULL || src == NULL) {
			return 0;
		}
		pr_info("restore link remap: %s %s", src, dst);
		char *tmpsrc = strdup(src);
		if (!tmpsrc) {
			pr_err("strdup failed for src: %s", src);
			return -ENOMEM;
		}

		char *bname = strrchr(tmpsrc, '/');
		if (bname == NULL) {
			pr_warn("No path separator in source: %s", src);
			free(tmpsrc);
			return -EINVAL;
		}

		*bname = '\0';
		int err = cr_system(-1, -1, -1, "tar",
						(char *[]){ "tar", "--extract", "--no-unquote", "--no-wildcards", "--sparse",
						"--file", dst, "--directory", tmpsrc, NULL }, 0);
		if (err) {
			pr_err("tar extract failed for %s (exit code: %d)\n", tmpsrc, err);
			free(tmpsrc);
			return err;
		}
		free(tmpsrc);
	}
	return 0;
}

void npu_plugin_fini(int stage, int ret)
{
	char *dst = NULL;
	char *src = NULL;
	if (stage == CR_PLUGIN_STAGE__RESTORE) {
		pr_info("restore stage for (Davinci NPU)\n");
		return;
	} else if (stage == CR_PLUGIN_STAGE__DUMP) {
		dst = getenv(LINK_REMAP_DST);
		src = getenv(LINK_REMAP_SRC);

		if (dst == NULL || src == NULL) {
			return;
		}

		char *tmpsrc = strdup(src);
		if (!tmpsrc) {
			pr_err("[npu-plugin fini-dump err] strdup failed for src: %s", src);
			return;
		}

		char *bname = strrchr(tmpsrc, '/');
		if (bname == NULL) {
			pr_err("[npu-plugin fini-dump err] No path separator in source: %s", src);
			free(tmpsrc);
			return;
		}

		*bname = '\0';
		bname = bname + 1;

		int err = cr_system(-1, -1, -1, "tar",
				(char *[]){"tar", "--create", "--no-unquote", "--no-wildcards",
							"--one-file-system", "--check-links", "--preserve-permissions", "--sparse",
							"--numeric-owner", "--file", dst, "--directory", tmpsrc, bname, NULL}, 0);
		if (err) {
			pr_err("[npu-plugin fini-dump err] tar extract failed for %s %s (exit code: %d)\n", tmpsrc, bname, err);
			free(tmpsrc);
			return;
		}
		free(tmpsrc);
	}
	pr_info("finished: %s (Davinci NPU) in %s, %s\n", CR_PLUGIN_DESC.name,
        dst ? dst : "unknown", src ? src : "unknown");
}
CR_PLUGIN_REGISTER("npu_plugin", npu_plugin_init, npu_plugin_fini)

int npu_plugin_handle_device_vma(int fd, const struct stat *st_buf)
{
	return 0;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__HANDLE_DEVICE_VMA, npu_plugin_handle_device_vma)

#define MAX_FILE_NAME_LEN 256

int get_file_name_by_fd(int fd, char *buf)
{
	char fn[MAX_FILE_NAME_LEN] = {0};
	snprintf(fn, MAX_FILE_NAME_LEN, "/proc/self/fd/%d", fd);

	ssize_t tt = readlink(fn, buf, MAX_FILE_NAME_LEN);
	if (tt == -1) {
		pr_err("get_file_name_by_fd readlink failed");
		return -1;
	}
	if (tt >= MAX_FILE_NAME_LEN) {
		pr_err("File path too long for fd %d, truncated\n", fd);
		buf[MAX_FILE_NAME_LEN - 1] = '\0'; 
		return -ENAMETOOLONG;
	}

	buf[tt] = '\0';
	return tt;
}

int get_fd_flags(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	if (flags == -1) {
		return -1;
	}
	return flags;
}

int npu_plugin_dump_file(int fd, int id)
{
	int ret = 0;
	char file_name_buf[MAX_FILE_NAME_LEN] = {0}; /* origin file name */
	char img_name_buf[MAX_FILE_NAME_LEN] = {0}; /* image file name */
	CriuDfd *cd = NULL;
	struct stat st;
	int flag = 0;
	unsigned char *pb_buf = NULL;
	pid_t pid = getpid();

	if (fstat(fd, &st) == -1) {
		pr_err("fstat error");
		return -1;
	}

	ret = get_file_name_by_fd(fd, file_name_buf);
	if (ret < 0) {
		pr_err("Failed to get file name for fd %d", fd);
		return -1;
	}
	pr_info("Processing device file: %s (fd: %d)", file_name_buf, fd);

	flag = get_fd_flags(fd);
	if (flag == -1) {
		pr_err("Failed to get flag for fd %d", fd);
		return -1;
	}
	pr_info("device file flag: %d (fd: %d)", flag, fd);

	cd = xmalloc(sizeof(*cd));
	if (cd == NULL) {
		pr_perror("out of memory");
		return -ENOMEM;
	}
	criu_dfd__init(cd);

	cd->org_fd = fd;
	cd->flag = flag;
	cd->file_name = file_name_buf;
	size_t len = criu_dfd__get_packed_size(cd);
	pb_buf = xmalloc(len);
	if (pb_buf == NULL) {
		xfree(cd);
		return -ENOMEM;
	}

	criu_dfd__pack(cd, pb_buf);
	snprintf(img_name_buf, sizeof(img_name_buf), "davinci-dev-%d.img", id);

	ret = write_img_file(img_name_buf, pb_buf, len);
	if (ret != 0) {
		pr_err("Failed to dump data (ret:%d)\n", ret);
	}

	xfree(cd);
	xfree(pb_buf);
	pr_info("NPU ===> dump file(%s : %d) with id: %d for %d.\n", img_name_buf, fd, id, pid);

	return ret;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__DUMP_EXT_FILE, npu_plugin_dump_file)

static int do_recreate_davinci_devs(void)
{
	struct stat fst;

	const char *lock_path = "/run/npu_device_recreate.lock";
	int ffd = open(lock_path, O_CLOEXEC | O_CREAT, 0600);
	if (ffd == -1) {
		pr_err("NPU: open flock %s failed\n", strerror(errno));
		return -1;
	}
	if (flock(ffd, LOCK_EX) != 0) {
		pr_err("NPU: flock failed: %s\n", strerror(errno));
		close(ffd);
		return -1;
	}

	pr_info("Recreating NPU devices...\n");
	int devices_recreated = 0;
	// recreate node
	struct npu_device_node *device;

	list_for_each_entry(device, &npu_device_list, list) {
		const struct npu_device_info *d = &device->info;

		if (stat(d->name, &fst) != 0) {
			pr_err("NPU: Device %s does not exist\n", d->name);
			flock(ffd, LOCK_UN);
			close(ffd);
			return -1;
		} else {
			if ((major(fst.st_rdev) == d->major) &&
				(minor(fst.st_rdev) == d->minor)) {
				pr_debug("NPU: Device %s already exists and matches\n", d->name);
				continue;
			}

			pr_info("NPU: Mismatch detected on %s: Current(%d,%d) Expected(%d,%d)\n",
					d->name,
					major(fst.st_rdev), minor(fst.st_rdev),
					d->major, d->minor);
		}

		if (unlink(d->name) != 0) {
			if (errno != ENOENT) {
				pr_err("NPU: unlink for %s", d->name);
				continue;
			}
		}

		if (mknod(d->name, S_IFCHR | d->n_mode, makedev(d->major, d->minor)) != 0) {
			pr_err("NPU: mknod for %s", d->name);
			continue;
		}

		if (chown(d->name, d->n_uid, d->n_gid) != 0) {
			pr_err("NPU: chown for %s", d->name);
		}

		if (chmod(d->name, d->n_mode) != 0) {
			pr_err("NPU: chmod for %s", d->name);
		}

		devices_recreated++;
		pr_info("NPU: Successfully recreated device: %s\n", d->name);
	}

	flock(ffd, LOCK_UN);
	close(ffd);

	pr_info("Recreated %d NPU devices\n", devices_recreated);
	return 0;
}

int npu_plugin_restore_file(int id)
{
	char name_buf[MAX_FILE_NAME_LEN] = {0};
	FILE *img_fp = NULL;
	CriuDfd *cd = NULL;
	size_t img_size;
	unsigned char *buf;
	int ret;
	int recreate_ret;
	int fd = -1;
	pid_t pid = getpid();

	recreate_ret = do_recreate_davinci_devs();
	if (recreate_ret != 0) {
		pr_err("Critical: Failed to recreate NPU devices (ret=%d). Cannot restore device.", recreate_ret);
		return -ENODEV;
	}

	snprintf(name_buf, sizeof(name_buf), "davinci-dev-%d.img", id);
	img_fp = open_img_file(name_buf, false, &img_size);
	if (img_fp == NULL) {
		return -EINVAL;
	}

	buf = xmalloc(img_size);
	if (!buf) {
		pr_err("Failed to allocate memory");
		fclose(img_fp);
		return -ENOMEM;
	}

	ret = read_fp(img_fp, buf, img_size);
	fclose(img_fp);
	if (ret != 0) {
		pr_err("Unable to read from %s", name_buf);
		xfree(buf);
		return -1;
	}

	cd = criu_dfd__unpack(NULL, img_size, buf);
	xfree(buf);
	if (cd == NULL) {
		pr_err("Unable to parse the davinci message %d", id);
		return -1;
	}

	if (!cd->file_name || cd->file_name[0] == '\0') {
		pr_err("file_name is empty/NULL in CriuDfd\n");
		fd = -ENOTSUP;
		goto out;
	}
	fd = open(cd->file_name, cd->flag);
	if (fd < 0) {
		pr_err("open(%s, 0x%x) failed: %s\n", cd->file_name, cd->flag, strerror(errno));
		goto out;
	}

	pr_info("%d NPU ===> restore %s file(%d --> %d) for %d.\n", id, cd->file_name, cd->org_fd, fd, pid);
out:
	criu_dfd__free_unpacked(cd, NULL);
	return fd;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__RESTORE_EXT_FILE, npu_plugin_restore_file)


#define NPU_DEVICE "/dev/davinci_manager"
#define TMP_MEM_DEVICE "npu_evasive"
/* return 0 if no match found
 * return -1 for error.
 * return 1 if vmap map must be adjusted.
 * TODO: NPU device driver should to handle vmamap
*/
int npu_plugin_update_vmamap(
const char *in_path, const uint64_t addr, const uint64_t old_offset, uint64_t *new_offset, int *updated_fd)
{
	if (!in_path) {
		pr_warn("npu_plugin_update_vmamap in_path is NULL\n");
		return 0;
	}
	if (strcmp(in_path, NPU_DEVICE) != 0)
		return 0;

	*new_offset = 0;
	*updated_fd = memfd_create(TMP_MEM_DEVICE, MFD_CLOEXEC);
	if (*updated_fd < 0) {
		pr_err("memfd_create failed: %s\n", strerror(errno));
		return -ENOTSUP;
	}

	pr_info("Handling %s addr=%#llx old_pgoff=%#llx -> new_pgoff=%#llx fd=%d\n",
			in_path, (unsigned long long)addr, (unsigned long long)old_offset,
			(unsigned long long)*new_offset, *updated_fd);
	return 1;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__UPDATE_VMA_MAP, npu_plugin_update_vmamap)

int npu_plugin_resume_devices_late(int target_pid)
{
	return 0;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__RESUME_DEVICES_LATE, npu_plugin_resume_devices_late)

#define INETSK_LOCAL_IPV4 "INETSK_LOCAL_IPV4_KEY"

static int ipv4_to_ipv6_mapped(const char *ipv4_str, struct in6_addr *result)
{
	struct in_addr ipv4_addr;
	memset(result, 0, sizeof(*result));
	if (inet_pton(AF_INET, ipv4_str, &ipv4_addr) <= 0) {
		pr_err("IPv6: inet_pton ip %s failed \n", ipv4_str);
		return -EINVAL;
	}
	// Set the prefix for IPv4-mapped IPv6 addresses ::ffff:
	result->s6_addr[10] = 0xff;
	result->s6_addr[11] = 0xff;

	const uint8_t *ipv4_bytes = (const uint8_t *)&ipv4_addr.s_addr;
	memcpy(&result->s6_addr[12], ipv4_bytes, 4);
	return 0;
}

static int handle_ipv4(const char *env_ip, uint32_t state, uint32_t *src_ip, uint32_t *dst_ip)
{
	struct in_addr new_ip;
	if (inet_pton(AF_INET, env_ip, &new_ip) <= 0) {
		pr_err("IPv4: inet_pton ip %s failed \n", env_ip);
		return -1;
	}

	src_ip[0] = new_ip.s_addr;
	// loopback ip
	if (state == ND_TYPE__LOOPBACK)
		dst_ip[0] = new_ip.s_addr;

	pr_info("IPv4 updated - src: %08x, dst: %08x \n", src_ip[0], (state == 1) ? dst_ip[0] : 0);
	return 0;
}

#define IPV6_WORD_COUNT 4

static int handle_ipv6(const char *env_ip, uint32_t state, uint32_t *src_ip, uint32_t *dst_ip)
{
	struct in6_addr mapped_ipv6;
	if (ipv4_to_ipv6_mapped(env_ip, &mapped_ipv6) < 0) {
		pr_err("IPv6-Mapping: Invalid IPv4 address '%s'\n", env_ip);
		return -EINVAL;
	}
	uint32_t *mapped_words = (uint32_t *)&mapped_ipv6;
	// Copy all IPv6 words to source IP array
	for (int i = 0; i < IPV6_WORD_COUNT; i++)
	src_ip[i] = mapped_words[i];

	pr_info("IPv6 updated - src: %08x:%08x:%08x:%08x\n", src_ip[0], src_ip[1], src_ip[2], src_ip[3]);
	// loopback ip
	if (state == ND_TYPE__LOOPBACK) {
		for (int i = 0; i < IPV6_WORD_COUNT; i++)
			dst_ip[i] = mapped_words[i];

		pr_info("            dst: %08x:%08x:%08x:%08x\n", dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);
	}
	return 0;
}

int npu_plugin_update_inetsk(uint32_t family, uint32_t state, uint32_t *src_ip, uint32_t *dst_ip) {
	if (src_ip == NULL)
		return 0;

	// Skip 127.0.0.1
	if (family == AF_INET) {
    	pr_info("Start change IPv4 src : %08x, state=%u\n", src_ip[0], state);
    	if (src_ip[0] == 0 || src_ip[0] == 0x7f000001) {
        	return 0;
    	}
	} else {
		pr_info("IPv6 src: %08x:%08x:%08x:%08x, state=%u\n",
				src_ip[0], src_ip[1], src_ip[2], src_ip[3], state);
		if ((src_ip[0] == 0) && (src_ip[1] == 0) && (src_ip[2] == 0) &&
			(src_ip[3] == 0 || src_ip[3] == 1)) {
			return 0;
		}
	}

	const char *env_ip = getenv(INETSK_LOCAL_IPV4);
	if (env_ip == NULL || strlen(env_ip) == 0) {
		pr_warn("No IPv4 ENV config\n");
		return 0;
	}

	pr_info("Updating IP using ENV: %s\n", env_ip);
	if (family == AF_INET) {
		return handle_ipv4(env_ip, state, src_ip, dst_ip);
	}
	return handle_ipv6(env_ip, state, src_ip, dst_ip);
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__UPDATE_INETSK, npu_plugin_update_inetsk)