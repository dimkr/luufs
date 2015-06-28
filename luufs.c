#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>
#include <string.h>

#include <zlib.h>
#define FUSE_USE_VERSION (26)
#include <fuse.h>

#include "tree.h"

typedef struct {
	DIR *handle;
	char path[PATH_MAX];
} dir_t;

typedef struct {
	char name[1 + NAME_MAX];
	struct stat attributes;
	uLong hash;
} dir_entry_t;

typedef struct {
	dir_t ro;
	dir_t rw;
	dir_entry_t *entries;
	unsigned int entries_count;
} dir_pair_t;

static int luufs_open(const char *name, struct fuse_file_info *fi);

static const char *g_ro_directory = NULL;
static const char *g_rw_directory = NULL;
static const char *g_mount_point = NULL;

static void *luufs_init(struct fuse_conn_info *conn) {
	(void) tree_create("/", g_rw_directory, g_ro_directory);
	return NULL;
}

static int luufs_create(const char *name,
                        mode_t mode,
                        struct fuse_file_info *fi) {
	/* the file path */
	char path[PATH_MAX] = {'\0'};

	/* the file attributes */
	struct stat attributes = {0};

	/* the operation context */
	struct fuse_context *context = NULL;

	/* get the operation context */
	context = fuse_get_context();
	if (NULL == context) {
		return -ENOMEM;
	}

	/* make sure the file does not exist under the read-only directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_ro_directory, name);
	if (0 == lstat(path, &attributes)) {
		return -EEXIST;
	}
	if (ENOENT != errno) {
		return -errno;
	}

	/* create the file, under the writeable directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_rw_directory, name);
	fi->fh = creat(path, mode);
	if (-1 == fi->fh) {
		return -errno;
	}

	/* set the file owner */
	if (-1 == chown(path, context->uid, context->gid)) {
		(void) unlink(path);
		return -EACCES;
	}

	/* close the file */
	(void) close(fi->fh);

	/* open the newly created file with the requested flags */
	fi->flags &= ~O_CREAT;
	fi->flags &= ~O_EXCL;
	return luufs_open(name, fi);
}

static int luufs_truncate(const char *name, off_t size) {
	/* the file path */
	char path[PATH_MAX] = {'\0'};

	/* the file attributes */
	struct stat attributes = {0};

	/* disallow the operation if the file exists under the read-only
	 * directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_ro_directory, name);
	if (0 == lstat(path, &attributes)) {
		return -EROFS;
	}
	if (ENOENT != errno) {
		return -errno;
	}

	/* truncate the file, under the writeable directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_rw_directory, name);
	if (0 != truncate(path, size)) {
		return -errno;
	}
	return 0;
}

static int luufs_open(const char *name, struct fuse_file_info *fi) {
	/* the file path */
	char path[PATH_MAX] = {'\0'};

	/* the file attributes */
	struct stat attributes = {0};

	/* the file descriptor */
	int fd = (-1);

	(void) snprintf(path, sizeof(path), "%s/%s", g_ro_directory, name);

	/* when a file is opened for reading, try both directories but prefer the
	 * read-only one */
	if (O_RDONLY == (fi->flags & 3)) {
		fd = open(path, fi->flags);
		if (-1 != fd) {
			goto save_handle;
		}
		if (ENOENT != errno) {
			return -errno;
		}
	} else {
		/* otherwise, open the file under the writeable directory but ensure it
		 * does not exist under the read-only one */
		if (0 == lstat(path, &attributes)) {
			return -EROFS;
		}
		if (ENOENT != errno) {
			return -errno;
		}
	}

	(void) snprintf(path, sizeof(path), "%s/%s", g_rw_directory, name);
	fd = open(path, fi->flags);
	if (-1 == fd) {
		return -errno;
	}

save_handle:
	/* make sure the file descriptor does not exceed INT_MAX, to prevent
	 * truncation when casting uint64_t to int */
	if (INT_MAX <= fd) {
		(void) close(fd);
		return -EMFILE;
	}

	fi->fh = (uint64_t) fd;

	return 0;
}

static int luufs_access(const char *name, int mask) {
	/* the file path */
	char path[PATH_MAX] = {'\0'};

	/* try to access() the file under both directories; prefer the read-only
	 * one */
	(void) snprintf(path, sizeof(path), "%s/%s", g_ro_directory, name);
	if (0 == access(path, mask)) {
		return 0;
	}
	if (ENOENT != errno) {
		return -errno;
	}

	(void) snprintf(path, sizeof(path), "%s/%s", g_rw_directory, name);
	if (0 != access(path, mask)) {
		return -errno;
	}
	return 0;
}

