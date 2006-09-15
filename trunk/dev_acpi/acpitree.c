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

#include <stdint.h>

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;

typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

#include <acpi/actypes.h>

#define ACPI_MAX_STRING 80
#define ACPI_PATHNAME_MAX 256
#include "dev_acpi.h"

#define DEVICE "/dev/acpi"

static char *acpi_type[0x20] =
{	"Any",
	"Integer",
	"String",
	"Buffer",
	"Package",
	"Field Unit",
	"Device",
	"Event",
	"Method",
	"Mutex",
	"Region",
	"Power",
	"Processor",
	"Thermal",
	"Buffer Field",
	"DDB Handle",
	"Debug Object/External Max", /* 0x10 */
	"Local Region Field",
	"Local Bank Field",
	"Local Index Field",
	"Local Reference",
	"Local Alias",
	"Local Method Alias",
	"Local Notify",
	"Local Address Handler",
	"Local Resource",
	"Local Resource Field",
	"Local Scope/NS Node Max",
	"Local Extra",
	"Local Data/Local Max",
	"Invalid",
	"Not Found" /* 0x1F */
};
	
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

/*
 * Print out a "directory" and recurse through sub-dirs
 */
void
print_level(int fd, char *path, int *entries, int level)
{
	dev_acpi_t	data;
	int		i, cnt, tmp_type;
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
			tmp_type = get_type(fd, new_path);
			if (tmp_type == ACPI_TYPE_NOT_FOUND)
				tmp_type = sizeof(acpi_type) - 1;
			printf("%s [%s]\n", cur_obj, acpi_type[tmp_type]);
			print_level(fd, new_path, entries, level + 1);
		} else {
			indent(entries, level);
			if (!strcmp(cur_obj, "_HID") ||
			    !strcmp(cur_obj, "_CID")) {
				get_hid(fd, new_path, hid);
				tmp_type = get_type(fd, new_path);
				if (tmp_type == ACPI_TYPE_NOT_FOUND)
					tmp_type = sizeof(acpi_type) - 1;
				printf("%s (%s) [%s]\n", cur_obj, hid, acpi_type[tmp_type]);
			} else if (!strcmp(cur_obj, "_STR") ||
			           get_type(fd, new_path) == ACPI_TYPE_STRING) {
				char *str = NULL;
				get_string(fd, new_path, &str);
				tmp_type = get_type(fd, new_path);
				if (tmp_type == ACPI_TYPE_NOT_FOUND)
					tmp_type = sizeof(acpi_type) - 1;
				printf("%s (%s) [%s]\n", cur_obj, str, acpi_type[tmp_type]);
				free(str);
			} else if (!strcmp(cur_obj, "_STA") ||
			           get_type(fd, new_path)==ACPI_TYPE_INTEGER) {
				tmp_type = get_type(fd, new_path);
				if (tmp_type == ACPI_TYPE_NOT_FOUND)
					tmp_type = sizeof(acpi_type) - 1;
				printf("%s (0x%llx) [%s]\n", cur_obj,
				       get_integer(fd, new_path),
				       acpi_type[tmp_type]);
			} else if (!strcmp(cur_obj, "_CRS") ||
			           !strcmp(cur_obj, "_PRS") ||
			           !strcmp(cur_obj, "_PRT") ||
			           !strcmp(cur_obj, "_MAT")) {
				u8 *buf;
				int i, len;

				len = dump_raw(fd, new_path, &buf);

				tmp_type = get_type(fd, new_path);
				if (tmp_type == ACPI_TYPE_NOT_FOUND)
					tmp_type = sizeof(acpi_type) - 1;
				if (len <= 0)
					printf("%s (empty/failed) [%s]\n", cur_obj, acpi_type[tmp_type]);
				else {
					printf("%s [%s]\n", cur_obj, acpi_type[tmp_type]);

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
			} else {
				tmp_type = get_type(fd, new_path);
				if (tmp_type == ACPI_TYPE_NOT_FOUND)
					tmp_type = sizeof(acpi_type) - 1;
				printf("%s [%s]\n", cur_obj, acpi_type[tmp_type]);
			}
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

	fd = open(DEVICE, O_RDONLY);

	if (fd < 0) {
		printf("Error opening file\n");
		return 1;
	}

	printf("\\\\\n");
	print_level(fd, NULL, entries, 1);	
	close(fd);
	return 0;
}

