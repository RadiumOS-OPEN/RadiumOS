#ifndef MV_H
#define MV_H
#include <stdbool.h>

void mv_command(int argc, char* argv[]);

// Path and copy helpers shared with cp (fileutils.c)
void mv_resolve(const char *path, char *out);
bool mv_basename(const char *path, char *out);
bool mv_join(const char *dir, const char *name, char *out);
bool mv_exists(const char *path);
int  mv_copy_file(const char *src, const char *dst);
int  mv_copy_tree(const char *src, const char *dst, int depth);
bool mv_is_inside(const char *resolved_path, const char *resolved_dir);
#endif // MV_H