static int luufs_stat(const char *name, struct stat *stbuf) {
	/* the file path */
	char path[PATH_MAX] = {'\0'};

	/* try to stat() the file, under both directories; prefer the read-only
	 * one */
	(void) snprintf(path, sizeof(path), "%s/%s", g_ro_directory, name);
	if (0 == lstat(path, stbuf)) {
		return 0;
	}
	if (ENOENT != errno) {
		return -errno;
	}

	(void) snprintf(path, sizeof(path), "%s/%s", g_rw_directory, name);
	if (0 != lstat(path, stbuf)) {
		return -errno;
	}
	return 0;
}

static int luufs_close(const char *name, struct fuse_file_info *fi) {
	int fd = (int) fi->fh;

	if (-1 == fd) {
		return -EBADF;
	}

	if (0 != close(fd)) {
		return -errno;
	}

	fi->fh = (uint64_t) (-1);
	return 0;
}

static int luufs_read(const char *path,
                      char *buf,
                      size_t size,
                      off_t off,
                      struct fuse_file_info *fi) {
	ssize_t return_value = (-1);
	int fd = (int) fi->fh;

	if (-1 == fd) {
		return -EBADF;
	}

	return_value = pread(fd, buf, size, off);
	if (-1 == return_value) {
		return -errno;
	}

	return (int) return_value;
}

static int luufs_write(const char *path,
                       const char *buf,
                       size_t size,
                       off_t off,
                       struct fuse_file_info *fi) {
	ssize_t return_value = (-1);
	int fd = (int) fi->fh;

	if (-1 == fd) {
		return -EBADF;
	}

	return_value = pwrite(fd, buf, size, off);
	if (-1 == return_value) {
		return -errno;
	}

	return (int) return_value;
}

static int luufs_mkdir(const char *name, mode_t mode) {
	/* the file path */
	char path[PATH_MAX] = {'\0'};

	/* the directory attributes */
	struct stat attributes = {0};

	/* the operation context */
	struct fuse_context *context = NULL;

	/* get the operation context */
	context = fuse_get_context();
	if (NULL == context) {
		return -ENOMEM;
	}

	/* make sure the directory does not under the read-only directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_ro_directory, name);
	if (0 == lstat(path, &attributes)) {
		return -EEXIST;
	}
	if (ENOENT != errno) {
		return -errno;
	}

	/* create the directory, under the writeable directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_rw_directory, name);
	if (-1 == mkdir(path, mode)) {
		return -errno;
	}

	/* set the directory owner */
	if (0 == chown(path, context->uid, context->gid)) {
		return 0;
	}

	/* delete the directory */
	(void) rmdir(path);

	return -EACCES;
}

static int luufs_rmdir(const char *name) {
	/* the file path */
	char path[PATH_MAX] = {'\0'};

	/* the directory attributes */
	struct stat attributes = {0};

	/* if the directory exists under the read-only directory, report failure */
	(void) snprintf(path, sizeof(path), "%s/%s", g_ro_directory, name);
	if (0 == stat(path, &attributes)) {
		return -EROFS;
	}
	if (ENOENT != errno) {
		return -errno;
	}

	/* remove the directory from the writeable directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_rw_directory, name);
	if (0 != rmdir(path)) {
		return -errno;
	}
	return 0;
}

static int luufs_unlink(const char *name) {
	/* the file path */
	char path[PATH_MAX] = {'\0'};

	/* the file attributes */
	struct stat attributes = {0};

	/* if the file exists under the read-only directory, report failure */
	(void) snprintf(path, sizeof(path), "%s/%s", g_ro_directory, name);
	if (0 == stat(path, &attributes)) {
		return -EROFS;
	}
	if (ENOENT != errno) {
		return -errno;
	}

	/* try to remove the file from the writeable directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_rw_directory, name);
	if (0 != unlink(path)) {
		return -errno;
	}

	return 0;
}

static int luufs_opendir(const char *name, struct fuse_file_info *fi) {
	/* the return value */
	int result = -ENOMEM;

	/* the directory pair */
	dir_pair_t *pair = NULL;

	/* initialize the directory handle */
	fi->fh = (uint64_t) (intptr_t) NULL;

	/* allocate memory for the directory pair */
	pair = malloc(sizeof(*pair));
	if (NULL == pair) {
		goto end;
	}

	/* open the writeable directory */
	(void) snprintf(pair->rw.path,
	                sizeof(pair->rw.path),
	                "%s/%s",
	                g_rw_directory,
	                name);
	pair->rw.handle = opendir(pair->rw.path);
	if (NULL != pair->rw.handle) {
		result = 0;
	} else {
		result = -errno;
	}

	/* open the read-only directory */
	(void) snprintf(pair->ro.path,
	                sizeof(pair->ro.path),
	                "%s/%s",
	                g_ro_directory,
	                name);
	pair->ro.handle = opendir(pair->ro.path);
	if (NULL != pair->ro.handle) {
		result = 0;
	} else {
		if (ENOENT != errno) {
			result = -errno;
		}

		/* if both directories could not be opened, free the directory pair */
		if (0 != result) {
			free(pair);
			return result;
		}
	}

	/* otherwise, initialize the list of files under the directory */
	pair->entries = NULL;
	pair->entries_count = 0;

	/* save the directory pair handle */
	fi->fh = (uint64_t) (intptr_t) pair;

end:
	return result;
}

