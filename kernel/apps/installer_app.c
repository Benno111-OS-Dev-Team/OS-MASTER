#include "apps/kapi.h"

#define INSTALLER_BG 0x08111D
#define INSTALLER_PANEL 0x10233D
#define INSTALLER_ACCENT 0x4DA2FF
#define INSTALLER_TEXT 0xF8FAFC
#define INSTALLER_MUTED 0x93A4B8
#define INSTALLER_OK 0x2EB67D
#define INSTALLER_WARN 0xF59E0B

#define INSTALLER_KEY_ENTER 13
#define INSTALLER_KEY_NEWLINE 10
#define INSTALLER_COPY_CHUNK 32768

typedef struct installer_disk_entry {
    int disk_index;
    os8_disk_info_t info;
    int partition_count;
} installer_disk_entry_t;

typedef struct installer_progress_state {
    char status[160];
    char detail[160];
    int progress_done;
    int progress_total;
    int complete;
} installer_progress_state_t;

static int installer_text_len(const char *text) {
    int len = 0;

    if (!text)
        return 0;
    while (text[len])
        len++;
    return len;
}

static void installer_copy_text(char *dst, int max, const char *src) {
    int idx = 0;

    if (!dst || max <= 0)
        return;
    if (!src)
        src = "";

    while (src[idx] && idx < max - 1) {
        dst[idx] = src[idx];
        idx++;
    }
    dst[idx] = '\0';
}

static void installer_append_text(char *dst, int max, const char *src) {
    int idx = 0;

    if (!dst || max <= 0 || !src)
        return;
    while (dst[idx] && idx < max - 1)
        idx++;
    while (*src && idx < max - 1)
        dst[idx++] = *src++;
    dst[idx] = '\0';
}

