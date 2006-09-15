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

/*
 * get type
 */
acpi_object_type
get_type(int fd, char *path)
{
	dev_acpi_t		data;
	union acpi_object	obj;

	memset(&data, 0, sizeof(data));

	strcpy(data.pathname, path);

	if (ioctl(fd, DEV_ACPI_GET_TYPE, &data))
		return -1;

	if (data.return_size != sizeof(obj))
		return -1;

	if (read(fd, &obj, data.return_size) != data.return_size)
		return -1;

	dump("get_type", (unsigned char *)&obj, data.return_size);
	if (obj.type != ACPI_TYPE_INTEGER)
		return -1;

	return (acpi_object_type)obj.integer.value;
}

long long
get_integer(int fd, char *path)
{
	dev_acpi_t		data;
	union acpi_object	obj;

	memset(&data, 0, sizeof(data));
	strcpy(data.pathname, path);

	if (ioctl(fd, DEV_ACPI_EVALUATE_OBJ, &data)) {
		return -1;
	}

	if (data.return_size != sizeof(obj))
		return -1;

	if (read(fd, &obj, data.return_size) != data.return_size)
		return -1;

	if (obj.type != ACPI_TYPE_INTEGER)
		return -1;

	return obj.integer.value;
}

long long
get_method(int fd, char *path, char *method)
{
	char	str[ACPI_PATHNAME_MAX] = {0};

	sprintf(str, "%s.%s", path, method);

	return get_integer(fd, str);
}

void
set_dos(int fd, char *path, int value)
{
	unsigned char		*buf;
	struct acpi_object_list *list;
	union acpi_object	*obj;
	int			size;
	dev_acpi_t		data;

	memset(&data, 0, sizeof(data));
	size = sizeof(struct acpi_object_list) + sizeof(union acpi_object);

	buf = malloc(size);

	if (!buf)
		return;

	memset(buf, 0, size);

	list = (struct acpi_object_list *)buf;

	list->count = 1;
	list->pointer = (union acpi_object *)sizeof(struct acpi_object_list);

	obj = (union acpi_object *)((unsigned long)buf +
	                            sizeof(struct acpi_object_list));

	obj->type = ACPI_TYPE_INTEGER;
	obj->integer.value = value;

	if (write(fd, buf, size) != size) {
		printf("%s() Error: write failed: %s\n", __FUNCTION__,
		       strerror(errno));
		free(buf);
		return;
	}

	free(buf);

	sprintf(data.pathname, "%s._DOS", path);

	if (ioctl(fd, DEV_ACPI_EVALUATE_OBJ, &data)) {
		printf("%s() Error: ioctl failed\n", __FUNCTION__);
		return;
	}
	return;
}

