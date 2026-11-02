// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_vfmig - tiny userspace helper for the /dev/mlx5_vfmig/<bdf> cdev.
 * Intended for development and triage; production users should speak the
 * ioctls from a real CRIU plugin.
 *
 * Build via the directory Makefile:
 *   make -C tools/testing/criu_rdma
 *
 * Use:
 *   mlx5_vfmig <pf-bdf> list
 *   mlx5_vfmig <pf-bdf> query_vf          <vf_id>
 *   mlx5_vfmig <pf-bdf> get_vhca_id       <vf_id>
 *   mlx5_vfmig <pf-bdf> set_tracked       <vf_id> <0|1>
 *   mlx5_vfmig <pf-bdf> enable_migratable <vf_id>
 *   mlx5_vfmig <pf-bdf> set_vf_uuid       <vf_id> <uuid-string>
 *   mlx5_vfmig <pf-bdf> clear_vf_uuid     <vf_id> <expected-uuid-string>
 *   mlx5_vfmig <pf-bdf> suspend_vhca      <vf_id> [initiator|responder|both]
 *   mlx5_vfmig <pf-bdf> resume_vhca       <vf_id> [initiator|responder|both]
 *   mlx5_vfmig <pf-bdf> save_vhca_state   <vf_id> <blob_path> [keep_suspended]
 *   mlx5_vfmig <pf-bdf> load_vhca_state   <vf_id> <blob_path>
 *   mlx5_vfmig <pf-bdf> mark_restored     <vf_id> [defer_resume]
 *
 * Verbs accept either '_' or '-' between words.
 *
 * Examples:
 *   mlx5_vfmig 0000:08:00.0 list
 *   mlx5_vfmig 0000:08:00.0 save_vhca_state 0 /tmp/vf.blob
 *   mlx5_vfmig 0000:08:00.0 load_vhca_state 1 /tmp/vf.blob
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../../include/uapi/linux/mlx5_vfmig.h"

static int do_enable_migratable(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_enable_migratable arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_ENABLE_MIGRATABLE, &arg) < 0) {
		perror("ENABLE_MIGRATABLE");
		return 1;
	}
	printf("vf %u: migratable cap enabled (call before driver bind)\n",
	       vf_id);
	return 0;
}

static int do_set_tracked(int fd, unsigned int vf_id, unsigned int enable)
{
	struct mlx5_vfmig_set_tracked arg = {
		.vf_id  = vf_id,
		.enable = enable ? 1 : 0,
	};

	if (ioctl(fd, MLX5_VFMIG_IOC_SET_TRACKED, &arg) < 0) {
		perror("SET_TRACKED");
		return 1;
	}
	printf("vf %u: vfmig tracked=%u (call before driver bind)\n",
	       vf_id, arg.enable);
	return 0;
}

/*
 * Parse a canonical RFC 4122 UUID string ("8-4-4-4-12" hex) into 16
 * bytes in network byte order, matching what the kernel stores and
 * what util-linux's libuuid produces. Returns 0 on success, -EINVAL on
 * parse failure.
 */
static int parse_uuid(const char *s, unsigned char out[16])
{
	static const int dash_at[] = { 8, 13, 18, 23 };
	unsigned int i, j, b;

	if (!s || strlen(s) != 36)
		return -EINVAL;
	for (i = 0; i < sizeof(dash_at) / sizeof(dash_at[0]); i++)
		if (s[dash_at[i]] != '-')
			return -EINVAL;

	b = 0;
	for (i = 0; i < 36; i++) {
		unsigned int hi = 0, lo = 0;

		if (s[i] == '-')
			continue;
		if (i + 1 >= 36 || s[i + 1] == '-')
			return -EINVAL;
		for (j = 0; j < 2; j++) {
			char c = s[i + j];
			unsigned int v;

			if (c >= '0' && c <= '9')
				v = c - '0';
			else if (c >= 'a' && c <= 'f')
				v = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F')
				v = c - 'A' + 10;
			else
				return -EINVAL;
			if (j == 0)
				hi = v;
			else
				lo = v;
		}
		if (b >= 16)
			return -EINVAL;
		out[b++] = (unsigned char)((hi << 4) | lo);
		i++;
	}
	return b == 16 ? 0 : -EINVAL;
}

