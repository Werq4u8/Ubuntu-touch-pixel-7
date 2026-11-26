/*
 * ut_stage1_init.c - Early Stage-1 /init for Google Pixel 7 (panther / gs201)
 *
 * Stage 2 (Ubuntu Touch userspace):
 *   1. Mounts /dev (tmpfs), /proc, /sys, creates essential device nodes.
 *   2. Loads display + IOMMU + DMA-heap modules via /system/bin/modprobe -d /lib/modules:
 *        i2c-exynos5.ko, samsung_iommu.ko, samsung-iommu-group.ko,
 *        samsung_dma_heap.ko, phy-exynos-mipi-dsim.ko, exynos-drm.ko,
 *        panel-samsung-s6e3fc3-p10.ko
 *   3. Brings up Exynos DRM/KMS (1080x2400) and renders an on-screen boot console.
 *   4. Locates Ubuntu Touch rootfs:
 *        - Priority 1: Logical partition "system_b" inside /dev/block/by-name/super
 *                      (parsed via Android liblp metadata & mapped with dm-linear).
 *        - Priority 2: /dev/block/by-name/userdata (ext4/f2fs) -> /ubuntu.img or /data/ubuntu.img.
 *   5. Copies /lib/modules into the target rootfs (tmpfs overlay if needed) so systemd/udev
 *      and Halium have access to matching kernel modules.
 *   6. Moves /dev, /proc, /sys, /run into /newroot and hands off via switch_root + execv
 *      to /sbin/init (or /lib/systemd/systemd) with systemd.log_target=kmsg.
 *   7. On any error, displays clear diagnostic info on the 1080x2400 screen and stays alive.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LINUX_REBOOT_MAGIC1 0xfee1dead
#define LINUX_REBOOT_MAGIC2 672274793
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321fedc
#define LINUX_REBOOT_CMD_RESTART2  0xa1b2c3d4

struct input_event_u {
    long tv_sec;
    long tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

/* ---- Minimal DRM definitions (avoids external kernel header dependency) ---- */
#define DRM_IOCTL_BASE 'd'
#define DRM_IOWR(nr, type) _IOWR(DRM_IOCTL_BASE, nr, type)
#define DRM_IO(nr) _IO(DRM_IOCTL_BASE, nr)

struct drm_mode_modeinfo_u {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
};

struct drm_mode_card_res_u {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width, max_width;
    uint32_t min_height, max_height;
};