static int luufs_closedir(const char *name, struct fuse_file_info *fi) {
	/* the directory pair */
	dir_pair_t *pair = (dir_pair_t *) (intptr_t) fi->fh;

	/* make sure the directory was opened */
	if (NULL == pair) {
		return -EBADF;
	}

	/* free the list of files */
	if (NULL != pair->entries) {
		free(pair->entries);
	}

	/* close the read-only directory */
	if (NULL !=pair->ro.handle) {
		(void) closedir(pair->ro.handle);
	}

	/* close the writeable directory */
	if (NULL != pair->rw.handle) {
		(void) closedir(pair->rw.handle);
	}

	/* free the allocated structure */
	free(pair);

	/* unset the handler, to make it possible to detect attempts to close a
	 * directory twice */
	fi->fh = (uint64_t) (intptr_t) NULL;

	return 0;
}

static int _read_directory(dir_t *directory,
                           dir_entry_t **entries,
                           unsigned int *entries_count) {
	/* a file path */
	char path[PATH_MAX] = {'\0'};

	/* the file attributes */
	struct stat attributes = {0};

	/* a file under the directory */
	struct dirent entry = {0};

	/* a loop index */
	unsigned int i = 0;

	/* the file name hash */
	uLong hash = 0L;

	/* the enlarged entries array */
	dir_entry_t *more_entries = NULL;

	/* the current entry */
	dir_entry_t *current_entry = NULL;

	/* the return value of readdir_r() */
	struct dirent *entry_pointer = NULL;

	/* upon failure to open the directory, report success */
	if (NULL == directory->handle) {
		goto end;
	}

	do {
next:
		/* read the name of one file under the directory */
		if (0 != readdir_r(directory->handle, &entry, &entry_pointer)) {
			return -errno;
		}
		if (NULL == entry_pointer) {
			break;
		}

		/* hash the file name */
		hash = crc32(crc32(0L, Z_NULL, 0),
		             (const Bytef *) entry_pointer->d_name,
		             (uInt) strnlen(entry_pointer->d_name, NAME_MAX));

		/* if there's another file with the same hash, continue to the next
		 * one */
		if (NULL != *entries) {
			for (i = 0; *entries_count > i; ++i) {
				if (hash == (*entries)[i].hash) {
					goto next;
				}
			}
		}

		/* get the file attributes */
		(void) snprintf(path,
		                sizeof(path),
		                "%s/%s",
		                directory->path,
		                entry_pointer->d_name);
		if (-1 == lstat(path, &attributes)) {
			continue;
		}

		/* enlarge the entries array */
		more_entries = realloc(*entries,
		                       sizeof(dir_entry_t) * (1 + *entries_count));
		if (NULL == more_entries) {
			return -ENOMEM;
		}

		/* add the file to the array */
		*entries = more_entries;
		current_entry = &(*entries)[*entries_count];
		current_entry->hash = hash;
		(void) strncpy(current_entry->name,
		               entry_pointer->d_name,
		               sizeof(current_entry->name) / sizeof(char));
		(void) memcpy(&current_entry->attributes,
		              &attributes,
		              sizeof(attributes));
		++(*entries_count);
	} while (1);

end:
	return 0;
}