/* Inverse of parse_uuid; writes 36 bytes + NUL. @buf must be >= 37. */
static void format_uuid(const unsigned char in[16], char *buf)
{
	snprintf(buf, 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 in[0], in[1], in[2], in[3],
		 in[4], in[5],
		 in[6], in[7],
		 in[8], in[9],
		 in[10], in[11], in[12], in[13], in[14], in[15]);
}

static int uuid_is_zero(const unsigned char in[16])
{
	static const unsigned char zero[16];

	return memcmp(in, zero, 16) == 0;
}

/* @flags 0 stamps @uuid_str; MLX5_VFMIG_SET_VF_UUID_F_CLEAR clears it. */
static int do_set_vf_uuid(int fd, unsigned int vf_id, const char *uuid_str,
			  unsigned int flags)
{
	struct mlx5_vfmig_set_vf_uuid arg = { .vf_id = vf_id, .flags = flags };
	int clear = flags & MLX5_VFMIG_SET_VF_UUID_F_CLEAR;

	if (parse_uuid(uuid_str, arg.vf_uuid)) {
		fprintf(stderr,
			"SET_VF_UUID: bad UUID %s (expected 8-4-4-4-12 hex)\n",
			uuid_str);
		return 1;
	}
	if (uuid_is_zero(arg.vf_uuid)) {
		fprintf(stderr,
			"SET_VF_UUID: refusing to send all-zeros UUID\n");
		return 1;
	}

	if (ioctl(fd, MLX5_VFMIG_IOC_SET_VF_UUID, &arg) < 0) {
		if (errno == EBUSY && clear)
			fprintf(stderr,
				"vf %u: holds a different UUID; nothing cleared\n",
				vf_id);
		else if (errno == EBUSY)
			fprintf(stderr,
				"vf %u: a different UUID is already stamped on this slot; "
				"clear_vf_uuid it first, or use the same UUID\n",
				vf_id);
		else if (errno == EEXIST)
			fprintf(stderr,
				"vf %u: another VF of this PF holds this UUID; "
				"clear_vf_uuid it there first\n",
				vf_id);
		else if (errno == ENOENT && clear)
			fprintf(stderr, "vf %u: no UUID stamped\n", vf_id);
		else
			perror("SET_VF_UUID");
		return 1;
	}
	printf("vf %u: vf_uuid %s\n", vf_id, clear ? "cleared" : "stamped");
	return 0;
}

static int do_mark(int fd, unsigned int vf_id, unsigned int defer_resume)
{
	struct mlx5_vfmig_mark_restored arg = {
		.vf_id = vf_id,
		.flags = defer_resume ?
			 MLX5_VFMIG_MARK_RESTORED_DEFER_RESUME : 0,
	};

	if (ioctl(fd, MLX5_VFMIG_IOC_MARK_RESTORED, &arg) < 0) {
		if (errno == EALREADY)
			fprintf(stderr,
				"vf %u is already marked restored\n", vf_id);
		else
			perror("MARK_RESTORED");
		return 1;
	}
	printf("marked vf %u as restored%s\n", vf_id,
	       defer_resume ? " (resume deferred to RESUME_VHCA)" : "");
	return 0;
}

/* flags == 0 means the fused pair, same as MLX5_VFMIG_DIR_FLAG_ALL. */
static const char *dir_label(unsigned int flags)
{
	unsigned int d = flags ? (flags & MLX5_VFMIG_DIR_FLAG_ALL) :
				 MLX5_VFMIG_DIR_FLAG_ALL;

	switch (d) {
	case MLX5_VFMIG_DIR_FLAG_INITIATOR: return "initiator";
	case MLX5_VFMIG_DIR_FLAG_RESPONDER: return "responder";
	case MLX5_VFMIG_DIR_FLAG_ALL:       return "both";
	default:                            return "?";
	}
}

/*
 * Accepts initiator/init, responder/resp, both/all, or a raw numeric
 * mask (so harnesses can drive invalid-bit negative tests).
 */
