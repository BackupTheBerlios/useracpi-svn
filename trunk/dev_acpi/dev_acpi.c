/*
 * Copyright (c) 2004-2005 Hewlett Packard, LLC
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

#include <linux/module.h>
#include <linux/config.h>
#include <linux/init.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
/* The ioctl32 conversion stuff on 2.4 needs work */
# undef CONFIG_COMPAT
/* Random stuff that's changed since 2.4 */
# define __user
# define try_module_get(x) MOD_INC_USE_COUNT
# define module_put(x) MOD_DEC_USE_COUNT
#endif

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/list.h>
#ifdef CONFIG_COMPAT
# include <linux/ioctl32.h>
# include <linux/syscalls.h>
#endif

#include <asm/semaphore.h>
#include <asm/uaccess.h>

#include <acpi/acpi.h>
#include <acpi/acpi_drivers.h>
#include <acpi/acpiosxf.h>

#include "dev_acpi.h"

MODULE_AUTHOR("Alex Williamson, HP (alex.williamson@hp.com)");
MODULE_DESCRIPTION("Device file access to ACPI namespace");
MODULE_LICENSE("GPL");

static int major;

#define DEV_ACPI_NAME "dev_acpi"
#define DEV_ACPI_DEVICE_NAME "acpi"

#define TO_POINTER 0
#define TO_OFFSET 1

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
# include <linux/device.h>
# if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,13)
#  define CLASS_CREATE class_simple_create
#  define CLASS_DEVICE_CREATE class_simple_device_add
#  define CLASS_DEVICE_DESTROY(x, y) class_simple_device_remove(y)
#  define CLASS_DESTROY class_simple_destroy
   static struct class_simple *dev_acpi_class;
# else
#  define CLASS_CREATE class_create
#  if  LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16)
#    define CLASS_DEVICE_CREATE class_device_create
#  else
#    define CLASS_DEVICE_CREATE(cls, devt, device, fmt) \
            class_device_create(cls, NULL, devt, device, fmt)
#  endif
#  define CLASS_DEVICE_DESTROY class_device_destroy
#  define CLASS_DESTROY class_destroy
   static struct class *dev_acpi_class;
# endif
#endif

typedef struct {
	struct semaphore	sem;
	struct acpi_buffer	read;
	struct acpi_buffer	write;
	struct list_head	notify;
} priv_data_t;

struct notify_list {
	struct list_head	node;
	acpi_handle		device;
	u32			type;
};
	
#define RBUF(x)			(&((priv_data_t *)(x->private_data))->read)
#define WBUF(x)			(&((priv_data_t *)(x->private_data))->write)

#define PADDR(ptr)		((unsigned long)(ptr))
#define PADDR_END(ptr, size)	(PADDR(ptr) + PADDR(size))
#define POFFSET(ptr, end)	(PADDR(end) - PADDR(ptr))

static char *
strdup(char *orig)
{
	char *new = kmalloc(strlen(orig) + 1, GFP_KERNEL);
	if (!new)
		return NULL;
	return strcpy(new, orig);
}

static int
range_ok(
	void			*ptr,
	struct acpi_buffer	*range,
	ssize_t			size)
{
	if (PADDR(ptr) < PADDR(range->pointer))
		return 0;
	if (PADDR_END(ptr, size) > PADDR_END(range->pointer, range->length))
		return 0;

	return 1;
}

/*
 * The next few function are meant to replaces pointers in data structures
 * with offsets or vica versa.  It's important to check the range to make
 * sure a malicious program doesn't try to send us off somewhere else.
 */
static void *
fixup_pointer(
	struct acpi_buffer	*range,
	void			*off,
	int			direction)
{
	if (direction == TO_POINTER)
		return (void *)PADDR_END(range->pointer, off);
	else
		return (void *)POFFSET(range->pointer, off);
}

static int
fixup_string(
	union acpi_object	*obj,
	struct acpi_buffer	*range,
	int			direction)
{
	char **pointer = &obj->string.pointer;

	if (direction == TO_OFFSET) {
		if (!range_ok(*pointer, range, obj->string.length))
			return 0;
	}

	*pointer = fixup_pointer(range, *pointer, direction);

	if (direction == TO_POINTER) {
		if (!range_ok(*pointer, range, obj->string.length))
			return 0;
	}
	return 1;
}

static int
fixup_buffer(
	union acpi_object	*obj,
	struct acpi_buffer	*range,
	int			direction)
{
	unsigned char **pointer = &obj->buffer.pointer;

	if (direction == TO_OFFSET) {
		if (!range_ok(*pointer, range, obj->buffer.length))
			return 0;
	}

	*pointer = fixup_pointer(range, *pointer, direction);

	if (direction == TO_POINTER) {
		if (!range_ok(*pointer, range, obj->buffer.length))
			return 0;
	}
	return 1;
}

static int fixup_package(union acpi_object *, struct acpi_buffer *, int);

/*
 * strings, buffers, and packages contain pointers.  These should just
 * be pointing further down in the buffer, so before passing to user
 * space, change the pointers into offsets from the beginning of the buffer.
 */
static int
fixup_element(
	union acpi_object	*obj,
	struct acpi_buffer	*range,
	int			direction)
{
	if (!obj)
		return 0;

	switch (obj->type) {
		case ACPI_TYPE_STRING:
			return fixup_string(obj, range, direction);
		case ACPI_TYPE_BUFFER:
			return fixup_buffer(obj, range, direction);
		case ACPI_TYPE_PACKAGE:
			return fixup_package(obj, range, direction);
		default:
			/* No fixup necessary */
			return 1;
	}
}

static int
fixup_package(
	union acpi_object	*obj,
	struct acpi_buffer	*range,
	int			direction)
{
	int			count;
	union acpi_object	*element, **pointer;

	element = NULL;
	pointer = &obj->package.elements;

	if (direction == TO_OFFSET) {
		element = *pointer;
		if (!range_ok(*pointer, range, sizeof(union acpi_object)))
			return 0;
	}

	*pointer = fixup_pointer(range, *pointer, direction);

	if (direction == TO_POINTER) {
		element = *pointer;
		if (!range_ok(*pointer, range, sizeof(union acpi_object)))
			return 0;
	}

	for (count = 0 ; count < obj->package.count ; count++) {
		if (!fixup_element(element, range, direction))
			return 0;
		element++;
	}
	return 1;
}