struct drm_mode_get_connector_u {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width, mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

struct drm_mode_create_dumb_u {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct drm_mode_fb_cmd_u {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
};

struct drm_mode_map_dumb_u {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_mode_crtc_u {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct drm_mode_modeinfo_u mode;
};

struct drm_mode_fb_dirty_cmd_u {
    uint32_t fb_id;
    uint32_t flags;
    uint32_t color;
    uint32_t num_clips;
    uint64_t clips_ptr;
};

#define DRM_IOCTL_SET_MASTER        DRM_IO(0x1e)
#define DRM_IOCTL_DROP_MASTER       DRM_IO(0x1f)
#define DRM_IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct drm_mode_card_res_u)
#define DRM_IOCTL_MODE_GETCRTC      DRM_IOWR(0xA1, struct drm_mode_crtc_u)
#define DRM_IOCTL_MODE_SETCRTC      DRM_IOWR(0xA2, struct drm_mode_crtc_u)
#define DRM_IOCTL_MODE_GETCONNECTOR DRM_IOWR(0xA7, struct drm_mode_get_connector_u)
#define DRM_IOCTL_MODE_ADDFB        DRM_IOWR(0xAE, struct drm_mode_fb_cmd_u)
#define DRM_IOCTL_MODE_DIRTYFB      DRM_IOWR(0xB1, struct drm_mode_fb_dirty_cmd_u)
#define DRM_IOCTL_MODE_CREATE_DUMB  DRM_IOWR(0xB2, struct drm_mode_create_dumb_u)
#define DRM_IOCTL_MODE_MAP_DUMB     DRM_IOWR(0xB3, struct drm_mode_map_dumb_u)

/* ---- Minimal Device Mapper (dm-linear) ioctls ---- */
#define DM_IOCTL 0xfd
#define DM_VERSION_MAJOR 4
#define DM_VERSION_MINOR 0
#define DM_VERSION_PATCHLEVEL 0
#define DM_NAME_LEN 128
#define DM_UUID_LEN 129

struct dm_ioctl_u {
    uint32_t version[3];
    uint32_t data_size;
    uint32_t data_start;
    uint32_t target_count;
    int32_t  open_count;
    uint32_t flags;
    uint32_t event_nr;
    uint32_t padding;
    uint64_t dev;
    char name[DM_NAME_LEN];
    char uuid[DM_UUID_LEN];
    char data[7];
};

struct dm_target_spec_u {
    uint64_t sector_start;
    uint64_t length;
    int32_t  status;
    uint32_t next;
    char target_type[16];
};

#define DM_DEV_CREATE  _IOWR(DM_IOCTL, 3, struct dm_ioctl_u)
#define DM_DEV_SUSPEND _IOWR(DM_IOCTL, 6, struct dm_ioctl_u)
#define DM_TABLE_LOAD  _IOWR(DM_IOCTL, 9, struct dm_ioctl_u)

/* ---- Android liblp (super partition) structures ---- */
#define LP_METADATA_GEOMETRY_MAGIC 0x616c4467u
#define LP_METADATA_HEADER_MAGIC   0x414c5030u
#define LP_TARGET_TYPE_LINEAR      0u

struct lp_geometry {
    uint32_t magic;
    uint32_t struct_size;
    uint8_t  checksum[32];
    uint32_t metadata_max_size;
    uint32_t metadata_slot_count;
    uint32_t logical_block_size;
} __attribute__((packed));

struct lp_table_desc {
    uint32_t offset;
    uint32_t num_entries;
    uint32_t entry_size;
} __attribute__((packed));

struct lp_header {
    uint32_t magic;
    uint16_t major_version;
    uint16_t minor_version;
    uint32_t header_size;
    uint8_t  header_checksum[32];
    uint32_t tables_size;
    uint8_t  tables_checksum[32];
    struct lp_table_desc partitions;
    struct lp_table_desc extents;
    struct lp_table_desc groups;
    struct lp_table_desc block_devices;
} __attribute__((packed));

struct lp_partition {
    char     name[36];
    uint32_t attributes;
    uint32_t first_extent_index;
    uint32_t num_extents;
    uint32_t group_index;
} __attribute__((packed));

struct lp_extent {
    uint64_t num_sectors;
    uint32_t target_type;
    uint64_t target_data;
    uint32_t target_source;
} __attribute__((packed));

/* ---- Logging ---- */
static void klog(const char *msg) {
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        write(fd, msg, strlen(msg));
        close(fd);
    }
}

/* ---- Minimal 8x8 bitmap font (ASCII 32..126) ---- */
static const uint8_t font8x8[96][8] = {
    [' ' - 32] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['!' - 32] = {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    ['"' - 32] = {0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00},
    ['#' - 32] = {0x66,0xFF,0x66,0x66,0xFF,0x66,0x00,0x00},
    ['$' - 32] = {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00},
    ['%' - 32] = {0x62,0x64,0x08,0x10,0x26,0x46,0x00,0x00},
    ['&' - 32] = {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00},
    ['\''- 32] = {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    ['(' - 32] = {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
    [')' - 32] = {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    ['*' - 32] = {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    ['+' - 32] = {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    [',' - 32] = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
    ['-' - 32] = {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    ['.' - 32] = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    ['/' - 32] = {0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x00},
    ['0' - 32] = {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00},
    ['1' - 32] = {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    ['2' - 32] = {0x3C,0x66,0x06,0x1C,0x30,0x60,0x7E,0x00},
    ['3' - 32] = {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},
    ['4' - 32] = {0x0C,0x1C,0x3C,0x6C,0xFE,0x0C,0x0C,0x00},
    ['5' - 32] = {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},
    ['6' - 32] = {0x3C,0x60,0x7C,0x66,0x66,0x66,0x3C,0x00},
    ['7' - 32] = {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00},
    ['8' - 32] = {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00},
    ['9' - 32] = {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00},
    [':' - 32] = {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00},
    [';' - 32] = {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00},
    ['<' - 32] = {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00},
    ['=' - 32] = {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    ['>' - 32] = {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00},
    ['?' - 32] = {0x3C,0x66,0x0C,0x18,0x18,0x00,0x18,0x00},
    ['@' - 32] = {0x3C,0x66,0x6E,0x6A,0x6E,0x60,0x3C,0x00},
    ['A' - 32] = {0x18,0x3C,0x66,0x7E,0x66,0x66,0x66,0x00},
    ['B' - 32] = {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00},
    ['C' - 32] = {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},
    ['D' - 32] = {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00},
    ['E' - 32] = {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00},
    ['F' - 32] = {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00},
    ['G' - 32] = {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00},
    ['H' - 32] = {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00},
    ['I' - 32] = {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    ['J' - 32] = {0x06,0x06,0x06,0x06,0x06,0x66,0x3C,0x00},
    ['K' - 32] = {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00},
    ['L' - 32] = {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00},
    ['M' - 32] = {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00},
    ['N' - 32] = {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00},
    ['O' - 32] = {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    ['P' - 32] = {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00},
    ['Q' - 32] = {0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00},
    ['R' - 32] = {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00},
    ['S' - 32] = {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00},
    ['T' - 32] = {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
    ['U' - 32] = {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    ['V' - 32] = {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00},
    ['W' - 32] = {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    ['X' - 32] = {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00},
    ['Y' - 32] = {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00},
    ['Z' - 32] = {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00},
    ['[' - 32] = {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    ['\\' - 32]= {0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x00},
    [']' - 32] = {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    ['^' - 32] = {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00},
    ['_' - 32] = {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00},
    ['a' - 32] = {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00},
    ['b' - 32] = {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00},
    ['c' - 32] = {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00},
    ['d' - 32] = {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00},
    ['e' - 32] = {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},
    ['f' - 32] = {0x1C,0x30,0x7C,0x30,0x30,0x30,0x30,0x00},
    ['g' - 32] = {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C},
    ['h' - 32] = {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00},
    ['i' - 32] = {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    ['j' - 32] = {0x06,0x00,0x0E,0x06,0x06,0x66,0x3C,0x00},
    ['k' - 32] = {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00},
    ['l' - 32] = {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    ['m' - 32] = {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00},
    ['n' - 32] = {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00},
    ['o' - 32] = {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
    ['p' - 32] = {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60},
    ['q' - 32] = {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06},
    ['r' - 32] = {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00},
    ['s' - 32] = {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},
    ['t' - 32] = {0x30,0x30,0x7C,0x30,0x30,0x34,0x18,0x00},
    ['u' - 32] = {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00},
    ['v' - 32] = {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00},
    ['w' - 32] = {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
    ['x' - 32] = {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00},
    ['y' - 32] = {0x00,0x00,0x66,0x66,0x3E,0x06,0x3C,0x00},
    ['z' - 32] = {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00},
};

/* ---- Framebuffer state ---- */
static int g_drm_fd = -1;
static uint32_t g_fb_id = 0;
static struct drm_mode_crtc_u g_crtc_cfg;
static uint32_t g_conn_id = 0;
static uint32_t *fb_pixels = NULL;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch_px = 0;
static int console_y = 280;

static void fb_flush(void) {
    if (g_drm_fd < 0 || !g_fb_id) return;
    __asm__ volatile("dsb sy" ::: "memory");
    struct drm_mode_fb_dirty_cmd_u dirty = { .fb_id = g_fb_id };
    ioctl(g_drm_fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
    g_crtc_cfg.set_connectors_ptr = (uint64_t)(uintptr_t)&g_conn_id;
    ioctl(g_drm_fd, DRM_IOCTL_MODE_SETCRTC, &g_crtc_cfg);
}

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!fb_pixels) return;
    for (int py = y; py < y + h && py < (int)fb_height; py++) {
        if (py < 0) continue;
        for (int px = x; px < x + w && px < (int)fb_width; px++) {
            if (px < 0) continue;
            fb_pixels[py * fb_pitch_px + px] = color;
        }
    }
}

static void draw_char(int x, int y, char c, uint32_t color, int scale) {
    if (!fb_pixels || c < 32 || c > 126) return;
    const uint8_t *g = font8x8[c - 32];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *s, uint32_t color, int scale) {
    int cx = x;
    while (*s) {
        draw_char(cx, y, *s, color, scale);
        cx += 8 * scale;
        s++;
    }
}

static int g_kmsg_scale = 2;
static int g_screen_frozen = 0;

static void ui_draw_line_ex(const char *msg, uint32_t color, int do_flush) {
    if (!fb_pixels || !msg || g_screen_frozen) return;
    int scale = g_kmsg_scale;
    int char_w = 8 * scale;
    int line_h = (scale == 1) ? 10 : 28;
    int max_chars = ((int)fb_width - 80) / char_w;
    if (max_chars < 10) max_chars = 10;
    size_t len = strlen(msg);
    size_t off = 0;
    do {
        char chunk[160];
        size_t take = len - off;
        if ((int)take > max_chars) take = (size_t)max_chars;
        if (take >= sizeof(chunk)) take = sizeof(chunk) - 1;
        memcpy(chunk, msg + off, take);
        chunk[take] = 0;

        if (console_y + line_h >= (int)fb_height - 90) {
            if (scale == 1) {
                g_screen_frozen = 1;
                draw_text(40, fb_height - 102, "[-- Screen full (200+ lines). Press VOL- for Fastboot --]", 0xFFFACC15, 1);
                if (do_flush) fb_flush();
                return;
            }
            fill_rect(40, 270, fb_width - 80, fb_height - 370, 0xFF0E141B);
            console_y = 280;
        }
        draw_text(40, console_y, chunk, color, scale);
        console_y += line_h;
        off += take;
    } while (off < len);
    if (do_flush) fb_flush();
}

static void draw_status_bar(const char *msg, uint32_t color) {
    if (!fb_pixels) return;
    int bar_y = (int)fb_height - 72;
    fill_rect(0, bar_y - 8, fb_width, 80, 0xFF0A0F14);
    fill_rect(40, bar_y - 8, fb_width - 80, 2, 0xFF334155);
    draw_text(40, bar_y + 8, msg, color, 2);
    fb_flush();
}

static void ui_log(const char *msg, uint32_t color) {
    klog("<5>UT-STAGE1: ");
    klog(msg);
    klog("\n");
    ui_draw_line_ex(msg, color, 1);
}

/* ---- Load display + PMIC + IOMMU modules via /system/bin/modprobe ---- */
static int load_display_modules(void) {
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {
            "/system/bin/modprobe",
            "-s",
            "-d", "/lib/modules",
            "i2c-exynos5.ko",
            "i2c-acpm.ko",
            "s2mpg12-regulator.ko",
            "s2mpg13-regulator.ko",
            "s2mpg1x-gpio.ko",
            "samsung_iommu.ko",
            "samsung-iommu-group.ko",
            "samsung_dma_heap.ko",
            "phy-exynos-mipi-dsim.ko",
            "exynos-drm.ko",
            "panel-samsung-s6e3fc3-p10.ko",
            NULL
        };
        char *envp[] = { NULL };
        execve("/system/bin/modprobe", argv, envp);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status = 0;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int load_storage_and_key_modules(void) {
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {
            "/system/bin/modprobe",
            "-s",
            "-d", "/lib/modules",
            "ufs-pixel-fips140.ko",
            "ufs-exynos-gs.ko",
            "s2mpg12-key.ko",
            NULL
        };
        char *envp[] = { NULL };
        execve("/system/bin/modprobe", argv, envp);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status = 0;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int modprobe_one(const char *mod) {
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = { "/system/bin/modprobe", "-s", "-d", "/lib/modules", (char *)mod, NULL };
        char *envp[] = { NULL };
        execve("/system/bin/modprobe", argv, envp);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status = 0;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int load_usb_udc_modules(void) {
    /* Load any max77* gpio/pinctrl module first (required by 13-0025 i2c-max77759tcpc) */
    DIR *md = opendir("/lib/modules");
    if (md) {
        char glist[256] = "[gpio-ko]";
        struct dirent *ent;
        while ((ent = readdir(md)) != NULL) {
            if (!strstr(ent->d_name, ".ko")) continue;
            if (strstr(ent->d_name, "gpio") || strstr(ent->d_name, "pinctrl")) {
                if (strlen(glist) + strlen(ent->d_name) + 2 < sizeof(glist)) {
                    strcat(glist, " ");
                    strcat(glist, ent->d_name);
                }
                if (strstr(ent->d_name, "max77")) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "[..] loading %s", ent->d_name);
                    ui_log(msg, 0xFF38BDF8);
                    modprobe_one(ent->d_name);
                }
            }
        }
        closedir(md);
        ui_log(glist, 0xFF94A3B8);
    }

    const char *mods[] = {
        "max77729-pmic.ko",
        "max77759-charger.ko",
        "max77759_helper.ko",
        "tcpci_max77759.ko",
        "google_tcpci_shim.ko",
        "phy-exynos-usbdrd-super.ko",
        "dwc3-exynos-usb.ko",
        NULL
    };
    int fail = 0;
    for (int i = 0; mods[i]; i++) {
        if (strcmp(mods[i], "max77759-charger.ko") == 0) {
            ui_log("[..] loading max77759-charger.ko...", 0xFFFACC15);
        }
        int rc = modprobe_one(mods[i]);
        if (strcmp(mods[i], "max77759-charger.ko") == 0) {
            ui_log(rc == 0 ? "[OK] max77759-charger.ko loaded" : "[!!] max77759-charger.ko failed",
                   rc == 0 ? 0xFF4ADE80 : 0xFFF87171);
        }
        if (rc != 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "[!!] modprobe failed: %s", mods[i]);
            ui_log(msg, 0xFFF87171);
            fail = 1;
        }
    }
    return fail ? -1 : 0;
}

static void load_remaining_boot_modules(void) {
    FILE *f = fopen("/lib/modules/modules.load", "r");
    if (!f) {
        ui_log("[dbg] /lib/modules/modules.load not found", 0xFFFACC15);
        return;
    }
    char *args[256];
    int argc = 0;
    args[argc++] = "/system/bin/modprobe";
    args[argc++] = "-s";
    args[argc++] = "-d";
    args[argc++] = "/lib/modules";
    args[argc++] = "-a";
    char line[256];
    while (fgets(line, sizeof(line), f) && argc < 250) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0 || line[0] == '#') continue;
        args[argc++] = strdup(line);
    }
    fclose(f);
    args[argc] = NULL;
    if (argc > 5) {
        pid_t pid = fork();
        if (pid == 0) {
            char *envp[] = { NULL };
            execve("/system/bin/modprobe", args, envp);
            _exit(127);
        }
        if (pid > 0) {
            int st = 0;
            waitpid(pid, &st, 0);
        }
    }
    for (int i = 5; i < argc; i++) free(args[i]);
}

static int open_input_devices(int *fds, int max_fds) {
    mkdir("/dev/input", 0755);
    int nfds = 0;
    DIR *d = opendir("/sys/class/input");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && nfds < max_fds) {
            if (strncmp(ent->d_name, "event", 5) != 0) continue;
            char devp[256], nodep[256];
            snprintf(devp, sizeof(devp), "/sys/class/input/%s/dev", ent->d_name);
            FILE *f = fopen(devp, "r");
            if (!f) continue;
            unsigned int maj = 0, min = 0;
            if (fscanf(f, "%u:%u", &maj, &min) == 2) {
                snprintf(nodep, sizeof(nodep), "/dev/input/%s", ent->d_name);
                mknod(nodep, S_IFCHR | 0600, makedev(maj, min));
                int fd = open(nodep, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (fd >= 0) fds[nfds++] = fd;
            }
            fclose(f);
        }
        closedir(d);
    }
    return nfds;
}

static void wait_key_power_or_fastboot(void) {
    ui_log("------------------------------------------------", 0xFF64748B);
    ui_log("KEYS: [VOL DOWN] = Fastboot | [POWER] = Power Off", 0xFFFACC15);
    int fds[16];
    int nfds = open_input_devices(fds, 16);
    if (nfds == 0) {
        ui_log("[dbg] No input/event* devices found", 0xFF94A3B8);
        while (1) sleep(5);
    }
    struct pollfd pfds[16];
    for (int i = 0; i < nfds; i++) { pfds[i].fd = fds[i]; pfds[i].events = POLLIN; }
    while (1) {
        if (poll(pfds, nfds, 1000) > 0) {
            for (int i = 0; i < nfds; i++) {
                if (!(pfds[i].revents & POLLIN)) continue;
                struct input_event_u ev;
                while (read(fds[i], &ev, sizeof(ev)) == sizeof(ev)) {
                    if (ev.type == 1 && ev.value == 1) {
                        if (ev.code == 114) { /* KEY_VOLUMEDOWN */
                            ui_log("Rebooting to Fastboot...", 0xFF38BDF8);
                            sync();
                            syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, "bootloader");
                        } else if (ev.code == 116) { /* KEY_POWER */
                            ui_log("Powering off...", 0xFFF87171);
                            sync();
                            syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_POWER_OFF, NULL);
                        }
                    }
                }
            }
        }
    }
}

static int write_sysfs_str(const char *path, const char *val) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t w = write(fd, val, strlen(val));
    close(fd);
    return (w >= 0) ? 0 : -1;
}

static void probe_usb_gadget_functions_only(void) {
    mkdir("/sys/kernel/config", 0755);
    mount("configfs", "/sys/kernel/config", "configfs", 0, NULL);
    const char *g = "/sys/kernel/config/usb_gadget/utlog";
    if (mkdir(g, 0755) != 0 && errno != EEXIST) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[usb-cfg] mkdir usb_gadget/utlog failed: %s", strerror(errno));
        ui_log(msg, 0xFFF87171);
        return;
    }
    const char *fns[] = { "acm.usb0", "ncm.usb0", "rndis.usb0", "ecm.usb0", "ffs.adb", NULL };
    char fmsg[160] = "[usb-fn]";
    for (int i = 0; fns[i]; i++) {
        char fp[160];
        snprintf(fp, sizeof(fp), "%s/functions/%s", g, fns[i]);
        int ok = (mkdir(fp, 0755) == 0 || errno == EEXIST);
        char item[32];
        snprintf(item, sizeof(item), " %s=%d", fns[i], ok);
        strncat(fmsg, item, sizeof(fmsg) - strlen(fmsg) - 1);
        if (ok) rmdir(fp);
    }
    rmdir(g);
    ui_log(fmsg, 0xFF38BDF8);
}

static void debug_usb_deferred_and_kmsg(int kmsg_rd_fd) {
    if (kmsg_rd_fd >= 0) {
        char buf[1024];
        int shown = 0;
        while (shown < 20) {
            ssize_t n = read(kmsg_rd_fd, buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = 0;
            char *semi = strchr(buf, ';');
            char *text = semi ? (semi + 1) : buf;
            char *nl = strpbrk(text, "\r\n");
            if (nl) *nl = 0;
            if (strstr(text, "modprobe: Loading module") ||
                strstr(text, "modprobe: Loaded kernel module") ||
                strstr(text, "max77759_fg")) continue;
            if (strstr(text, "usb") || strstr(text, "dwc") || strstr(text, "phy") ||
                strstr(text, "tcpc") || strstr(text, "max77759") || strstr(text, "max77729") ||
                strstr(text, "max777x9") || strstr(text, "udc") ||
                strstr(text, "gadget") || strstr(text, "extcon") ||
                strstr(text, "CHARGER_MODE") || strstr(text, "GBMS_MODE") ||
                strstr(text, "votable") || strstr(text, "13-0025") || strstr(text, "13-0066")) {
                char msg[160];
                snprintf(msg, sizeof(msg), "[kmsg] %.140s", text);
                ui_log(msg, 0xFFFACC15);
                shown++;
            }
        }
    }
    /* Check 13-0025 and 13-0066 existence, driver, and waiting_for_supplier */
    {
        const char *devs[3] = { "13-0025", "13-0066", NULL };
        for (int d = 0; devs[d]; d++) {
            char p_dev[128], p_drv[128], p_wfs[128];
            snprintf(p_dev, sizeof(p_dev), "/sys/bus/i2c/devices/%s", devs[d]);
            snprintf(p_drv, sizeof(p_drv), "/sys/bus/i2c/devices/%s/driver", devs[d]);
            snprintf(p_wfs, sizeof(p_wfs), "/sys/bus/i2c/devices/%s/waiting_for_supplier", devs[d]);
            int exists = (access(p_dev, F_OK) == 0);
            char drv[256] = {0};
            ssize_t rl = readlink(p_drv, drv, sizeof(drv) - 1);
            int wfs = -1;
            FILE *wf = fopen(p_wfs, "r");
            if (wf) { fscanf(wf, "%d", &wfs); fclose(wf); }
            char msg[256];
            snprintf(msg, sizeof(msg), "[%s] exists=%d drv=%s wait_sup=%d",
                     devs[d], exists, (rl > 0) ? drv : "NONE", wfs);
            ui_log(msg, 0xFF38BDF8);
        }
    }
    mkdir("/mnt_dbg", 0755);
    if (mount("debugfs", "/mnt_dbg", "debugfs", 0, NULL) == 0) {
        FILE *f = fopen("/mnt_dbg/devices_deferred", "r");
        if (f) {
            char line[256];
            int cnt = 0;
            while (fgets(line, sizeof(line), f) && cnt < 12) {
                line[strcspn(line, "\r\n")] = 0;
                if (strstr(line, "usb") || strstr(line, "dwc") || strstr(line, "phy") ||
                    strstr(line, "tcpc") || strstr(line, "max77") || strstr(line, "13-0025") ||
                    strstr(line, "13-0066") || strstr(line, "i2c")) {
                    char msg[160];
                    snprintf(msg, sizeof(msg), "[defer] %.140s", line);
                    ui_log(msg, 0xFFF87171);
                    cnt++;
                }
            }
            fclose(f);
        }
        umount("/mnt_dbg");
    }
    rmdir("/mnt_dbg");
}

static int try_setup_usb_serial_gadget(void) {
    char udc_name[128] = {0};
    DIR *ud = opendir("/sys/class/udc");
    if (ud) {
        struct dirent *ent;
        while ((ent = readdir(ud)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            snprintf(udc_name, sizeof(udc_name), "%s", ent->d_name);
            break;
        }
        closedir(ud);
    }
    if (udc_name[0] == 0) {
        ui_log("[udc-after] no controller registered", 0xFFF87171);
        return -1;
    }
    char udc_msg[160];
    snprintf(udc_msg, sizeof(udc_msg), "[udc-after] %s", udc_name);
    ui_log(udc_msg, 0xFF38BDF8);

    mkdir("/sys/kernel/config", 0755);
    mount("configfs", "/sys/kernel/config", "configfs", 0, NULL);

    const char *g = "/sys/kernel/config/usb_gadget/utlog";
    if (mkdir(g, 0755) != 0 && errno != EEXIST) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[usb-cfg] mkdir usb_gadget/utlog failed: %s", strerror(errno));
        ui_log(msg, 0xFFF87171);
        return -1;
    }

    write_sysfs_str("/sys/kernel/config/usb_gadget/utlog/idVendor", "0x1d6b");
    write_sysfs_str("/sys/kernel/config/usb_gadget/utlog/idProduct", "0x0104");
    write_sysfs_str("/sys/kernel/config/usb_gadget/utlog/bDeviceClass", "0xEF");
    write_sysfs_str("/sys/kernel/config/usb_gadget/utlog/bDeviceSubClass", "0x02");
    write_sysfs_str("/sys/kernel/config/usb_gadget/utlog/bDeviceProtocol", "0x01");
    mkdir("/sys/kernel/config/usb_gadget/utlog/strings/0x409", 0755);
    write_sysfs_str("/sys/kernel/config/usb_gadget/utlog/strings/0x409/serialnumber", "panther-ut");
    write_sysfs_str("/sys/kernel/config/usb_gadget/utlog/strings/0x409/manufacturer", "Google");
    write_sysfs_str("/sys/kernel/config/usb_gadget/utlog/strings/0x409/product", "Pixel 7 UT NCM+ACM");
    mkdir("/sys/kernel/config/usb_gadget/utlog/configs/c.1", 0755);
    mkdir("/sys/kernel/config/usb_gadget/utlog/configs/c.1/strings/0x409", 0755);
    write_sysfs_str("/sys/kernel/config/usb_gadget/utlog/configs/c.1/strings/0x409/configuration", "ncm+acm");
    if (mkdir("/sys/kernel/config/usb_gadget/utlog/functions/ncm.usb0", 0755) == 0 || errno == EEXIST) {
        write_sysfs_str("/sys/kernel/config/usb_gadget/utlog/functions/ncm.usb0/dev_addr", "02:00:00:00:00:02");
        write_sysfs_str("/sys/kernel/config/usb_gadget/utlog/functions/ncm.usb0/host_addr", "02:00:00:00:00:01");
        symlink("/sys/kernel/config/usb_gadget/utlog/functions/ncm.usb0",
                "/sys/kernel/config/usb_gadget/utlog/configs/c.1/ncm.usb0");
    }
    if (mkdir("/sys/kernel/config/usb_gadget/utlog/functions/acm.usb0", 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    symlink("/sys/kernel/config/usb_gadget/utlog/functions/acm.usb0",
            "/sys/kernel/config/usb_gadget/utlog/configs/c.1/acm.usb0");
    if (write_sysfs_str("/sys/kernel/config/usb_gadget/utlog/UDC", udc_name) != 0) {
        ui_log("[usb-acm] could not bind gadget to UDC", 0xFFF87171);
        return -1;
    }
    ui_log("[usb-acm] gadget bound to UDC", 0xFF38BDF8);

    for (int r = 0; r < 20; r++) {
        FILE *f = fopen("/sys/class/tty/ttyGS0/dev", "r");
        if (f) {
            unsigned int maj = 0, min = 0;
            if (fscanf(f, "%u:%u", &maj, &min) == 2) {
                mknod("/dev/ttyGS0", S_IFCHR | 0666, makedev(maj, min));
            }
            fclose(f);
            int tfd = open("/dev/ttyGS0", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            if (tfd >= 0) return tfd;
        }
        usleep(50000);
    }
    return -1;
}

static void write_file_mode(const char *path, const char *content, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd >= 0) {
        write(fd, content, strlen(content));
        fchmod(fd, mode);
        close(fd);
    }
}

static void configure_newroot_ssh_and_usb(void) {
    int has_sshd = (access("/newroot/usr/sbin/sshd", X_OK) == 0);
    ui_log(has_sshd ? "[OK] /usr/sbin/sshd found in rootfs" : "[!!] /usr/sbin/sshd NOT found!",
           has_sshd ? 0xFF4ADE80 : 0xFFF87171);

    /* Replace panther-usb-debug.sh to configure NCM usb0 IP and start standalone sshd */
    mkdir("/newroot/usr/local/sbin", 0755);
    write_file_mode("/newroot/usr/local/sbin/panther-usb-debug.sh",
        "#!/bin/sh\n"
        "exec >> /dev/kmsg 2>&1\n"
        "echo \"panther-usb-debug: configuring usb0 NCM + ssh\"\n"
        "ip link set usb0 up 2>/dev/null || true\n"
        "ip addr replace 10.15.19.82/24 dev usb0 2>/dev/null || ip addr add 10.15.19.82/24 dev usb0 2>/dev/null || true\n"
        "mkdir -p /run/sshd\n"
        "chmod 0755 /run/sshd\n"
        "if [ ! -f /run/ut_host_ed25519_key ]; then ssh-keygen -t ed25519 -N '' -f /run/ut_host_ed25519_key || true; fi\n"
        "/usr/sbin/sshd -f /etc/ssh/ut_sshd_config -E /dev/kmsg || true\n",
        0755);

    /* Prevent NetworkManager from resetting usb0 */
    mkdir("/newroot/etc/NetworkManager", 0755);
    mkdir("/newroot/etc/NetworkManager/conf.d", 0755);
    write_file_mode("/newroot/etc/NetworkManager/conf.d/99-unmanaged-usb0.conf",
        "[keyfile]\nunmanaged-devices=interface-name:usb0\n", 0644);

    /* Mask lxc-android-config-disable-ssh-socket.service and enable ssh */
    mkdir("/newroot/etc/systemd/system", 0755);
    unlink("/newroot/etc/systemd/system/lxc-android-config-disable-ssh-socket.service");
    symlink("/dev/null", "/newroot/etc/systemd/system/lxc-android-config-disable-ssh-socket.service");
    mkdir("/newroot/etc/systemd/system/multi-user.target.wants", 0755);
    symlink("/lib/systemd/system/ssh.service",
            "/newroot/etc/systemd/system/multi-user.target.wants/ssh.service");

    /* Fix upower.service failing on PrivateUsers=yes in GKI kernel */
    mkdir("/newroot/etc/systemd/system/upower.service.d", 0755);
    write_file_mode("/newroot/etc/systemd/system/upower.service.d/no-userns.conf",
        "[Service]\nPrivateUsers=no\n", 0644);

    /* Allow root SSH with user's id_ed25519 key via standalone config */
    mkdir("/newroot/etc/ssh", 0755);
    write_file_mode("/newroot/etc/ssh/ut_sshd_config",
        "Port 22\n"
        "ListenAddress 0.0.0.0\n"
        "HostKey /run/ut_host_ed25519_key\n"
        "PermitRootLogin yes\n"
        "PubkeyAuthentication yes\n"
        "PasswordAuthentication no\n"
        "AuthorizedKeysFile /root/.ssh/authorized_keys\n"
        "UsePAM no\n"
        "StrictModes no\n"
        "Subsystem sftp internal-sftp\n", 0644);

#ifndef UT_SSH_PUBKEY
#define UT_SSH_PUBKEY "ssh-ed25519 REPLACE_WITH_YOUR_SSH_PUBLIC_KEY user@host\n"
#endif
    const char *pubkey = UT_SSH_PUBKEY;
    mkdir("/newroot/root", 0700);
    mkdir("/newroot/root/.ssh", 0700);
    write_file_mode("/newroot/root/.ssh/authorized_keys", pubkey, 0600);

    struct stat ph_st;
    if (stat("/newroot/home/phablet", &ph_st) == 0) {
        mkdir("/newroot/home/phablet/.ssh", 0700);
        chown("/newroot/home/phablet/.ssh", ph_st.st_uid, ph_st.st_gid);
        write_file_mode("/newroot/home/phablet/.ssh/authorized_keys", pubkey, 0600);
        chown("/newroot/home/phablet/.ssh/authorized_keys", ph_st.st_uid, ph_st.st_gid);
    }
    ui_log("[OK] Configured NCM+ACM USB + SSH key in /newroot", 0xFF4ADE80);
}

static pid_t start_kmsg_console_monitor(int kmsg_rd_fd, int *input_fds, int nfds, int usb_tty_fd) {
    pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }

    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    g_kmsg_scale = 1;

    int auto_reboot_sec = 180;
    int auto_enabled = 1;
    struct timespec ts_last;
    clock_gettime(CLOCK_MONOTONIC, &ts_last);

    char bar[128];
    snprintf(bar, sizeof(bar), "VOL- Fastboot | PWR Off | VOL+ Keep | Auto:%ds", auto_reboot_sec);
    draw_status_bar(bar, 0xFFFACC15);

    struct pollfd pfds[17];
    int n_poll = 0;
    int kmsg_idx = -1;
    if (kmsg_rd_fd >= 0) {
        kmsg_idx = n_poll;
        pfds[n_poll].fd = kmsg_rd_fd;
        pfds[n_poll].events = POLLIN;
        n_poll++;
    }
    int in_start = n_poll;
    for (int i = 0; i < nfds && n_poll < 17; i++) {
        pfds[n_poll].fd = input_fds[i];
        pfds[n_poll].events = POLLIN;
        n_poll++;
    }

    while (1) {
        int r = poll(pfds, n_poll, 250);
        int need_flush = 0;

        if (r > 0 && kmsg_idx >= 0 && (pfds[kmsg_idx].revents & POLLIN)) {
            char buf[2048];
            int batch = 0;
            while (batch < 24) {
                ssize_t n = read(kmsg_rd_fd, buf, sizeof(buf) - 1);
                if (n <= 0) break;
                buf[n] = 0;

                int prio = atoi(buf);
                int level = prio & 7;
                char *semi = strchr(buf, ';');
                char *text = semi ? (semi + 1) : buf;
                char *nl = strpbrk(text, "\r\n");
                if (nl) *nl = 0;
                if (text[0] == 0) continue;

                if (usb_tty_fd >= 0) {
                    char line_out[1024];
                    int l = snprintf(line_out, sizeof(line_out), "[L%d] %s\r\n", level, text);
                    if (l > 0) write(usb_tty_fd, line_out, (size_t)l);
                }

                /* Keep only UT-STAGE1, systemd[1] milestones/errors, USB debug, and panics */
                int show = 0;
                if (strstr(text, "UT-STAGE1:") || strstr(text, "panther-usb") ||
                    strstr(text, "Kernel panic") || strstr(text, "Attempted to kill init") ||
                    strstr(text, "rndis") || strstr(text, "usb0")) {
                    show = 1;
                } else if (strstr(text, "systemd[1]:") && !strstr(text, "error-name=") && (
                    level <= 4 ||
                    strstr(text, "Started ") || strstr(text, "Failed ") ||
                    strstr(text, "Reached ") || strstr(text, "Welcome") ||
                    strstr(text, "usb") || strstr(text, "ssh") ||
                    strstr(text, "panther") || strstr(text, "failed"))) {
                    show = 1;
                }
                if (!show) continue;

                uint32_t col = 0xFFE2E8F0;
                if (level <= 3) col = 0xFFF87171;
                else if (level == 4) col = 0xFFFACC15;
                else if (strstr(text, "systemd[1]:") != NULL) col = 0xFF38BDF8;
                else if (strstr(text, "panther-usb-debug") != NULL) col = 0xFF4ADE80;

                ui_draw_line_ex(text, col, 0);
                need_flush = 1;
                batch++;
            }
        }

        if (need_flush) {
            fb_flush();
        }

        for (int i = in_start; i < n_poll; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;
            struct input_event_u ev;
            while (read(pfds[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == 1 && ev.value == 1) {
                    if (ev.code == 114) { /* KEY_VOLUMEDOWN */
                        draw_status_bar("Rebooting to Fastboot (preserving ramoops)...", 0xFF38BDF8);
                        sync();
                        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, "bootloader");
                    } else if (ev.code == 116) { /* KEY_POWER */
                        draw_status_bar("Powering off...", 0xFFF87171);
                        sync();
                        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_POWER_OFF, NULL);
                    } else if (ev.code == 115 && auto_enabled) { /* KEY_VOLUMEUP */
                        auto_enabled = 0;
                        draw_status_bar("Auto-reboot cancelled | VOL- Fastboot | PWR Off", 0xFF4ADE80);
                    }
                }
            }
        }

        if (auto_enabled) {
            struct timespec ts_now;
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            if (ts_now.tv_sec > ts_last.tv_sec) {
                long diff = ts_now.tv_sec - ts_last.tv_sec;
                ts_last = ts_now;
                if (diff > 0 && diff < 1000) {
                    auto_reboot_sec -= (int)diff;
                }
                if (auto_reboot_sec <= 0) {
                    draw_status_bar("Auto-rebooting to Fastboot (ramoops)...", 0xFF38BDF8);
                    sync();
                    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, "bootloader");
                } else {
                    snprintf(bar, sizeof(bar), "VOL- Fastboot | PWR Off | VOL+ Keep | Auto:%ds", auto_reboot_sec);
                    draw_status_bar(bar, 0xFFFACC15);
                }
            }
        }
    }
    _exit(0);
}

static int init_drm_display(void) {
    /* Wait for panel DSI-1 connector to bind before opening card0 */
    for (int r = 0; r < 100; r++) {
        if (access("/sys/class/drm/card0-DSI-1/status", R_OK) == 0) {
            klog("<5>UT-STAGE1: card0-DSI-1 status is readable\n");
            break;
        }
        usleep(100000);
    }

    int fd = -1;
    for (int r = 0; r < 60; r++) {
        fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        if (fd >= 0) break;
        usleep(100000);
    }
    if (fd < 0) return -1;

    ioctl(fd, DRM_IOCTL_SET_MASTER, 0);

    struct drm_mode_card_res_u res = {0};
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) return -1;

    uint32_t *fb_ids = calloc(res.count_fbs + 1, sizeof(uint32_t));
    uint32_t *crtc_ids = calloc(res.count_crtcs + 1, sizeof(uint32_t));
    uint32_t *conn_ids = calloc(res.count_connectors + 1, sizeof(uint32_t));
    uint32_t *enc_ids = calloc(res.count_encoders + 1, sizeof(uint32_t));
    res.fb_id_ptr = (uint64_t)(uintptr_t)fb_ids;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)enc_ids;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) return -1;

    uint32_t connector_id = 0;
    struct drm_mode_modeinfo_u mode = {0};
    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector_u conn = {0};
        conn.connector_id = conn_ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) == 0 && conn.connection == 1 && conn.count_modes > 0) {
            connector_id = conn.connector_id;
            struct drm_mode_modeinfo_u *modes = calloc(conn.count_modes, sizeof(struct drm_mode_modeinfo_u));
            conn.modes_ptr = (uint64_t)(uintptr_t)modes;
            conn.count_encoders = 0;
            conn.count_props = 0;
            if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) == 0) {
                mode = modes[0];
            }
            free(modes);
            break;
        }
    }
    if (!connector_id || !mode.hdisplay) return -1;

    fb_width = mode.hdisplay;
    fb_height = mode.vdisplay;

    struct drm_mode_create_dumb_u creq = { .width = fb_width, .height = fb_height, .bpp = 32 };
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) return -1;
    fb_pitch_px = creq.pitch / 4;

    struct drm_mode_fb_cmd_u fbreq = {
        .width = fb_width, .height = fb_height,
        .pitch = creq.pitch, .bpp = creq.bpp, .depth = 24,
        .handle = creq.handle
    };
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbreq) < 0) return -1;

    struct drm_mode_map_dumb_u mreq = { .handle = creq.handle };
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) return -1;

    fb_pixels = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (fb_pixels == MAP_FAILED) { fb_pixels = NULL; return -1; }

    g_drm_fd = fd;
    g_fb_id = fbreq.fb_id;
    g_conn_id = connector_id;
    memset(&g_crtc_cfg, 0, sizeof(g_crtc_cfg));
    g_crtc_cfg.crtc_id = res.count_crtcs > 0 ? crtc_ids[0] : 0;
    g_crtc_cfg.fb_id = fbreq.fb_id;
    g_crtc_cfg.set_connectors_ptr = (uint64_t)(uintptr_t)&g_conn_id;
    g_crtc_cfg.count_connectors = 1;
    g_crtc_cfg.mode = mode;
    g_crtc_cfg.mode_valid = 1;

    fill_rect(0, 0, fb_width, fb_height, 0xFF0E141B);
    fill_rect(40, 100, fb_width - 80, 8, 0xFFE95420); /* Ubuntu orange */
    draw_text(48, 130, "UBUNTU TOUCH - PIXEL 7", 0xFFFFFFFF, 4);
    draw_text(48, 185, "Stage 2: Userspace Bring-Up", 0xFFE95420, 4);
    fill_rect(40, 240, fb_width - 80, 4, 0xFF2B3948);

    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &g_crtc_cfg) < 0) return -1;
    fb_flush();
    return fd;
}