static int luufs_readdir(const char *path,
                         void *buf,
                         fuse_fill_dir_t filler,
                         off_t offset,
                         struct fuse_file_info *fi) {
	/* the return value of _read_directory() */
	int result = (-1);

	/* the directory pair */
	dir_pair_t *pair = (dir_pair_t *) (intptr_t) fi->fh;

	/* make sure the directory was opened */
	if (NULL == pair) {
		return -EBADF;
	}

	if (0 < offset) {
		/* if the last file was reached, report success */
		if ((unsigned int) offset == pair->entries_count) {
			goto success;
		}

		/* if the offset is too big, report failure */
		if ((unsigned int) offset > pair->entries_count) {
			return -ENOMEM;
		}

		goto fetch;
	}

	/* if the offset is 0, rewinddir() was called and the list of files should
	 * be updated; first, empty the list of files */
	if (NULL != pair->entries) {
		free(pair->entries);
		pair->entries = NULL;
		pair->entries_count = 0;
	}

	/* then, list the files under both directories; start with the first file */
	if (NULL != pair->ro.handle) {
		rewinddir(pair->ro.handle);
		result = _read_directory(&pair->ro,
		                         &pair->entries,
		                         &pair->entries_count);
		if (0 != result) {
			return result;
		}
	}

	if (NULL != pair->rw.handle) {
		rewinddir(pair->rw.handle);
		result = _read_directory(&pair->rw,
		                         &pair->entries,
		                         &pair->entries_count);
		if (0 != result) {
			return result;
		}
	}

	/* if both directories are empty (which may happen if they do not contain .
	 * and ..), report success */
	if (0 == pair->entries_count) {
		goto success;
	}

fetch:
	/* fetch one entry */
	if (0 != filler(buf,
	                pair->entries[offset].name,
	                &pair->entries[offset].attributes,
	                1 + offset)) {
		return -ENOMEM;
	}

success:
	return 0;
}

static int luufs_symlink(const char *to, const char *from) {
	/* the link path */
	char path[PATH_MAX] = {'\0'};

	/* the link attributes */
	struct stat attributes = {0};

	/* if the link exists under the read-only directory, report failure */
	(void) snprintf(path, sizeof(path), "%s/%s", g_ro_directory, to);
	if (0 == lstat(path, &attributes)) {
		return -EROFS;
	}
	if (ENOENT != errno) {
		return -errno;
	}

	/* create the link, under the writeable directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_rw_directory, from);
	if (0 != symlink(to, path)) {
		return -errno;
	}

	return 0;
}

static int luufs_readlink(const char *name, char *buf, size_t size) {
	/* the link path */
	char path[PATH_MAX] = {'\0'};

	/* the path length */
	ssize_t length = (-1);

	/* read the link target, under the read-only directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_ro_directory, name);
	length = readlink(path, buf, (size - sizeof(char)));
	if (-1 != length) {
		goto terminate;
	}
	if (ENOENT != errno) {
		return -errno;
	}

	/* read the link target, under the writeable directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_rw_directory, name);
	length = readlink(path, buf, (size - sizeof(char)));
	if (-1 == length) {
		return -errno;
	}

terminate:
	/* terminate the path */
	buf[length] = '\0';

	return 0;
}

static int luufs_utimens(const char *name, const struct timespec tv[2]) {
	/* the file path */
	char path[PATH_MAX] = {'\0'};

	/* the file attributes */
	struct stat attributes = {0};

	/* if the file exists under the read-only directory, report failure */
	(void) snprintf(path, sizeof(path), "%s/%s", g_ro_directory, name);
	if (0 == lstat(path, &attributes)) {
		return -EROFS;
	}
	if (ENOENT != errno) {
		return -errno;
	}

	/* try to change the file modification time, in the writeable directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_rw_directory, name);
	if (0 != utimensat(0, path, tv, AT_SYMLINK_NOFOLLOW)) {
		return -errno;
	}

	return 0;
}

static int luufs_chmod(const char *name, mode_t mode) {
	/* the file path */
	char path[PATH_MAX] = {'\0'};

	/* the file attributes */
	struct stat attributes = {0};

	/* if the file exists under the read-only directory, report failure */
	(void) snprintf(path, sizeof(path), "%s/%s", g_ro_directory, name);
	if (0 == lstat(path, &attributes)) {
		return -EROFS;
	}
	if (ENOENT != errno) {
		return -errno;
	}

	/* try to change the file permissions, under the writeable directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_rw_directory, name);
	if (0 != chmod(path, mode)) {
		return -errno;
	}

	return 0;
}

static int luufs_chown(const char *name, uid_t uid, gid_t gid) {
	/* the file path */
	char path[PATH_MAX] = {'\0'};

	/* the file attributes */
	struct stat attributes = {0};

	/* if the file exists under the read-only directory, report failure */
	(void) snprintf(path, sizeof(path), "%s/%s", g_ro_directory, name);
	if (0 == lstat(path, &attributes)) {
		return -EROFS;
	}
	if (ENOENT != errno) {
		return -errno;
	}

	/* try to change the file owner, in the writeable directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_rw_directory, name);
	if (0 != chown(path, uid, gid)) {
		return -errno;
	}

	return 0;
}

static int luufs_rename(const char *oldpath, const char *newpath) {
	/* the original path */
	char source[PATH_MAX] = {'\0'};

	/* the new file path */
	char dest[PATH_MAX] = {'\0'};

	/* the file attributes */
	struct stat attributes = {0};

	/* if the file exists under the read-only directory, report failure */
	(void) snprintf(source, sizeof(source), "%s/%s", g_ro_directory, oldpath);
	if (0 == lstat(source, &attributes)) {
		return -EROFS;
	}
	if (ENOENT != errno) {
		return -errno;
	}

	/* try to move the file, in the writeable directory */
	(void) snprintf(source, sizeof(source), "%s/%s", g_rw_directory, oldpath);
	(void) snprintf(dest, sizeof(dest), "%s/%s", g_rw_directory, newpath);
	if (0 != rename(source, dest)) {
		return -errno;
	}

	return 0;
}

