#include "sm_platform.h"
#include "sm_path_utils.h"
#include "sm_paths.h"

const char *get_filename_component(const char *path) {
  const char *base = strrchr(path, '/');
  if (!base)
    base = strrchr(path, '\\');
  return base ? base + 1 : path;
}

bool path_exists(const char *path) {
  struct stat st;
  return (path && path[0] != '\0' && stat(path, &st) == 0);
}

bool is_under_image_mount_base(const char *path) {
  size_t image_prefix_len = strlen(IMAGE_MOUNT_BASE);
  return (strncmp(path, IMAGE_MOUNT_BASE, image_prefix_len) == 0 &&
          path[image_prefix_len] == '/');
}

bool build_backports_root_path(const char *scan_path, char out[MAX_PATH]) {
  if (is_under_image_mount_base(scan_path))
    return false;

  snprintf(out, MAX_PATH, "%s/%s", scan_path, DEFAULT_BACKPORTS_DIR_NAME);
  return true;
}
