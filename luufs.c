#define FUSE_USE_VERSION (26)
#include <fuse.h>
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
#include "tree.h"
#include "crc32.h"
#include "config.h"

typedef struct {
	DIR *handle;
	char path[PATH_MAX];
} _dir_t;

typedef struct {
	_dir_t ro;
	_dir_t rw;
} _dir_pair_t;

static int luufs_stat(const char *name, struct stat *stbuf);

static void *luufs_init(struct fuse_conn_info *conn) {
	(void) tree_create("/");
	return NULL;
}

static void luufs_destroy(void *data) {
}

static int luufs_create(const char *name,
                        mode_t mode,
                        struct fuse_file_info *fi) {
	/* the file path */
	char path[PATH_MAX];

	/* the file attributes */
	struct stat attributes;

	/* make sure the file does not exist */
	if (-ENOENT != luufs_stat(name, &attributes)) {
		errno = EEXIST;
		goto failure;
	}

	/* create the file, under the writeable directory */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	fi->fh = creat((char *) &path, mode);
	if (-1 == fi->fh)
		goto failure;

	/* report success */
	return 0;

failure:
	return -errno;
}

static int luufs_truncate(const char *name, off_t size) {
	/* the file path */
	char path[PATH_MAX];

	/* truncate the file, under the writeable directory */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	if (0 == truncate((char *) &path, size))
		return 0;

	return -errno;
}

static int luufs_open(const char *name, struct fuse_file_info *fi) {
	/* the file path */
	char path[PATH_MAX];

	/* try to open the file, under both directories */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	fi->fh = open((char *) &path, fi->flags);
	if (-1 != fi->fh)
		return 0;
	else {
		if (ENOENT != errno)
			goto failure;
	}

	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_READ_ONLY_DIRECTORY,
	                name);
	fi->fh = open((char *) &path, fi->flags);
	if (-1 != fi->fh)
		return 0;

failure:
	return -errno;
}

static int luufs_access(const char *name, int mask) {
	/* the file path */
	char path[PATH_MAX];

	/* try to access() the file, under both directories */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	if (0 == access((char *) &path, mask))
		return 0;
	else {
		if (ENOENT != errno)
			goto end;
	}

	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_READ_ONLY_DIRECTORY,
	                name);
	if (0 == access((char *) &path, mask))
		return 0;

end:
	return -errno;
}

static int luufs_stat(const char *name, struct stat *stbuf) {
	/* the file path */
	char path[PATH_MAX];

	/* try to stat() the file, under both directories */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	if (0 == lstat((char *) &path, stbuf))
		return 0;
	else {
		if (ENOENT != errno)
			goto end;
	}

	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_READ_ONLY_DIRECTORY,
	                name);
	if (0 == lstat((char *) &path, stbuf))
		return 0;

end:
	return -errno;
}

static int luufs_close(const char *name, struct fuse_file_info *fi) {
	if (-1 == close(fi->fh))
		return -errno;

	return 0;
}

static int luufs_read(const char *path,
                      char *buf,
                      size_t size,
                      off_t off,
                      struct fuse_file_info *fi) {
	ssize_t return_value;

	return_value = pread(fi->fh, buf, size, off);
	if (-1 == return_value)
		return -errno;

	return (int) return_value;
}

static int luufs_write(const char *path,
                       const char *buf,
                       size_t size,
                       off_t off,
                       struct fuse_file_info *fi) {
	ssize_t return_value;

	return_value = pwrite(fi->fh, buf, size, off);
	if (-1 == return_value)
		return -errno;

	return (int) return_value;
}

static int luufs_mkdir(const char *name, mode_t mode) {
	/* the return value */
	int return_value = -EEXIST;

	/* the file path */
	char path[PATH_MAX];

	/* the directory attributes */
	struct stat attributes;

	/* make sure the directory does not exist */
	if (-ENOENT != luufs_stat(name, &attributes))
		goto end;

	/* create the directory, under the writeable directory */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	if (-1 == mkdir((char *) &path, mode)) {
		return_value = -errno;
		goto end;
	}

	/* report success */
	return_value = 0;

end:
	return return_value;
}