static struct acpi_object_list *
fixup_arglist(struct acpi_buffer *buffer)
{
	struct acpi_object_list *arg_list;
	union acpi_object *cur_arg;
	unsigned int i;

	/* make sure length is at least big enough to validate the count */
	if (buffer->length < sizeof(*arg_list))
        	return NULL;

	arg_list = (struct acpi_object_list *)buffer->pointer;

	if (buffer->length < sizeof(*arg_list) +
                     (arg_list->count * sizeof(union acpi_object)))
		return NULL;

	/*
	 * The pointer in the object list points to an array of acpi_objects.
	 * Fix the pointer to the array, then fix each element.
	 */
	cur_arg = (union acpi_object *)PADDR_END(arg_list, arg_list->pointer);

	arg_list->pointer = cur_arg;

	for (i = 0; i < arg_list->count ; i++, cur_arg++) {

		if (!range_ok(cur_arg, buffer, sizeof(union acpi_object)))
			return NULL;

		if (!fixup_element(cur_arg, buffer, TO_POINTER))
			return NULL;
	}
	return arg_list;
}

/*
 * Clean up the private data pointer
 */
#define READ_CLEAR 0x1
#define WRITE_CLEAR 0x2

static void
dev_acpi_clear(struct file *f, int type)
{
	struct acpi_buffer *buffer;
	
	while (type) {
		if (type & READ_CLEAR) {
			buffer = RBUF(f);
			type &= ~READ_CLEAR;
		} else if (type & WRITE_CLEAR) {
			buffer = WBUF(f);
			type &= ~WRITE_CLEAR;
		} else
			return;

		if (!buffer->pointer)
			continue;

		kfree(buffer->pointer);
		buffer->pointer = NULL;
		buffer->length = 0;
	}
}

/*
 * Try to handle paths from the filesystem, guess root from "ACPI"
 * directory, convert '/' to '.'.  Should I just let userpsace
 * take care of this?
 */
static char *
dev_acpi_parse_path(char *orig_path)
{
	char *new_path, *tmp;

	tmp = strstr(orig_path, "ACPI");
	if (tmp)
		tmp += 5;
	else
		tmp = orig_path;

	new_path = strdup(tmp);

	if (!new_path)
		return NULL;

	while ((tmp = strchr(new_path, '/')) != NULL)
		*tmp = '.';

	return new_path;
}

/*
 * Given path, try to get an ACPI handle
 */
static acpi_handle
dev_acpi_get_handle(char *path)
{
	char *new_path;
	acpi_handle handle;
	acpi_status status;

	if (!strlen(path))
		return ACPI_ROOT_OBJECT;

	new_path = dev_acpi_parse_path(path);

	if (!new_path)
		return NULL;

	status = acpi_get_handle(NULL, new_path, &handle);
	kfree(new_path);

	return ACPI_SUCCESS(status) ? handle : NULL;
}

/* Return a buffer of object below a given handle */
static acpi_status
dev_acpi_get_next(
	acpi_handle		handle,
	struct acpi_buffer	*buffer)
{
	acpi_handle		chandle;
	size_t			new_size;
	char			*new_buf, pathname[ACPI_PATHNAME_MAX];
	struct acpi_buffer	path_buf = {ACPI_PATHNAME_MAX, pathname};

	if (buffer->length || buffer->pointer)
		return AE_ALREADY_EXISTS;

	/* Setup the string terminator, then just push it along */
	buffer->pointer = kmalloc(1, GFP_KERNEL);
	if (!buffer->pointer)
		return AE_NO_MEMORY;

	buffer->length = 1;
	memset(buffer->pointer, 0, 1);

	chandle = NULL;

	while (ACPI_SUCCESS(acpi_get_next_object(ACPI_TYPE_ANY, handle,
	                                         chandle, &chandle))) {

		path_buf.length = sizeof(pathname);
		memset(pathname, 0, sizeof(pathname));

		if (ACPI_FAILURE(acpi_get_name(chandle, ACPI_SINGLE_NAME,
		                               &path_buf)))
			continue;

		/*
		 * length includes terminator, which we'll replace
		 * with a line feed.
		 */
		new_size = buffer->length + path_buf.length;
		new_buf = kmalloc(new_size, GFP_KERNEL);

		if (!new_buf) {
			kfree(buffer->pointer);
			buffer->pointer = NULL;
			buffer->length = 0;
			return AE_NO_MEMORY;
		}

		memset(new_buf, 0, new_size);

		sprintf(new_buf, "%s%s\n", (char *)buffer->pointer, pathname);

		kfree(buffer->pointer);

		buffer->pointer = new_buf;
		buffer->length = new_size;
	}

	/* if nothing found, return nothing */
	if (buffer->length == 1) {
		kfree(buffer->pointer);
		buffer->pointer = NULL;
		buffer->length = 0;
	}

	return AE_OK;
}

static acpi_status
dev_acpi_get_devices_callback(
	acpi_handle	handle,
	u32		depth,
	void		*context,
	void		**ret)
{
	struct acpi_buffer	*ret_buf;
	acpi_status		status;
	size_t			new_size;
	char			*new_buf, pathname[ACPI_PATHNAME_MAX];
	struct acpi_buffer	buffer = {ACPI_PATHNAME_MAX, pathname};

	ret_buf = *ret;

	memset(pathname, 0, sizeof(pathname));
	status = acpi_get_name(handle, ACPI_FULL_PATHNAME, &buffer);

	if (ACPI_FAILURE(status))
		return status;

	/*
	 * length includes terminator, which we'll replace
	 * with a line feed.
	 */
	new_size = buffer.length + ret_buf->length;
	new_buf = kmalloc(new_size, GFP_KERNEL);

	if (!new_buf)
		return AE_NO_MEMORY;

	memset(new_buf, 0, new_size);

	sprintf(new_buf, "%s%s\n", (char *)ret_buf->pointer, pathname);

	kfree(ret_buf->pointer);

	ret_buf->pointer = new_buf;
	ret_buf->length = new_size;

	return AE_OK;
}

static acpi_status
dev_acpi_get_devices(char *hid, struct acpi_buffer *buffer)
{
	acpi_status	status;

	if (buffer->length || buffer->pointer)
		return AE_ALREADY_EXISTS;

	/* Setup the string terminator, then just push it along */
	buffer->pointer = kmalloc(1, GFP_KERNEL);
	if (!buffer->pointer)
		return AE_NO_MEMORY;

	buffer->length = 1;
	memset(buffer->pointer, 0, 1);

	status = acpi_get_devices(hid, dev_acpi_get_devices_callback,
	                          NULL, (void **)&buffer);
	
	if (ACPI_FAILURE(status) || buffer->length == 1) {
		kfree(buffer->pointer);
		buffer->length = 0;
		buffer->pointer = NULL;
		if (buffer->length == 1)
			status = AE_NOT_FOUND;
	}
	return status;
}

