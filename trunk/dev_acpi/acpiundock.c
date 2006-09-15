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

#define MAX_VIDS 10

struct vid {
	char		path[ACPI_PATHNAME_MAX];
	unsigned int	dod;
	unsigned int	dcs;
	unsigned int	dgs;
	unsigned int	state;
};

sigjmp_buf env;

#ifdef DEBUG
static void
dump(char *msg, unsigned char *buf, int size)
{
	int i;

	printf("\n%s:\n", msg);

	for (i = 0 ; i < size ;) {
		printf("%02x ",buf[i++]);
		if (i % 16 == 0)
			printf("\n");
	}

	printf("\n");
	return;
}
#else
#define dump(msg, buf, size)
#endif

int
call_method(int fd, char *path, char *method, unsigned int state)
{
	unsigned char		*buf;
	struct acpi_object_list *list;
	union acpi_object	*obj, ret;
	int			size;
	dev_acpi_t		data;

	memset(&data, 0, sizeof(data));
	size = sizeof(struct acpi_object_list) + sizeof(union acpi_object);

	buf = malloc(size);

	if (!buf)
		return 0;

	memset(buf, 0, size);

	list = (struct acpi_object_list *)buf;

	list->count = 1;
	list->pointer = (union acpi_object *)sizeof(struct acpi_object_list);

	obj = (union acpi_object *)((unsigned long)buf +
	                            sizeof(struct acpi_object_list));

	obj->type = ACPI_TYPE_INTEGER;
	obj->integer.value = state;

	if (write(fd, buf, size) != size) {
		printf("%s() Error: write failed: %s\n", __FUNCTION__,
		       strerror(errno));
		free(buf);
		return 0;
	}

	free(buf);

	sprintf(data.pathname, "%s.%s", path, method);

	if (ioctl(fd, DEV_ACPI_EVALUATE_OBJ, &data)) {
		printf("%s() Error: ioctl failed\n", __FUNCTION__);
		return 0;
	}

	if (read(fd, &ret, sizeof(ret)) != sizeof(ret)) {
		printf("%s() Error: read failed\n", __FUNCTION__);
		return 0;
	}

	if (ret.type != ACPI_TYPE_INTEGER) {
		printf("%s() Error: unexpected read value\n", __FUNCTION__);
		return 0;
	}

	return ret.integer.value;
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
	char		*chr, *dck_path, *dock_root;
	char		dock[ACPI_PATHNAME_MAX];

	fd = open(DEVICE, O_RDWR | O_NONBLOCK);

	if (fd < 0) {
		printf("Error opening file\n");
		return 1;
	}

	get_objects(fd, "_DCK", &dck_path);
	if (!dck_path) {
		printf("System does not appear to have an ACPI dock device\n");
		close(fd);
		return 1;
	}

	/* 
	 * sorry, I'm not worrying about multiple docks,
	 * just using the first
	 */
	chr = strchr(dck_path, '\n');
	if (chr)
		*chr = 0;

	get_parent(fd, dck_path, &dock_root);
	if (!dock_root) {
		printf("Error: unable to get parent for _DCK method\n");
		free(dck_path);
		close(fd);
		return 1;
	}

	memset(dock, 0, sizeof(dock));
	strcpy(dock, dock_root);

	free(dock_root);
	free(dck_path);

	if (!call_method(fd, dock, "_DCK", 0)) {
		printf("Error: _DCK method unsuccessful\n");
		close(fd);
		return 1;
	}

	call_method(fd, dock, "_EJ0", 1);

	close(fd);
	return 0;
}

