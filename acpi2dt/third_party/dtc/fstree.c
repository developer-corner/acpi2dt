// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright David Gibson <dwg@au1.ibm.com>, IBM Corporation.  2005.
 */

#include "dtc.h"

#include <sys/stat.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOGDI                   /* wingdi.h #defines ERROR, which clashes with dtc */
#include <windows.h>

static struct node* read_fstree(const char* dirname)
{
	WIN32_FIND_DATAA fd;
	HANDLE h;
	char* pattern;
	struct node* tree;

	pattern = join_path(dirname, "*");
	h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND)
		die("FindFirstFile(\"%s\") failed: error %lu\n",
			pattern, GetLastError());
	free(pattern);

	tree = build_node(NULL, NULL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return tree;                    /* no entries at all */

	do {
		char* tmpname;

		if (streq(fd.cFileName, ".")
			|| streq(fd.cFileName, ".."))
			continue;               /* jumps to FindNextFileA below */

		tmpname = join_path(dirname, fd.cFileName);

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			struct node* newchild;

			newchild = read_fstree(tmpname);
			newchild = name_node(newchild, xstrdup(fd.cFileName));
			add_child(tree, newchild);
		}
		else if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DEVICE)) {
			struct property* prop;
			FILE* pfile;
			ULONGLONG size = ((ULONGLONG)fd.nFileSizeHigh << 32)
				| fd.nFileSizeLow;

			pfile = fopen(tmpname, "rb");
			if (!pfile) {
				fprintf(stderr,
					"WARNING: Cannot open %s: %s\n",
					tmpname, strerror(errno));
			}
			else {
				prop = build_property(fd.cFileName,
					data_copy_file(pfile,
						(size_t)size),
					NULL);
				add_property(tree, prop);
				fclose(pfile);
			}
		}

		free(tmpname);
	} while (FindNextFileA(h, &fd));

	if (GetLastError() != ERROR_NO_MORE_FILES)
		die("FindNextFile(\"%s\") failed: error %lu\n",
			dirname, GetLastError());

	FindClose(h);
	return tree;
}

#else

#include <dirent.h>

static struct node *read_fstree(const char *dirname)
{
	DIR *d;
	struct dirent *de;
	struct stat st;
	struct node *tree;

	d = opendir(dirname);
	if (!d)
		die("Couldn't opendir() \"%s\": %s\n", dirname, strerror(errno));

	tree = build_node(NULL, NULL, NULL);

	while ((de = readdir(d)) != NULL) {
		char *tmpname;

		if (streq(de->d_name, ".")
		    || streq(de->d_name, ".."))
			continue;

		tmpname = join_path(dirname, de->d_name);

		if (stat(tmpname, &st) < 0)
			die("stat(%s): %s\n", tmpname, strerror(errno));

		if (S_ISREG(st.st_mode)) {
			struct property *prop;
			FILE *pfile;

			pfile = fopen(tmpname, "rb");
			if (! pfile) {
				fprintf(stderr,
					"WARNING: Cannot open %s: %s\n",
					tmpname, strerror(errno));
			} else {
				prop = build_property(de->d_name,
						      data_copy_file(pfile,
								     st.st_size),
						      NULL);
				add_property(tree, prop);
				fclose(pfile);
			}
		} else if (S_ISDIR(st.st_mode)) {
			struct node *newchild;

			newchild = read_fstree(tmpname);
			newchild = name_node(newchild, xstrdup(de->d_name));
			add_child(tree, newchild);
		}

		free(tmpname);
	}

	closedir(d);
	return tree;
}

#endif

struct dt_info *dt_from_fs(const char *dirname)
{
	struct node *tree;

	tree = read_fstree(dirname);
	tree = name_node(tree, "");

	return build_dt_info(DTSF_V1, NULL, tree, guess_boot_cpuid(tree));
}