static int luufs_mknod(const char *name, mode_t mode, dev_t dev) {
	/* the device node path */
	char path[PATH_MAX] = {'\0'};

	/* the device node attributes */
	struct stat attributes = {0};

	/* make sure the device node does not exist under the read-only directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_ro_directory, name);
	if (0 == lstat(path, &attributes)) {
		return -EEXIST;
	}
	if (ENOENT != errno) {
		return -errno;
	}

	/* try to create the device node, in the writeable directory */
	(void) snprintf(path, sizeof(path), "%s/%s", g_rw_directory, name);
	if (0 != mknod(path, mode, dev)) {
		return -errno;
	}

	return 0;
}

static struct fuse_operations luufs_oper = {
	.init		= luufs_init,

	.access		= luufs_access,
	.getattr	= luufs_stat,

	.create		= luufs_create,
	.truncate	= luufs_truncate,
	.open		= luufs_open,
	.read		= luufs_read,
	.write		= luufs_write,
	.release	= luufs_close,
	.unlink		= luufs_unlink,

	.mkdir		= luufs_mkdir,
	.rmdir		= luufs_rmdir,

	.opendir	= luufs_opendir,
	.readdir	= luufs_readdir,
	.releasedir	= luufs_closedir,

	.symlink	= luufs_symlink,
	.readlink	= luufs_readlink,

	.utimens	= luufs_utimens,

	.chmod		= luufs_chmod,
	.chown		= luufs_chown,

	.rename		= luufs_rename,
	.mknod		= luufs_mknod
};

static int _parse_parameter(void *data,
                            const char *arg,
                            int key,
                            struct fuse_args *outargs) {
	if (FUSE_OPT_KEY_NONOPT != key) {
		return 1;
	}

	if (NULL == g_ro_directory) {
		g_ro_directory = realpath(arg, NULL);
		return 0;
	}

	if (NULL == g_rw_directory) {
		g_rw_directory = realpath(arg, NULL);
		return 0;
	}

	if (NULL == g_mount_point) {
		g_mount_point = realpath(arg, NULL);
		return 1;
	}

	return (-1);
}

int main(int argc, char *argv[]) {
	/* the exit code */
	int exit_code = EXIT_FAILURE;

	/* the command-pine arguments passed to FUSE */
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	/* the parse the command-line */
	if (-1 == fuse_opt_parse(&args, NULL, NULL, _parse_parameter)) {
		goto free_paths;
	}

	/* if not all arguments were passed, report failure */
	if (NULL == g_mount_point) {
		goto free_paths;
	}

	/* run FUSE */
	exit_code = fuse_main(args.argc, args.argv, &luufs_oper, NULL);

free_paths:
	/* free absolute the paths */
	if (NULL != g_mount_point) {
		free((void *) g_mount_point);
	}
	if (NULL != g_rw_directory) {
		free((void *) g_rw_directory);
	}
	if (NULL != g_ro_directory) {
		free((void *) g_ro_directory);
	}

	/* free the command-line arguments list */
	fuse_opt_free_args(&args);

	return exit_code;
}
