#include <limits.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "tree.h"

bool tree_create(const char *name, const char *upper, const char *lower) {
	/* the directory path */
	char path[PATH_MAX] = {'\0'};

	/* the directory attributes */
	struct stat attributes = {0};

	/* a file under the directory */
	struct dirent _entry = {0};

	/* the return value */
	bool result = false;

	/* the directory */
	DIR *directory = NULL;

	/* the return value of readdir_r() */
	struct dirent *entry;

	/* get the directory attributes */
	(void) snprintf(path, sizeof(path), "%s/%s", lower, name);
	if (-1 == stat(path, &attributes)) {
		goto end;
	}

	/* open the directory */
	directory = opendir(path);
	if (NULL == directory) {
		goto end;
	}

	/* create the directory, under the writeable branch */
	(void) snprintf(path, sizeof(path), "%s/%s", upper, name);
	if (-1 == mkdir(path, attributes.st_mode)) {
		if (EEXIST != errno) {
			goto close_directory;
		}
	}

	/* set the directory ownership */
	if (-1 == chown((char *) &path, attributes.st_uid, attributes.st_gid)) {
		goto close_directory;
	}

	do {
		/* read the details of a file under the directory */
		if (0 != readdir_r(directory, &_entry, &entry)) {
			break;
		}
		if (NULL == entry) {
			result = true;
			break;
		}

		/* skip non-directory entries */
		if (DT_DIR != entry->d_type) {
			continue;
		}

		/* skip relative paths */
		if ((0 == strcmp(".", (char *) &entry->d_name)) ||
		    (0 == strcmp("..", (char *) &entry->d_name))) {
			continue;
		}

		/* recurse into sub-directories */
		(void) snprintf(path, sizeof(path), "%s/%s", name, entry->d_name);
		if (false == tree_create(path, upper, lower)) {
			break;
		}
	} while (1);

close_directory:
	/* close the directory */
	(void) closedir(directory);

end:
	return result;
}
