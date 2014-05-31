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
	/* the return value */
	bool is_success = false;

	/* the directory path */
	char path[PATH_MAX];

	/* the directory */
	DIR *directory;

	/* the directory attributes */
	struct stat attributes;

	/* a file under the directory */
	struct dirent _entry;
	struct dirent *entry;

	/* get the directory attributes */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                lower,
	                name);
	if (-1 == stat((char *) &path, &attributes))
		goto end;

	/* open the directory */
	directory = opendir(path);
	if (NULL == directory)
		goto end;

	/* create the directory, under the writeable branch */
	(void) snprintf((char *) &path,
	                sizeof(path),
	                "%s/%s",
	                upper,
	                name);
	if (-1 == mkdir((char *) &path, attributes.st_mode)) {
		if (EEXIST != errno)
			goto close_directory;
	}

	do {
		/* read the details of a file under the directory */
		if (0 != readdir_r(directory, &_entry, &entry))
			goto close_directory;
		if (NULL == entry)
			break;

		/* skip non-directory entries */
		if (DT_DIR != entry->d_type)
			continue;

		/* skip relative paths */
		if ((0 == strcmp(".", (char *) &entry->d_name)) ||
		    (0 == strcmp("..", (char *) &entry->d_name)))
			continue;

		/* recurse into sub-directories */
		(void) snprintf((char *) &path,
		                sizeof(path),
		                "%s/%s",
		                name,
		                (char *) &entry->d_name);
		if (false == tree_create((char *) &path, upper, lower))
			goto close_directory;
	} while (1);

	/* report success */
	is_success = true;

close_directory:
	/* close the directory */
	(void) closedir(directory);

end:
	return is_success;
}
