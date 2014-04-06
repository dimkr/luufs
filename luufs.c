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
	char name[1 + NAME_MAX];
	struct stat attributes;
	crc32_t hash;
} _entry_t;

typedef struct {
	_dir_t ro;
	_dir_t rw;
	_entry_t *entries;
	unsigned int entries_count;
} _dir_pair_t;

static int luufs_stat(const char *name, struct stat *stbuf);

static void *luufs_init(struct fuse_conn_info *conn) {
	(void) tree_create("/");
	return NULL;
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

	/* initialize the list of files under the directory */
	((_dir_pair_t *) (intptr_t) fi->fh)->entries = NULL;
	((_dir_pair_t *) (intptr_t) fi->fh)->entries_count = 0;

end:
	return return_value;
}

static int luufs_closedir(const char *name, struct fuse_file_info *fi) {
	/* the return value */
	int return_value = -EBADF;

	/* make sure the directory was opened */
	if (NULL == (void *) (intptr_t) fi->fh)
		goto end;

	/* free the list of files */
	if (NULL != ((_dir_pair_t *) (intptr_t) fi->fh)->entries)
		free(((_dir_pair_t *) (intptr_t) fi->fh)->entries);

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

int _read_directory(_dir_t *directory,
                    _entry_t **entries,
                    unsigned int *entries_count) {
	/* the return value */
	int return_value;

	/* a file under the directory */
	struct dirent entry;
	struct dirent *entry_pointer;

	/* the file attributes */
	struct stat attributes;

	/* the file name hash */
	crc32_t hash;

	/* the file path */
	char path[PATH_MAX];

	/* the enlarged entries array */
	_entry_t *more_entries;

	/* the current entry */
	_entry_t *current_entry;

	/* a loop index */
	unsigned int i;

	/* upon failure to open the directory, report success */
	if (NULL == directory->handle)
		goto success;

	do {
next:
		/* read the name of one file under the directory */
		if (0 != readdir_r(directory->handle, &entry, &entry_pointer)) {
			return_value = -errno;
			goto end;
		} else {
			if (NULL == entry_pointer)
				break;
		}

		/* hash the file name */
		hash = crc32_hash((const unsigned char *) &entry_pointer->d_name,
		                  strnlen((char *) &entry_pointer->d_name, NAME_MAX));

		/* if there's another file with the same hash, continue to the next
		 * one */
		for (i = 0; *entries_count > i; ++i) {
			if (hash == (*entries)[i].hash)
				goto next;
		}

		/* get the file attributes */
		(void) snprintf((char *) &path,
		                sizeof(path),
		                "%s/%s",
		                (char *) &directory->path,
		                (char *) &entry_pointer->d_name);
		if (-1 == lstat((char *) &path, &attributes))
			continue;

		/* enlarge the entries array */
		more_entries = realloc(*entries,
		                       sizeof(_entry_t) * (1 + *entries_count));
		if (NULL == more_entries) {
			return_value = -ENOMEM;
			goto end;
		}

		/* add the file to the array */
		*entries = more_entries;
		current_entry = &(*entries)[*entries_count];
		current_entry->hash = hash;
		(void) strncpy((char *) &current_entry->name,
		               (char *) &entry_pointer->d_name,
		               sizeof(current_entry->name) / sizeof(char));
		(void) memcpy(&current_entry->attributes,
		              &attributes,
		              sizeof(attributes));
		++(*entries_count);
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

	/* the directory pair */
	_dir_pair_t *pair;

	/* make sure the directory was opened */
	pair = (_dir_pair_t *) (intptr_t) fi->fh;
	if (NULL == pair)
		goto end;

	/* update the list of files each time the first file is requested (i.e after
	 * a rewinddir() call) */
	if (0 == offset) {
		/* first, empty the list of files */
		if (NULL != pair->entries) {
			free(pair->entries);
			pair->entries = NULL;
			pair->entries_count = 0;
		}

		/* then, list the files under both directories; start with the first
		 * file */
		if (NULL != pair->rw.handle) {
			rewinddir(pair->rw.handle);
			return_value = _read_directory(&pair->rw,
			                               &pair->entries,
			                               &pair->entries_count);
			if (0 != return_value)
				goto end;
		}

		if (NULL != pair->ro.handle) {
			rewinddir(pair->ro.handle);
			return_value = _read_directory(&pair->ro,
			                               &pair->entries,
			                               &pair->entries_count);
			if (0 != return_value)
				goto end;
		}
	} else {
		/* if the last file was reached, report success */
		if ((unsigned int) offset == pair->entries_count)
			goto success;

		/* if the offset is too big, report failure */
		if ((unsigned int) offset > pair->entries_count) {
			return_value = -ENOMEM;
			goto end;
		}
	}


	/* fetch one entry */
	if (0 != filler(buf,
	                (char *) &pair->entries[offset].name,
	                &pair->entries[offset].attributes,
	                1 + offset)) {
		return_value = -ENOMEM;
		goto end;
	}

success:
	return_value = 0;

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
