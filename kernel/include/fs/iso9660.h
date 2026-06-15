#ifndef _FS_ISO9660_H
#define _FS_ISO9660_H

int iso9660_init(void);
int iso9660_copy_to_ramfs(const char *disk_location, const char *dst_root);

#endif