static acpi_status
dev_acpi_get_objects_callback(
	acpi_handle	handle,
	u32		depth,
	void		*context,
	void		**ret)
{
	struct acpi_buffer	*ret_buf;
	acpi_status		status;
	size_t			new_size;
	char			*new_buf, *name, pathname[ACPI_PATHNAME_MAX];
	struct acpi_buffer	buffer = {ACPI_PATHNAME_MAX, pathname};

	ret_buf = *ret;

	memset(pathname, 0, sizeof(pathname));
	name = (char *)context;

	/*
	 * First get the single name to see if this is what we're looking
	 * for.  If it is, get the full path.
	 */
	status = acpi_get_name(handle, ACPI_SINGLE_NAME, &buffer);

	if (ACPI_FAILURE(status))
		return status;

	if (strcmp(name, pathname))
		return AE_OK; /* Not it */

	memset(pathname, 0, sizeof(pathname));
	buffer.length = sizeof(pathname);

	status = acpi_get_name(handle, ACPI_FULL_PATHNAME, &buffer);

	if (ACPI_FAILURE(status))
		return status;

	/*
	 * length includes terminator, which we'll replace
	 * with a line feed.
	 */
	new_size = buffer.length + ret_buf->length;
	new_buf = kmalloc(new_size, GFP_KERNEL);

	if (!new_buf)
		return AE_NO_MEMORY;

	memset(new_buf, 0, new_size);

	sprintf(new_buf, "%s%s\n", (char *)ret_buf->pointer, pathname);

	kfree(ret_buf->pointer);

	ret_buf->pointer = new_buf;
	ret_buf->length = new_size;

	return AE_OK;
}

static acpi_status
dev_acpi_get_objects(char *name, struct acpi_buffer *buffer)
{
	acpi_status status;

	if (buffer->length || buffer->pointer)
		return AE_ALREADY_EXISTS;

	/* Setup the string terminator, then just push it along */
	buffer->pointer = kmalloc(1, GFP_KERNEL);

	if (!buffer->pointer)
		return AE_NO_MEMORY;

	buffer->length = 1;
	memset(buffer->pointer, 0, 1);

	status = acpi_walk_namespace(ACPI_TYPE_ANY,
	                             ACPI_ROOT_OBJECT,
	                             ACPI_UINT32_MAX,
	                             dev_acpi_get_objects_callback,
	                             (void *)name,
	                             (void **)&buffer);
	
	if (ACPI_FAILURE(status) || buffer->length == 1) {
		kfree(buffer->pointer);
		buffer->length = 0;
		buffer->pointer = NULL;
		if (buffer->length == 1)
			status = AE_NOT_FOUND;
	}
	return status;
}

static ssize_t
dev_acpi_read(
	struct file	*f,
	char __user	*buf,
	size_t		len,
	loff_t		*off)
{
	unsigned char		*copy_addr;
	size_t			copy_len;
	struct acpi_buffer	*buffer;
	priv_data_t		*priv;

	priv = (priv_data_t *)f->private_data;

try_again:
	if (down_trylock(&priv->sem)) {
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (down_interruptible(&priv->sem))
			return -ERESTARTSYS;
	}

	buffer = RBUF(f);

	if (!buffer->length || !buffer->pointer) {
		/*
		 * If notifiers are setup, allow the reader to wait for data.
		 * Otherwise, there's none here now an none on the way.
		 */
		if (list_empty(&priv->notify)) {
			up(&priv->sem);
			return -ENODEV;
		}
		goto try_again;
	}

	up(&priv->sem);

	if (*off > buffer->length)
		return -EFAULT;

	copy_addr = buffer->pointer + *off;
	copy_len = min((size_t)(buffer->length - *off), len);

	if (copy_to_user(buf, copy_addr, copy_len))
		return -EFAULT;

	return copy_len;
}

static ssize_t
dev_acpi_write(
	struct file		*f,
	const char __user	*buf,
	size_t			len,
	loff_t			*off)
{
	unsigned char		*start;
	struct acpi_buffer	*buffer;

	buffer = WBUF(f);

	if (len + *off > buffer->length) {
		void *new_buf;

		new_buf = kmalloc(len + *off, GFP_KERNEL);

		if (!new_buf)
			return -ENOMEM;

		memset(new_buf, 0, len + *off);

		if (buffer->length && buffer->pointer)
			memcpy(new_buf, buffer->pointer, buffer->length);

		kfree(buffer->pointer);

		buffer->pointer = new_buf;
		buffer->length = len + *off;
	}

	start = buffer->pointer + *off;

	if (copy_from_user(start, buf, len))
		return -EFAULT;

	return len;
}

static void
dev_acpi_notify(
	acpi_handle	handle,
	u32		event,
	void		*data)
{
	struct file		*f;
	priv_data_t		*priv;
	char			*str;
	int			size;
	char			pathname[ACPI_PATHNAME_MAX];
	struct acpi_buffer	*buffer, strbuf = {ACPI_PATHNAME_MAX, pathname};
	
	f = (struct file *)data;
	priv = (priv_data_t *)f->private_data;

	memset(pathname, 0, sizeof(pathname));

	if (ACPI_FAILURE(acpi_get_name(handle, ACPI_FULL_PATHNAME, &strbuf)))
		sprintf(pathname, "????");

	size = strlen(pathname) + 10; /* see sprintf below */
	str = kmalloc(size, GFP_KERNEL);

	if (!str) {
		printk(KERN_WARNING "%s() kmalloc failed, event %s,%08x lost\n",
		       __FUNCTION__, pathname, event);
		return;
	}

	memset(str, 0, size);
	dev_acpi_clear(f, READ_CLEAR | WRITE_CLEAR);

	sprintf(str, "%s,%08x", pathname, event);

	buffer = RBUF(f);
	buffer->pointer = str;
	buffer->length = size;

	up(&priv->sem);
}

static int
dev_acpi_open(
	struct inode	*i,
	struct file	*f)
{
	priv_data_t	*priv;

	if (!try_module_get(THIS_MODULE))
		return -EBUSY;

	f->private_data = kmalloc(sizeof(priv_data_t), GFP_KERNEL);
	if (!f->private_data) {
		module_put(THIS_MODULE);
		return -ENOMEM;
	}

	memset(f->private_data, 0, sizeof(priv_data_t));

	priv = (priv_data_t *)f->private_data;
	sema_init(&priv->sem, 1);
	INIT_LIST_HEAD(&priv->notify);
	return 0;
}

static int
dev_acpi_release(
	struct inode	*i,
	struct file	*f)
{
	struct list_head	*list;
	struct notify_list	*notify;
	priv_data_t		*priv = (priv_data_t *)f->private_data;

	list = &priv->notify;
	while (!list_empty(list)) {
		notify = list_entry(list->next, struct notify_list, node);
		if (notify->device && notify->type)
			acpi_remove_notify_handler(notify->device, notify->type,
			                           dev_acpi_notify);

		list_del(&notify->node);
		kfree(notify);
	}

