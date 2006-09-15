#ifndef __ACPI_SYSFS_H__
#define __ACPI_SYSFS_H__

#ifdef __KERNEL__
#ifdef ACPI_NO_INTEGER64_SUPPORT
#error "No support for < 64bit integers"
#endif
#endif

typedef struct {
	char		pathname[ACPI_PATHNAME_MAX];
	u32		return_size;
} dev_acpi_t;

#define DEV_ACPI_MAGIC 'A'

/* Clear all state associated w/ device
 *  input - none
 *  output - none
 */
#define DEV_ACPI_CLEAR			_IO(DEV_ACPI_MAGIC, 0)

/* Return success if pathname exists
 *  input - pathname
 *  output - none
 */
#define DEV_ACPI_EXISTS			_IOW(DEV_ACPI_MAGIC, 1, dev_acpi_t)

/* Return type of object
 *  input - pathname
 *  output - data.return_size = length of read buffer
 *           read buffer = acpi object (integer) describing the size
 */
#define DEV_ACPI_GET_TYPE		_IOWR(DEV_ACPI_MAGIC, 2, dev_acpi_t)

/* Evaluate an object
 *  input - pathname, write buffer = arg list
 *  output - data.return_size = length of read buffer
 *           read buffer = eval data
 */
#define DEV_ACPI_EVALUATE_OBJ		_IOWR(DEV_ACPI_MAGIC, 3, dev_acpi_t)

/* Get Next objects
 *  input - pathname
 *  output - data.return_size = length of read buffer
 *           read buffer = list of child objects
 */
#define DEV_ACPI_GET_NEXT		_IOWR(DEV_ACPI_MAGIC, 4, dev_acpi_t)

/* Get Devices
 *  input - pathname = PNP _HID/_CID value to look for
 *  output - data.return_size = length of read buffer
 *           read buffer = list of matching devices
 */
#define DEV_ACPI_GET_DEVICES		_IOWR(DEV_ACPI_MAGIC, 5, dev_acpi_t)

/* Get Objects
 *  input - pathname = Object name to find
 *  output - data.return_size = length of read buffer
 *           read buffer = list of matching devices
 */
#define DEV_ACPI_GET_OBJECTS		_IOWR(DEV_ACPI_MAGIC, 6, dev_acpi_t)

/* Get parent object
 *  input - pathname
 *  output - data.return_size = length of read buffer
 *           read buffer = path of parent object
 */
#define DEV_ACPI_GET_PARENT		_IOWR(DEV_ACPI_MAGIC, 7, dev_acpi_t)

/* Get system info
 *  input - none
 *  output - data.return_size = length of read buffer
 *           read buffer = system info structure
 */
#define DEV_ACPI_SYS_INFO		_IOWR(DEV_ACPI_MAGIC, 8, dev_acpi_t)

/* Set Device/System Notify - install notify handler on device
 *  input - pathname
 *  output - none (notify events occur through read buffer as:
 *                 "%s,%08x", pathname, event)
 */
#define DEV_ACPI_DEVICE_NOTIFY		_IOW(DEV_ACPI_MAGIC, 9, dev_acpi_t)
#define DEV_ACPI_SYSTEM_NOTIFY		_IOW(DEV_ACPI_MAGIC, 10, dev_acpi_t)

/* Remove Device/System Notify - remove notify handler on device
 *  input - pathname
 *  output - none
 */
#define DEV_ACPI_REMOVE_DEVICE_NOTIFY	_IOW(DEV_ACPI_MAGIC, 11, dev_acpi_t)
#define DEV_ACPI_REMOVE_SYSTEM_NOTIFY	_IOW(DEV_ACPI_MAGIC, 12, dev_acpi_t)

/* Generate an ACPI event on a device
 *  input - "%s,%d,%d", pathname, type, event
 *  output - none
 */
#define DEV_ACPI_BUS_GENERATE_EVENT	_IOW(DEV_ACPI_MAGIC, 13, dev_acpi_t)

#endif /* __ACPI_SYSFS_H__ */