static int parse_dir(const char *s, unsigned int *out)
{
	char *end;
	unsigned long v;

	if (!strcmp(s, "initiator") || !strcmp(s, "init"))
		*out = MLX5_VFMIG_DIR_FLAG_INITIATOR;
	else if (!strcmp(s, "responder") || !strcmp(s, "resp"))
		*out = MLX5_VFMIG_DIR_FLAG_RESPONDER;
	else if (!strcmp(s, "both") || !strcmp(s, "all"))
		*out = MLX5_VFMIG_DIR_FLAG_ALL;
	else {
		v = strtoul(s, &end, 0);
		if (*end != '\0')
			return -1;
		*out = (unsigned int)v;
	}
	return 0;
}

static int do_suspend(int fd, unsigned int vf_id, unsigned int flags)
{
	struct mlx5_vfmig_suspend_vhca arg = { .vf_id = vf_id, .flags = flags };

	if (ioctl(fd, MLX5_VFMIG_IOC_SUSPEND_VHCA, &arg) < 0) {
		if (errno == EOPNOTSUPP)
			fprintf(stderr,
				"vf %u: not migration-enabled (run enable_migratable first)\n",
				vf_id);
		else
			perror("SUSPEND_VHCA");
		return 1;
	}
	printf("suspended vf %u datapath (dir=%s flags=0x%x)\n",
	       vf_id, dir_label(flags), flags);
	return 0;
}

static int do_resume(int fd, unsigned int vf_id, unsigned int flags)
{
	struct mlx5_vfmig_resume_vhca arg = { .vf_id = vf_id, .flags = flags };

	if (ioctl(fd, MLX5_VFMIG_IOC_RESUME_VHCA, &arg) < 0) {
		perror("RESUME_VHCA");
		return 1;
	}
	printf("resumed vf %u datapath (dir=%s flags=0x%x)\n",
	       vf_id, dir_label(flags), flags);
	return 0;
}

static int do_get(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_get_vhca_id arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_GET_VHCA_ID, &arg) < 0) {
		perror("GET_VHCA_ID");
		return 1;
	}
	printf("vf %u vhca_id 0x%04x\n", vf_id, arg.vhca_id);
	return 0;
}

/*
 * The kernel fills @out on success and on -ERANGE alike, so callers that
 * bail on -ERANGE can still read @num_vfs.
 */
static int query_one(int fd, unsigned int vf_id, struct mlx5_vfmig_query_vf *out)
{
	struct mlx5_vfmig_query_vf arg = { .vf_id = vf_id };
	int err;

	err = ioctl(fd, MLX5_VFMIG_IOC_QUERY_VF, &arg) ? -errno : 0;
	*out = arg;
	return err;
}

static int do_query(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_query_vf info;
	int err = query_one(fd, vf_id, &info);
	char ub[37];

	if (err == -ERANGE) {
		fprintf(stderr, "vf_id %u out of range; PF has %u VFs\n",
			vf_id, info.num_vfs);
		return 1;
	}
	if (err) {
		errno = -err;
		perror("QUERY_VF");
		return 1;
	}
	if (uuid_is_zero(info.vf_uuid))
		snprintf(ub, sizeof(ub), "<unset>");
	else
		format_uuid(info.vf_uuid, ub);
	printf("vf %u vhca_id 0x%04x restored=%u tracked=%u (num_vfs=%u) vf_uuid=%s\n",
	       vf_id, info.vhca_id, info.restored, info.tracked,
	       info.num_vfs, ub);
	return 0;
}

static int do_list(int fd)
{
	struct mlx5_vfmig_query_vf info;
	unsigned int i, n;
	int err;

	err = query_one(fd, 0, &info);
	if (err && err != -ERANGE) {
		errno = -err;
		perror("QUERY_VF");
		return 1;
	}
	n = info.num_vfs;
	if (n == 0) {
		printf("no VFs provisioned (sriov_numvfs == 0)\n");
		return 0;
	}

	printf("%-6s %-9s %-9s %-9s %s\n",
	       "vf_id", "vhca_id", "restored", "tracked", "vf_uuid");
	for (i = 0; i < n; i++) {
		char ub[37];

		err = query_one(fd, i, &info);
		if (err) {
			fprintf(stderr, "QUERY_VF vf %u: %s\n",
				i, strerror(-err));
			continue;
		}
		if (uuid_is_zero(info.vf_uuid))
			snprintf(ub, sizeof(ub), "<unset>");
		else
			format_uuid(info.vf_uuid, ub);
		printf("%-6u 0x%04x    %-9u %-9u %s\n",
		       i, info.vhca_id, info.restored, info.tracked, ub);
	}
	return 0;
}