	dev_acpi_clear(f, READ_CLEAR | WRITE_CLEAR);
	kfree(f->private_data);
	module_put(THIS_MODULE);
	return 0;
}

static int
dev_acpi_ioctl(
	struct inode	*i,
	struct file	*f,
	unsigned int	cmd,
	unsigned long	arg)
{
	priv_data_t *priv = (priv_data_t *)f->private_data;

	/* Do stuff... */
	if (cmd == DEV_ACPI_EXISTS) {
		dev_acpi_t	data;
		acpi_handle	handle;

		dev_acpi_clear(f, READ_CLEAR | WRITE_CLEAR);

		if (copy_from_user(&data, (dev_acpi_t *)arg, sizeof(data)))
			return -EFAULT;

		handle = dev_acpi_get_handle(data.pathname);

		if (!handle)
			return -ENOENT;

		return 0;

	} else if (cmd == DEV_ACPI_GET_TYPE) {
		dev_acpi_t		data;
		acpi_handle		handle;
		acpi_object_type	type;
		struct acpi_buffer	*buffer;
		union acpi_object	*obj;

		dev_acpi_clear(f, READ_CLEAR | WRITE_CLEAR);

		if (copy_from_user(&data, (dev_acpi_t *)arg, sizeof(data)))
			return -EFAULT;

		handle = dev_acpi_get_handle(data.pathname);

		if (!handle)
			return -ENOENT;

		if (ACPI_FAILURE(acpi_get_type(handle, &type)))
			return -EFAULT;

		buffer = RBUF(f);
		buffer->pointer = kmalloc(sizeof(*obj), GFP_KERNEL);

		if (!buffer->pointer)
			return -ENOMEM;

		memset(buffer->pointer, 0, sizeof(*obj));
		buffer->length = sizeof(*obj);

		obj = buffer->pointer;
		obj->type = ACPI_TYPE_INTEGER;
		obj->integer.value = type;

		data.return_size = buffer->length;
		if (copy_to_user((dev_acpi_t *)arg, &data, sizeof(data))) {
			dev_acpi_clear(f, READ_CLEAR);
			return -EFAULT;
		}

		up(&priv->sem);
		return 0;

	} else if (cmd == DEV_ACPI_EVALUATE_OBJ) {
		dev_acpi_t		data;
		acpi_handle		handle;
		struct acpi_object_list	*args;
		acpi_status		status;
		struct acpi_buffer	*wbuf, *rbuf;
		struct acpi_buffer	buffer = {ACPI_ALLOCATE_BUFFER, NULL};

		dev_acpi_clear(f, READ_CLEAR);

		if (copy_from_user(&data, (dev_acpi_t *)arg, sizeof(data)))
			return -EFAULT;

		handle = dev_acpi_get_handle(data.pathname);

		if (!handle)
			return -ENOENT;

		args = NULL;
		wbuf = WBUF(f);
		rbuf = RBUF(f);

		/* check for object list in write buffer */
		if (wbuf->pointer && wbuf->length >=
		                     sizeof(struct acpi_object_list) +
		                     sizeof(union acpi_object)) {

			args = fixup_arglist(wbuf);
			if (!args) {
				dev_acpi_clear(f, WRITE_CLEAR);
				return -EINVAL;
			}
		}

		status = acpi_evaluate_object(handle, NULL, args, &buffer);

		dev_acpi_clear(f, WRITE_CLEAR);

		if (ACPI_FAILURE(status))
			return -ENOENT;

		data.return_size = 0;

		if (buffer.pointer)  {
			if (fixup_element((union acpi_object *)buffer.pointer,
			                  &buffer, TO_OFFSET)) {
				rbuf->length = data.return_size = buffer.length;
				rbuf->pointer = buffer.pointer;
			} else
				kfree(buffer.pointer);
		}

		if (copy_to_user((dev_acpi_t *)arg, &data, sizeof(data))) {
			dev_acpi_clear(f, READ_CLEAR);
			return -EFAULT;
		}

		up(&priv->sem);
		return 0;

	/* List of child objects for a path */
	} else if (cmd == DEV_ACPI_GET_NEXT) {
		dev_acpi_t			data;
		acpi_handle			handle;
		struct acpi_buffer		*buffer = RBUF(f);

		dev_acpi_clear(f, READ_CLEAR | WRITE_CLEAR);

		if (copy_from_user(&data, (dev_acpi_t *)arg, sizeof(data)))
			return -EFAULT;

		handle = dev_acpi_get_handle(data.pathname);

		if (!handle)
			return -ENOENT;

		if (ACPI_FAILURE(dev_acpi_get_next(handle, buffer)))
			return -EFAULT;

		data.return_size = buffer->length;

		if (copy_to_user((dev_acpi_t *)arg, &data, sizeof(data))) {
			dev_acpi_clear(f, READ_CLEAR);
			return -EFAULT;
		}

		up(&priv->sem);
		return 0;

	} else if (cmd == DEV_ACPI_CLEAR) {
		dev_acpi_clear(f, READ_CLEAR | WRITE_CLEAR);
		return 0;
	/*
	 * Given a HID/CID value as the path, return a list of paths
	 * with the requested ID
	 */
	} else if (cmd == DEV_ACPI_GET_DEVICES) {
		dev_acpi_t			data;
		struct acpi_buffer		*buffer = RBUF(f);

		dev_acpi_clear(f, READ_CLEAR | WRITE_CLEAR);

		if (copy_from_user(&data, (dev_acpi_t *)arg, sizeof(data)))
			return -EFAULT;

		if (ACPI_FAILURE(dev_acpi_get_devices(data.pathname, buffer)))
			return -EFAULT;

		data.return_size = buffer->length;

		if (copy_to_user((dev_acpi_t *)arg, &data, sizeof(data))) {
			dev_acpi_clear(f, READ_CLEAR);
			return -EFAULT;
		}
		up(&priv->sem);
		return 0;

	} else if (cmd == DEV_ACPI_GET_OBJECTS) {
		dev_acpi_t			data;
		struct acpi_buffer		*buffer = RBUF(f);

		dev_acpi_clear(f, READ_CLEAR | WRITE_CLEAR);

		if (copy_from_user(&data, (dev_acpi_t *)arg, sizeof(data)))
			return -EFAULT;

		if (ACPI_FAILURE(dev_acpi_get_objects(data.pathname, buffer)))
			return -EFAULT;

		data.return_size = buffer->length;

		if (copy_to_user((dev_acpi_t *)arg, &data, sizeof(data))) {
			dev_acpi_clear(f, READ_CLEAR);
			return -EFAULT;
		}
		up(&priv->sem);
		return 0;

	} else if (cmd == DEV_ACPI_GET_PARENT) {
		dev_acpi_t		data;
		acpi_handle		handle, phandle;
		struct acpi_buffer	*buffer;
		char			pathname[ACPI_PATHNAME_MAX];
		struct acpi_buffer	strbuf = {ACPI_PATHNAME_MAX, pathname};

		memset(pathname, 0, sizeof(pathname));
		dev_acpi_clear(f, READ_CLEAR | WRITE_CLEAR);

		if (copy_from_user(&data, (dev_acpi_t *)arg, sizeof(data)))
			return -EFAULT;

		handle = dev_acpi_get_handle(data.pathname);

		if (!handle)
			return -ENOENT;

		if (ACPI_FAILURE(acpi_get_parent(handle, &phandle)))
			return -EFAULT;

		if (ACPI_FAILURE(acpi_get_name(phandle, ACPI_FULL_PATHNAME,
		                               &strbuf)))
			return -EFAULT;

		buffer = RBUF(f);
		buffer->pointer = strdup(pathname);

		if (!buffer->pointer)
			return -ENOMEM;

		data.return_size = buffer->length = strlen(pathname) + 1;

		if (copy_to_user((dev_acpi_t *)arg, &data, sizeof(data))) {
			dev_acpi_clear(f, READ_CLEAR);
			return -EFAULT;
		}
		up(&priv->sem);
		return 0;

	} else if (cmd == DEV_ACPI_SYS_INFO) {
#if 0
		dev_acpi_t			data;
		struct acpi_buffer		*buffer = RBUF(f);

		dev_acpi_clear(f, READ_CLEAR | WRITE_CLEAR);

		if (copy_from_user(&data, (dev_acpi_t *)arg, sizeof(data)))
			return -EFAULT;

		buffer->length = ACPI_ALLOCATE_BUFFER;
		buffer->pointer = NULL;

		if (ACPI_FAILURE(acpi_get_system_info(buffer)))
			return -ENOMEM;

		data.return_size = buffer->length;
		if (copy_to_user((dev_acpi_t *)arg, &data, sizeof(data))) {
			dev_acpi_clear(f, READ_CLEAR);
			return -EFAULT;
		}
		up(&priv->sem);
		return 0;
#else
		return -EINVAL;
#endif
	} else if (cmd == DEV_ACPI_DEVICE_NOTIFY ||
	           cmd == DEV_ACPI_SYSTEM_NOTIFY) {

		dev_acpi_t		data;
		acpi_handle		handle;
		acpi_status		status;
		struct notify_list	*entry;
		u32			type;

		dev_acpi_clear(f, READ_CLEAR | WRITE_CLEAR);

		if (copy_from_user(&data, (dev_acpi_t *)arg, sizeof(data)))
			return -EFAULT;

		handle = dev_acpi_get_handle(data.pathname);

		if (!handle)
			return -ENOENT;

		entry = kmalloc(sizeof(*entry), GFP_KERNEL);

		if (!entry)
			return -ENOMEM;

		memset(entry, 0, sizeof(*entry));

		type = (cmd == DEV_ACPI_SYSTEM_NOTIFY) ?
		       ACPI_SYSTEM_NOTIFY : ACPI_DEVICE_NOTIFY;

		status = acpi_install_notify_handler(handle, type,
		                                     dev_acpi_notify, f);

		if (ACPI_FAILURE(status)) {
			kfree(entry);
			if (status == AE_ALREADY_EXISTS)
				return -EEXIST;
			return -EIO;
		}

		entry->device = handle;
		entry->type = type;
		list_add_tail(&entry->node, &priv->notify);

		return 0;

	} else if (cmd == DEV_ACPI_REMOVE_DEVICE_NOTIFY ||
	           cmd == DEV_ACPI_REMOVE_SYSTEM_NOTIFY) {

		dev_acpi_t		data;
		acpi_handle		handle;
		acpi_status		status;
		struct notify_list	*entry;
		struct list_head	*node;
		u32			type;

		dev_acpi_clear(f, READ_CLEAR | WRITE_CLEAR);

		if (copy_from_user(&data, (dev_acpi_t *)arg, sizeof(data)))
			return -EFAULT;

		handle = dev_acpi_get_handle(data.pathname);

		if (!handle)
			return -ENOENT;

		type = (cmd == DEV_ACPI_REMOVE_SYSTEM_NOTIFY) ?
		       ACPI_SYSTEM_NOTIFY : ACPI_DEVICE_NOTIFY;

		status = acpi_remove_notify_handler(handle, type,
		                                    dev_acpi_notify);

		if (ACPI_FAILURE(status))
			return -EIO;

		list_for_each(node, &priv->notify) {
			entry = list_entry(node, struct notify_list, node);

			if (entry->device == handle && entry->type == type) {
				list_del(&entry->node);
				kfree(entry);
				break;
			}
		}
		return 0;

	} else if (cmd == DEV_ACPI_BUS_GENERATE_EVENT) {

		dev_acpi_t			data;
		unsigned long			length;
		acpi_handle			handle;
		struct acpi_device		*device;
		int				ret, evdata;
		char				*chr, *endp, *path;
		u8				type;

		dev_acpi_clear(f, READ_CLEAR | WRITE_CLEAR);

		if (copy_from_user(&data, (dev_acpi_t *)arg, sizeof(data)))
			return -EFAULT;

		if (strlen(data.pathname) > ACPI_PATHNAME_MAX)
			return -EINVAL;

		chr = strchr(data.pathname, ',');
		if (!chr)
			return -EINVAL;

		length = POFFSET(data.pathname, chr);
		path = kmalloc(length + 1, GFP_KERNEL);
		if (!path)
			return -ENOMEM;
		memset(path, 0, length + 1);

		strncpy(path, data.pathname, length);

		handle = dev_acpi_get_handle(path);

		kfree(path);

		if (!handle)
			return -ENOENT;

		chr++;
		type = simple_strtoul(chr, &endp, 0);
			
		chr = strchr(chr, ',');
		if (!chr)
			return -EINVAL;
		chr++;
		evdata = simple_strtoul(chr, &endp, 0);

		device = NULL;
		ret = acpi_bus_get_device(handle, &device);

		if (ret)
			return ret;

		return acpi_bus_generate_event(device, type, evdata);
	}
	return -EINVAL;
}