static int luufs_rmdir(const char *name) {
	/* the return value */
	int return_value;

	/* the file path */
	char path[PATH_MAX];

	/* the directory attributes */
	struct stat attributes;

	/* if the directory exists under the read-only directory, report failure
	 * immediately */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_READ_ONLY_DIRECTORY,
	                name);
	if (0 == stat((char *) &path, &attributes)) {
		return_value = -EPERM;
		goto end;
	} else {
		if (ENOENT != errno) {
			errno = -ENOMEM;
			goto end;
		}
	}

	/* try to remove the directory from the writeable directory */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	return_value = rmdir((char *) &path);
	if (-1 == return_value)
		return_value = -errno;

end:
	return return_value;
}

static int luufs_unlink(const char *name) {
	/* the return value */
	int return_value;

	/* the file path */
	char path[PATH_MAX];

	/* the file attributes */
	struct stat attributes;

	/* make sure the file exists */
	return_value = luufs_stat(name, &attributes);
	if (-ENOENT == return_value)
		goto end;

	/* try to remove the file from the writeable directory */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	if (0 == unlink((char *) &path))
		return 0;

	/* if the file exists only under the read-only directory, return EACCESS
	 * upon failure to delete it */
	if (ENOENT == errno)
		return_value = -EACCES;
	else
		return_value = -errno;

end:
	return return_value;
}

