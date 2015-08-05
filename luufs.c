/*
 * this file is part of luufs.
 *
 * Copyright (c) 2014, 2015 Dima Krasner
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <dirent.h>
#include <stdio.h>
#include <stdarg.h>

#include <zlib.h>
#define FUSE_USE_VERSION (26)
#include <fuse.h>

#ifdef HAVE_WAIVE
#	include <waive.h>
#endif

#define DIRENT_MAX 255

struct luufs_ctx {
	uLong init;
	int (*openat)(int, const char *, int, ...);
	int (*unlinkat)(int, const char *, int);
	int (*fchownat)(int, const char *, uid_t, gid_t, int);
	int (*mkdirat)(int, const char *, mode_t);
	int (*mknodat)(int, const char *, mode_t, dev_t);
	int (*renameat)(int, const char *, int, const char *);
	int (*symlinkat)(const char *, int, const char *);
	int (*utimensat)(int, const char *, const struct timespec[2], int);
	int ro;
	int rw;
};

struct luufs_dir_ctx {
	DIR *dirs[2];
	int fds[2];
};

#define f_ro fds[0]
#define f_rw fds[1]

#define d_ro dirs[0]
#define d_rw dirs[1]

/* since luufs runs as root, no permission checks are performed (in other words:
 * the process that actually calls the *at() system calls is luufs, which runs
 * as root), so when the calling process runs as an unprivileged user, return
 * EPERM in errno */
#define LUUFS_CALL_HEAD()                                    \
	const struct luufs_ctx *ctx;                             \
	const struct fuse_context *fuse_ctx;                     \
	                                                         \
	fuse_ctx = fuse_get_context();                           \
	ctx = (const struct luufs_ctx *) fuse_ctx->private_data; \
	                                                         \
	if ((0 != fuse_ctx->uid) || (0 != fuse_ctx->gid))        \
		return -EPERM

