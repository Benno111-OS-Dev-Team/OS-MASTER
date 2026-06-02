#ifndef _KERNEL_MEDIA_H
#define _KERNEL_MEDIA_H

#include "types.h"

typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t *pixels; /* JPEG: 0x00RRGGBB, PNG may use 0xAARRGGBB */
} media_image_t;

typedef struct {
  int16_t *samples;      /* interleaved PCM */
  uint32_t sample_count; /* per-channel samples */
  uint32_t sample_rate;
  uint8_t channels;
} media_audio_t;

int media_load_file(const char *path, uint8_t **out_data, size_t *out_size);
void media_free_file(uint8_t *data);
int media_install_file(const char *path, const uint8_t *data, size_t size);
int media_install_text_file(const char *path, const char *content);
int media_zip_pack_tree(const char *src_root, uint8_t **out_data,
                        size_t *out_size);
int media_zip_count_files(const uint8_t *data, size_t size);
int media_zip_count_file_entries(const char *archive_path);
int media_zip_has_entry(const uint8_t *data, size_t size, const char *path);
int media_zip_file_has_entry(const char *archive_path, const char *path);
int media_zip_extract_to_root(const uint8_t *data, size_t size,
                              const char *dst_root, int *copied_files,
                              int *failed_files);
int media_zip_extract_file_to_root(const char *archive_path,
                                   const char *dst_root, int *copied_files,
                                   int *failed_files);

int media_decode_jpeg(const uint8_t *data, size_t size, media_image_t *out);
int media_decode_jpeg_buffer(const uint8_t *data, size_t size,
                             media_image_t *out, uint32_t *buffer,
                             size_t buffer_size);
void media_free_image(media_image_t *image);

int media_decode_mp3(const uint8_t *data, size_t size, media_audio_t *out);
void media_free_audio(media_audio_t *audio);

int media_decode_png(const uint8_t *data, size_t size, media_image_t *out);
int media_decode_svg(const uint8_t *data, size_t size, media_image_t *out);

int boot_splash_prepare(void);
const media_image_t *boot_splash_get_logo(void);

#endif /* _KERNEL_MEDIA_H */
