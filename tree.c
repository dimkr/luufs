#include <string.h>
#include <libgen.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <stdio.h>
#include "tree.h"
#include "config.h"

bool tree_create(const char *path) {
	/* the return value */
	bool is_success = false;

	/* a copy of the passed path */
	char *path_copy;

	/* directories to create */
	char *directory;
	char **directories;
	char **more_directories;
	unsigned int directories_count;
	int i;

	/* a read-only directory path */
	char read_only_directory[PATH_MAX];

	/* the read-only directory attributes */
	struct stat attributes;

	/* a writeable directory path */
	char writeable_directory[PATH_MAX];

	/* copy the path, so it can be modified */
	path_copy = strdup(path);
	if (NULL == path_copy)
		goto end;

	directories_count = 0;
	directories = NULL;
	do {
		/* separate the parent directory path */
		directory = dirname(path_copy);

		/* break the loop once the file system root was reached */
		if (0 == strcmp("/", directory))
			break;

		/* duplicate the parent directory path */
		directory = strdup(directory);
		if (NULL == directory)
			goto free_directories;

		/* enlarge the directories array by one element */
		++directories_count;
		more_directories = realloc(directories,
		                           (sizeof(char *) * directories_count));
		if (NULL == more_directories)
			goto free_directories;
		directories = more_directories;

		/* append the directory to the array */
		directories[directories_count - 1] = directory;
	} while (1);

	for (i = (directories_count - 1); 0 <= i; --i) {
		/* if a writeable directory exists, do nothing */
		(void) snprintf((char *) &writeable_directory,
		                sizeof(writeable_directory),
		                "%s/%s",
		                CONFIG_WRITEABLE_DIRECTORY,
		                directories[i]);
		if (0 == lstat((char *) &writeable_directory, &attributes))
			continue;
		else {
			if (ENOENT != errno)
				goto free_directories;
		}

		/* obtain the read-only directory permissions */
		(void) snprintf((char *) &read_only_directory,
		                sizeof(read_only_directory),
		                "%s/%s",
		                CONFIG_READ_ONLY_DIRECTORY,
		                directories[i]);
		if (-1 == lstat((char *) &read_only_directory, &attributes))
			goto free_directories;

		/* create a writeable directory */
		if (-1 == mkdir((char *) &writeable_directory, attributes.st_mode))
			goto free_directories;
	}

	/* report success */
	is_success = true;

free_directories:
	/* free the list of parent directories */
	if (NULL != directories) {
		for (i = (directories_count - 1); 0 <= i; --i)
			free(directories[i]);
		free(directories);
	}

	/* free the copied path */
	free(path_copy);

end:
	return is_success;
}