int
call_dss(int fd, char *path, unsigned int state)
{
	unsigned char		*buf;
	struct acpi_object_list *list;
	union acpi_object	*obj;
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

	sprintf(data.pathname, "%s._DSS", path);

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

	memset(&data, 0, sizeof(data));
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
get_next(int fd, char *name, char **ret)
{
	dev_acpi_t	data;

	memset(&data, 0, sizeof(data));
	*ret = NULL;

	if (!name) {
		printf("%s() ERROR: nothing to search for\n", __FUNCTION__);
		return;
	}

	strcpy(data.pathname, name);

	if (ioctl(fd, DEV_ACPI_GET_NEXT, &data)) {
		printf("%s() ERROR: GET_NEXT failed: %s\n", __FUNCTION__,
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

	memset(&data, 0, sizeof(data));
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

void
evaluate(int fd, char *name, char **ret)
{
	dev_acpi_t		data;

	memset(&data, 0, sizeof(data));
	*ret = NULL;

	if (!name) {
		printf("%s() ERROR: nothing to search for\n", __FUNCTION__);
		return;
	}

	strcpy(data.pathname, name);

	if (ioctl(fd, DEV_ACPI_EVALUATE_OBJ, &data)) {
		printf("%s() ERROR: EVALUATE_OBJ failed: %s\n", __FUNCTION__,
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
eval_dod(int fd, char *path, struct vid *vids)
{
	char			*buf;
	union acpi_object	*obj;
	int			i, count;
	char			str[ACPI_PATHNAME_MAX] = {0};

	sprintf(str, "%s._DOD", path);
	evaluate(fd, str, &buf);

	if (!buf) {
		printf("%s() ERROR: no data from eval\n", __FUNCTION__);
		return 0;
	}

	obj = (union acpi_object *)buf;

	if (obj->type != ACPI_TYPE_PACKAGE) {
		printf("%s() ERROR: not a package\n", __FUNCTION__);
		free(buf);
		return 0;
	}

	count = obj->package.count;

	obj = (union acpi_object *)((unsigned long)obj +
	                            (unsigned long)obj->package.elements);

	for (i = 0 ; i < count ; i++, obj++) {
		if (obj->type !=  ACPI_TYPE_INTEGER) {
			printf("%s() ERROR: package contains non-int (%d)\n",
			       __FUNCTION__, obj->type); 
			free(buf);
			return 0;
		}
		vids[i].dod = obj->integer.value;
	}
	free(buf);
	return count;
}

void
handle_signal(int sig)
{
	siglongjmp(env, 1);
}

int
main (int argc, char **argv)
{
	int		fd, i, j, disp_count;
	char		*chr, *dod_path, *video_root, *tmp;
	char		*a, *b;
	struct vid	vids[MAX_VIDS];
	char		vga[ACPI_PATHNAME_MAX];
	char		str[ACPI_PATHNAME_MAX];
	unsigned int	tmp_adr;

	memset(vids, 0, MAX_VIDS * sizeof(struct vid));
	fd = open(DEVICE, O_RDWR | O_NONBLOCK);

	if (fd < 0) {
		printf("Error opening file\n");
		return 1;
	}

	get_objects(fd, "_DOD", &dod_path);
	if (!dod_path) {
		printf("System does not support ACPI output switching\n");
		close(fd);
		return 1;
	}

	/* 
	 * sorry, I'm not worrying about multiple displays,
	 * just using the first
	 */
	chr = strchr(dod_path, '\n');
	if (chr)
		*chr = 0;

	get_parent(fd, dod_path, &video_root);
	if (!video_root) {
		printf("Error: unable to get parent for _DOD method\n");
		free(dod_path);
		close(fd);
		return 1;
	}

	memset(vga, 0, sizeof(vga));
	strcpy(vga, video_root);

	free(video_root);
	free(dod_path);

	printf("Using device %s for video root\n", vga);

	disp_count = eval_dod(fd, vga, vids);

	if (!disp_count) {
		printf("Error: No displays found\n");
		close(fd);
		return 1;
	}

	get_next(fd, vga, &tmp);

	if (!tmp) {
		printf("Error: unable to get objects under video root\n");
		close(fd);
		return 1;
	}

	a = tmp;
	b = strchr(a, '\n');

	do {
		if (b)
			*b = '\0';

		memset(str, 0, ACPI_PATHNAME_MAX);
		sprintf(str , "%s.%s", vga, a);

		a = ++b;

		if (get_type(fd, str) != ACPI_TYPE_DEVICE)
			continue;

		tmp_adr = get_method(fd, str, "_ADR");
		for (i = 0 ; i < disp_count ; i++)
			if ((vids[i].dod & 0xFFFFU) == tmp_adr)
				sprintf(vids[i].path, "%s", str);

	} while ((b = strchr(a, '\n')) != NULL);

	free(tmp);
	
	if (signal(SIGINT, handle_signal) == SIG_ERR)
		printf("Error: installing singal handler\n");

	set_dos(fd, vga, 0);
again:
	if (sigsetjmp(env, 0))
		goto done;


	if (argc > 1) {
		for (i = 0 ; i < disp_count ; i++)
			vids[i].state = 0;
		for (i = 1 ; i < argc ; i++) {
			for (j = 0 ; j < disp_count; j++) {
				if ((vids[j].dod & 0xFFFFU) == 0x100 &&
				    !strcmp(argv[i], "crt"))
					vids[j].state = 1;
				else if ((vids[j].dod & 0xFFFFU) == 0x110 &&
				         !strcmp(argv[i], "lcd"))
					vids[j].state = 1;
				else if ((vids[i].dod & 0xFF00U) == 0x200 &&
				         !strcmp(argv[i], "tv0"))
					vids[j].state = 1;
				else if ((vids[i].dod & 0xFF00U) == 0x210 &&
				         !strcmp(argv[i], "tv1"))
					vids[j].state = 1;
			}
		}
		vids[disp_count - 1].state |= 0x80000000U;
	} else {
		for (i = 0 ; i < disp_count ; i++)
			vids[i].state = vids[i].dgs =
			                 get_method(fd, vids[i].path, "_DGS");

		vids[disp_count - 1].state |= 0x80000000U;
	}

	for (i = 0 ; i < disp_count ; i++)
		call_dss(fd, vids[i].path, vids[i].state);

	for (i = 0 ; i < disp_count ; i++)
		vids[i].dcs = get_method(fd, vids[i].path, "_DCS");

	printf("Available Video ports:\n");
	for (i = 0 ; i < disp_count ; i++) {
		printf("  %s [%08x]", vids[i].path, vids[i].dod);
		if ((vids[i].dod & 0xFFFFU) == 0x100)
			printf(" (CRT Monitor)");
		else if ((vids[i].dod & 0xFFFFU) == 0x110)
			printf(" (LCD Panel)");
		else if ((vids[i].dod & 0xFF00U) == 0x200)
			printf(" (TV Port %d)", (vids[i].dod >> 4) & 0xF);
		else
			printf(" (Unknown)");

		if (vids[i].state & 0x2)
			printf("*");
		printf(" (0x%02x)", vids[i].dcs);
		if (vids[i].dgs)
			printf("*");
		printf("\n");

	}

	if (argc > 1)
		goto done;

	{
		dev_acpi_t	data;
		int		ev;
		char		event[ACPI_PATHNAME_MAX];

		ev = open(DEVICE, O_RDWR);
		strcpy(data.pathname, vga);

		if (ioctl(ev, DEV_ACPI_DEVICE_NOTIFY, &data)) {
			printf("Error: notify failed\n");
			close(ev);
			goto done;
		}

		read(ev, event, ACPI_PATHNAME_MAX);
		printf("\nEvent: %s\n\n", event);
		close(ev);
		goto again;
	}
done:
	set_dos(fd, vga, 1);
	close(fd);
	return 0;
}