/* Copy @in_fd to @out_fd until EOF; partial reads/writes are fine. */
static int copy_fd(int in_fd, int out_fd, size_t *out_bytes)
{
	enum { CHUNK = 1u << 16 };
	char *buf = malloc(CHUNK);
	size_t total = 0;

	if (!buf)
		return -ENOMEM;

	for (;;) {
		ssize_t r = read(in_fd, buf, CHUNK);
		ssize_t off = 0;

		if (r == 0)
			break;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			*out_bytes = total;
			return -errno;
		}
		while (off < r) {
			ssize_t w = write(out_fd, buf + off, r - off);

			if (w < 0) {
				if (errno == EINTR)
					continue;
				free(buf);
				*out_bytes = total;
				return -errno;
			}
			if (w == 0) {
				free(buf);
				*out_bytes = total;
				return -EIO;
			}
			off += w;
			total += w;
		}
	}

	free(buf);
	*out_bytes = total;
	return 0;
}

static int do_load(int fd, unsigned int vf_id, const char *blob_path)
{
	struct mlx5_vfmig_load_state arg = { .vf_id = vf_id };
	int blob_fd, load_fd, err;
	size_t bytes = 0;

	blob_fd = open(blob_path, O_RDONLY);
	if (blob_fd < 0) {
		fprintf(stderr, "open %s: %s\n", blob_path, strerror(errno));
		return 1;
	}

	if (ioctl(fd, MLX5_VFMIG_IOC_LOAD_VHCA_STATE, &arg) < 0) {
		perror("LOAD_VHCA_STATE");
		close(blob_fd);
		return 1;
	}
	load_fd = arg.load_fd;

	err = copy_fd(blob_fd, load_fd, &bytes);
	close(blob_fd);
	if (err) {
		fprintf(stderr, "vfmig: write failed after %zu bytes: %s\n",
			bytes, strerror(-err));
		close(load_fd);
		return 1;
	}

	/* close() hands the staged blob to the VF's pending-load slot. */
	if (close(load_fd) < 0) {
		perror("close(load_fd)");
		return 1;
	}
	printf("loaded %zu bytes of state into vf %u\n", bytes, vf_id);
	return 0;
}

static int do_save(int fd, unsigned int vf_id, const char *blob_path,
		   unsigned int flags)
{
	struct mlx5_vfmig_save_state arg = {
		.vf_id = vf_id,
		.flags = flags,
	};
	int blob_fd, save_fd, err;
	size_t bytes = 0;

	blob_fd = open(blob_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (blob_fd < 0) {
		fprintf(stderr, "open %s: %s\n", blob_path, strerror(errno));
		return 1;
	}

	if (ioctl(fd, MLX5_VFMIG_IOC_SAVE_VHCA_STATE, &arg) < 0) {
		perror("SAVE_VHCA_STATE");
		close(blob_fd);
		return 1;
	}
	save_fd = arg.save_fd;

	err = copy_fd(save_fd, blob_fd, &bytes);
	if (close(save_fd) < 0)
		perror("close(save_fd)");
	if (close(blob_fd) < 0)
		perror("close(blob_fd)");
	if (err) {
		fprintf(stderr, "vfmig: drain failed after %zu bytes: %s\n",
			bytes, strerror(-err));
		return 1;
	}
	printf("saved %zu bytes from vf %u to %s%s\n", bytes, vf_id, blob_path,
	       (flags & MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED) ?
		" (left suspended)" : "");
	return 0;
}

/* Treats '_' and '-' as equivalent. */
static int verb_eq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char ca = (*a == '-') ? '_' : *a;
		char cb = (*b == '-') ? '_' : *b;

		if (ca != cb)
			return 0;
	}
	return *a == 0 && *b == 0;
}