static struct file_operations dev_acpi_fops = {
	.owner		= THIS_MODULE,
	.read		= dev_acpi_read,
	.write		= dev_acpi_write,
	.ioctl		= dev_acpi_ioctl,
	.open		= dev_acpi_open,
	.release	= dev_acpi_release,
};

#ifdef CONFIG_COMPAT

/*
 * First define a 32bit data structure that has 4 byte alignment
 * regardless of the native alignement.
 */
#define align32(x) __attribute__ ((aligned((x) < 4 ? x : 4)))

typedef acpi_object_type align32(sizeof(acpi_object_type)) acpi_object_type32;
typedef acpi_integer align32(sizeof(acpi_integer)) acpi_integer32;
/* just for completeness */
typedef u32 align32(sizeof(u32)) u32_32;
typedef u8 align32(sizeof(u8)) u8_32;

/*
 * ILP32 acpi_object
 */
union acpi_object32
{
	acpi_object_type32		type;
	struct
	{
		acpi_object_type32	type;
		acpi_integer32		value;
	} integer;

	struct
	{
		acpi_object_type32	type;
		u32_32			length;
		u32_32			pointer;
	} string;

	struct
	{
		acpi_object_type32	type;
		u32_32			length;
		u32_32			pointer;
	} buffer;

	struct
	{
		acpi_object_type32	type;
		u32_32			count;
		u32_32			elements;
	} package;
	/* sizeof(union acpi_object) on ILP32 system */
	u8_32				raw[16];
};

