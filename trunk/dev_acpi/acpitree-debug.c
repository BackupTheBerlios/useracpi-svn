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

static char *acpi_table_names[] = {
	"RSDP", "DSDT", "FADT", "FACS", "PSDT", "SSDT", "XSDT" };

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

/*
 * Do we consider this path a file or directory?
 */
int
is_dev(int fd, char *path)
{
	switch (get_type(fd, path)) {
		case ACPI_TYPE_DEVICE:
		case ACPI_TYPE_PROCESSOR:
		case ACPI_TYPE_THERMAL:
		case ACPI_TYPE_POWER:
			return 1;
		default:
			return 0;
	}
}

/*
 * This probably has bugs, it's whole purpose is to get
 * the right associativity lines.
 */
void
indent(int *entries, int level)
{
	int i;

	for (i = 0 ; i < level - 1; i++) {
		if (entries[i + 1] > 1)
			printf("|   ");
		else
			printf("    ");
	}

	if (entries[level] > 1)
		printf("|-- ");
	else if (entries[level] == 1)
		printf("`-- ");
}

/*
 * Blatantly copied from acpi source code in the kernel
 */
u32
acpi_ut_dword_byte_swap(u32 value) {
	union {
		u32	value;
		u8	bytes[4];
	} out;

	union {
		u32	value;
		u8	bytes[4];
	} in;

	in.value = value;
	out.bytes[0] = in.bytes[3];
	out.bytes[1] = in.bytes[2];
	out.bytes[2] = in.bytes[1];
	out.bytes[3] = in.bytes[0];

	return (out.value);
}

static const char acpi_gbl_hex_to_ascii[] = {'0','1','2','3','4','5','6','7',
                                             '8','9','A','B','C','D','E','F'};

char
acpi_ut_hex_to_ascii_char(acpi_integer integer, u32 position) {
	return (acpi_gbl_hex_to_ascii[(integer >> position) & 0xF]);
}

void
acpi_ex_eisa_id_to_string(u32 numeric_id, char *out_string) {
	u32	eisa_id;

	/* Swap ID to big-endian to get contiguous bits */
	eisa_id = acpi_ut_dword_byte_swap (numeric_id);

	out_string[0] = (char) ('@' + (((unsigned long) eisa_id >> 26) & 0x1f));
	out_string[1] = (char) ('@' + ((eisa_id >> 21) & 0x1f));
	out_string[2] = (char) ('@' + ((eisa_id >> 16) & 0x1f));
	out_string[3] = acpi_ut_hex_to_ascii_char ((acpi_integer) eisa_id, 12);
	out_string[4] = acpi_ut_hex_to_ascii_char ((acpi_integer) eisa_id, 8);
	out_string[5] = acpi_ut_hex_to_ascii_char ((acpi_integer) eisa_id, 4);
	out_string[6] = acpi_ut_hex_to_ascii_char ((acpi_integer) eisa_id, 0);
	out_string[7] = 0;
}

/*
 * evaluate _HID object, only know how to handle integer _HIDs right now
 */
void
get_hid(int fd, char *path, char *hid)
{
	dev_acpi_t		data;
	union acpi_object	eisa_id;

	memset(hid, 0, 8);
	memset(&data, 0, sizeof(data));

	strcpy(data.pathname, path);

	if (ioctl(fd, DEV_ACPI_EVALUATE_OBJ, &data))
		return;

	if (data.return_size != sizeof(eisa_id)) {
		sprintf(hid, "BUG:sz");
		return;
	}

	if (read(fd, &eisa_id, data.return_size) != data.return_size)
		return;

	if (eisa_id.type != ACPI_TYPE_INTEGER) {
		sprintf(hid, "BUG:typ");
		return;
	}
	acpi_ex_eisa_id_to_string(eisa_id.integer.value, hid);

	return;
}

