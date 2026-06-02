/*
 * WebUSB support for ZephyrLink
 *
 * Defines:
 *   - WebUSB Platform Capability Descriptor (added to BOS)
 *   - WebUSB URL Descriptor (landing page for browsers)
 *   - Vendor request callback that handles GET_URL
 *
 * Reference: https://wicg.github.io/webusb/
 * Adapted from zephyr/samples/subsys/dap/src/webusb.h
 */

#ifndef ZEPHYRLINK_WEBUSB_H
#define ZEPHYRLINK_WEBUSB_H

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/min_heap.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/bos.h>
#include <zephyr/net_buf.h>
#include <zephyr/logging/log.h>

/* WebUSB protocol constants */
#define WEBUSB_REQ_GET_URL             0x02U
#define WEBUSB_DESC_TYPE_URL           0x03U
#define WEBUSB_URL_PREFIX_HTTP         0x00U
#define WEBUSB_URL_PREFIX_HTTPS        0x01U

/* Vendor request code and landing page index
 * Must be unique among all vendor request codes registered on this device.
 */
#define ZEPHYRLINK_WEBUSB_VENDOR_CODE  0x01U
#define ZEPHYRLINK_WEBUSB_LANDING_PAGE 0x01U

/* Combined descriptor: platform + webusb cap
 * The usbd stack concatenates these into a single BOS capability entry.
 */
struct usb_bos_webusb_desc {
	struct usb_bos_platform_descriptor platform;
	struct usb_bos_capability_webusb  cap;
} __packed;

static const struct usb_bos_webusb_desc bos_cap_webusb = {
	/* WebUSB Platform Capability Descriptor
	 * https://wicg.github.io/webusb/#webusb-platform-capability-descriptor
	 */
	.platform = {
		.bLength = sizeof(struct usb_bos_platform_descriptor)
			 + sizeof(struct usb_bos_capability_webusb),
		.bDescriptorType     = USB_DESC_DEVICE_CAPABILITY,
		.bDevCapabilityType  = USB_BOS_CAPABILITY_PLATFORM,
		.bReserved           = 0,
		/* WebUSB UUID: 3408b638-09a9-47a0-8bfd-a0768815b665
		 * (little-endian on the wire)
		 */
		.PlatformCapabilityUUID = {
			0x38, 0xB6, 0x08, 0x34,
			0xA9, 0x09,
			0xA0, 0x47,
			0x8B, 0xFD,
			0xA0, 0x76, 0x88, 0x15, 0xB6, 0x65,
		},
	},
	.cap = {
		.bcdVersion   = sys_cpu_to_le16(0x0100),
		.bVendorCode  = ZEPHYRLINK_WEBUSB_VENDOR_CODE,
		.iLandingPage = ZEPHYRLINK_WEBUSB_LANDING_PAGE,
	}
};

/* WebUSB URL Descriptor
 * When the user plugs the device in, Chrome/Edge fetches this URL and
 * uses it for origin validation. The origin (scheme + host + port) of
 * this URL must match the origin of any page that calls
 * navigator.usb.requestDevice() and wants to access this device.
 *
 * Currently pointing at the official mbed dapjs example page on GitHub
 * Pages, so the device can be used directly with dapjs's WebUSB demos:
 *   https://armmbed.github.io/dapjs/examples/index.html
 *
 * Format: bLength | bDescriptorType | bScheme | UTF-8 URL (no scheme prefix)
 */
static const uint8_t webusb_origin_url[] = {
	0x2E,                                    /* bLength = 46 bytes (3 + 43) */
	WEBUSB_DESC_TYPE_URL,                    /* bDescriptorType = URL */
	WEBUSB_URL_PREFIX_HTTPS,                 /* bScheme = https */
	'a', 'r', 'm', 'm', 'b', 'e', 'd', '.', 'g', 'i', 't', 'h', 'u', 'b', '.', 'i', 'o',
	'/', 'd', 'a', 'p', 'j', 's', '/', 'e', 'x', 'a', 'm', 'p', 'l', 'e', 's',
	'/', 'i', 'n', 'd', 'e', 'x', '.', 'h', 't', 'm', 'l',
};

/* GET_URL vendor request handler
 *
 * Invoked by the usbd stack when the host issues a control transfer
 * with bmRequestType = Device-to-Host, bRequest = bVendorCode, wIndex = 0x02.
 * We return the URL descriptor for the requested landing page index.
 */
static int webusb_to_host_cb(const struct usbd_context *const ctx,
			     const struct usb_setup_packet *const setup,
			     struct net_buf *const buf)
{
	if (setup->wIndex == WEBUSB_REQ_GET_URL) {
		uint8_t index = USB_GET_DESCRIPTOR_INDEX(setup->wValue);

		if (index != ZEPHYRLINK_WEBUSB_LANDING_PAGE) {
			return -ENOTSUP;
		}

		net_buf_add_mem(buf, &webusb_origin_url,
				MIN(net_buf_tailroom(buf), sizeof(webusb_origin_url)));

		LOG_INF("WebUSB: GET_URL index=%u", index);
		return 0;
	}

	return -ENOTSUP;
}

/* Single-shot no-op for host-to-device direction (not used by WebUSB). */
static int webusb_to_dev_cb(const struct usbd_context *const ctx,
			    const struct usb_setup_packet *const setup,
			    const struct net_buf *const buf)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(setup);
	ARG_UNUSED(buf);
	return -ENOTSUP;
}

/* Register the WebUSB BOS capability + its vendor request handler */
USBD_DESC_BOS_VREQ_DEFINE(bos_vreq_webusb,
			  sizeof(bos_cap_webusb),
			  &bos_cap_webusb,
			  ZEPHYRLINK_WEBUSB_VENDOR_CODE,
			  webusb_to_host_cb,
			  webusb_to_dev_cb);

#endif /* ZEPHYRLINK_WEBUSB_H */