struct acpi_object_list32
{
	u32_32			count;
	u32_32			pointer;
};

#ifdef DEBUG
static void
dump_buffer(
	char			*msg,
	struct acpi_buffer	*buffer)
{
	int		i;
	unsigned char	*out;

	printk("\n%s:\n",msg);

	out = buffer->pointer;

	printk("%04x: ", 0);
	for (i = 0 ; i < buffer->length ;) {
		printk("%02x ", out[i++]);
		if (i % 16 == 0 && i < buffer->length)
			printk("\n%04x: ", i);
	}
	printk("\n");
}
#else
#define dump_buffer(msg, buffer)
#endif

/* Count number of acpi_objects and additional bytes required for an element */
static int
calc_element_size32(
	union acpi_object32	*obj,
	unsigned long		start,
	int			*objs,
	int			*bytes)
{
	int			i;
	union acpi_object32	*next;

	*objs +=1;

	switch (obj->type) {
	case ACPI_TYPE_STRING:
		*bytes += ACPI_ROUND_UP_TO_NATIVE_WORD(obj->string.length);
		return 1;
	case ACPI_TYPE_BUFFER:
		*bytes += ACPI_ROUND_UP_TO_NATIVE_WORD(obj->buffer.length);
		return 1;
	case ACPI_TYPE_PACKAGE:
		next = (union acpi_object32 *)PADDR_END(start,
		                                        obj->package.elements);

		for (i = 0 ; i < obj->package.count ; i++, next++)
			if (!calc_element_size32(next, start, objs, bytes))
				return 0;

		return 1;
	default:
		return 1;
	}
}

static int
calc_element_size(
	union acpi_object	*obj,
	unsigned long		start,
	int			*objs,
	int			*bytes)
{
	int			i;
	union acpi_object	*next;

	*objs +=1;

	switch (obj->type) {
	case ACPI_TYPE_STRING:
		*bytes += obj->string.length;
		return 1;
	case ACPI_TYPE_BUFFER:
		*bytes += obj->buffer.length;
		return 1;
	case ACPI_TYPE_PACKAGE:
		next = (union acpi_object *)(start + 
		                         (unsigned long)obj->package.elements);

		for (i = 0 ; i < obj->package.count ; i++, next++)
			if (!calc_element_size(next, start, objs, bytes))
				return 0;

		return 1;
	default:
		return 1;
	}
}

/* Calculate new buffer size for given object size and byte size */
static int
calc_buffer_size32(
	union acpi_object32	*buffer,
	unsigned long		start)
{
	int	objs, bytes;

	objs = 0;
	bytes = 0;

	if (!calc_element_size32(buffer, start, &objs, &bytes))
		return -1;

	return ((objs * sizeof(union acpi_object)) + bytes);
}

static int
calc_buffer_size(
	union acpi_object	*buffer,
	unsigned long		start)
{
	int	objs, bytes;

	objs = 0;
	bytes = 0;

	if (!calc_element_size(buffer, start, &objs, &bytes))
		return -1;

	return ((objs * sizeof(union acpi_object32)) + bytes);
}

static int
copy_element32(
	union acpi_object32	*src,
	unsigned long		src_start,
	union acpi_object	*target,
	unsigned long		target_start,
	union acpi_object	**next)
{
	union acpi_object	*tmp;
	union acpi_object32	*new_src;
	u8			*dest, *ptr;
	int			i;

	switch (src->type) {
	case ACPI_TYPE_STRING:
		target->string.type = src->string.type;
		target->string.length = src->string.length;
		target->string.pointer = (char *)POFFSET(target_start, *next);

		dest = (u8 *)*next;
		ptr = (u8 *)(src->string.pointer + src_start);
		memcpy(dest, ptr, src->string.length);
		dest += ACPI_ROUND_UP_TO_NATIVE_WORD(src->string.length);
		*next = (union acpi_object *)dest;

		break;
	case ACPI_TYPE_BUFFER:
		target->buffer.type = src->buffer.type;
		target->buffer.length = src->buffer.length;
		target->buffer.pointer = (u8 *)POFFSET(target_start, *next);

		dest = (u8 *)*next;
		ptr = (u8 *)(src->buffer.pointer + src_start);
		memcpy(dest, ptr, src->buffer.length);
		dest += ACPI_ROUND_UP_TO_NATIVE_WORD(src->buffer.length);
		*next = (union acpi_object *)dest;

		break;
	case ACPI_TYPE_PACKAGE:
		target->package.type = src->package.type;
		target->package.count = src->package.count;
		target->package.elements = (union acpi_object *)
		                                  POFFSET(target_start, *next);

		tmp = *next;
		*next += src->package.count;
		new_src = (union acpi_object32 *)
		                   PADDR_END(src_start, src->package.elements);
		
		for (i = 0 ; i < src->package.count ; i++, tmp++, new_src++)
			if (!copy_element32(new_src, src_start, tmp,
			                    target_start, next))
				return 0;

		break;
	case ACPI_TYPE_INTEGER:
	case ACPI_TYPE_ANY:
		target->integer.type = src->integer.type;
		target->integer.value = src->integer.value;
		break;
	default:
		return 0;
	}
	return 1;
}

static int
copy_element(
	union acpi_object	*src,
	unsigned long		src_start,
	union acpi_object32	*target,
	unsigned long		target_start,
	union acpi_object32	**next)
{
	union acpi_object32	*tmp;
	union acpi_object	*new_src;
	u8			*dest, *ptr;
	int			i;

