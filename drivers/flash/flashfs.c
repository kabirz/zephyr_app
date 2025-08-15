#define DT_DRV_COMPAT zephyr_flash_fs

#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_interface.h>


LOG_MODULE_REGISTER(flash_fs, CONFIG_FLASH_LOG_LEVEL);

struct flashfs_config {
	uint32_t flash_size;
	struct k_sem sem;
	const char *name;
	struct fs_file_t file;
	bool open;
#ifdef CONFIG_FLASH_PAGE_LAYOUT
	struct flash_pages_layout layout;
#endif
};

static inline uint32_t dev_flash_size(const struct device *dev)
{
	struct flashfs_config *cfg = dev->data;

	return cfg->flash_size;
}

static const struct flash_parameters flash_fs_parameters = {
	.write_block_size = 1,
	.erase_value = 0xff,
};

static int read_write_init(struct flashfs_config *cfg)
{
	int ret = 0;

	if (!cfg->open) {
		ret = fs_open(&cfg->file, cfg->name, FS_O_READ | FS_O_WRITE);
		if (ret != 0) {
			LOG_ERR("Failed to open backing file: %d", ret);
			return ret;
		}
		cfg->open = true;
	}
	return 0;
}

static void acquire_device(const struct device *dev)
{
	if (IS_ENABLED(CONFIG_MULTITHREADING)) {
		struct flashfs_config *cfg = dev->data;
		k_sem_take(&cfg->sem, K_FOREVER);
	}
}

static void release_device(const struct device *dev)
{
	if (IS_ENABLED(CONFIG_MULTITHREADING)) {
		struct flashfs_config *cfg = dev->data;
		k_sem_give(&cfg->sem);
	}
}

static int flash_fs_read(const struct device *dev, off_t addr, void *dest, size_t size)
{
	struct flashfs_config *cfg = dev->data;
	int ret = 0;

	if (addr + size > cfg->flash_size) {
		LOG_WRN("Tried to read past end of backing file");
		ret = -EIO;
		goto end1;
	}
	acquire_device(dev);
	if (read_write_init(cfg)) {
		LOG_ERR("Open file failed");
		ret = -EIO;
		goto end1;
	}

	ret = fs_seek(&cfg->file, addr, FS_SEEK_SET);

	if (ret != 0) {
		LOG_ERR("Failed to seek backing file: %d", ret);
		goto end1;
	}

	size_t len_left = size;
	uint8_t *read_pos = dest;

	while (len_left > 0) {
		ret = fs_read(&cfg->file, read_pos, len_left);
		if (ret < 0) {
			LOG_ERR("Failed to read from backing file: %d", ret);
			ret = -EIO;
			goto end1;
		}
		if (ret == 0) {
			LOG_WRN("Tried to read past end of backing file");
			ret = -EIO;
			goto end1;
		}
		__ASSERT(ret <= len_left,
			 "fs_read returned more than we asked for: %d instead of %ld", ret,
			 len_left);
		len_left -= ret;
		read_pos += ret;
	}
	ret = 0;
end1:
	release_device(dev);
	return ret;
}

static int flash_fs_write(const struct device *dev, off_t addr, const void *src, size_t size)
{
	struct flashfs_config *cfg = dev->data;
	int ret = 0;

	if (addr + size > cfg->flash_size) {
		LOG_WRN("Tried to write past end of backing file");
		ret = -EIO;
		goto end2;
	}

	acquire_device(dev);
	if (read_write_init(cfg)) {
		LOG_ERR("Open file failed");
		ret = -EIO;
		goto end2;
	}

	ret = fs_seek(&cfg->file, addr, FS_SEEK_SET);
	if (ret != 0) {
		LOG_ERR("Failed to seek backing file: %d", ret);
		goto end2;
	}

	size_t len_left = size;
	uint8_t *write_pos = (void *)src;

	while (len_left > 0) {
		ret = fs_write(&cfg->file, write_pos, len_left);
		if (ret < 0) {
			LOG_ERR("Failed to write to backing file: %d", ret);
			goto end2;
		}
		if (ret == 0) {
			LOG_ERR("0-byte write to backing file");
			ret = -EIO;
			goto end2;
		}
		len_left -= ret;
		write_pos += ret;
	}
	ret = 0;
end2:
	release_device(dev);
	return ret;
}

static int flash_fs_erase(const struct device *dev, off_t addr, size_t size)
{
	return 0;
}


static int flash_fs_init(const struct device *dev)
{
	struct flashfs_config *cfg = dev->data;
	int ret = 0;

	if (IS_ENABLED(CONFIG_MULTITHREADING)) {
		k_sem_init(&cfg->sem, 1, K_SEM_MAX_LIMIT);
	}

	fs_file_t_init(&cfg->file);
	ret = fs_open(&cfg->file, cfg->name, FS_O_READ | FS_O_WRITE);
	if (ret != 0) {
		LOG_ERR("Failed to open backing file: %d", ret);
		return ret;
	}
	fs_close(&cfg->file);
	return ret;
}

#if defined(CONFIG_FLASH_PAGE_LAYOUT)

static void flash_fs_pages_layout(const struct device *dev,
				 const struct flash_pages_layout **layout,
				 size_t *layout_size)
{
	struct flashfs_config *cfg = dev->data;

	*layout = &cfg->layout;

	*layout_size = 1;
}

#endif /* CONFIG_FLASH_PAGE_LAYOUT */

static const struct flash_parameters * flash_fs_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);

	return &flash_fs_parameters;
}

static int flash_fs_get_size(const struct device *dev, uint64_t *size)
{
	*size = (uint64_t)dev_flash_size(dev);

	return 0;
}

static DEVICE_API(flash, flash_fs_api) = {
	.read = flash_fs_read,
	.write = flash_fs_write,
	.erase = flash_fs_erase,
	.get_parameters = flash_fs_get_parameters,
	.get_size = flash_fs_get_size,
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	.page_layout = flash_fs_pages_layout,
#endif
};

#define GENERATE_CONFIG_STRUCT(idx)								\
	static struct flashfs_config flash_fs_##idx##_config = {				\
		IF_ENABLED(CONFIG_FLASH_PAGE_LAYOUT, (.layout = {				\
			.pages_count = DT_INST_PROP(idx, size) / DT_INST_PROP(idx, page_size),  \
			.pages_count = DT_INST_PROP(idx, page_size),				\
		},))										\
		.name = DT_INST_PROP(idx, file_name),						\
		.flash_size = DT_INST_PROP(idx, size)};


#define FLASH_FS_INST(idx)							         \
	GENERATE_CONFIG_STRUCT(idx)						         \
	DEVICE_DT_INST_DEFINE(idx, flash_fs_init, NULL, &flash_fs_##idx##_config, NULL,  \
			POST_KERNEL, CONFIG_FLASHFS_INIT_PRIORITY, &flash_fs_api);

DT_INST_FOREACH_STATUS_OKAY(FLASH_FS_INST)