static int luufs_opendir(const char *name, struct fuse_file_info *fi) {
	/* the return value */
	int return_value = -ENOMEM;

	/* allocate memory for the directory pair */
	fi->fh = (uint64_t) (intptr_t) malloc(sizeof(_dir_pair_t));
	if (NULL == (void *) (intptr_t) fi->fh)
		goto end;

	/* open the writeable directory */
	(void) snprintf((char *) &(((_dir_pair_t *) (intptr_t) fi->fh)->rw.path),
	                sizeof(((_dir_t *) NULL)->path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	((_dir_pair_t *) (intptr_t) fi->fh)->rw.handle = opendir(
	                  (char *) &(((_dir_pair_t *) (intptr_t) fi->fh)->rw.path));
	if (NULL != ((_dir_pair_t *) (intptr_t) fi->fh)->rw.handle)
		return_value = 0;
	else
		return_value = -errno;

	/* open the read-only directory */
	(void) snprintf((char *) &(((_dir_pair_t *) (intptr_t) fi->fh)->ro.path),
	                sizeof(((_dir_t *) NULL)->path),
	                "%s/%s",
	                CONFIG_READ_ONLY_DIRECTORY,
	                name);
	((_dir_pair_t *) (intptr_t) fi->fh)->ro.handle = opendir(
	                  (char *) &(((_dir_pair_t *) (intptr_t) fi->fh)->ro.path));
	if (NULL != ((_dir_pair_t *) (intptr_t) fi->fh)->ro.handle)
		return_value = 0;

end:
	return return_value;
}

static int luufs_closedir(const char *name, struct fuse_file_info *fi) {
	/* the return value */
	int return_value = -EBADF;

	/* make sure the directory was opened */
	if (NULL == (void *) (intptr_t) fi->fh)
		goto end;

	/* close the read-only directory */
	if (NULL != ((_dir_pair_t *) (intptr_t) fi->fh)->ro.handle)
		(void) closedir(((_dir_pair_t *) (intptr_t) fi->fh)->ro.handle);

	/* close the writeable directory */
	if (NULL != ((_dir_pair_t *) (intptr_t) fi->fh)->rw.handle)
		(void) closedir(((_dir_pair_t *) (intptr_t) fi->fh)->rw.handle);

	/* free the allocated structure */
	free((void *) (intptr_t) fi->fh);
	fi->fh = (uint64_t) (intptr_t) NULL;

	/* report success */
	return_value = 0;

end:
	return return_value;
}

bool _add_hash(crc32_t **hashes, unsigned int *count, const crc32_t hash) {
	/* the return value */
	bool is_success = false;

	/* the enlarged hashes array */
	crc32_t *more_hashes;

	/* enlarge the hashes array */
	++(*count);
	more_hashes = realloc(*hashes, (sizeof(crc32_t) * (*count)));
	if (NULL == more_hashes)
		goto end;
	*hashes = more_hashes;

	/* append the hash to the array */
	(*hashes)[(*count) - 1] = hash;

	/* report success */
	is_success = true;

end:
	return is_success;
}

bool _hash_filler(const char *parent,
                  fuse_fill_dir_t filler,
                  void *buf,
                  crc32_t **hashes,
                  unsigned int *count,
                  const char *name) {
	/* the return value */
	bool is_success = false;

	/* the file name hash */
	crc32_t hash;

	/* a loop index */
	int i;

	/* the file path */
	char path[PATH_MAX];

	/* the file attributes */
	struct stat attributes;
	struct stat *attributes_pointer;

	/* hash the file name */
	hash = crc32_hash((const unsigned char *) name, strnlen(name, NAME_MAX));

	/* if the file was already listed, skip it */
	for (i = 0; (*count) > i; ++i) {
		if ((*hashes)[i] == hash)
			goto success;
	}

	/* get the file attributes */
	if (NULL == parent)
		attributes_pointer = NULL;
	else {
		(void) snprintf((char *) &path, sizeof(path), "%s/%s", parent, name);
		if (-1 == lstat((char *) &path, &attributes))
			goto end;
		attributes_pointer = &attributes;
	}

	/* add the hash to the array */
	if (false == _add_hash(hashes, count, hash))
		goto end;

	/* add the file to the result */
	if (0 != filler(buf, name, attributes_pointer, 0))
		goto end;

success:
	/* report success */
	is_success = true;

end:
	return is_success;
}

int _readdir_single(_dir_t *directory,
                    crc32_t **hashes,
                    unsigned int *hashes_count,
                    fuse_fill_dir_t filler,
                    void *buf) {
	/* the return value */
	int return_value;

	/* a file under the directory */
	struct dirent entry;
	struct dirent *entry_pointer;

	/* upon failure to open the directory, report success */
	if (NULL == directory->handle)
		goto success;

	do {
		if (0 != readdir_r(directory->handle, &entry, &entry_pointer)) {
			return_value = -errno;
			goto end;
		} else {
			if (NULL == entry_pointer)
				break;
		}
		if (false == _hash_filler((char *) &directory->path,
		                          filler,
		                          buf,
		                          hashes,
		                          hashes_count,
		                          entry_pointer->d_name)) {
			return_value = -ENOMEM;
			goto end;
		}
	} while (1);

success:
	/* report success */
	return_value = 0;

end:
	return return_value;
}

static int luufs_readdir(const char *path,
                         void *buf,
                         fuse_fill_dir_t filler,
                         off_t offset,
                         struct fuse_file_info *fi) {
	/* the return value */
	int return_value = -EBADF;

	/* file name hashes */
	crc32_t *hashes;
	unsigned int hashes_count;

	/* make sure the directory was opened */
	if (NULL == (void *) (intptr_t) fi->fh)
		goto end;

	hashes = NULL;
	hashes_count = 0;

	/* list the files under the writeable directory */
	return_value = _readdir_single(&(((_dir_pair_t *) (intptr_t) fi->fh)->rw),
	                               &hashes,
	                               &hashes_count,
	                               filler,
	                               buf);
	if (0 != return_value)
		goto free_hashes;

	/* list the files under the read-only directory */
	return_value = _readdir_single(&(((_dir_pair_t *) (intptr_t) fi->fh)->ro),
	                               &hashes,
	                               &hashes_count,
	                               filler,
	                               buf);

free_hashes:
	/* free the hashes list */
	if (NULL != hashes)
		free(hashes);

end:
	return return_value;
}

static int luufs_symlink(const char *to, const char *from) {
	/* the link path */
	char path[PATH_MAX];

	/* the link attributes */
	struct stat attributes;

	/* make sure the link does not exist */
	if (-ENOENT != luufs_stat(from, &attributes)) {
		errno = EEXIST;
		goto failure;
	}

	/* create the link, under the writeable directory */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                from);
	if (0 == symlink(to, (char *) &path))
		return 0;

failure:
	return -errno;
}

static int luufs_readlink(const char *name, char *buf, size_t size) {
	/* the link path */
	char path[PATH_MAX];

	/* the path length */
	ssize_t length;

	/* read the link target, under the writeable directory */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	length = readlink((char *) &path, buf, (size - sizeof(char)));
	if (-1 == length) {
		/* upon failure to read the link target - if the link exists, report
		 * failure */
		if (ENOENT != errno)
			goto failure;

		/* read the link target, under the read-only directory */
		(void) snprintf((char *) &path,
		                sizeof(path),
		                "%s/%s",
		                CONFIG_READ_ONLY_DIRECTORY,
		                name);
		length = readlink((char *) &path, buf, (size - sizeof(char)));
		if (-1 == length)
			goto failure;
	}

	/* terminate the path */
	buf[length] = '\0';

	/* report success */
	return 0;

failure:
	return -errno;
}

static int luufs_utimens(const char *name, const struct timespec tv[2]) {
	/* the return value */
	int return_value;

	/* the file path */
	char path[PATH_MAX];

	/* the file attributes */
	struct stat attributes;

	/* make sure the file exists */
	return_value = luufs_stat(name, &attributes);
	if (-ENOENT == return_value)
		goto end;

	/* try to change the file modification time, in the writeable directory */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	if (0 == utimensat(0, (char *) &path, tv, AT_SYMLINK_NOFOLLOW))
		return 0;

	/* if the file exists only under the read-only directory, return EACCESS
	 * upon failure to change its modification time */
	if (ENOENT == errno)
		return_value = -EACCES;
	else
		return_value = -errno;

end:
	return return_value;
}

static int luufs_chmod(const char *name, mode_t mode) {
	/* the return value */
	int return_value;

	/* the file path */
	char path[PATH_MAX];

	/* the file attributes */
	struct stat attributes;

	/* make sure the file exists */
	return_value = luufs_stat(name, &attributes);
	if (-ENOENT == return_value)
		goto end;

	/* try to change the file permissions, in the writeable directory */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	if (0 == chmod((char *) &path, mode))
		return 0;

	/* if the file exists only under the read-only directory, return EACCESS
	 * upon failure to change its permissions */
	if (ENOENT == errno)
		return_value = -EACCES;
	else
		return_value = -errno;

end:
	return return_value;
}

static int luufs_chown(const char *name, uid_t uid, gid_t gid) {
	/* the return value */
	int return_value;

	/* the file path */
	char path[PATH_MAX];

	/* the file attributes */
	struct stat attributes;

	/* make sure the file exists */
	return_value = luufs_stat(name, &attributes);
	if (-ENOENT == return_value)
		goto end;

	/* try to change the file owner, in the writeable directory */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	if (0 == chown((char *) &path, uid, gid))
		return 0;

	/* if the file exists only under the read-only directory, return EACCESS
	 * upon failure to change its owner */
	if (ENOENT == errno)
		return_value = -EACCES;
	else
		return_value = -errno;

end:
	return return_value;
}

static int luufs_rename(const char *oldpath, const char *newpath) {
	/* the return value */
	int return_value;

	/* the original path */
	char original_path[PATH_MAX];

	/* the new file path */
	char new_path[PATH_MAX];

	/* the file attributes */
	struct stat attributes;

	/* make sure the file exists */
	return_value = luufs_stat(oldpath, &attributes);
	if (-ENOENT == return_value)
		goto end;

	/* try to move the file, in the writeable directory */
	(void) snprintf((char *) &original_path,
	                sizeof(original_path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                oldpath);
	(void) snprintf((char *) &new_path,
	                sizeof(new_path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                newpath);
	if (0 == rename((char *) &original_path, (char *) &new_path))
		return 0;

	/* if the file exists only under the read-only directory, return EACCESS
	 * upon failure to move it */
	if (ENOENT == errno)
		return_value = -EACCES;
	else
		return_value = -errno;

end:
	return return_value;
}

static int luufs_mknod(const char *name, mode_t mode, dev_t dev) {
	/* the return value */
	int return_value;

	/* the device node path */
	char path[PATH_MAX];

	/* the device node attributes */
	struct stat attributes;

	/* make sure the device node does not exist */
	if (-ENOENT != luufs_stat(name, &attributes)) {
		return_value = -EEXIST;
		goto end;
	}

	/* try to create the device node, in the writeable directory */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	if (0 == mknod((char *) &path, mode, dev))
		return_value = 0;
	else
		return_value = -errno;

end:
	return return_value;
}

static struct fuse_operations luufs_oper = {
	.init		= luufs_init,
	.destroy	= luufs_destroy,

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

int main(int argc, char *argv[]) {
	return fuse_main(argc, argv, &luufs_oper, NULL);
}