	switch (src->type) {
	case ACPI_TYPE_STRING:
		target->string.type = src->string.type;
		target->string.length = src->string.length;
		target->string.pointer = (u32)POFFSET(target_start, *next);

		dest = (u8 *)*next;
		ptr = (u8 *)(src->string.pointer + src_start);
		memcpy(dest, ptr, src->string.length);
		dest += src->string.length;
		*next = (union acpi_object32 *)dest;

		break;
	case ACPI_TYPE_BUFFER:
		target->buffer.type = src->buffer.type;
		target->buffer.length = src->buffer.length;
		target->buffer.pointer = (u32)POFFSET(target_start, *next);

		dest = (u8 *)*next;
		ptr = (u8 *)(src->buffer.pointer + src_start);
		memcpy(dest, ptr, src->buffer.length);
		dest += src->buffer.length;
		*next = (union acpi_object32 *)dest;

		break;
	case ACPI_TYPE_PACKAGE:
		target->package.type = src->package.type;
		target->package.count = src->package.count;
		target->package.elements = POFFSET(target_start, *next);

		tmp = *next;
		*next += src->package.count;
		new_src = (union acpi_object *)
		                  PADDR_END(src_start, src->package.elements);
		
		for (i = 0 ; i < src->package.count ; i++, tmp++, new_src++)
			if (!copy_element(new_src, src_start, tmp,
			                  target_start, next))
				return 0;

		break;
	case ACPI_TYPE_INTEGER:
	case ACPI_TYPE_ANY:
		target->integer.type = src->integer.type;
		target->integer.value = src->integer.value;
		break;
	default:
		return 0;
	}
	return 1;
}

/*
 * convert an ILP32 arglist (with offsets) to an LP64 arglist (with offsets)
 */
static struct acpi_buffer *
convert_arglist32(
	struct acpi_object_list32	*args32)
{
	int				i, size, tmp;
	union acpi_object32		*next32;
	union acpi_object		*next, *end;
	struct acpi_buffer		*buffer;
	struct acpi_object_list		*args;

	buffer = kmalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return NULL;

	memset(buffer, 0, sizeof(*buffer));

	if (args32->count == 0)
		return buffer;

	/* base size, not counting the objects */
	size = sizeof(struct acpi_object_list);

	next32 = (union acpi_object32 *)PADDR_END(args32, args32->pointer);

	for (i = 0 ; i < args32->count ; i++, next32++) {

		tmp = calc_buffer_size32(next32, (unsigned long)args32);

		if (tmp < 0) {
			kfree(buffer);
			return NULL;
		}

		size += tmp;
	}

	buffer->pointer = kmalloc(size, GFP_KERNEL);

	if (!buffer->pointer) {
		kfree(buffer);
		return NULL;
	}
	memset(buffer->pointer, 0, size);
	buffer->length = size;

	args = (struct acpi_object_list *)buffer->pointer;

	args->count = args32->count;
	/* still an offset, so point right after the object list */
	args->pointer = (union acpi_object *)sizeof(struct acpi_object_list);

	next = (union acpi_object *)PADDR_END(args, args->pointer);
	next32 = (union acpi_object32 *)PADDR_END(args32, args32->pointer);

	/*
	 * first chance for object data storage is just off the end of the
	 * array of acpi objects.
	 * */
	end = &next[args->count];

	for (i = 0 ; i < args->count ; i++, next++, next32++) {

		if (!copy_element32(next32, (unsigned long)args32,
		                    next, (unsigned long)args, &end)) {
			kfree(buffer->pointer);
			kfree(buffer);
			return NULL;
		}
	}
	return buffer;
}

/*
 * This is currently unnecessary, the only incoming objects that need to
 * be converted are object lists.  Those use the above function.  If we need
 * functionality to pass in objects outside of an object list, re-instantiate
 * this code.
 */
#if 0
static struct acpi_buffer *
convert_element32(
	union acpi_object32	*obj32)
{
	int			size;
	struct acpi_buffer	*buffer;
	union acpi_object	*end;

	buffer = kmalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return NULL;

	memset(buffer, 0, sizeof(*buffer));

	if (!obj32)
		return buffer;

	size = calc_buffer_size32(obj32, (unsigned long)obj32);
	buffer->pointer = kmalloc(size, GFP_KERNEL);

	if (!buffer->pointer) {
		kfree(buffer);
		return NULL;
	}
	memset(buffer->pointer, 0, size);
	buffer->length = size;

	end = (union acpi_object *)buffer->pointer;
	end++;

	if (!copy_element32(obj32, (unsigned long)obj32,
	                    (union acpi_object *)buffer->pointer,
			    (unsigned long)buffer->pointer, &end)) {
		kfree(buffer->pointer);
		kfree(buffer);
		return NULL;
	}
	return buffer;

}
#endif

static struct acpi_buffer *
convert_element(
	union acpi_object	*obj)
{
	int			size;
	struct acpi_buffer	*buffer;
	union acpi_object32	*end;

	buffer = kmalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return NULL;

	memset(buffer, 0, sizeof(*buffer));

	if (!obj)
		return buffer;

	size = calc_buffer_size(obj, (unsigned long)obj);
	buffer->pointer = kmalloc(size, GFP_KERNEL);

	if (!buffer->pointer) {
		kfree(buffer);
		return NULL;
	}
	memset(buffer->pointer, 0, size);
	buffer->length = size;

	end = (union acpi_object32 *)buffer->pointer;
	end++;

	if (!copy_element(obj, (unsigned long)obj,
	                  (union acpi_object32 *)buffer->pointer,
			  (unsigned long)buffer->pointer, &end)) {
		kfree(buffer->pointer);
		kfree(buffer);
		return NULL;
	}
	return buffer;

}

static int
fix_return32(unsigned long arg, acpi_size length)
{
	dev_acpi_t	data;

	if (copy_from_user(&data, (dev_acpi_t *)arg, sizeof(data)))
		return 0;

	data.return_size = length;

	if (copy_to_user((dev_acpi_t *)arg, &data, sizeof(data)))
		return 0;

	return 1;
}

static int
ioctl32_get_type(
	unsigned int	fd,
	unsigned int	cmd,
	unsigned long	arg,
	struct file	*f)
{
	int			ret;
	struct acpi_buffer	*buffer, *rbuf;

	ret = sys_ioctl(fd, cmd, arg);

	if (ret < 0)
		return ret;

	rbuf = RBUF(f);
	dump_buffer("ioctl32_get_type: pre", rbuf);
	buffer = convert_element((union acpi_object *)rbuf->pointer);
	dump_buffer("ioctl32_get_type: post", buffer);

	dev_acpi_clear(f, READ_CLEAR);

	if (!buffer)
		return -EPIPE;

	if (!fix_return32(arg, buffer->length)) {
		if (buffer->pointer)
			kfree(buffer->pointer);
		kfree(buffer);
		return -EPIPE;
	}