static void installer_append_decimal(char *dst, int max, int value) {
    char tmp[16];
    int ti = 0;

    if (value == 0) {
        installer_append_text(dst, max, "0");
        return;
    }
    if (value < 0) {
        installer_append_text(dst, max, "-");
        value = -value;
    }
    while (value > 0 && ti < (int)sizeof(tmp)) {
        tmp[ti++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (ti > 0) {
        char c[2];

        c[0] = tmp[--ti];
        c[1] = '\0';
        installer_append_text(dst, max, c);
    }
}

static void installer_append_mib(char *dst, int max, uint32_t value_mib) {
    uint32_t gib_whole;
    uint32_t gib_tenths;

    if (value_mib >= 1024U) {
        gib_whole = value_mib / 1024U;
        gib_tenths = ((value_mib % 1024U) * 10U) / 1024U;
        installer_append_decimal(dst, max, (int)gib_whole);
        installer_append_text(dst, max, ".");
        installer_append_decimal(dst, max, (int)gib_tenths);
        installer_append_text(dst, max, " GiB");
        return;
    }

    installer_append_decimal(dst, max, (int)value_mib);
    installer_append_text(dst, max, " MiB");
}

static int installer_text_equal(const char *a, const char *b) {
    int idx = 0;

    if (!a || !b)
        return 0;
    while (a[idx] && b[idx]) {
        if (a[idx] != b[idx])
            return 0;
        idx++;
    }
    return a[idx] == '\0' && b[idx] == '\0';
}

static int installer_path_join(char *dst, int max, const char *left,
                               const char *right) {
    int idx = 0;

    if (!dst || max <= 0 || !left || !right)
        return -1;
    dst[0] = '\0';

    while (*left && idx < max - 1)
        dst[idx++] = *left++;
    if (idx > 0 && dst[idx - 1] != '/' && idx < max - 1)
        dst[idx++] = '/';
    while (*right == '/')
        right++;
    while (*right && idx < max - 1)
        dst[idx++] = *right++;
    dst[idx] = '\0';
    return *right ? -1 : 0;
}

static int installer_collect_disks(kapi_t *api, installer_disk_entry_t *entries,
                                   int max_entries) {
    int total_disks;
    int count = 0;

    if (!api || !entries || max_entries <= 0 || !api->disk_count ||
        !api->disk_info) {
        return 0;
    }

    total_disks = api->disk_count();
    for (int disk_index = 0; disk_index < total_disks && count < max_entries;
         disk_index++) {
        installer_disk_entry_t entry;

        if (api->disk_info(disk_index, &entry.info) != 0)
            continue;
        if (!entry.info.writable)
            continue;

        entry.disk_index = disk_index;
        entry.partition_count =
            api->partition_count ? api->partition_count(disk_index) : 0;
        entries[count++] = entry;
    }

    return count;
}

static void installer_build_disk_label(const installer_disk_entry_t *entry,
                                       char *buf, int max) {
    if (!buf || max <= 0)
        return;

    buf[0] = '\0';
    if (!entry) {
        installer_append_text(buf, max, "Unknown disk");
        return;
    }

    installer_append_text(buf, max, entry->info.location[0]
                                        ? entry->info.location
                                        : "Unnamed disk");
    installer_append_text(buf, max, "  ");
    installer_append_mib(buf, max, entry->info.capacity_mib);
    installer_append_text(buf, max,
                          entry->info.writable ? "  writable" : "  read-only");
    installer_append_text(buf, max, "  ");
    installer_append_decimal(buf, max, entry->partition_count);
    installer_append_text(buf, max,
                          entry->partition_count == 1 ? " partition"
                                                      : " partitions");
}

static void installer_fill(kapi_t *api, uint32_t color) {
    if (!api || !api->fb_fill_rect || !api->fb_width || !api->fb_height)
        return;
    api->fb_fill_rect(0, 0, api->fb_width, api->fb_height, color);
}

static void installer_draw_text(kapi_t *api, int x, int y, const char *text,
                                uint32_t fg, uint32_t bg) {
    if (!api || !api->fb_draw_string || !text)
        return;
    api->fb_draw_string((uint32_t)x, (uint32_t)y, text, fg, bg);
}

static void installer_draw_centered(kapi_t *api, int y, const char *text,
                                    uint32_t fg, uint32_t bg) {
    int text_w;
    int x;

    if (!api || !text)
        return;
    text_w = installer_text_len(text) * 8;
    x = ((int)api->fb_width - text_w) / 2;
    if (x < 20)
        x = 20;
    installer_draw_text(api, x, y, text, fg, bg);
}

static void installer_draw_progress(kapi_t *api, int percent) {
    int bar_x;
    int bar_y;
    int bar_w;
    int bar_h;
    int fill_w;

    if (!api || !api->fb_fill_rect)
        return;

    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    bar_w = (int)api->fb_width - 160;
    if (bar_w < 240)
        bar_w = (int)api->fb_width - 40;
    if (bar_w < 120)
        bar_w = 120;
    bar_h = 24;
    bar_x = ((int)api->fb_width - bar_w) / 2;
    bar_y = (int)api->fb_height - 120;
    fill_w = (bar_w * percent) / 100;

    api->fb_fill_rect((uint32_t)bar_x, (uint32_t)bar_y, (uint32_t)bar_w,
                      (uint32_t)bar_h, INSTALLER_PANEL);
    if (fill_w > 0) {
        api->fb_fill_rect((uint32_t)bar_x, (uint32_t)bar_y, (uint32_t)fill_w,
                          (uint32_t)bar_h, INSTALLER_ACCENT);
    }
}

static void installer_render_selection(kapi_t *api, int selected,
                                       int disk_count,
                                       installer_disk_entry_t *entries,
                                       const char *status) {
    int panel_x = 56;
    int panel_y = 44;
    int panel_w = (int)api->fb_width - 112;
    int panel_h = (int)api->fb_height - 88;

    installer_fill(api, INSTALLER_BG);
    if (panel_w > 0 && panel_h > 0)
        api->fb_fill_rect((uint32_t)panel_x, (uint32_t)panel_y,
                          (uint32_t)panel_w, (uint32_t)panel_h,
                          INSTALLER_PANEL);

    installer_draw_centered(api, panel_y + 26, "OS8 Installer App",
                            INSTALLER_TEXT, INSTALLER_PANEL);
    installer_draw_centered(api, panel_y + 54,
                            "Select a target disk and press Enter to install",
                            INSTALLER_MUTED, INSTALLER_PANEL);

    for (int i = 0; i < disk_count && i < 8; i++) {
        int row_y = panel_y + 108 + i * 36;
        char line[128];
        char label[96];

        line[0] = '\0';
        installer_build_disk_label(&entries[i], label, sizeof(label));
        installer_append_text(line, sizeof(line), (i == selected) ? "> " : "  ");
        installer_append_decimal(line, sizeof(line), i + 1);
        installer_append_text(line, sizeof(line), ". ");
        installer_append_text(line, sizeof(line), label);
        installer_draw_text(api, panel_x + 28, row_y, line,
                            i == selected ? INSTALLER_ACCENT : INSTALLER_TEXT,
                            INSTALLER_PANEL);
    }

    installer_draw_text(api, panel_x + 28, panel_y + panel_h - 88,
                        "Controls: W/S move  Enter installs  R reboots",
                        INSTALLER_MUTED, INSTALLER_PANEL);
    installer_draw_text(api, panel_x + 28, panel_y + panel_h - 52,
                        status ? status : "", INSTALLER_WARN, INSTALLER_PANEL);
}

static void installer_render_progress(kapi_t *api, const char *status,
                                      const char *detail, int progress_done,
                                      int progress_total, int complete) {
    int percent = 0;
    char percent_buf[32];

    installer_fill(api, INSTALLER_BG);
    installer_draw_centered(api, 72,
                            complete ? "Installation Complete"
                                     : "Installing OS8",
                            complete ? INSTALLER_OK : INSTALLER_TEXT,
                            INSTALLER_BG);

    if (progress_total > 0)
        percent = (progress_done * 100) / progress_total;

    installer_draw_centered(api, 132, status ? status : "",
                            INSTALLER_ACCENT, INSTALLER_BG);
    installer_draw_centered(api, 164, detail ? detail : "",
                            INSTALLER_MUTED, INSTALLER_BG);
    installer_draw_progress(api, percent);

    percent_buf[0] = '\0';
    installer_append_decimal(percent_buf, sizeof(percent_buf), percent);
    installer_append_text(percent_buf, sizeof(percent_buf), "%");
    installer_draw_centered(api, (int)api->fb_height - 152, percent_buf,
                            INSTALLER_TEXT, INSTALLER_BG);

    if (complete) {
        installer_draw_centered(api, (int)api->fb_height - 72,
                                "Press R to reboot into the installed system",
                                INSTALLER_OK, INSTALLER_BG);
    }
}

static void installer_progress_render(kapi_t *api,
                                      installer_progress_state_t *progress) {
    installer_render_progress(api, progress->status, progress->detail,
                              progress->progress_done, progress->progress_total,
                              progress->complete);
    if (api->sleep_ms)
        api->sleep_ms(16);
    else if (api->yield)
        api->yield();
}

static void installer_progress_set(installer_progress_state_t *progress,
                                   const char *status, const char *detail,
                                   int progress_done, int progress_total,
                                   int complete) {
    if (!progress)
        return;
    installer_copy_text(progress->status, sizeof(progress->status), status);
    installer_copy_text(progress->detail, sizeof(progress->detail), detail);
    progress->progress_done = progress_done;
    progress->progress_total = progress_total > 0 ? progress_total : 1;
    progress->complete = complete;
}

static int installer_path_is_dir(kapi_t *api, const char *path) {
    void *handle;
    int is_dir = 0;

    if (!api || !api->open || !api->close || !api->is_dir || !path)
        return 0;

    handle = api->open(path);
    if (!handle)
        return 0;
    is_dir = api->is_dir(handle);
    api->close(handle);
    return is_dir;
}

static int installer_path_exists(kapi_t *api, const char *path) {
    void *handle;

    if (!api || !api->open || !api->close || !path)
        return 0;
    handle = api->open(path);
    if (!handle)
        return 0;
    api->close(handle);
    return 1;
}

static int installer_ensure_dir(kapi_t *api, const char *path) {
    void *handle;

    if (!api || !path || !path[0])
        return -1;
    handle = api->open(path);
    if (handle) {
        int ok = api->is_dir ? api->is_dir(handle) : 0;
        api->close(handle);
        return ok ? 0 : -1;
    }
    if (!api->mkdir_fn)
        return -1;
    handle = api->mkdir_fn(path);
    if (!handle)
        return -1;
    api->close(handle);
    return 0;
}

static int installer_ensure_parent_dirs(kapi_t *api, const char *path) {
    char partial[256];
    int idx = 0;

    if (!api || !path)
        return -1;
    partial[0] = '\0';

    for (int i = 0; path[i] && idx < (int)sizeof(partial) - 1; i++) {
        partial[idx++] = path[i];
        partial[idx] = '\0';
        if (i > 0 && path[i] == '/' && path[i + 1] != '\0') {
            if (installer_ensure_dir(api, partial) != 0)
                return -1;
        }
    }

    return 0;
}

static int installer_count_tree_files(kapi_t *api, const char *path) {
    void *handle;
    int total = 0;

    if (!api || !path || !api->open || !api->close)
        return 0;

    handle = api->open(path);
    if (!handle)
        return 0;
    if (!api->is_dir || !api->is_dir(handle)) {
        api->close(handle);
        return 1;
    }

    for (int index = 0;; index++) {
        char name[96];
        char child_path[256];
        uint8_t type = 0;

        if (!api->readdir ||
            api->readdir(handle, index, name, sizeof(name), &type) != 0)
            break;
        if (!name[0])
            continue;
        if (installer_path_join(child_path, sizeof(child_path), path, name) != 0)
            continue;
        total += installer_count_tree_files(api, child_path);
    }

    api->close(handle);
    return total;
}

static int installer_count_boot_aliases(kapi_t *api, const char *target_root) {
    char boot_bios_path[256];

    if (installer_path_join(boot_bios_path, sizeof(boot_bios_path), target_root,
                            "boot/limine-bios.sys") != 0) {
        return 0;
    }
    return installer_path_exists(api, boot_bios_path) ? 3 : 0;
}

static int installer_copy_file(kapi_t *api, const char *src_path,
                               const char *dst_path) {
    void *src;
    char *buffer;
    int file_size;
    int offset = 0;
    int wrote_any = 0;

    if (!api || !api->open || !api->close || !api->read || !api->save_file ||
        !src_path || !dst_path) {
        return -1;
    }

    src = api->open(src_path);
    if (!src || (api->is_dir && api->is_dir(src))) {
        if (src)
            api->close(src);
        return -1;
    }

    file_size = api->file_size ? api->file_size(src) : 0;
    if (api->delete)
        api->delete(dst_path);
    if (installer_ensure_parent_dirs(api, dst_path) != 0) {
        api->close(src);
        return -1;
    }

    if (file_size <= 0) {
        int ret = api->save_file(dst_path, "", 0, OS8_SAVE_CREATE_PARENTS);
        if (ret != 0 && api->create) {
            void *created = api->create(dst_path);
            if (created) {
                api->close(created);
                ret = 0;
            }
        }
        api->close(src);
        return ret;
    }

    buffer = api->malloc ? (char *)api->malloc(INSTALLER_COPY_CHUNK) : NULL;
    if (!buffer) {
        api->close(src);
        return -1;
    }

    while (offset < file_size) {
        int chunk = file_size - offset;
        int read_count;
        uint32_t flags = OS8_SAVE_CREATE_PARENTS;

        if (chunk > INSTALLER_COPY_CHUNK)
            chunk = INSTALLER_COPY_CHUNK;
        read_count = api->read(src, buffer, (size_t)chunk, (size_t)offset);
        if (read_count <= 0) {
            api->free(buffer);
            api->close(src);
            return -1;
        }
        if (wrote_any)
            flags |= OS8_SAVE_APPEND;
        if (api->save_file(dst_path, buffer, (size_t)read_count, flags) != 0) {
            api->free(buffer);
            api->close(src);
            return -1;
        }
        wrote_any = 1;
        offset += read_count;
    }

    api->free(buffer);
    api->close(src);
    return 0;
}

static int installer_copy_tree(kapi_t *api, const char *src_root,
                               const char *dst_root,
                               installer_progress_state_t *progress) {
    void *dir;

    if (!api || !src_root || !dst_root || !progress)
        return -1;

    dir = api->open(src_root);
    if (!dir)
        return -1;
    if (!api->is_dir || !api->is_dir(dir)) {
        api->close(dir);
        return installer_copy_file(api, src_root, dst_root);
    }

    if (installer_ensure_dir(api, dst_root) != 0) {
        api->close(dir);
        return -1;
    }

    for (int index = 0;; index++) {
        char name[96];
        char src_path[256];
        char dst_path[256];
        uint8_t type = 0;

        if (!api->readdir ||
            api->readdir(dir, index, name, sizeof(name), &type) != 0)
            break;
        if (!name[0])
            continue;
        if (installer_path_join(src_path, sizeof(src_path), src_root, name) != 0 ||
            installer_path_join(dst_path, sizeof(dst_path), dst_root, name) != 0) {
            api->close(dir);
            return -1;
        }
        if (installer_path_is_dir(api, src_path)) {
            if (installer_copy_tree(api, src_path, dst_path, progress) != 0) {
                api->close(dir);
                return -1;
            }
            continue;
        }

        installer_copy_text(progress->detail, sizeof(progress->detail), src_path);
        installer_progress_render(api, progress);
        if (installer_copy_file(api, src_path, dst_path) != 0) {
            api->close(dir);
            return -1;
        }
        progress->progress_done++;
        installer_progress_render(api, progress);
    }

    api->close(dir);
    return 0;
}

static int installer_copy_boot_aliases(kapi_t *api, const char *target_root,
                                       installer_progress_state_t *progress) {
    static const char *alias_suffixes[] = {
        "limine-bios.sys",
        "limine/limine-bios.sys",
        "boot/limine/limine-bios.sys",
    };
    char src_path[256];

    if (installer_path_join(src_path, sizeof(src_path), target_root,
                            "boot/limine-bios.sys") != 0) {
        return -1;
    }
    if (!installer_path_exists(api, src_path))
        return 0;

    for (int i = 0; i < (int)(sizeof(alias_suffixes) / sizeof(alias_suffixes[0]));
         i++) {
        char dst_path[256];

        if (installer_path_join(dst_path, sizeof(dst_path), target_root,
                                alias_suffixes[i]) != 0) {
            return -1;
        }
        installer_copy_text(progress->detail, sizeof(progress->detail), dst_path);
        installer_progress_render(api, progress);
        if (installer_copy_file(api, src_path, dst_path) != 0)
            return -1;
        progress->progress_done++;
        installer_progress_render(api, progress);
    }

    return 0;
}

static int installer_save_text_file(kapi_t *api, const char *path,
                                    const char *content) {
    int len = 0;

    if (!api || !api->save_file || !path || !content)
        return -1;
    while (content[len])
        len++;
    return api->save_file(path, content, (size_t)len, OS8_SAVE_CREATE_PARENTS);
}

static int installer_write_metadata_copy(kapi_t *api, const char *base_root,
                                         const char *relative_path,
                                         const char *content) {
    char full_path[256];

    if (!base_root || !base_root[0])
        return 0;
    if (installer_path_join(full_path, sizeof(full_path), base_root,
                            relative_path) != 0) {
        return -1;
    }
    return installer_save_text_file(api, full_path, content);
}

static int installer_has_partition_kind(kapi_t *api, int disk_index,
                                        uint32_t kind, uint32_t *size_mib,
                                        int *partition_index) {
    int count;

    if (!api || !api->partition_count || !api->partition_info)
        return 0;
    count = api->partition_count(disk_index);
    for (int i = 0; i < count; i++) {
        os8_partition_info_t info;

        if (api->partition_info(disk_index, i, &info) != 0)
            continue;
        if (info.kind != kind)
            continue;
        if (size_mib)
            *size_mib = info.size_mib;
        if (partition_index)
            *partition_index = i;
        return 1;
    }
    return 0;
}

static int installer_prepare_partitions(kapi_t *api, int disk_index) {
    os8_disk_info_t disk_info;
    uint32_t used_mib = 0;
    uint32_t system_size = 0;
    uint32_t free_mib;
    int partition_count;
    int system_partition_index = -1;
    int has_efi;
    int has_system;
    int has_data;

    if (!api || !api->disk_info || !api->partition_count || !api->partition_info ||
        !api->partition_create || !api->partition_update) {
        return -1;
    }
    if (api->disk_info(disk_index, &disk_info) != 0)
        return -1;

    partition_count = api->partition_count(disk_index);
    for (int i = 0; i < partition_count; i++) {
        os8_partition_info_t info;

        if (api->partition_info(disk_index, i, &info) != 0)
            continue;
        used_mib += info.size_mib;
    }

    has_efi = installer_has_partition_kind(api, disk_index, OS8_PARTITION_EFI,
                                           NULL, NULL);
    has_system = installer_has_partition_kind(api, disk_index, OS8_PARTITION_SYSTEM,
                                              &system_size, &system_partition_index);
    has_data = installer_has_partition_kind(api, disk_index, OS8_PARTITION_DATA,
                                            NULL, NULL);

    if (!has_efi && api->partition_create(disk_index, OS8_PARTITION_EFI, 256) != 0)
        return -1;

    if (!has_system) {
        uint32_t desired_system_size = disk_info.capacity_mib / 2U;

        if (desired_system_size < 8192U)
            desired_system_size = 8192U;
        if (desired_system_size > 65536U)
            desired_system_size = 65536U;
        if (api->partition_create(disk_index, OS8_PARTITION_SYSTEM,
                                  desired_system_size) != 0) {
            return -1;
        }
        has_system = 1;
        system_size = desired_system_size;
        used_mib += desired_system_size;
    }

    free_mib = disk_info.capacity_mib > used_mib ? disk_info.capacity_mib - used_mib : 0;
    if (!has_data && free_mib >= 4096U) {
        uint32_t data_size = free_mib;

        if (data_size > 65536U)
            data_size = 65536U;
        if (api->partition_create(disk_index, OS8_PARTITION_DATA, data_size) == 0)
            return 0;
    }

    if (!has_data && partition_count == 1 && system_partition_index >= 0) {
        uint32_t data_size = system_size / 4U;
        uint32_t new_system_size;

        if (data_size < 4096U)
            data_size = 4096U;
        if (data_size > 16384U)
            data_size = 16384U;
        if (system_size <= data_size + 8192U)
            return 0;

        new_system_size = system_size - data_size;
        if (new_system_size < 8192U)
            return 0;

        if (api->partition_update(disk_index, system_partition_index,
                                  OS8_PARTITION_SYSTEM, new_system_size) != 0) {
            return -1;
        }
        if (api->partition_create(disk_index, OS8_PARTITION_DATA, data_size) != 0) {
            api->partition_update(disk_index, system_partition_index,
                                  OS8_PARTITION_SYSTEM, system_size);
            return -1;
        }
    }

    return 0;
}

static int installer_write_install_target_config(kapi_t *api,
                                                 const installer_disk_entry_t *disk,
                                                 const char *target_root,
                                                 const char *physical_root) {
    char manifest[256];
    char label[96];
    int idx = 0;
    int partition_count = 0;
    int has_efi = 0;

    if (!api || !disk)
        return -1;

    installer_build_disk_label(disk, label, sizeof(label));
    for (const char *p = "disk="; *p && idx < (int)sizeof(manifest) - 1; p++)
        manifest[idx++] = *p;
    for (int i = 0; label[i] && idx < (int)sizeof(manifest) - 2; i++)
        manifest[idx++] = label[i];
    manifest[idx++] = '\n';

    if (disk->info.location[0]) {
        for (const char *p = "disk_location=";
             *p && idx < (int)sizeof(manifest) - 1; p++) {
            manifest[idx++] = *p;
        }
        for (int i = 0; disk->info.location[i] &&
                        idx < (int)sizeof(manifest) - 2;
             i++) {
            manifest[idx++] = disk->info.location[i];
        }
        manifest[idx++] = '\n';
    }

    if (api->partition_count)
        partition_count = api->partition_count(disk->disk_index);
    has_efi = installer_has_partition_kind(api, disk->disk_index, OS8_PARTITION_EFI,
                                           NULL, NULL);

    for (const char *p = "partitions="; *p && idx < (int)sizeof(manifest) - 1; p++)
        manifest[idx++] = *p;
    if (idx < (int)sizeof(manifest) - 1)
        installer_append_decimal(manifest + idx, (int)sizeof(manifest) - idx,
                                 partition_count);
    while (manifest[idx] && idx < (int)sizeof(manifest) - 1)
        idx++;
    if (idx < (int)sizeof(manifest) - 1)
        manifest[idx++] = '\n';

    for (const char *p = "efi="; *p && idx < (int)sizeof(manifest) - 1; p++)
        manifest[idx++] = *p;
    if (idx < (int)sizeof(manifest) - 1)
        installer_append_decimal(manifest + idx, (int)sizeof(manifest) - idx,
                                 has_efi ? 1 : 0);
    while (manifest[idx] && idx < (int)sizeof(manifest) - 1)
        idx++;
    if (idx < (int)sizeof(manifest) - 1)
        manifest[idx++] = '\n';
    manifest[idx] = '\0';

    if (installer_save_text_file(api, "/System/install-target.cfg", manifest) != 0)
        return -1;
    if (installer_write_metadata_copy(api, target_root, "install-target.cfg",
                                      manifest) != 0) {
        return -1;
    }
    if (physical_root && physical_root[0] &&
        !installer_text_equal(physical_root, target_root) &&
        installer_write_metadata_copy(api, physical_root, "install-target.cfg",
                                      manifest) != 0) {
        return -1;
    }

    return 0;
}

static int installer_write_boot_metadata(kapi_t *api, const char *target_root,
                                         const char *physical_root) {
    struct metadata_file {
        const char *relative_path;
        const char *content;
    };
    static const struct metadata_file files[] = {
        {"BOOTABLE.CFG", "bootable=1\nloader=limine\nsource=installed-system\n"},
        {"EFI/BOOT/BOOTABLE.CFG",
         "bootable=1\nloader=limine\nsource=installed-system\n"},
        {"boot/BOOTABLE.CFG",
         "bootable=1\nscheme=mbr\nactive_partition=System\nloader=limine\n"
         "source=installed-system\n"},
        {"System/installer-state.txt",
         "installed=1\nprofile=system-image\nsource=installed-system\n"
         "first_boot_setup=1\n"},
        {"System/efi-boot.cfg",
         "bootable=1\nloader=limine\nsource=installed-system\n"},
        {"System/mbr-boot.cfg",
         "bootable=1\nscheme=mbr\nactive_partition=System\nloader=limine\n"
         "source=installed-system\n"},
    };

    if (!api || !target_root || !target_root[0])
        return -1;

    for (int i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++) {
        char live_path[128];

        if (installer_path_join(live_path, sizeof(live_path), "/", files[i].relative_path) !=
            0) {
            return -1;
        }
        if (installer_save_text_file(api, live_path, files[i].content) != 0)
            return -1;
        if (installer_write_metadata_copy(api, target_root, files[i].relative_path,
                                          files[i].content) != 0) {
            return -1;
        }
        if (physical_root && physical_root[0] &&
            !installer_text_equal(physical_root, target_root) &&
            installer_write_metadata_copy(api, physical_root, files[i].relative_path,
                                          files[i].content) != 0) {
            return -1;
        }
    }

    return 0;
}

static int installer_finalize_app_install(kapi_t *api,
                                          const installer_disk_entry_t *disk,
                                          const char *target_root,
                                          int raw_disk_image_install,
                                          installer_progress_state_t *progress) {
    char physical_root[160];

    if (!api || !disk || !target_root || !target_root[0] || !progress)
        return -1;

    physical_root[0] = '\0';
    if (api->installer_target_physical_root) {
        if (api->installer_target_physical_root(physical_root,
                                                sizeof(physical_root)) != 0) {
            physical_root[0] = '\0';
        }
    }

    if (!raw_disk_image_install &&
        installer_prepare_partitions(api, disk->disk_index) != 0) {
        installer_progress_set(progress, "Install failed while preparing partitions.",
                               "The installer could not prepare EFI, system, or data partitions.",
                               progress->progress_done, progress->progress_total, 0);
        installer_progress_render(api, progress);
        return -1;
    }

    if (installer_write_install_target_config(api, disk, target_root,
                                              physical_root) != 0) {
        installer_progress_set(progress, "Install failed while writing target metadata.",
                               "The installer could not persist install-target configuration.",
                               progress->progress_done, progress->progress_total, 0);
        installer_progress_render(api, progress);
        return -1;
    }

    if (installer_write_boot_metadata(api, target_root, physical_root) != 0) {
        installer_progress_set(progress, "Install failed while writing boot metadata.",
                               "The installer could not write first-boot and bootable-state files.",
                               progress->progress_done, progress->progress_total, 0);
        installer_progress_render(api, progress);
        return -1;
    }

    return 0;
}

static int installer_run_tree_install(kapi_t *api,
                                      const installer_disk_entry_t *disk,
                                      int raw_disk_image_install,
                                      installer_progress_state_t *progress) {
    char target_root[160];
    char system_root[160];
    char boot_root[160];
    int total_files = 0;

    if (!api || !disk || !progress || !api->installer_target_root ||
        !api->installer_system_image_root || !api->installer_boot_payload_root) {
        return -1;
    }

    if (api->installer_target_root(target_root, sizeof(target_root)) != 0 ||
        api->installer_system_image_root(system_root, sizeof(system_root)) != 0 ||
        api->installer_boot_payload_root(boot_root, sizeof(boot_root)) != 0) {
        installer_progress_set(progress, "Installer could not resolve payload paths.",
                               "The installer API did not return a usable source or target path.",
                               0, 1, 0);
        installer_progress_render(api, progress);
        return -1;
    }

    total_files = installer_count_tree_files(api, system_root);
    if (!installer_text_equal(system_root, boot_root))
        total_files += installer_count_tree_files(api, boot_root);
    total_files += installer_count_boot_aliases(api, target_root);
    if (total_files <= 0)
        total_files = 1;

    installer_progress_set(progress, "Preparing install target",
                           "Counting files and creating target directories.",
                           0, total_files, 0);
    installer_progress_render(api, progress);

    if (installer_copy_tree(api, system_root, target_root, progress) != 0) {
        installer_progress_set(progress, "Install failed during system image copy.",
                               "A system image file could not be copied to the target disk.",
                               progress->progress_done, progress->progress_total, 0);
        installer_progress_render(api, progress);
        return -1;
    }

    if (!installer_text_equal(system_root, boot_root) &&
        installer_copy_tree(api, boot_root, target_root, progress) != 0) {
        installer_progress_set(progress, "Install failed during boot payload copy.",
                               "A boot payload file could not be copied to the target disk.",
                               progress->progress_done, progress->progress_total, 0);
        installer_progress_render(api, progress);
        return -1;
    }

    if (installer_copy_boot_aliases(api, target_root, progress) != 0) {
        installer_progress_set(progress, "Install failed while preparing boot aliases.",
                               "The installer could not create the Limine boot file aliases.",
                               progress->progress_done, progress->progress_total, 0);
        installer_progress_render(api, progress);
        return -1;
    }

    installer_progress_set(progress, "Finalizing installation",
                           "Writing install metadata and first-boot state.",
                           progress->progress_total, progress->progress_total, 0);
    installer_progress_render(api, progress);
    if (installer_finalize_app_install(api, disk, target_root,
                                       raw_disk_image_install, progress) != 0) {
        installer_progress_set(progress, "Install failed during finalization.",
                               "The installer copied files but could not refresh boot metadata.",
                               progress->progress_total, progress->progress_total, 0);
        installer_progress_render(api, progress);
        return -1;
    }

    installer_progress_set(progress, "Installation Complete",
                           "Press R to reboot into the installed system.",
                           progress->progress_total, progress->progress_total, 1);
    installer_progress_render(api, progress);
    return 0;
}

int installer_app_main(kapi_t *api, int argc, char **argv) {
    installer_disk_entry_t disks[8];
    installer_progress_state_t app_progress;
    int disk_count = 0;
    int selected = 0;
    int mode = 0;
    int install_started = 0;
    int install_complete = 0;

    (void)argc;
    (void)argv;

    if (!api)
        return -1;

    if (api->installer_mode)
        mode = api->installer_mode();
    if (!mode) {
        if (api->puts)
            api->puts("installer: not running in installer boot mode\n");
        return -1;
    }

    installer_progress_set(&app_progress, "", "", 0, 100, 0);

    for (;;) {
        int key = -1;

        if (!install_started) {
            disk_count = installer_collect_disks(api, disks, 8);
            if (selected >= disk_count)
                selected = disk_count > 0 ? disk_count - 1 : 0;
            if (disk_count <= 0) {
                installer_copy_text(app_progress.status, sizeof(app_progress.status),
                                    "No writable installer target disks were detected.");
            } else {
                app_progress.status[0] = '\0';
            }
            installer_render_selection(api, selected, disk_count, disks,
                                       app_progress.status);
        } else {
            installer_render_progress(api, app_progress.status, app_progress.detail,
                                      app_progress.progress_done,
                                      app_progress.progress_total,
                                      app_progress.complete);
            install_complete = app_progress.complete;
        }

        if (api->has_key && api->has_key())
            key = api->getc ? api->getc() : -1;

        if (key == 'r' || key == 'R') {
            if (api->installer_reboot)
                api->installer_reboot();
            return 0;
        }

        if (!install_started) {
            if (key == 'w' || key == 'W') {
                if (selected > 0)
                    selected--;
            } else if (key == 's' || key == 'S') {
                if (selected + 1 < disk_count)
                    selected++;
            } else if ((key == INSTALLER_KEY_ENTER ||
                        key == INSTALLER_KEY_NEWLINE) &&
                       disk_count > 0) {
                int has_raw_disk_image = api->installer_has_raw_disk_image
                                             ? api->installer_has_raw_disk_image()
                                             : 0;
                char target_root[160];
                char system_root[160];
                char boot_root[160];
                int system_archive = 0;
                int boot_archive = 0;
                int use_backend_apply = 0;

                if (!api->installer_select_disk_index ||
                    api->installer_select_disk_index(disks[selected].disk_index) != 0) {
                    installer_copy_text(app_progress.status,
                                        sizeof(app_progress.status),
                                        "Installer could not select the target disk.");
                    continue;
                }

                target_root[0] = '\0';
                system_root[0] = '\0';
                boot_root[0] = '\0';
                if (api->installer_target_root)
                    api->installer_target_root(target_root, sizeof(target_root));
                if (api->installer_system_image_root)
                    api->installer_system_image_root(system_root,
                                                    sizeof(system_root));
                if (api->installer_boot_payload_root)
                    api->installer_boot_payload_root(boot_root, sizeof(boot_root));
                if (api->installer_payload_is_archive && system_root[0])
                    system_archive =
                        api->installer_payload_is_archive(system_root) > 0;
                if (api->installer_payload_is_archive && boot_root[0])
                    boot_archive =
                        api->installer_payload_is_archive(boot_root) > 0;

                install_started = 1;
                use_backend_apply =
                    has_raw_disk_image > 0 || system_archive || boot_archive;

                installer_progress_set(
                    &app_progress, "Installer started.",
                    use_backend_apply
                        ? "Applying installer payload through OS8 installer APIs."
                        : "Copying the staged system image through OS8 app APIs.",
                    0, 100, 0);
                installer_progress_render(api, &app_progress);

                if (has_raw_disk_image > 0) {
                    installer_progress_set(
                        &app_progress, "Writing raw disk image",
                        "Applying the staged raw system disk image to the selected disk.",
                        40, 100, 0);
                    installer_progress_render(api, &app_progress);
                    if (!api->installer_apply_raw_disk_image ||
                        api->installer_apply_raw_disk_image() != 0) {
                        installer_progress_set(
                            &app_progress, "Install failed during raw disk image write.",
                            "The installer could not write the staged raw disk image to the target disk.",
                            40, 100, 0);
                        installer_progress_render(api, &app_progress);
                        continue;
                    }
                    installer_progress_set(
                        &app_progress, "Finalizing installation",
                        "Refreshing boot metadata and first-boot state.",
                        85, 100, 0);
                    installer_progress_render(api, &app_progress);
                    if (installer_finalize_app_install(api, &disks[selected],
                                                       target_root,
                                                       1, &app_progress) != 0) {
                        installer_progress_set(
                            &app_progress, "Install failed during finalization.",
                            "The raw disk image was written, but boot metadata could not be updated.",
                            85, 100, 0);
                        installer_progress_render(api, &app_progress);
                        continue;
                    }
                    installer_progress_set(
                        &app_progress, "Installation Complete",
                        "Press R to reboot into the installed system.",
                        100, 100, 1);
                    installer_progress_render(api, &app_progress);
                    install_complete = 1;
                } else if (system_archive || boot_archive) {
                    installer_progress_set(
                        &app_progress, "Extracting installer payload",
                        "Applying archived installer payload to the selected disk.",
                        45, 100, 0);
                    installer_progress_render(api, &app_progress);
                    if (!api->installer_apply_system_payload ||
                        api->installer_apply_system_payload() != 0) {
                        installer_progress_set(
                            &app_progress, "Install failed during archive extraction.",
                            "The installer could not apply the archived payload to the target disk.",
                            45, 100, 0);
                        installer_progress_render(api, &app_progress);
                        continue;
                    }
                    installer_progress_set(
                        &app_progress, "Finalizing installation",
                        "Refreshing boot metadata and first-boot state.",
                        85, 100, 0);
                    installer_progress_render(api, &app_progress);
                    if (installer_finalize_app_install(api, &disks[selected],
                                                       target_root,
                                                       0, &app_progress) != 0) {
                        installer_progress_set(
                            &app_progress, "Install failed during finalization.",
                            "The archive payload was applied, but boot metadata could not be updated.",
                            85, 100, 0);
                        installer_progress_render(api, &app_progress);
                        continue;
                    }
                    installer_progress_set(
                        &app_progress, "Installation Complete",
                        "Press R to reboot into the installed system.",
                        100, 100, 1);
                    installer_progress_render(api, &app_progress);
                    install_complete = 1;
                } else if (installer_run_tree_install(api, &disks[selected], 0,
                                                     &app_progress) == 0) {
                    install_complete = 1;
                }
            }
        } else if (install_complete) {
            if (key == 'q' || key == 'Q')
                return 0;
        }

        if (api->sleep_ms)
            api->sleep_ms(16);
        else if (api->yield)
            api->yield();
    }
}