/* ---- Map logical partition (e.g. system_a/b) from /dev/block/by-name/super ---- */
static int map_super_partition(const char *part_name, const char *dm_dev_path) {
    const char *super_path = "/dev/block/by-name/super";
    struct stat st;
    if (stat(super_path, &st) != 0) {
        ui_log("[dbg] stat(/dev/block/by-name/super) failed", 0xFFFACC15);
        return -1;
    }
    unsigned int super_maj = major(st.st_rdev);
    unsigned int super_min = minor(st.st_rdev);

    int sfd = open(super_path, O_RDONLY | O_CLOEXEC);
    if (sfd < 0) {
        ui_log("[dbg] open(super) failed", 0xFFFACC15);
        return -1;
    }

    struct lp_geometry geom;
    if (pread(sfd, &geom, sizeof(geom), 4096) != sizeof(geom) || geom.magic != LP_METADATA_GEOMETRY_MAGIC) {
        ui_log("[dbg] bad lp_geometry magic at 4096", 0xFFFACC15);
        close(sfd);
        return -1;
    }

    /* Primary LpMetadataHeader starts after 4096 reserved + 4096 primary geom + 4096 backup geom = 12288 */
    uint64_t hdr_off = 12288;
    struct lp_header hdr;
    int hdr_ok = 0;
    for (uint32_t slot = 0; slot < geom.metadata_slot_count && slot < 4; slot++) {
        uint64_t off = 12288ULL + (uint64_t)slot * geom.metadata_max_size;
        if (pread(sfd, &hdr, sizeof(hdr), off) == sizeof(hdr) && hdr.magic == LP_METADATA_HEADER_MAGIC) {
            hdr_off = off;
            hdr_ok = 1;
            /* Check if part_name has extents in this slot */
            uint8_t *tmp_tbl = malloc(hdr.tables_size);
            if (tmp_tbl) {
                if (pread(sfd, tmp_tbl, hdr.tables_size, hdr_off + hdr.header_size) == (ssize_t)hdr.tables_size) {
                    for (uint32_t i = 0; i < hdr.partitions.num_entries; i++) {
                        struct lp_partition *p = (struct lp_partition *)(tmp_tbl + hdr.partitions.offset + i * hdr.partitions.entry_size);
                        if (strncmp(p->name, part_name, 36) == 0 && p->num_extents > 0) {
                            free(tmp_tbl);
                            goto found_hdr;
                        }
                    }
                }
                free(tmp_tbl);
            }
        }
    }
    if (!hdr_ok) {
        ui_log("[dbg] bad lp_header magic at 12288", 0xFFFACC15);
        close(sfd);
        return -1;
    }

found_hdr:;
    uint8_t *tables = malloc(hdr.tables_size);
    if (!tables) { close(sfd); return -1; }
    if (pread(sfd, tables, hdr.tables_size, hdr_off + hdr.header_size) != (ssize_t)hdr.tables_size) {
        free(tables); close(sfd); return -1;
    }
    close(sfd);

    int found_idx = -1;
    for (uint32_t i = 0; i < hdr.partitions.num_entries; i++) {
        struct lp_partition *p = (struct lp_partition *)(tables + hdr.partitions.offset + i * hdr.partitions.entry_size);
        if (strncmp(p->name, part_name, 36) == 0) {
            found_idx = (int)i;
            break;
        }
    }
    if (found_idx < 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "[dbg] %s not in lp_header", part_name);
        ui_log(msg, 0xFFFACC15);
        free(tables); return -1;
    }

    struct lp_partition *part = (struct lp_partition *)(tables + hdr.partitions.offset + found_idx * hdr.partitions.entry_size);
    if (part->num_extents == 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "[dbg] %s has 0 extents", part_name);
        ui_log(msg, 0xFFFACC15);
        free(tables); return -1;
    }

    mkdir("/dev/mapper", 0755);
    mknod("/dev/mapper/control", S_IFCHR | 0600, makedev(10, 236));

    int dmfd = open("/dev/mapper/control", O_RDWR | O_CLOEXEC);
    if (dmfd < 0) { free(tables); return -1; }

    uint8_t buf[16384];
    memset(buf, 0, sizeof(buf));
    struct dm_ioctl_u *dmi = (struct dm_ioctl_u *)buf;
    dmi->version[0] = DM_VERSION_MAJOR;
    dmi->data_size = sizeof(buf);
    dmi->data_start = sizeof(struct dm_ioctl_u);
    snprintf(dmi->name, DM_NAME_LEN, "%s", part_name);
    if (ioctl(dmfd, DM_DEV_CREATE, dmi) < 0) {
        close(dmfd); free(tables); return -1;
    }

    memset(buf, 0, sizeof(buf));
    dmi->version[0] = DM_VERSION_MAJOR;
    dmi->data_size = sizeof(buf);
    dmi->data_start = sizeof(struct dm_ioctl_u);
    dmi->target_count = part->num_extents;
    snprintf(dmi->name, DM_NAME_LEN, "%s", part_name);

    uint32_t offset = sizeof(struct dm_ioctl_u);
    uint64_t logical_sector = 0;
    for (uint32_t e = 0; e < part->num_extents; e++) {
        struct lp_extent *ext = (struct lp_extent *)(tables + hdr.extents.offset + (part->first_extent_index + e) * hdr.extents.entry_size);
        if (ext->target_type != LP_TARGET_TYPE_LINEAR) {
            close(dmfd); free(tables); return -1;
        }
        struct dm_target_spec_u *ts = (struct dm_target_spec_u *)(buf + offset);
        ts->sector_start = logical_sector;
        ts->length = ext->num_sectors;
        snprintf(ts->target_type, 16, "linear");
        char *params = (char *)(buf + offset + sizeof(struct dm_target_spec_u));
        int plen = snprintf(params, 128, "%u:%u %llu", super_maj, super_min, (unsigned long long)ext->target_data);
        uint32_t step = sizeof(struct dm_target_spec_u) + plen + 1;
        step = (step + 7) & ~7u;
        ts->next = step;
        offset += step;
        logical_sector += ext->num_sectors;
    }
    dmi->data_size = offset;
    if (ioctl(dmfd, DM_TABLE_LOAD, dmi) < 0) {
        close(dmfd); free(tables); return -1;
    }

    memset(buf, 0, sizeof(buf));
    dmi->version[0] = DM_VERSION_MAJOR;
    dmi->data_size = sizeof(buf);
    dmi->data_start = sizeof(struct dm_ioctl_u);
    snprintf(dmi->name, DM_NAME_LEN, "%s", part_name);
    if (ioctl(dmfd, DM_DEV_SUSPEND, dmi) < 0) {
        close(dmfd); free(tables); return -1;
    }
    unsigned int dm_minor = minor(dmi->dev);
    unsigned int dm_major = major(dmi->dev);
    mknod(dm_dev_path, S_IFBLK | 0600, makedev(dm_major, dm_minor));

    close(dmfd);
    free(tables);
    return 0;
}