	rbuf->pointer = buffer->pointer;
	rbuf->length = buffer->length;

	kfree(buffer);

	return ret;
}

static int
ioctl32_evaluate_object(
	unsigned int	fd,
	unsigned int	cmd,
	unsigned long	arg,
	struct file	*f)
{
	struct acpi_buffer	*buffer, *wbuf, *rbuf;
	int			ret;

	wbuf = WBUF(f);
	if (wbuf->pointer && wbuf->length >=
	    sizeof(struct acpi_object_list32) + sizeof(union acpi_object32)) {

		dump_buffer("ioctl32_evaluate_object: pre", wbuf);
		buffer = convert_arglist32(wbuf->pointer);
		dump_buffer("ioctl32_evaluate_object: post", buffer);

		if (!buffer)
			return -EINVAL;

		dev_acpi_clear(f, WRITE_CLEAR);
		wbuf->pointer = buffer->pointer;
		wbuf->length = buffer->length;
		kfree(buffer);
	}
		
	ret = sys_ioctl(fd, cmd, arg);

	if (ret < 0)
		return ret;

	rbuf = RBUF(f);
	if (rbuf->pointer && rbuf->length) {
		dump_buffer("ioctl32_evaluate_object: pre", rbuf);
		buffer = convert_element((union acpi_object *)rbuf->pointer);
		dump_buffer("ioctl32_evaluate_object: post", buffer);

		dev_acpi_clear(f, READ_CLEAR);

		if (!buffer)
			return -EPIPE;

		if (!fix_return32(arg, buffer->length)) {
			kfree(buffer->pointer);
			kfree(buffer);
			return -EPIPE;
		}

		rbuf->pointer = buffer->pointer;
		rbuf->length = buffer->length;
	}
	return ret;
}

static void __init
dev_acpi_register_ioctl32(void)
{
	int err;

	err = register_ioctl32_conversion(DEV_ACPI_CLEAR, NULL);
	err |= register_ioctl32_conversion(DEV_ACPI_DEVICE_NOTIFY, NULL);
	err |= register_ioctl32_conversion(DEV_ACPI_EVALUATE_OBJ,
	                                   ioctl32_evaluate_object);
	err |= register_ioctl32_conversion(DEV_ACPI_EXISTS, NULL);
	err |= register_ioctl32_conversion(DEV_ACPI_BUS_GENERATE_EVENT, NULL);
	err |= register_ioctl32_conversion(DEV_ACPI_GET_DEVICES, NULL);
	err |= register_ioctl32_conversion(DEV_ACPI_GET_NEXT, NULL);
	err |= register_ioctl32_conversion(DEV_ACPI_GET_OBJECTS, NULL);
	err |= register_ioctl32_conversion(DEV_ACPI_GET_PARENT, NULL);
	err |= register_ioctl32_conversion(DEV_ACPI_GET_TYPE, ioctl32_get_type);
	err |= register_ioctl32_conversion(DEV_ACPI_REMOVE_DEVICE_NOTIFY, NULL);
	err |= register_ioctl32_conversion(DEV_ACPI_REMOVE_SYSTEM_NOTIFY, NULL);
	err |= register_ioctl32_conversion(DEV_ACPI_SYS_INFO, NULL);
	err |= register_ioctl32_conversion(DEV_ACPI_SYSTEM_NOTIFY, NULL);

	if (err)
		printk(KERN_WARNING "%s: error registering ioctl32 "\
		                    "conversions\n", DEV_ACPI_NAME);
}

static void __exit
dev_acpi_unregister_ioctl32(void)
{
	int err;

	err = unregister_ioctl32_conversion(DEV_ACPI_CLEAR);
	err |= unregister_ioctl32_conversion(DEV_ACPI_DEVICE_NOTIFY);
	err |= unregister_ioctl32_conversion(DEV_ACPI_EVALUATE_OBJ);
	err |= unregister_ioctl32_conversion(DEV_ACPI_EXISTS);
	err |= unregister_ioctl32_conversion(DEV_ACPI_BUS_GENERATE_EVENT);
	err |= unregister_ioctl32_conversion(DEV_ACPI_GET_DEVICES);
	err |= unregister_ioctl32_conversion(DEV_ACPI_GET_NEXT);
	err |= unregister_ioctl32_conversion(DEV_ACPI_GET_OBJECTS);
	err |= unregister_ioctl32_conversion(DEV_ACPI_GET_PARENT);
	err |= unregister_ioctl32_conversion(DEV_ACPI_GET_TYPE);
	err |= unregister_ioctl32_conversion(DEV_ACPI_REMOVE_DEVICE_NOTIFY);
	err |= unregister_ioctl32_conversion(DEV_ACPI_REMOVE_SYSTEM_NOTIFY);
	err |= unregister_ioctl32_conversion(DEV_ACPI_SYS_INFO);
	err |= unregister_ioctl32_conversion(DEV_ACPI_SYSTEM_NOTIFY);

	if (err)
		printk(KERN_WARNING "%s: error unregistering ioctl32 "\
		                    "conversions\n", DEV_ACPI_NAME);
}
#else
#define dev_acpi_register_ioctl32()
#define dev_acpi_unregister_ioctl32()
#endif

static int __init
dev_acpi_init(void)
{
	major = register_chrdev(0, DEV_ACPI_DEVICE_NAME, &dev_acpi_fops);

	if (major < 0) {
		printk(KERN_ALERT "%s: cannot register device!\n",
		       DEV_ACPI_NAME);
		return -EBUSY;
	}
	printk(KERN_INFO "%s: registered on char major %d\n", DEV_ACPI_NAME,
	                                                      major);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	dev_acpi_class = CLASS_CREATE(THIS_MODULE, DEV_ACPI_DEVICE_NAME);
	if (IS_ERR(dev_acpi_class)) {
		printk(KERN_ERR "%s: failure creating class, error %ld\n",
		       DEV_ACPI_NAME, PTR_ERR(dev_acpi_class));
		unregister_chrdev(major, DEV_ACPI_DEVICE_NAME);
		return PTR_ERR(dev_acpi_class);
	}
	CLASS_DEVICE_CREATE(dev_acpi_class, MKDEV(major, 0), NULL, "acpi");
#endif

	dev_acpi_register_ioctl32();
	return 0; 
}

static void __exit
dev_acpi_exit(void)
{
	unregister_chrdev(major, DEV_ACPI_DEVICE_NAME);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	CLASS_DEVICE_DESTROY(dev_acpi_class, MKDEV(major, 0));
	CLASS_DESTROY(dev_acpi_class);
#endif
	dev_acpi_unregister_ioctl32();
	return;
}

module_init(dev_acpi_init);
module_exit(dev_acpi_exit);