static int looks_like_bdf(const char *s)
{
	return strchr(s, ':') && !strchr(s, '/');
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s <pf-bdf|cdev-path> <verb> [args]\n"
		"  list\n"
		"  query_vf          <vf_id>\n"
		"  get_vhca_id       <vf_id>\n"
		"  set_tracked       <vf_id> <0|1>\n"
		"  enable_migratable <vf_id>\n"
		"  set_vf_uuid       <vf_id> <uuid-string>\n"
		"  clear_vf_uuid     <vf_id> <expected-uuid-string>\n"
		"  suspend_vhca      <vf_id> [initiator|responder|both]\n"
		"  resume_vhca       <vf_id> [initiator|responder|both]\n"
		"  save_vhca_state   <vf_id> <blob_path> [keep_suspended]\n"
		"  load_vhca_state   <vf_id> <blob_path>\n"
		"  mark_restored     <vf_id> [defer_resume]\n"
		"verbs accept '-' or '_' interchangeably\n",
		argv0);
}

int main(int argc, char **argv)
{
	unsigned int vf_id = 0;
	char path[256];
	const char *target;
	const char *verb;
	int fd, ret;

	if (argc < 3) {
		usage(argv[0]);
		return 2;
	}
	verb = argv[2];
	if (argc >= 4)
		vf_id = strtoul(argv[3], NULL, 0);

	if (looks_like_bdf(argv[1])) {
		snprintf(path, sizeof(path), "/dev/mlx5_vfmig/%s", argv[1]);
		target = path;
	} else {
		target = argv[1];
	}

	fd = open(target, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", target, strerror(errno));
		return 1;
	}

	if (verb_eq(verb, "list")) {
		if (argc != 3)
			goto badargs;
		ret = do_list(fd);
	} else if (verb_eq(verb, "query_vf")) {
		if (argc != 4)
			goto badargs;
		ret = do_query(fd, vf_id);
	} else if (verb_eq(verb, "get_vhca_id")) {
		if (argc != 4)
			goto badargs;
		ret = do_get(fd, vf_id);
	} else if (verb_eq(verb, "set_tracked")) {
		if (argc != 5)
			goto badargs;
		ret = do_set_tracked(fd, vf_id, strtoul(argv[4], NULL, 0));
	} else if (verb_eq(verb, "enable_migratable")) {
		if (argc != 4)
			goto badargs;
		ret = do_enable_migratable(fd, vf_id);
	} else if (verb_eq(verb, "set_vf_uuid")) {
		if (argc != 5)
			goto badargs;
		ret = do_set_vf_uuid(fd, vf_id, argv[4], 0);
	} else if (verb_eq(verb, "clear_vf_uuid")) {
		if (argc != 5)
			goto badargs;
		ret = do_set_vf_uuid(fd, vf_id, argv[4],
				     MLX5_VFMIG_SET_VF_UUID_F_CLEAR);
	} else if (verb_eq(verb, "suspend_vhca") || verb_eq(verb, "suspend")) {
		unsigned int dir = 0;

		if (argc != 4 && argc != 5)
			goto badargs;
		if (argc == 5 && parse_dir(argv[4], &dir))
			goto badargs;
		ret = do_suspend(fd, vf_id, dir);
	} else if (verb_eq(verb, "resume_vhca") || verb_eq(verb, "resume")) {
		unsigned int dir = 0;

		if (argc != 4 && argc != 5)
			goto badargs;
		if (argc == 5 && parse_dir(argv[4], &dir))
			goto badargs;
		ret = do_resume(fd, vf_id, dir);
	} else if (verb_eq(verb, "save_vhca_state")) {
		unsigned int flags = 0;

		if (argc != 5 && argc != 6)
			goto badargs;
		if (argc == 6) {
			if (!verb_eq(argv[5], "keep_suspended"))
				goto badargs;
			flags |= MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED;
		}
		ret = do_save(fd, vf_id, argv[4], flags);
	} else if (verb_eq(verb, "load_vhca_state")) {
		if (argc != 5)
			goto badargs;
		ret = do_load(fd, vf_id, argv[4]);
	} else if (verb_eq(verb, "mark_restored")) {
		unsigned int defer_resume = 0;

		if (argc != 4 && argc != 5)
			goto badargs;
		if (argc == 5) {
			if (!verb_eq(argv[4], "defer_resume"))
				goto badargs;
			defer_resume = 1;
		}
		ret = do_mark(fd, vf_id, defer_resume);
	} else {
		fprintf(stderr, "unknown verb: %s\n", verb);
		ret = 2;
	}

	close(fd);
	return ret;

badargs:
	close(fd);
	usage(argv[0]);
	return 2;
}
