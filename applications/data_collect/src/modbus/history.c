#include <zephyr/kernel.h>
#include <zephyr/posix/dirent.h>
#include <zephyr/posix/fcntl.h>
#include <zephyr/posix/unistd.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modbus_history, LOG_LEVEL_INF);
#if DT_NODE_EXISTS(DT_NODELABEL(lfs1))
#define ROOT DT_PROP(DT_NODELABEL(lfs1), mount_point)
#elif DT_NODE_EXISTS(DT_INST(0, zephyr_flash_disk))
#define ROOT "/"DT_PROP(DT_INST(0, zephyr_flash_disk), disk_name)":"
#else
#error "Must enable filesystem"
#endif
#define MAX_FILE_SIZE (1024ul*1024ul)
#define MAX_FILE_NUM  (10ul*1024ul*1024ul/MAX_FILE_SIZE)
static int fd = -1;
static int fd_offset;
static bool enable_write;

static K_MUTEX_DEFINE(h_lock);
/*
 * @param path: buffer for get path
 * @param latest: true: latest file, false: oldest file
 * @param file_num: file number
 * return: file size
 */
static int get_file(char *path, bool latest, int *file_num)
{
    DIR *dir;
    struct stat st;
    struct dirent *ptr;
    uint32_t val = 0, tmp;
    int size = 0, num = 0;
    char file_name[64];

    dir = opendir(ROOT);
    if (dir == NULL) {
        LOG_ERR("ROOT is not found");
        return -1;
    }

    while ((ptr = readdir(dir)) != NULL) {
        if ((strncmp(ptr->d_name, "data_", strlen("data_")) != 0)) continue;
        if (snprintf(path, 128, ROOT"/%s", ptr->d_name) < 0) return -1;
    	if (sscanf(ptr->d_name, "data_%u.raw", &tmp) != 1) continue;

        if (stat(path, &st) == 0) {
            if (!S_ISREG(st.st_mode)) continue;
            num += 1;
            if (val == 0) {
                val = tmp;
                size = st.st_size;
                if (snprintf(file_name, sizeof(file_name), "%s", ptr->d_name) < 0) return -1;
            } else if (latest) {
                if (val < tmp) {
                    val = tmp;
                    size = st.st_size;
                    if (snprintf(file_name, sizeof(file_name), "%s", ptr->d_name) < 0) return -1;
                }
            } else {
                if (val > tmp) {
                    val = tmp;
                    size = st.st_size;
                    if (snprintf(file_name, sizeof(file_name), "%s", ptr->d_name) < 0) return -1;
                }
            }
        }
    }
    if (val == 0) {
        closedir(dir);
        return -1;
    }

    if (snprintf(path, 128, ROOT"/%s", file_name) < 0) return -1;
    if (file_num)
        *file_num = num;
    closedir(dir);

    return size;
}

static int open_latest_file(bool create)
{
    char file_name[128];
    int file_n;
    
    fd_offset = get_file(file_name, true, &file_n);
    if (!(file_n > MAX_FILE_NUM) && !create && (fd_offset >= 0)) {
        fd = open(file_name, O_WRONLY);
        lseek(fd, 0, SEEK_END);
        return fd;
    } else if (file_n >= MAX_FILE_NUM){
        if (get_file(file_name, false, NULL) >= 0) {
            unlink(file_name);
        }
    }

    /* create new file */
    {
        uint32_t t = time(NULL);

        if (snprintf(file_name, sizeof(file_name), ROOT"/data_%u.raw", t) < 0) return -1;
        LOG_INF("file name: %s", file_name);
        fd = open(file_name, O_CREAT|O_WRONLY);
        fd_offset = 0;
    }
    return fd;
}

int write_history_data(void *data, size_t size)
{
    int ret = -1;
    bool create = false;

    k_mutex_lock(&h_lock, K_FOREVER);
    if (!enable_write) return -1;
    if ((fd_offset + size) > MAX_FILE_SIZE) {
        close(fd);
        fd = -1;
        create = true;
    }

    if (fd < 0) {
       fd = open_latest_file(create); 
       if (fd < 0) {
           LOG_ERR("open history data file failed");
	        k_mutex_unlock(&h_lock);
           return -1;
       }
    }

    ret = write(fd, data, size);
    if (ret < 0) {
        LOG_ERR("%s: write failed", __FUNCTION__);
	    k_mutex_unlock(&h_lock);
        return -1;
    }
    fd_offset += size;
    if (fd_offset % 4096 == 0) fsync(fd);

    k_mutex_unlock(&h_lock);

    return 0;
}

void history_enable_write(bool enable)
{
    k_mutex_lock(&h_lock, K_FOREVER);
    if (!enable) {
        if (fd >= 0) {
            close(fd); 
            fd = -1;
        }
    }
    enable_write = enable;
    k_mutex_unlock(&h_lock);
}