static int luufs_open(const char *name, struct fuse_file_info *fi)
{
	struct stat stbuf;
	int fd;

	LUUFS_CALL_HEAD();

	/* when a file is opened for reading, prefer the read-only directory */
	if ((0 == (O_WRONLY & fi->flags)) && (0 == (O_RDWR & fi->flags))) {
		fd = ctx->openat(ctx->ro, &name[1], fi->flags);
		if (-1 != fd)
			goto ok;
		if (ENOENT != errno)
			return -errno;
	}

	/* return EROFS in errno if it's an attempt to overwrite a file under the
	 * read-only directory */
	if (0 == fstatat(ctx->ro,
	                 &name[1],
	                 &stbuf,
	                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EROFS;
	if (ENOENT != errno)
		return -errno;

	fd = ctx->openat(ctx->rw, &name[1], fi->flags);
	if (-1 == fd)
		return -errno;

ok:
	/* make sure the file descriptor does not exceed INT_MAX, to prevent
	 * truncation when casting the uint64_t back to int later */
	if (INT_MAX < fd) {
		(void) close(fd);
		return -EMFILE;
	}

	fi->fh = (uint64_t) fd;

	return 0;
}

static int luufs_create(const char *name,
                        mode_t mode,
                        struct fuse_file_info *fi)
{
	struct stat stbuf;
	int ret;
	int fd;

	LUUFS_CALL_HEAD();

	/* if the file already exists, return EEXIST in errno */
	if (0 == fstatat(ctx->ro,
	                 &name[1],
	                 &stbuf,
	                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) {
		ret = -EEXIST;
		goto out;
	}

	fd = ctx->openat(ctx->rw, &name[1], O_CREAT | O_EXCL | fi->flags, mode);
	if (-1 == fd) {
		ret = -errno;
		goto out;
	}

	if (INT_MAX < fd) {
		ret = -EMFILE;
		goto close_fd;
	}

	/* change the file owner, using the calling process credentials */
	if (-1 == fchown(fd, fuse_ctx->uid, fuse_ctx->gid)) {
		ret = -errno;
		goto close_fd;
	}

	fi->fh = (uint64_t) fd;

	return 0;

close_fd:
	(void) close(fd);

out:
	return ret;
}

static int luufs_close(const char *name, struct fuse_file_info *fi)
{
	int fd;

	fd = (int) fi->fh;
	if (-1 == fd)
		return -EBADF;

	if (0 == close(fd)) {
		fi->fh = -1;
		return 0;
	}

	return -errno;
}

static int luufs_truncate(const char *name, off_t size)
{
	struct stat stbuf;
	int fd;
	int ret;

	LUUFS_CALL_HEAD();

	/* if the file exists under the read-only directory, return EROFS in
	 * errno */
	if (0 == fstatat(ctx->ro,
	                 &name[1],
	                 &stbuf,
	                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) {
		ret = -EROFS;
		goto out;
	}

	/* try to open the file - if it's missing, create it */
	fd = ctx->openat(ctx->rw, &name[1], O_WRONLY);
	if (-1 == fd) {
		if (ENOENT == errno) {
			fd = ctx->openat(ctx->rw, &name[1], O_WRONLY | O_CREAT | O_EXCL);
			if (-1 != fd)
				goto trunc;
		}

		ret = -errno;
		goto out;
	}

trunc:
	ret = ftruncate(fd, size);
	if (0 != ret)
		ret = -errno;

	(void) close(fd);

out:
	return ret;
}

static int luufs_stat(const char *name, struct stat *stbuf)
{
	LUUFS_CALL_HEAD();

	/* try the read-only directory first */
	if (0 == fstatat(ctx->ro,
	                 &name[1],
	                 stbuf,
	                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return 0;
	if (ENOENT != errno)
		return -errno;

	if (0 == fstatat(ctx->rw,
	                 &name[1],
	                 stbuf,
	                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return 0;

	return -errno;
}

static int luufs_access(const char *name, int mask)
{
	struct stat stbuf;
	const char *namep;

	LUUFS_CALL_HEAD();

	if (0 == strcmp("/", name))
		namep = name;
	else
		namep = &name[1];

	if (0 != (F_OK & mask))
		return luufs_stat(namep, &stbuf);

	/* perform all access checks except W_OK on the read-only directory; musl
	 * does not support AT_SYMLINK_NOFOLLOW so we don't pass this flag */
	if (0 != (W_OK & mask)) {
		if (-1 == faccessat(ctx->rw, namep, mask, 0))
			return -errno;
	}
	else {
		if (-1 == faccessat(ctx->ro, namep, mask, 0))
			return -errno;
	}

	return 0;
}

static int luufs_read(const char *path,
                      char *buf,
                      size_t size,
                      off_t off,
                      struct fuse_file_info *fi)
{
	ssize_t ret;
	int fd;

	fd = (int) fi->fh;
	if (-1 == fd)
		return -EBADF;

	ret = pread(fd, buf, size, off);
	if (-1 == ret)
		return -errno;

	return (int) ret;
}

static int luufs_write(const char *path,
                       const char *buf,
                       size_t size,
                       off_t off,
                       struct fuse_file_info *fi)
{
	ssize_t ret;
	int fd;

	fd = (int) fi->fh;
	if (-1 == fd)
		return -EBADF;

	ret = pwrite(fd, buf, size, off);
	if (-1 == ret)
		return -errno;

	return (int) ret;
}

static int luufs_unlink(const char *name)
{
	struct stat stbuf;

	LUUFS_CALL_HEAD();

	/* if the file exists under the read-only directory, return EROFS in
	 * errno */
	if (0 == fstatat(ctx->ro,
	                 &name[1],
	                 &stbuf,
	                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EROFS;
	if (ENOENT != errno)
		return -errno;

	if (0 == ctx->unlinkat(ctx->rw, &name[1], 0))
		return 0;

	return -errno;
}

static int luufs_mkdir(const char *name, mode_t mode)
{
	struct stat stbuf;

	LUUFS_CALL_HEAD();

	/* if the directory exists under the read-only directory, return EEXIST in
	 * errno */
	if (0 == fstatat(ctx->ro,
	                 &name[1],
	                 &stbuf,
	                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EEXIST;
	if (ENOENT != errno)
		return -errno;

	if (-1 == ctx->mkdirat(ctx->rw, &name[1], mode))
		return -errno;

	if (-1 == ctx->fchownat(ctx->rw,
	                        &name[1],
	                        fuse_ctx->uid,
	                        fuse_ctx->gid,
	                        AT_SYMLINK_NOFOLLOW)) {
		(void) ctx->unlinkat(ctx->rw, &name[1], AT_REMOVEDIR);
		return -errno;
	}

	return 0;
}

static int luufs_rmdir(const char *name)
{
	struct stat stbuf;

	LUUFS_CALL_HEAD();

	/* if the directory exists under the read-only directory, return EROFS in
	 * errno */
	if (0 == fstatat(ctx->ro,
	                 &name[1],
	                 &stbuf,
	                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EROFS;
	if (ENOENT != errno)
		return -errno;

	if (0 == ctx->unlinkat(ctx->rw, &name[1], AT_REMOVEDIR))
		return 0;

	return -errno;
}

static int luufs_opendir(const char *name, struct fuse_file_info *fi)
{
	int ret;
	int cmp;
	struct luufs_dir_ctx *dir_ctx;

	LUUFS_CALL_HEAD();

	dir_ctx = malloc(sizeof(*dir_ctx));
	if (NULL == dir_ctx) {
		ret = -ENOMEM;
		goto end;
	}

	ctx = (const struct luufs_ctx *) fuse_get_context()->private_data;

	cmp = strcmp("/", name);
	if (0 == cmp)
		dir_ctx->f_ro = dup(ctx->ro);
	else
		dir_ctx->f_ro = ctx->openat(ctx->ro, &name[1], O_DIRECTORY);
	if (-1 == dir_ctx->f_ro) {
		if (ENOENT != errno) {
			ret = -errno;
			goto free_ctx;
		}
	}

	if (-1 == ctx->rw)
		dir_ctx->f_rw = -1;
	else {
		if (0 == cmp)
			dir_ctx->f_rw = dup(ctx->rw);
		else
			dir_ctx->f_rw = ctx->openat(ctx->rw, &name[1], O_DIRECTORY);
		if (-1 == dir_ctx->f_rw) {
			ret = -errno;
			goto close_ro;
		}
	}

	if (-1 == dir_ctx->f_ro)
		dir_ctx->d_ro = NULL;
	else {
		dir_ctx->d_ro = fdopendir(dir_ctx->f_ro);
		if (NULL == dir_ctx->d_ro) {
			ret = -errno;
			goto close_rw;
		}
	}

	if (-1 == dir_ctx->f_rw)
		dir_ctx->d_rw = NULL;
	else {
		dir_ctx->d_rw = fdopendir(dir_ctx->f_rw);
		if (NULL == dir_ctx->d_rw) {
			ret = -errno;
			goto close_rw;
		}
	}

	fi->fh = (uint64_t) (uintptr_t) dir_ctx;

	return 0;

	(void) closedir(dir_ctx->d_ro);
	dir_ctx->f_ro = -1;

close_rw:
	if (-1 != dir_ctx->f_rw)
		(void) close(dir_ctx->f_rw);

close_ro:
	if (-1 != dir_ctx->f_ro)
		(void) close(dir_ctx->f_ro);

free_ctx:
	free(dir_ctx);

end:
	return ret;
}

static int luufs_closedir(const char *name, struct fuse_file_info *fi)
{
	struct luufs_dir_ctx *dir_ctx;
	unsigned int i;

	dir_ctx = (struct luufs_dir_ctx *) (uintptr_t) fi->fh;
	if (NULL == dir_ctx)
		return -EBADF;

	for (i = 0; 2 > i; ++i) {
		if (NULL != dir_ctx->dirs[i]) {
			if (-1 == closedir(dir_ctx->dirs[i]))
				return -errno;
		}
	}

	free(dir_ctx);

	fi->fh = (uint64_t) (uintptr_t) NULL;

	return 0;
}

static int luufs_readdir(const char *path,
                         void *buf,
                         fuse_fill_dir_t filler,
                         off_t offset,
                         struct fuse_file_info *fi)
{
	uLong *crc;
	struct dirent ent;
	struct stat stbuf;
	struct luufs_dir_ctx *dir_ctx;
	const struct luufs_ctx *ctx;
	struct dirent *entp;
	unsigned int i;
	unsigned int j;
	unsigned int k;
	int ret;

	dir_ctx = (struct luufs_dir_ctx *) (uintptr_t) fi->fh;
	if (NULL == dir_ctx) {
		ret = -EBADF;
		goto end;
	}

	crc = malloc(DIRENT_MAX * sizeof(*crc));
	if (NULL == crc) {
		ret = -ENOMEM;
		goto end;
	}

	if (0 == offset) {
		for (i = 0; 2 > i; ++i) {
			if (NULL != dir_ctx->dirs[i])
				rewinddir(dir_ctx->dirs[i]);
		}
	}

	ctx = (const struct luufs_ctx *) fuse_get_context()->private_data;

	ret = 0;
	j = 0;
	for (i = 0; 2 > i; ++i) {
		if (NULL == dir_ctx->dirs[i])
			continue;

		do {
next:
			if (0 != readdir_r(dir_ctx->dirs[i], &ent, &entp)) {
				ret = -errno;
				goto free_crc;
			}
			if (NULL == entp) {
				ret = 0;
				break;
			}

			if (DIRENT_MAX == j) {
				ret = -ENOMEM;
				goto free_crc;
			}

			crc[j] = crc32(ctx->init,
			               (const Bytef *) entp->d_name,
			               strlen(entp->d_name));
			for (k = 0; j > k; ++k) {
				if (crc[k] == crc[j])
					goto next;
			}

			if (0 != fstatat(dir_ctx->fds[i],
			                 entp->d_name,
			                 &stbuf,
			                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) {
				ret = -errno;
				goto free_crc;
			}

			if (1 == filler(buf, entp->d_name, &stbuf, 0)) {
				ret = -ENOMEM;
				goto free_crc;
			}

			++j;
		} while (1);
	}

free_crc:
	free(crc);

end:
	return ret;
}

static int luufs_symlink(const char *to, const char *from)
{
	struct stat stbuf;

	LUUFS_CALL_HEAD();

	/* if the link source exists under the read-only directory, return EEXIST in
	 * errno */
	if (0 == fstatat(ctx->ro,
	                 &from[1],
	                 &stbuf,
	                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EEXIST;
	if (ENOENT != errno)
		return -errno;

	if (-1 == ctx->symlinkat(to, ctx->rw, &from[1]))
		return -errno;

	if (-1 == ctx->fchownat(ctx->rw,
	                   &from[1],
	                   fuse_ctx->uid,
	                   fuse_ctx->gid,
	                   AT_SYMLINK_NOFOLLOW)) {
		(void) ctx->unlinkat(ctx->rw, &from[1], AT_REMOVEDIR);
		return -errno;
	}

	return 0;
}

static int luufs_readlink(const char *name, char *buf, size_t size)
{
	int len;

	LUUFS_CALL_HEAD();

	len = readlinkat(ctx->ro, &name[1], buf, size - 1);
	if (-1 != len)
		goto nul;
	if (ENOENT != errno)
		return -errno;

	len = readlinkat(ctx->rw, &name[1], buf, size - 1);
	if (-1 == len)
		return -errno;

nul:
	buf[len] = '\0';

	return 0;
}

static int luufs_mknod(const char *name, mode_t mode, dev_t dev)
{
	struct stat stbuf;

	LUUFS_CALL_HEAD();

	/* if the device exists under the read-only directory, return EROFS in
	 * errno */
	if (0 == fstatat(ctx->ro,
	                 &name[1],
	                 &stbuf,
	                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EROFS;
	if (ENOENT != errno)
		return -errno;

	if (-1 == ctx->mknodat(ctx->rw,
	                  &name[1],
	                  mode,
	                  dev))
		return -errno;

	return 0;
}

static int luufs_chmod(const char *name, mode_t mode)
{
	struct stat stbuf;

	LUUFS_CALL_HEAD();

	/* if the file exists under the read-only directory, return EROFS in
	 * errno */
	if (0 == fstatat(ctx->ro,
	                 &name[1],
	                 &stbuf,
	                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EROFS;
	if (ENOENT != errno)
		return -errno;

	if (-1 == fchmodat(ctx->rw, &name[1], mode, AT_SYMLINK_NOFOLLOW))
		return -errno;

	return 0;
}

static int luufs_chown(const char *name, uid_t uid, gid_t gid)
{
	struct stat stbuf;

	LUUFS_CALL_HEAD();

	/* if the file exists under the read-only directory, return EROFS in
	 * errno */
	if (0 == fstatat(ctx->ro,
	                 &name[1],
	                 &stbuf,
	                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EROFS;
	if (ENOENT != errno)
		return -errno;

	if (-1 == ctx->fchownat(ctx->rw,
	                        &name[1],
	                        uid,
	                        gid,
	                        AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -errno;

	return 0;
}

static int luufs_utimens(const char *name, const struct timespec tv[2])
{
	struct stat stbuf;

	LUUFS_CALL_HEAD();

	/* if the file exists under the read-only directory, return EROFS in
	 * errno */
	if (0 == fstatat(ctx->ro,
	                 &name[1],
	                 &stbuf,
	                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EROFS;
	if (ENOENT != errno)
		return -errno;

	if (-1 == ctx->utimensat(ctx->rw, &name[1], tv, AT_SYMLINK_NOFOLLOW))
		return -errno;

	return 0;
}

static int luufs_rename(const char *oldpath, const char *newpath)
{
	struct stat stbuf;

	LUUFS_CALL_HEAD();

	/* if the file belongs to the read-only directory, return EROFS in errno */
	if (0 == fstatat(ctx->ro,
	                 &oldpath[1],
	                 &stbuf,
	                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EROFS;
	if (ENOENT != errno)
		return -errno;

	/* if the destination exists under the read-only directory, return EEXIST in
	 * errno */
	if (0 == fstatat(ctx->ro,
	                 &newpath[1],
	                 &stbuf,
	                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EEXIST;
	if (ENOENT != errno)
		return -errno;

	if (-1 == ctx->renameat(ctx->rw,
	                        &oldpath[1],
	                        ctx->rw,
	                        &newpath[1]))
		return -errno;

	return 0;
}

static struct fuse_operations luufs_oper = {
	.open		= luufs_open,
	.create		= luufs_create,
	.release	= luufs_close,

	.truncate	= luufs_truncate,

	.read		= luufs_read,
	.write		= luufs_write,

	.getattr	= luufs_stat,
	.access		= luufs_access,

	.unlink		= luufs_unlink,

	.mkdir		= luufs_mkdir,
	.rmdir		= luufs_rmdir,

	.opendir	= luufs_opendir,
	.releasedir	= luufs_closedir,
	.readdir	= luufs_readdir,

	.symlink	= luufs_symlink,
	.readlink	= luufs_readlink,

	.mknod		= luufs_mknod,

	.chmod		= luufs_chmod,
	.chown		= luufs_chown,
	.utimens	= luufs_utimens,
	.rename		= luufs_rename
};

static int mirror_dirs(const int src, const int dest) {
	struct stat stbuf;
	struct dirent ent;
	DIR *dir;
	struct dirent *entp;
	int nsrc;
	int ndest;
	int ret;

	dir = fdopendir(src);
	if (NULL == dir) {
		ret = -1;
		goto end;
	}

	do {
		if (0 != readdir_r(dir, &ent, &entp)) {
			ret = -1;
			break;
		}
		if (NULL == entp) {
			ret = 0;
			break;
		}

		if (DT_DIR != entp->d_type)
			continue;

		if ((0 == strcmp(".", entp->d_name)) ||
		    (0 == strcmp("..", entp->d_name)))
			continue;

		if (-1 == fstatat(src, entp->d_name, &stbuf, 0)) {
			ret = -1;
			break;
		}

		if (-1 == mkdirat(dest, entp->d_name, stbuf.st_mode)) {
			if (EEXIST != errno) {
				ret = -1;
				break;
			}
		}

		nsrc = openat(src, entp->d_name, O_DIRECTORY);
		if (-1 == nsrc) {
			ret = -1;
			break;
		}

		ndest = openat(dest, entp->d_name, O_DIRECTORY);
		if (-1 == ndest) {
			(void) close(nsrc);
			ret = -1;
			break;
		}

		if (-1 == mirror_dirs(nsrc, ndest)) {
			(void) close(ndest);
			(void) close(nsrc);
			ret = -1;
			break;
		}
	} while (1);

	(void) closedir(dir);

end:
	return ret;
}

static int openat_stub(int dirfd, const char *pathname, int flags, ...)
{
	va_list ap;
	int ret;

	if (0 != ((O_CREAT | O_WRONLY | O_RDWR) & flags))
		return -EROFS;

	va_start(ap, flags);
	ret = openat(dirfd, pathname, flags, va_arg(ap, mode_t));
	va_end(ap);
	return ret;
}

static int unlinkat_stub(int dirfd, const char *pathname, int flags)
{
	return -EROFS;
}

static int fchownat_stub(int dirfd,
                         const char *pathname,
                         uid_t owner,
                         gid_t group,
                         int flags)
{
	return -EROFS;
}

static int mkdirat_stub(int dirfd, const char *pathname, mode_t mode)
{
	return -EROFS;
}

static int mknodat_stub(int dirfd, const char *pathname, mode_t mode, dev_t dev)
{
	return -EROFS;
}

static int renameat_stub(int olddirfd,
                         const char *oldpath,
                         int newdirfd,
                         const char *newpath)
{
	return -EROFS;
}

static int symlinkat_stub(const char *oldpath,
                          int newdirfd,
                          const char *newpath)
{
	return -EROFS;
}

static int utimensat_stub(int dirfd,
                          const char *pathname,
                          const struct timespec times[2],
                          int flags)
{
	return -EROFS;
}

int main(int argc, char *argv[])
{
	char *fuse_argv[4];
	struct luufs_ctx ctx;
	int ret;
	int fd;

	if ((3 != argc) && (4 != argc)) {
		(void) fprintf(stderr, "Usage: %s RO [RW] TARGET\n", argv[0]);
		ret = EXIT_FAILURE;
		goto out;
	}

#ifdef HAVE_WAIVE
	if (-1 == waive(WAIVE_INET | WAIVE_PACKET | WAIVE_KILL)) {
		ret = EXIT_FAILURE;
		goto out;
	}
#endif

	/* open both directories, so we can pass their file descriptors to the
	 * *at() system calls later */
	ctx.ro = open(argv[1], O_DIRECTORY);
	if (-1 == ctx.ro) {
		ret = EXIT_FAILURE;
		goto out;
	}

	if (3 == argc) {
		ctx.rw = -1;
		fuse_argv[1] = argv[2];

		/* use stubs that fail with EROFS instead of real system calls that may
		 * alter the read-only directory */
		ctx.openat = openat_stub;
		ctx.unlinkat = unlinkat_stub;
		ctx.fchownat = fchownat_stub;
		ctx.mkdirat = mkdirat_stub;
		ctx.mknodat = mknodat_stub;
		ctx.renameat = renameat_stub;
		ctx.symlinkat = symlinkat_stub;
		ctx.utimensat = utimensat_stub;
	}
	else {
		ctx.rw = open(argv[2], O_DIRECTORY);
		if (-1 == ctx.rw) {
			ret = EXIT_FAILURE;
			goto close_ro;
		}

		/* mirror the read-only directory tree under the writeable directory */
		fd = dup(ctx.ro);
		if (-1 == fd) {
			ret = EXIT_FAILURE;
			goto close_ro;
		}
		ret = mirror_dirs(fd, ctx.rw);
		if (-1 == ret) {
			(void) close(fd);
			ret = EXIT_FAILURE;
			goto close_ro;
		}

		ctx.openat = openat;
		ctx.unlinkat = unlinkat;
		ctx.fchownat = fchownat;
		ctx.mkdirat = mkdirat;
		ctx.mknodat = mknodat;
		ctx.renameat = renameat;
		ctx.symlinkat = symlinkat;
		ctx.utimensat = utimensat;

		fuse_argv[1] = argv[3];
	}

	ctx.init = crc32(0L, Z_NULL, 0);
	fuse_argv[0] = argv[0];
	fuse_argv[2] = "-ononempty,suid,dev,allow_other,default_permissions";
	fuse_argv[3] = NULL;
	ret = fuse_main(3, fuse_argv, &luufs_oper, &ctx);

	if (-1 != ctx.rw)
		(void) close(ctx.rw);

close_ro:
	(void) close(ctx.ro);

out:
	return ret;
}
