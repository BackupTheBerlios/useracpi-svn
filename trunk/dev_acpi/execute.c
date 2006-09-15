/*
 * Copyright (c) 2004 Hewlett Packard, LLC
 *      Alex Williamson <alex.williamson@hp.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/ioctl.h>

#define ACPI_USE_SYSTEM_CLIBRARY
#define ACPI_USE_STANDARD_HEADERS
#define DEFINE_ALTERNATE_TYPES

#include <acpi/acconfig.h>
#include <acpi/platform/acenv.h>

typedef COMPILER_DEPENDENT_INT64       s64;

#include <acpi/actypes.h>

#define ACPI_MAX_STRING 80
#define ACPI_PATHNAME_MAX 256
#include "dev_acpi.h"

#define DEVICE "/dev/acpi"

int
call_method(int fd, char *path, char *method)
{
	dev_acpi_t		data;

	memset(&data, 0, sizeof(data));

	sprintf(data.pathname, "%s.%s", path, method);

	if (ioctl(fd, DEV_ACPI_EVALUATE_OBJ, &data)) {
		printf("%s() Error: ioctl failed\n", __FUNCTION__);
		return 0;
	}

	return 1;
}

void
get_objects(int fd, char *name, char **ret)
{
	dev_acpi_t	data;

	memset(&data, 0 , sizeof(data));
	*ret = NULL;

	if (!name) {
		printf("%s() ERROR: nothing to search for\n", __FUNCTION__);
		return;
	}

	strcpy(data.pathname, name);

	if (ioctl(fd, DEV_ACPI_GET_OBJECTS, &data)) {
		printf("%s() ERROR: GET_OBJECTS failed: %s\n", __FUNCTION__,
		       strerror(errno));
		return;
	}

	if (data.return_size == 0)
		return;

	*ret = malloc(data.return_size);

	if (!*ret) {
		printf("%s() ERROR: malloc failed for buffer\n", __FUNCTION__);
		return;
	}

	if (read(fd, *ret, data.return_size) != data.return_size) {
		printf("%s() ERROR: read() unexpected return\n", __FUNCTION__);
		free(*ret);
		*ret = NULL;
		return;
	}

	return;
}

void
get_parent(int fd, char *name, char **ret)
{
	dev_acpi_t		data;

	memset(&data, 0 , sizeof(data));
	*ret = NULL;

	if (!name) {
		printf("%s() ERROR: nothing to search for\n", __FUNCTION__);
		return;
	}

	strcpy(data.pathname, name);

	if (ioctl(fd, DEV_ACPI_GET_PARENT, &data)) {
		printf("%s() ERROR: GET_PARENT failed: %s\n", __FUNCTION__,
		       strerror(errno));
		return;
	}

	if (data.return_size == 0)
		return;

	*ret = malloc(data.return_size);

	if (!*ret) {
		printf("%s() ERROR: malloc failed for buffer\n", __FUNCTION__);
		return;
	}

	if (read(fd, *ret, data.return_size) != data.return_size) {
		printf("%s() ERROR: read() unexpected return\n", __FUNCTION__);
		free(*ret);
		*ret = NULL;
		return;
	}
	return;
}

int
main (int argc, char **argv)
{
	int		fd;
	char		*chr, *exec_path, *exec_root;
	char		path[ACPI_PATHNAME_MAX];

	if (argc < 2) {
		printf("requires 2 args\n");
		return 1;
	}

	fd = open(DEVICE, O_RDWR | O_NONBLOCK);

	if (fd < 0) {
		printf("Error opening file\n");
		return 1;
	}

	get_objects(fd, argv[1], &exec_path);
	if (!exec_path) {
		printf("exec path not found\n");
		close(fd);
		return 1;
	}

	chr = strchr(exec_path, '\n');
	if (chr)
		*chr = 0;

	get_parent(fd, exec_path, &exec_root);
	if (!exec_root) {
		printf("Error: unable to get parent for method\n");
		free(exec_path);
		close(fd);
		return 1;
	}

	memset(path, 0, sizeof(path));
	strcpy(path, exec_root);

	free(exec_root);
	free(exec_path);

	printf("Executing %s.%s\n", path, argv[1]);
	if (!call_method(fd, path, argv[1])) {
		printf("Error: method unsuccessful\n");
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}

