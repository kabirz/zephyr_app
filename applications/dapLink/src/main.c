/*
 * Copyright (c) 2024 Kabirz
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ZephyrLink - CMSIS-DAP Debug Probe
 *
 * Turns an STM32F103RCT6 (stm32f103_mini) board into a CMSIS-DAP v2
 * debug probe with CDC ACM serial bridge.
 *
 * Uses Zephyr built-in DAP subsystem (CONFIG_DAP + CONFIG_DAP_BACKEND_USB)
 * with the new USB device stack (CONFIG_USB_DEVICE_STACK_NEXT).
 */

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/dap/dap_link.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(zephyr_link, LOG_LEVEL_INF);

/* DAP Link context bound to the zephyr,swdp-gpio instance (dp0) */
DAP_LINK_CONTEXT_DEFINE(zl_dap_ctx, DEVICE_DT_GET_ONE(zephyr_swdp_gpio));

/* USB device context */
USBD_DEVICE_DEFINE(zl_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   0x0D28,  /* ARM Ltd. VID */
		   0x0204); /* CMSIS-DAP PID */

/* String descriptors */
USBD_DESC_LANG_DEFINE(zl_lang);
USBD_DESC_MANUFACTURER_DEFINE(zl_mfr, "ZephyrLink");
USBD_DESC_PRODUCT_DEFINE(zl_product, "CMSIS-DAP Probe");
USBD_DESC_SERIAL_NUMBER_DEFINE(zl_sn);

/* Full-Speed configuration */
USBD_DESC_CONFIG_DEFINE(zl_fs_cfg_desc, "ZephyrLink FS");
USBD_CONFIGURATION_DEFINE(zl_fs_config,
			  USB_SCD_SELF_POWERED,
			  125, /* 250 mA */
			  &zl_fs_cfg_desc);

int main(void)
{
	int ret;

	/* Initialize DAP Link (SWD bit-bang) */
	ret = dap_link_init(&zl_dap_ctx);
	if (ret) {
		LOG_ERR("DAP Link init failed: %d", ret);
		return ret;
	}
	LOG_INF("DAP Link initialized");

	/* Initialize DAP USB backend (binds context to USB class) */
	ret = dap_link_backend_usb_init(&zl_dap_ctx);
	if (ret) {
		LOG_ERR("DAP USB backend init failed: %d", ret);
		return ret;
	}
	LOG_INF("DAP USB backend initialized");

	/* Set up USB device descriptors */
	ret = usbd_add_descriptor(&zl_usbd, &zl_lang);
	if (ret) {
		LOG_ERR("Failed to add language descriptor: %d", ret);
		return ret;
	}

	ret = usbd_add_descriptor(&zl_usbd, &zl_mfr);
	if (ret) {
		LOG_ERR("Failed to add manufacturer descriptor: %d", ret);
		return ret;
	}

	ret = usbd_add_descriptor(&zl_usbd, &zl_product);
	if (ret) {
		LOG_ERR("Failed to add product descriptor: %d", ret);
		return ret;
	}

	ret = usbd_add_descriptor(&zl_usbd, &zl_sn);
	if (ret) {
		LOG_ERR("Failed to add serial number descriptor: %d", ret);
		return ret;
	}

	/* Add Full-Speed configuration */
	ret = usbd_add_configuration(&zl_usbd, USBD_SPEED_FS, &zl_fs_config);
	if (ret) {
		LOG_ERR("Failed to add FS configuration: %d", ret);
		return ret;
	}

	/* Register all USB classes (DAP bulk + CDC ACM) */
	ret = usbd_register_all_classes(&zl_usbd, USBD_SPEED_FS, 1, NULL);
	if (ret) {
		LOG_ERR("Failed to register USB classes: %d", ret);
		return ret;
	}

	/* Set composite device class code (IAD) */
	usbd_device_set_code_triple(&zl_usbd, USBD_SPEED_FS,
				    USB_BCC_MISCELLANEOUS, 0x02, 0x01);

	usbd_self_powered(&zl_usbd, true);

	/* Initialize USB device */
	ret = usbd_init(&zl_usbd);
	if (ret) {
		LOG_ERR("Failed to init USB device: %d", ret);
		return ret;
	}

	/* Enable USB (required for devices without VBUS detection, e.g. STM32F1) */
	ret = usbd_enable(&zl_usbd);
	if (ret) {
		LOG_ERR("Failed to enable USB device: %d", ret);
		return ret;
	}

	LOG_INF("ZephyrLink CMSIS-DAP ready (VID=0x0D28 PID=0x0204)");
	return 0;
}
