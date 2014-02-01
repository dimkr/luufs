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
	DIR *read_only;
	DIR *writeable;
} _dir_pair_t;

static int luufs_stat(const char *name, struct stat *stbuf);

static void *luufs_init(struct fuse_conn_info *conn) {
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

	/* create the file's parent directory, under the writeable directory */
	if (false == tree_create(name))
		goto failure;

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

	return return_value;
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
	int return_value;

	/* the file path */
	char path[PATH_MAX];

	/* the directory attributes */
	struct stat attributes;

	/* make sure the directory does not exist */
	if (-ENOENT != luufs_stat(name, &attributes)) {
		return_value = -EEXIST;
		goto end;
	}

	/* create the directory, under the writeable directory */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	if (0 == mkdir((char *) &path, mode))
		return 0;
	return_value = -errno;

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

	/* make sure the directory exists */
	return_value = luufs_stat(name, &attributes);
	if (-ENOENT == return_value)
		goto end;

	/* try to remove the directory from the writeable directory */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	if (0 == rmdir((char *) &path))
		return 0;

	/* if the directory exists only under the read-only directory, return
	 * EACCESS upon failure to delete it */
	if (ENOENT == errno)
		return_value = -EACCES;
	else
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

	/* the directory path */
	char path[PATH_MAX];

	/* allocate memory for the directory pair */
	fi->fh = (uint64_t) (intptr_t) malloc(sizeof(_dir_pair_t));
	if (NULL == (void *) (intptr_t) fi->fh)
		goto end;

	/* open the writeable directory */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_WRITEABLE_DIRECTORY,
	                name);
	((_dir_pair_t *) (intptr_t) fi->fh)->writeable = opendir((char *) &path);
	if (NULL != ((_dir_pair_t *) (intptr_t) fi->fh)->writeable)
		return_value = 0;
	else
		return_value = -errno;

	/* open the read-only directory */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                CONFIG_READ_ONLY_DIRECTORY,
	                name);
	((_dir_pair_t *) (intptr_t) fi->fh)->read_only = opendir((char *) &path);
	if (NULL != ((_dir_pair_t *) (intptr_t) fi->fh)->read_only)
		return_value = 0;

end:
	return return_value;
}

static int luufs_closedir(const char *name, struct fuse_file_info *fi) {
	/* close the read-only directory */
	if (NULL != ((_dir_pair_t *) (intptr_t) fi->fh)->read_only)
		(void) closedir(((_dir_pair_t *) (intptr_t) fi->fh)->read_only);

	/* close the writeable directory */
	if (NULL != ((_dir_pair_t *) (intptr_t) fi->fh)->writeable)
		(void) closedir(((_dir_pair_t *) (intptr_t) fi->fh)->writeable);

	/* free the allocated structure */
	free((void *) (intptr_t) fi->fh);

	return 0;
}

crc32_t _hash(const char *name) {
	return crc32_hash((const unsigned char *) name, strlen(name));
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
	hash = _hash(name);

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

int _readdir_single(const char *directory_path,
                    DIR *directory,
                    crc32_t **hashes,
                    unsigned int *hashes_count,
                    fuse_fill_dir_t filler,
                    void *buf) {
	/* the return value */
	int return_value;

	/* a file under the directory */
	struct dirent entry;
	struct dirent *entry_pointer;

	do {
		if (0 != readdir_r(directory, &entry, &entry_pointer)) {
			return_value = -errno;
			goto end;
		} else {
			if (NULL == entry_pointer)
				break;
		}
		if (false == _hash_filler(directory_path,
		                          filler,
		                          buf,
		                          hashes,
		                          hashes_count,
		                          entry_pointer->d_name)) {
			return_value = -ENOMEM;
			goto end;
		}
	} while (1);

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
	int return_value = -ENOMEM;

	/* file name hashes */
	crc32_t *hashes = NULL;
	unsigned int hashes_count = 0;

	/* the underlying directory path */
	char real_path[PATH_MAX];

	/* add relative paths */
	if (false == _hash_filler(NULL, filler, buf, &hashes, &hashes_count, "."))
		goto free_hashes;
	if (false == _hash_filler(NULL, filler, buf, &hashes, &hashes_count, ".."))
		goto free_hashes;

	return_value = 0;

	/* list the files under the read-only directory */
	if (NULL != ((_dir_pair_t *) (intptr_t) fi->fh)->read_only) {
		(void) snprintf((char *) &real_path,
		                sizeof(real_path),
		                "%s/%s",
		                CONFIG_READ_ONLY_DIRECTORY,
		                path);
		return_value = _readdir_single(
		                         (char *) &real_path,
		                         ((_dir_pair_t *) (intptr_t) fi->fh)->read_only,
		                         &hashes,
		                         &hashes_count,
		                         filler,
		                         buf);
		if (0 != return_value)
			goto free_hashes;
	}

	/* list the files under the writeable directory */
	if (NULL != ((_dir_pair_t *) (intptr_t) fi->fh)->writeable) {
		(void) snprintf((char *) &real_path,
		                sizeof(real_path),
		                "%s/%s",
		                CONFIG_WRITEABLE_DIRECTORY,
		                path);
		return_value = _readdir_single(
		                         (char *) &real_path,
		                         ((_dir_pair_t *) (intptr_t) fi->fh)->writeable,
		                         &hashes,
		                         &hashes_count,
		                         filler,
		                         buf);
		if (0 != return_value)
			goto free_hashes;
	}

free_hashes:
	/* free the hashes list */
	if (NULL != hashes)
		free(hashes);

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
	.readlink	= luufs_readlink
};

int main(int argc, char *argv[]) {
	return fuse_main(argc, argv, &luufs_oper, NULL);
}