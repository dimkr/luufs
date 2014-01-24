#define FUSE_USE_VERSION (26)
#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <dirent.h>
#include <stdbool.h>
#include <unistd.h>
#include "config.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#define JOIN_PATHS(buffer, first, second) \
	(void) snprintf((char *) &buffer, sizeof(buffer), "%s%s", first, second)

const static char *g_branches[] = {
	CONFIG_READ_ONLY_BRANCH,
	CONFIG_WRITABLE_BRANCH
};

static int luufs_getattr(const char *path, struct stat *buf) {
	/* the file path, under a branch */
	char branch_path[PATH_MAX];

	/* a loop index */
	int i;

	/* check whether the file exists under any branch */
	for (i = ARRAY_SIZE(g_branches) - 1; 0 <= i; --i) {
		JOIN_PATHS(branch_path, g_branches[i], path);
		if (0 == lstat((char *) &branch_path, buf))
			return 0;
	}

	return -errno;
}

static int luufs_readdir(const char *path,
                         void *buf,
                         fuse_fill_dir_t filler,
                         off_t offset,
                         struct fuse_file_info *fi) {
	/* the directory */
	DIR *directory;

	/* a file under the directory */
	struct dirent entry;
	struct dirent *entry_pointer;

	/* the return value */
	int return_value = 0;

	/* the directory path, under a branch */
	char directory_path[PATH_MAX];

	/* a loop index */
	int i;

	/* a flag which indicates whether the function succeeded */
	bool is_success = true;

	for (i = ARRAY_SIZE(g_branches) - 1; 0 <= i; --i) {
		/* open each branch */
		JOIN_PATHS(directory_path, g_branches[i], path);
		directory = opendir((char *) &directory_path);
		if (NULL == directory)
			continue;

		/* list the branch contentes and append them to the result */
		do {
			return_value = readdir_r(directory, &entry, &entry_pointer);
			if (0 != return_value) {
				is_success = false;
				return_value = -errno;
				break;
			} else {
				if (NULL == entry_pointer)
					break;
			}
			if (0 != filler(buf, entry_pointer->d_name, NULL, 0)) {
				is_success = false;
				return_value = -ENOMEM;
				break;
			}
		} while (1);

		/* close the branch */
		(void) closedir(directory);

		/* stop once an error occurs */
		if (false == is_success)
			break;
	}

	return return_value;
}

static int luufs_open(const char *path, struct fuse_file_info *fi) {
	/* the file path, under a branch */
	char branch_path[PATH_MAX];

	/* a loop index */
	int i;

	for (i = ARRAY_SIZE(g_branches) - 1; 0 <= i; --i) {
		/* try to open the file under all branches */
		JOIN_PATHS(branch_path, g_branches[i], path);
		fi->fh = open((char *) &branch_path, fi->flags);
		if (-1 != fi->fh)
			return 0;
	}

	return -errno;
}

static int luufs_write(const char *path,
                       const char *buf,
                       size_t size,
                       off_t offset,
                       struct fuse_file_info *fi) {
	/* the return value */
	int result;

	/* write to the file */
	result = pwrite(fi->fh, buf, size, offset);
	if (-1 == result)
		return -errno;

	return result;
}

int luufs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
	/* the file path, under a branch */
	char branch_path[PATH_MAX];

	/* a loop index */
	int i;

	for (i = ARRAY_SIZE(g_branches) - 1; 0 <= i; --i) {
		/* try to create the file, under all branches */
		JOIN_PATHS(branch_path, g_branches[i], path);
		fi->fh = creat((char *) &branch_path, mode);
		if (-1 != fi->fh)
			return 0;
	}

	return -errno;
}

int luufs_release(const char *path, struct fuse_file_info *fi) {
	/* close the file */
	return close(fi->fh);
}

static struct fuse_operations luufs_oper = {
	.getattr	= luufs_getattr,
	.readdir	= luufs_readdir,
	.open		= luufs_open,
	.release	= luufs_release,
	.write		= luufs_write,
	.create 	= luufs_create
};

int main(int argc, char *argv[]) {
	return fuse_main(argc, argv, &luufs_oper, NULL);
}