void
get_string(int fd, char *path, char **ret)
{
	dev_acpi_t		data;
	union acpi_object	*obj;
	unsigned short		*buf;
	int			i;

	memset(&data, 0, sizeof(data));

	strcpy(data.pathname, path);

	if (ioctl(fd, DEV_ACPI_EVALUATE_OBJ, &data)) {
		*ret = strdup("BUG: ioctl failed");
		return;
	}

	obj = malloc(data.return_size);
	if (!obj) {
		*ret = strdup("BUG: malloc failed");
		return;
	}

	if (read(fd, obj, data.return_size) != data.return_size) {
		*ret = strdup("BUG: read failed");
		free(obj);
		return;
	}

	dump("get_string", (unsigned char *)obj, data.return_size);

	buf = (unsigned short *)((unsigned long)obj +
	                                  (unsigned long)obj->string.pointer);

	if (obj->type == ACPI_TYPE_STRING) {
		*ret = malloc((obj->string.length) + 1);
		if (!*ret) {
			*ret = strdup("BUG: string malloc failed");
			free(obj);
			return;
		}
		memset(*ret, 0, (obj->string.length) + 1);
		strncpy(*ret, (char *)buf, obj->string.length);
		free(obj);
		return;
	}
	if (obj->type == ACPI_TYPE_BUFFER) {

		*ret = malloc((obj->buffer.length/2) + 1);
		if (!*ret) {
			*ret = strdup("BUG: buffer malloc failed");
			free(obj);
			return;
		}
		memset(*ret, 0, (obj->buffer.length/2) + 1);

		for (i = 0 ; i < obj->buffer.length/2 ; i++)
			(*ret)[i] = (unsigned char)buf[i];

		free(obj);
		return;
	}

	*ret = strdup("BUG: not STRING||BUFFER");
	free(obj);
	return;
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

int
dump_raw(int fd, char *path, u8 **buf)
{
	dev_acpi_t		data;
	union acpi_object	*obj;

	memset(&data, 0, sizeof(data));
	strcpy(data.pathname, path);

	if (ioctl(fd, DEV_ACPI_EVALUATE_OBJ, &data)) {
		return -1;
	}

	if (data.return_size == 0)
		return 0;

	obj = malloc(data.return_size);
	if (!obj)
		return -1;

	if (read(fd, obj, data.return_size) != data.return_size) {
		free(obj);
		return -1;
	}

	*buf = (u8 *)obj;
	return data.return_size;
}

void
print_objects(int fd, char *name, char **ret)
{
	dev_acpi_t	data;
	char		*buf;

	memset(&data, 0, sizeof(data));

	if (name)
		strcpy(data.pathname, name);

	if (ioctl(fd, DEV_ACPI_GET_OBJECTS, &data)) {
		printf("GET_OBJECTS failed (%s)\n", strerror(errno));
		return;
	}

	buf = malloc(data.return_size);

	if (!buf) {
		printf("failed to malloc get_objects buffer\n");
		return;
	}

	if (data.return_size == 0) {
		printf("return_size == 0\n");
		return;
	}

	if (read(fd, buf, data.return_size) != data.return_size) {
		printf("short read at %s\n", name);
		return;
	}
	*ret = malloc(data.return_size);

	if (!*ret) {
		printf("final malloc failed\n");
		return;
	}

	memcpy(*ret, buf, data.return_size);
}

void
spin_on(int fd, char *name)
{
	dev_acpi_t		data;
	int			size;
	char			*str;

	memset(&data, 0, sizeof(data));

	if (name)
		strcpy(data.pathname, name);

	if (ioctl(fd, DEV_ACPI_DEVICE_NOTIFY, &data)) {
		printf("DEVICE_NOTIFY failed (%s)\n", strerror(errno));
		return;
	}

	str = malloc(getpagesize());
	if (!str) {
		printf("malloc failed\n");
		return;
	}
	memset(str, 0, getpagesize());

	while (1) {
		size = read(fd, str, getpagesize());

		if (size < 0)
			continue;

		printf("Event: [%s]\n", str);
		return;

	}
}

void
print_system_info(int fd)
{
	dev_acpi_t		data;
	struct acpi_system_info	info;
	int			i;


	memset(&data, 0, sizeof(data));

	if (ioctl(fd, DEV_ACPI_SYS_INFO, &data)) {
		printf("SYS_INFO failed (%s)\n", strerror(errno));
		return;
	}
	
	if (data.return_size != sizeof(struct acpi_system_info)) {
		printf("size mismatch %d bytes available, %d bytes wanted\n",
		       data.return_size, (int)sizeof(struct acpi_system_info));
		return;
	}

	if (read(fd, &info, sizeof(info)) != data.return_size) {
		printf("wrong sized read\n");
		return;
	}

	printf("ACPI CA Version: %x\n", info.acpi_ca_version);
	printf("Flags: %08x\n", info.flags);
	printf("Timer resolution: %d\n", info.timer_resolution);
	printf("Debug Level: 0x%08x\n", info.debug_level);
	printf("Debug Layer: 0x%08x\n", info.debug_layer);
	printf("Tables: ");

	for (i = 0 ; i < info.num_table_types ; i++)
		printf("{%s, %d} ", acpi_table_names[i],
		       info.table_info[i].count);
	printf("\n");

}

void
generate_event(int fd, char *name, int type, int evdata)
{
	dev_acpi_t		data;

	memset(&data, 0, sizeof(data));

	sprintf(data.pathname, "%s,0x%08x,0x%08x", name, type, evdata);

	if (ioctl(fd, DEV_ACPI_BUS_GENERATE_EVENT, &data)) {
		printf("BUS_GENERATE_EVENT failed (%s)\n", strerror(errno));
		return;
	}
}

void
get_parent(int fd, char *name, char **buf)
{
	dev_acpi_t		data;

	memset(&data, 0, sizeof(data));

	if (name)
		strcpy(data.pathname, name);

	
	if (ioctl(fd, DEV_ACPI_GET_PARENT, &data)) {
		printf("GET_PARENT failed (%s)\n", strerror(errno));
		return;
	}

	if (read(fd, *buf, data.return_size) != data.return_size) {
		printf("read short\n");
		memset(*buf, 0, data.return_size);
		return;
	}
}

/*
 * Print out a "directory" and recurse through sub-dirs
 */
void
print_level(int fd, char *path, int *entries, int level)
{
	dev_acpi_t	data;
	int		i, cnt;
	char		*buf, *tmp, *tmp2;

	memset(&data, 0, sizeof(data));
	
	if (path)
		strcpy(data.pathname, path);

	if (ioctl(fd, DEV_ACPI_GET_NEXT, &data)) {
		printf("GET_NEXT failed at %s (%s)\n", path, strerror(errno));
		return;
	}

	buf = malloc(data.return_size);

	if (!buf) {
		printf("failed to malloc get_next buffer at %s\n", path);
		return;
	}

	if (data.return_size == 0)
		return;

	if (read(fd, buf, data.return_size) != data.return_size) {
		printf("short read at %s\n", path);
		return;
	}

	dump("GET_NEXT", buf, data.return_size);
	// printf("BUFFER: %s\n", buf);

	tmp = buf;
	for (cnt = 0; (tmp = strchr(tmp, '\n')) != NULL ; cnt++)
		tmp++;

	entries[level] = cnt;
	
	tmp = buf;
	for (i = 0 ; i < cnt ; i++) {
		unsigned long len;
		char *new_path, *cur_obj, hid[8];

		tmp2 = strchr(tmp, '\n');

		len = (unsigned long)tmp2 - (unsigned long)tmp;
		len++;

		cur_obj = malloc(len);

		if (!cur_obj) {
			free(buf);
			return;
		}
		memset(cur_obj, 0, len);
		strncpy(cur_obj, tmp, len - 1);

		tmp = tmp2 + 1;
		
		if (path)
			len += strlen(path) + 1;

		new_path = malloc(len);

		if (!new_path) {
			free(buf);
			return;
		}
		memset(new_path, 0, len);

		if (path) {
			strcat(new_path, path);
			strcat(new_path, ".");
		}
		strcat(new_path, cur_obj);
		
		if (is_dev(fd, new_path)) {
			indent(entries, level);
			printf("%s [%d]\n", cur_obj, get_type(fd, new_path));
			print_level(fd, new_path, entries, level + 1);
		} else {
			indent(entries, level);
			if (!strcmp(cur_obj, "_HID") ||
			    !strcmp(cur_obj, "_CID")) {
				get_hid(fd, new_path, hid);
				printf("%s (%s) [%d]\n", cur_obj, hid, get_type(fd, new_path));
			} else if (!strcmp(cur_obj, "_STR") ||
			           get_type(fd, new_path) == ACPI_TYPE_STRING) {
				char *str = NULL;
				get_string(fd, new_path, &str);
				printf("%s (%s) [%d]\n", cur_obj, str, get_type(fd, new_path));
				free(str);
			} else if (!strcmp(cur_obj, "_STA") ||
			           get_type(fd, new_path)==ACPI_TYPE_INTEGER) {
				printf("%s (0x%llx) [%d]\n", cur_obj,
				       get_integer(fd, new_path),
				       get_type(fd, new_path));
			} else if (!strcmp(cur_obj, "_CRS") ||
			           !strcmp(cur_obj, "_PRT") ||
			           !strcmp(cur_obj, "_MAT")) {
				u8 *buf;
				int i, len;

				len = dump_raw(fd, new_path, &buf);

				if (len <= 0)
					printf("%s (empty/failed) [%d]\n", cur_obj, get_type(fd, new_path));
				else {
					printf("%s [%d]\n", cur_obj, get_type(fd, new_path));

					indent(entries, level);
					printf("\t%04x: ", 0);
					for (i = 0 ; i < len ;) {
						printf("%02x ", buf[i++]);

						if (i % 8 == 0 && i < len) {
							printf("\n");
							indent(entries, level);
							printf("\t%04x: ", i);
						}
					}
					printf("\n");
				}
			} else
				printf("%s [%d]\n", cur_obj, get_type(fd, new_path));
		}

		entries[level]--;
		free(new_path);
		free(cur_obj);
	}


}

/*
 * No options for now, just print entire tree
 */
int
main (int argc, char **argv)
{
	int		entries[ACPI_MAX_STRING] = {0}; 
	int		fd;

	fd = open(DEVICE, O_RDONLY /*| O_NONBLOCK*/);

	if (fd < 0) {
		printf("Error opening file\n");
		return 1;
	}

#if 0
	print_objects(fd, "_LID", &buf);
	if (buf) {
		char *cr;

		printf("buf: [%s]\n", buf);
		cr = strchr(buf, '\n');
		if (cr) {
			*cr = 0;
			get_parent(fd, buf, &buf);
			printf("buf: [%s]\n", buf);
			printf("Tyring to spin on %s\n", buf);
//			spin_on(fd, buf);
		}
	}
	//return 0;
#endif
//	generate_event(fd, "\\_SB_.C139", 0x80, 0x4);
#if 1
	print_system_info(fd);
	printf("\\\\\n");
	print_level(fd, NULL, entries, 1);	
#endif
	close(fd);
	return 0;
}

