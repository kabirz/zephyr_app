/*
 * MS OS 2.0 Descriptor for ZephyrLink
 *
 * Required on Windows so the OS auto-loads WinUSB.sys for our device.
 * Without it, Chrome/Edge on Windows cannot claim the WebUSB interface
 * and requestDevice() will fail with "Access denied".
 *
 * Reference: https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/
 *            microsoft-os-2-0-descriptors-specification
 * Adapted from zephyr/samples/subsys/dap/src/msosv2.h
 */

#ifndef ZEPHYRLINK_MSOSV2_H
#define ZEPHYRLINK_MSOSV2_H

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/msos_desc.h>
#include <zephyr/net_buf.h>
#include <zephyr/logging/log.h>

/* Vendor request code for MS OS 2.0 descriptor requests
 * Must be unique across all vendor request codes on this device.
 * WebUSB uses 0x01, so MSOS 2.0 uses 0x02.
 */
#define ZEPHYRLINK_MSOS2_VENDOR_CODE  0x02U

/* Windows version (Windows 10) reported in dwWindowsVersion */
#define ZEPHYRLINK_MSOS2_OS_VERSION   0x0A000000UL

/* DeviceInterfaceGUID: {3367AA52-11A8-4FA6-A783-192849063453}
 * The sample uses a CMSIS-DAP specific GUID; we use a ZephyrLink GUID.
 * UTF-16LE encoded (without BOM).
 */
#define ZEPHYRLINK_DEVICE_INTERFACE_GUIDS_PROPERTY               \
	'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00,              \
	'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00,              \
	't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00,              \
	'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00,              \
	'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00,              \
	0x00, 0x00

#define ZEPHYRLINK_DEVICE_INTERFACE_GUID_DATA                   \
	'{', 0x00, '3', 0x00, '3', 0x00, '6', 0x00,              \
	'7', 0x00, 'A', 0x00, 'A', 0x00, '5', 0x00,              \
	'2', 0x00, '-', 0x00, '1', 0x00, '1', 0x00,              \
	'A', 0x00, '8', 0x00, '-', 0x00, '4', 0x00,              \
	'F', 0x00, 'A', 0x00, '6', 0x00, '-', 0x00,              \
	'A', 0x00, '7', 0x00, '8', 0x00, '3', 0x00,              \
	'-', 0x00, '1', 0x00, '9', 0x00, '2', 0x00,              \
	'8', 0x00, '4', 0x00, '9', 0x00, '0', 0x00,              \
	'6', 0x00, '3', 0x00, '4', 0x00, '5', 0x00,              \
	'3', 0x00, '}', 0x00, 0x00, 0x00

struct msosv2_descriptor {
	struct msosv2_descriptor_set_header header;
#if defined(CONFIG_USBD_CDC_ACM_CLASS)
	struct msosv2_function_subset_header subset_header;
#endif
	struct msosv2_compatible_id compatible_id;
	struct msosv2_guids_property guids_property;
} __packed;

static struct msosv2_descriptor msosv2_desc = {
	.header = {
		.wLength          = sizeof(struct msosv2_descriptor_set_header),
		.wDescriptorType  = MS_OS_20_SET_HEADER_DESCRIPTOR,
		.dwWindowsVersion = sys_cpu_to_le32(ZEPHYRLINK_MSOS2_OS_VERSION),
		.wTotalLength     = sizeof(msosv2_desc),
	},
#if defined(CONFIG_USBD_CDC_ACM_CLASS)
	.subset_header = {
		.wLength         = sizeof(struct msosv2_function_subset_header),
		.wDescriptorType = MS_OS_20_SUBSET_HEADER_FUNCTION,
		.bFirstInterface = 0,
		.wSubsetLength   = sizeof(struct msosv2_compatible_id)
				 + sizeof(struct msosv2_guids_property),
	},
#endif
	.compatible_id = {
		.wLength         = sizeof(struct msosv2_compatible_id),
		.wDescriptorType = MS_OS_20_FEATURE_COMPATIBLE_ID,
		.CompatibleID    = { 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00 },
	},
	.guids_property = {
		.wLength             = sizeof(struct msosv2_guids_property),
		.wDescriptorType     = MS_OS_20_FEATURE_REG_PROPERTY,
		.wPropertyDataType   = MS_OS_20_PROPERTY_DATA_REG_SZ,
		.wPropertyNameLength = sizeof(struct msosv2_guids_property) -
				       offsetof(struct msosv2_guids_property, PropertyName),
		.PropertyName        = ZEPHYRLINK_DEVICE_INTERFACE_GUIDS_PROPERTY,
		.wPropertyDataLength = sizeof(struct msosv2_guids_property) -
				       offsetof(struct msosv2_guids_property, bPropertyData),
		.bPropertyData       = ZEPHYRLINK_DEVICE_INTERFACE_GUID_DATA,
	},
};

/* MS OS 2.0 vendor request callback
 * Windows issues GET_DESCRIPTOR (wIndex=0x07) for MSOS 2.0 right after
 * enumeration. We return the full descriptor set in a single transfer.
 */
static int msosv2_to_host_cb(const struct usbd_context *const ctx,
			     const struct usb_setup_packet *const setup,
			     struct net_buf *const buf)
{
	if (setup->wIndex == MS_OS_20_DESCRIPTOR_INDEX) {
		LOG_INF("MS OS 2.0: GET_DESCRIPTOR");
		net_buf_add_mem(buf, &msosv2_desc,
				MIN(net_buf_tailroom(buf), sizeof(msosv2_desc)));
		return 0;
	}
	return -ENOTSUP;
}

static int msosv2_to_dev_cb(const struct usbd_context *const ctx,
			    const struct usb_setup_packet *const setup,
			    const struct net_buf *const buf)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(setup);
	ARG_UNUSED(buf);
	return -ENOTSUP;
}

USBD_DESC_BOS_VREQ_DEFINE(bos_vreq_msosv2,
			  sizeof(msosv2_desc),
			  &msosv2_desc,
			  ZEPHYRLINK_MSOS2_VENDOR_CODE,
			  msosv2_to_host_cb,
			  msosv2_to_dev_cb);

#endif /* ZEPHYRLINK_MSOSV2_H */