/* ---- Populate /dev/block and /dev/block/by-name from /sys/class/block ---- */
static void create_block_nodes_from_sysfs(void) {
    mkdir("/dev/block", 0755);
    mkdir("/dev/block/by-name", 0755);
    for (int i = 0; i < 40; i++) {
        if (access("/sys/class/block/sda/dev", F_OK) == 0) break;
        usleep(100000);
    }
    DIR *d = opendir("/sys/class/block");
    if (!d) {
        ui_log("[dbg] opendir(/sys/class/block) failed", 0xFFF87171);
        return;
    }
    int blk_cnt = 0, part_cnt = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char dev_path[256], node_path[256], uev_path[256], line[256];
        snprintf(dev_path, sizeof(dev_path), "/sys/class/block/%s/dev", ent->d_name);
        FILE *f = fopen(dev_path, "r");
        if (!f) continue;
        unsigned int maj = 0, min = 0;
        if (fscanf(f, "%u:%u", &maj, &min) == 2) {
            snprintf(node_path, sizeof(node_path), "/dev/block/%s", ent->d_name);
            mknod(node_path, S_IFBLK | 0600, makedev(maj, min));
            blk_cnt++;
        }
        fclose(f);

        snprintf(uev_path, sizeof(uev_path), "/sys/class/block/%s/uevent", ent->d_name);
        f = fopen(uev_path, "r");
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "PARTNAME=", 9) == 0) {
                    line[strcspn(line, "\r\n")] = 0;
                    if (line[9]) {
                        char link_path[256];
                        snprintf(link_path, sizeof(link_path), "/dev/block/by-name/%s", line + 9);
                        symlink(node_path, link_path);
                        part_cnt++;
                    }
                    break;
                }
            }
            fclose(f);
        }
    }
    closedir(d);
    char msg[96];
    snprintf(msg, sizeof(msg), "[dbg] block=%d by-name=%d sda=%d",
             blk_cnt, part_cnt, access("/dev/block/sda", F_OK) == 0 ? 1 : 0);
    ui_log(msg, 0xFF94A3B8);

    if (access("/dev/block/by-name/super", F_OK) != 0) {
        DIR *bd = opendir("/sys/class/block");
        if (bd) {
            char list[128] = "[blk]";
            int n = 0;
            while ((ent = readdir(bd)) != NULL && n < 8) {
                if (ent->d_name[0] == '.') continue;
                strncat(list, " ", sizeof(list) - strlen(list) - 1);
                strncat(list, ent->d_name, sizeof(list) - strlen(list) - 1);
                n++;
            }
            closedir(bd);
            ui_log(list, 0xFFFACC15);
        }
        DIR *md = opendir("/lib/modules");
        if (md) {
            char mlist[128] = "[mod]";
            int n = 0;
            while ((ent = readdir(md)) != NULL && n < 5) {
                if (strstr(ent->d_name, "ufs") || strstr(ent->d_name, "scsi") ||
                    strstr(ent->d_name, "phy") || strstr(ent->d_name, "key")) {
                    strncat(mlist, " ", sizeof(mlist) - strlen(mlist) - 1);
                    strncat(mlist, ent->d_name, sizeof(mlist) - strlen(mlist) - 1);
                    n++;
                }
            }
            closedir(md);
            ui_log(mlist, 0xFFFACC15);
        }
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    mkdir("/dev", 0755);
    mount("tmpfs", "/dev", "tmpfs", 0, "mode=0755");
    mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
    mknod("/dev/kmsg", S_IFCHR | 0600, makedev(1, 11));
    mknod("/dev/console", S_IFCHR | 0600, makedev(5, 1));
    mknod("/dev/urandom", S_IFCHR | 0444, makedev(1, 9));
    mkdir("/dev/dri", 0755);
    mknod("/dev/dri/card0", S_IFCHR | 0666, makedev(226, 0));

    mkdir("/proc", 0755);
    mount("proc", "/proc", "proc", 0, NULL);
    mkdir("/sys", 0755);
    mount("sysfs", "/sys", "sysfs", 0, NULL);

    klog("<5>UT-STAGE1: Early init started on Pixel 7 (panther)\n");

    if (load_display_modules() == 0) {
        klog("<5>UT-STAGE1: Display + IOMMU modules loaded via modprobe\n");
    } else {
        klog("<4>UT-STAGE1: modprobe returned non-zero\n");
    }

    int drm_fd = init_drm_display();
    ui_log("[OK] Exynos DRM/KMS + IOMMU active (1080x2400)", 0xFF4ADE80);

    if (load_storage_and_key_modules() == 0) {
        ui_log("[OK] UFS + key modules loaded", 0xFF4ADE80);
    } else {
        ui_log("[!!] UFS modprobe returned non-zero", 0xFFFACC15);
    }

    create_block_nodes_from_sysfs();
    ui_log("[..] Searching for Ubuntu Touch rootfs...", 0xFFFACC15);

    mkdir("/newroot", 0755);
    mkdir("/mnt_ud", 0755);
    int mounted = 0;
    int ud_mounted = 0;

    /* Check if userdata is unencrypted (ext4 or f2fs) */
    if (access("/dev/block/by-name/userdata", F_OK) == 0) {
        if (mount("/dev/block/by-name/userdata", "/mnt_ud", "ext4", MS_NOATIME, NULL) == 0 ||
            mount("/dev/block/by-name/userdata", "/mnt_ud", "f2fs", MS_NOATIME, NULL) == 0) {
            ud_mounted = 1;
            ui_log("[OK] Unencrypted userdata detected & mounted", 0xFF4ADE80);
        } else {
            ui_log("[--] userdata encrypted/unreadable (normal on Android)", 0xFF94A3B8);
        }
    }

    /* Determine active slot suffix from /proc/cmdline (default _a then _b) */
    const char *primary_sys = "system_a";
    const char *secondary_sys = "system_b";
    {
        int cmdfd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
        if (cmdfd >= 0) {
            char cmdline[2048];
            ssize_t n = read(cmdfd, cmdline, sizeof(cmdline) - 1);
            close(cmdfd);
            if (n > 0) {
                cmdline[n] = 0;
                if (strstr(cmdline, "androidboot.slot_suffix=_b") != NULL) {
                    primary_sys = "system_b";
                    secondary_sys = "system_a";
                }
            }
        }
    }

    /* Variant 1 (super logical partition): ext4 or erofs + overlayfs */
    const char *sys_candidates[3] = { primary_sys, secondary_sys, NULL };
    for (int s = 0; sys_candidates[s] && !mounted; s++) {
        if (map_super_partition(sys_candidates[s], "/dev/dm-ut-root") == 0) {
            if (mount("/dev/dm-ut-root", "/newroot", "ext4", MS_NOATIME, NULL) == 0) {
                char msg[96];
                snprintf(msg, sizeof(msg), "[OK] Mounted %s (ext4) from super", sys_candidates[s]);
                ui_log(msg, 0xFF4ADE80);
                mounted = 1;
            } else {
                mkdir("/ro_root", 0755);
                if (mount("/dev/dm-ut-root", "/ro_root", "erofs", MS_RDONLY, NULL) == 0) {
                    mkdir("/rw_ov", 0755);
                    if (ud_mounted) {
                        mkdir("/mnt_ud/ut_ov", 0755);
                        mkdir("/mnt_ud/ut_ov/upper", 0755);
                        mkdir("/mnt_ud/ut_ov/work", 0755);
                        if (mount("overlay", "/newroot", "overlay", 0,
                                  "lowerdir=/ro_root,upperdir=/mnt_ud/ut_ov/upper,workdir=/mnt_ud/ut_ov/work") == 0) {
                            char msg[96];
                            snprintf(msg, sizeof(msg), "[OK] %s (erofs + userdata overlay)", sys_candidates[s]);
                            ui_log(msg, 0xFF4ADE80);
                            mounted = 1;
                        }
                    }
                    if (!mounted) {
                        mount("tmpfs", "/rw_ov", "tmpfs", 0, "mode=0755");
                        mkdir("/rw_ov/upper", 0755);
                        mkdir("/rw_ov/work", 0755);
                        if (mount("overlay", "/newroot", "overlay", 0,
                                  "lowerdir=/ro_root,upperdir=/rw_ov/upper,workdir=/rw_ov/work") == 0) {
                            char msg[96];
                            snprintf(msg, sizeof(msg), "[OK] %s (erofs + tmpfs overlay)", sys_candidates[s]);
                            ui_log(msg, 0xFF4ADE80);
                            mounted = 1;
                        } else {
                            ui_log("[!!] erofs OK, but overlayfs mount failed", 0xFFF87171);
                        }
                    }
                } else {
                    char msg[96];
                    snprintf(msg, sizeof(msg), "[!!] %s mapped, ext4/erofs mount failed", sys_candidates[s]);
                    ui_log(msg, 0xFFF87171);
                }
            }
        }
    }
    if (!mounted) {
        ui_log("[--] No rootfs in super; trying userdata fallback", 0xFF94A3B8);
    }

    /* Variant 2 (Full userdata): loop image or direct rootfs on unencrypted userdata */
    if (!mounted && ud_mounted) {
        const char *img_candidates[] = {
            "/mnt_ud/ubuntu.img",
            "/mnt_ud/data/ubuntu.img",
            "/mnt_ud/ut-rootfs.img",
            NULL
        };
        for (int i = 0; img_candidates[i]; i++) {
            int img_fd = open(img_candidates[i], O_RDWR | O_CLOEXEC);
            if (img_fd >= 0) {
                mknod("/dev/loop0", S_IFBLK | 0600, makedev(7, 0));
                int loop_fd = open("/dev/loop0", O_RDWR | O_CLOEXEC);
                if (loop_fd >= 0 && ioctl(loop_fd, 0x4C00 /* LOOP_SET_FD */, img_fd) == 0) {
                    if (mount("/dev/loop0", "/newroot", "ext4", MS_NOATIME, NULL) == 0) {
                        ui_log("[OK] Mounted ubuntu.img via loop0 from userdata", 0xFF4ADE80);
                        mounted = 1;
                    }
                    close(loop_fd);
                }
                close(img_fd);
                if (mounted) break;
            }
        }
        if (!mounted && (access("/mnt_ud/sbin/init", X_OK) == 0 || access("/mnt_ud/lib/systemd/systemd", X_OK) == 0)) {
            if (mount("/mnt_ud", "/newroot", NULL, MS_MOVE, NULL) == 0) {
                ui_log("[OK] Mounted userdata directly as rootfs", 0xFF4ADE80);
                mounted = 1;
                ud_mounted = 0; /* already moved as /newroot */
            }
        }
    }

    if (!mounted) {
        ui_log("[ERR] No Ubuntu Touch rootfs mounted.", 0xFFF87171);
        ui_log("Variant 1 (safe): fastboot flash system_a ut-rootfs-system.img", 0xFF38BDF8);
        ui_log("Variant 2 (userdata): unencrypted /ubuntu.img on userdata", 0xFF38BDF8);
        wait_key_power_or_fastboot();
    }

    /* If rootfs is on system_b and userdata is unencrypted, expose it at /userdata */
    if (ud_mounted) {
        mkdir("/newroot/userdata", 0755);
        mount("/mnt_ud", "/newroot/userdata", NULL, MS_MOVE, NULL);
    }

    /* Verify /sbin/init exists in rootfs */
    const char *target_init = NULL;
    if (access("/newroot/sbin/init", X_OK) == 0) {
        target_init = "/sbin/init";
    } else if (access("/newroot/lib/systemd/systemd", X_OK) == 0) {
        target_init = "/lib/systemd/systemd";
    }

    if (!target_init) {
        ui_log("[ERR] Rootfs mounted, but /sbin/init missing!", 0xFFF87171);
        wait_key_power_or_fastboot();
    }

    /* Log available USB/UDC modules and UDC state before handoff */
    {
        DIR *md = opendir("/lib/modules");
        if (md) {
            char ulist[256] = "[usb-ko]";
            struct dirent *ent;
            while ((ent = readdir(md)) != NULL) {
                if (strstr(ent->d_name, "usb") || strstr(ent->d_name, "dwc") ||
                    strstr(ent->d_name, "udc") || strstr(ent->d_name, "rndis") ||
                    strstr(ent->d_name, "gadget") || strstr(ent->d_name, "composite")) {
                    if (strlen(ulist) + strlen(ent->d_name) + 2 < sizeof(ulist)) {
                        strcat(ulist, " ");
                        strcat(ulist, ent->d_name);
                    }
                }
            }
            closedir(md);
            ui_log(ulist, 0xFF94A3B8);
        }
        DIR *md2 = opendir("/lib/modules");
        if (md2) {
            char tlist[256] = "[typec-ko]";
            struct dirent *ent;
            while ((ent = readdir(md2)) != NULL) {
                if (strstr(ent->d_name, "tcpc") || strstr(ent->d_name, "typec") ||
                    strstr(ent->d_name, "max77759") || strstr(ent->d_name, "extcon")) {
                    if (strlen(tlist) + strlen(ent->d_name) + 2 < sizeof(tlist)) {
                        strcat(tlist, " ");
                        strcat(tlist, ent->d_name);
                    }
                }
            }
            closedir(md2);
            ui_log(tlist, 0xFF94A3B8);
        }
        DIR *ud = opendir("/sys/class/udc");
        if (ud) {
            char udc_list[128] = "[udc]";
            struct dirent *ent;
            while ((ent = readdir(ud)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                strcat(udc_list, " ");
                strncat(udc_list, ent->d_name, sizeof(udc_list) - strlen(udc_list) - 2);
            }
            closedir(ud);
            ui_log(udc_list, 0xFF38BDF8);
        }
    }

    int pre_kmsg_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (pre_kmsg_fd >= 0) lseek(pre_kmsg_fd, 0, SEEK_END);

    if (load_usb_udc_modules() == 0) {
        ui_log("[OK] USB PHY + DWC3 + TCPC modules loaded", 0xFF4ADE80);
    } else {
        ui_log("[!!] USB UDC modprobe returned non-zero", 0xFFFACC15);
    }
    for (int w = 0; w < 50; w++) {
        DIR *ud = opendir("/sys/class/udc");
        int found = 0;
        if (ud) {
            struct dirent *ent;
            while ((ent = readdir(ud)) != NULL) {
                if (ent->d_name[0] != '.') { found = 1; break; }
            }
            closedir(ud);
        }
        if (found) break;
        usleep(100000);
    }

    g_kmsg_scale = 1;
    debug_usb_deferred_and_kmsg(pre_kmsg_fd);
    if (pre_kmsg_fd >= 0) close(pre_kmsg_fd);
    probe_usb_gadget_functions_only();

    int usb_tty_fd = try_setup_usb_serial_gadget();
    if (usb_tty_fd >= 0) {
        ui_log("[OK] USB ACM Serial Log (/dev/ttyGS0) active!", 0xFF4ADE80);
    } else {
        ui_log("[--] USB ACM Serial not available", 0xFFFACC15);
    }

    configure_newroot_ssh_and_usb();
    ui_log("[OK] Found /sbin/init -> starting kmsg monitor & systemd", 0xFF4ADE80);
    (void)drm_fd;

    int kmsg_rd_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (kmsg_rd_fd >= 0) {
        lseek(kmsg_rd_fd, 0, SEEK_END);
    }
    int input_fds[16];
    int nfds = open_input_devices(input_fds, 16);

    pid_t mon_pid = start_kmsg_console_monitor(kmsg_rd_fd, input_fds, nfds, usb_tty_fd);
    if (kmsg_rd_fd >= 0) close(kmsg_rd_fd);
    if (usb_tty_fd >= 0) close(usb_tty_fd);
    for (int i = 0; i < nfds; i++) close(input_fds[i]);
    (void)mon_pid;

    /* Move virtual filesystems into /newroot */
    mkdir("/newroot/dev", 0755);
    mkdir("/newroot/proc", 0755);
    mkdir("/newroot/sys", 0755);
    mkdir("/newroot/run", 0755);
    if (mount("/dev", "/newroot/dev", NULL, MS_MOVE, NULL) != 0) {
        char ebuf[128];
        snprintf(ebuf, sizeof(ebuf), "<3>UT-STAGE1: mount --move /dev failed: %s\n", strerror(errno));
        klog(ebuf);
    }
    if (mount("/proc", "/newroot/proc", NULL, MS_MOVE, NULL) != 0) {
        char ebuf[128];
        snprintf(ebuf, sizeof(ebuf), "<3>UT-STAGE1: mount --move /proc failed: %s\n", strerror(errno));
        klog(ebuf);
    }
    if (mount("/sys", "/newroot/sys", NULL, MS_MOVE, NULL) != 0) {
        char ebuf[128];
        snprintf(ebuf, sizeof(ebuf), "<3>UT-STAGE1: mount --move /sys failed: %s\n", strerror(errno));
        klog(ebuf);
    }
    mount("tmpfs", "/newroot/run", "tmpfs", 0, "mode=0755");

    if (chdir("/newroot") != 0) {
        char ebuf[128];
        snprintf(ebuf, sizeof(ebuf), "<3>UT-STAGE1: chdir(/newroot) failed: %s\n", strerror(errno));
        klog(ebuf);
        while (1) sleep(5);
    }
    if (mount(".", "/", NULL, MS_MOVE, NULL) != 0) {
        char ebuf[128];
        snprintf(ebuf, sizeof(ebuf), "<3>UT-STAGE1: mount(., /, MS_MOVE) failed: %s\n", strerror(errno));
        klog(ebuf);
        while (1) sleep(5);
    }
    if (chroot(".") != 0) {
        char ebuf[128];
        snprintf(ebuf, sizeof(ebuf), "<3>UT-STAGE1: chroot(.) failed: %s\n", strerror(errno));
        klog(ebuf);
        while (1) sleep(5);
    }
    chdir("/");

    int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null_fd >= 0) {
        dup2(null_fd, 0);
        if (null_fd > 2) close(null_fd);
    }
    int cons = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (cons >= 0) {
        dup2(cons, 1);
        dup2(cons, 2);
        if (cons > 2) close(cons);
    }

    klog("<5>UT-STAGE1: execve target_init now...\n");

    char *init_argv[] = {
        (char *)target_init,
        "systemd.log_target=kmsg",
        "systemd.log_level=info",
        NULL
    };
    char *init_envp[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        NULL
    };
    execve(target_init, init_argv, init_envp);

    {
        char ebuf[128];
        snprintf(ebuf, sizeof(ebuf), "<3>UT-STAGE1: execve(%s) failed: %s\n", target_init, strerror(errno));
        klog(ebuf);
    }
    while (1) sleep(5);
    return 0;
}
