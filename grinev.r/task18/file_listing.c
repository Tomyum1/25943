#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <grp.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

static void make_mode_string(mode_t mode, char result[11])
{
    result[0] = S_ISDIR(mode) ? 'd' :
                S_ISREG(mode) ? '-' : '?';

    result[1] = (mode & S_IRUSR) ? 'r' : '-';
    result[2] = (mode & S_IWUSR) ? 'w' : '-';
    result[3] = (mode & S_IXUSR) ? 'x' : '-';

    result[4] = (mode & S_IRGRP) ? 'r' : '-';
    result[5] = (mode & S_IWGRP) ? 'w' : '-';
    result[6] = (mode & S_IXGRP) ? 'x' : '-';

    result[7] = (mode & S_IROTH) ? 'r' : '-';
    result[8] = (mode & S_IWOTH) ? 'w' : '-';
    result[9] = (mode & S_IXOTH) ? 'x' : '-';

    result[10] = '\0';
}

static void make_owner_name(uid_t uid, char *result, size_t result_size)
{
    struct passwd *owner;

    owner = getpwuid(uid);

    if (owner != NULL) {
        (void)snprintf(result, result_size, "%s", owner->pw_name);
    } else {
        (void)snprintf(result, result_size, "%" PRIuMAX,
                       (uintmax_t)uid);
    }
}

static void make_group_name(gid_t gid, char *result, size_t result_size)
{
    struct group *group;

    group = getgrgid(gid);

    if (group != NULL) {
        (void)snprintf(result, result_size, "%s", group->gr_name);
    } else {
        (void)snprintf(result, result_size, "%" PRIuMAX,
                       (uintmax_t)gid);
    }
}

static void make_size_string(const struct stat *information,
                             char *result, size_t result_size)
{
    if (S_ISREG(information->st_mode)) {
        (void)snprintf(result, result_size, "%" PRIdMAX,
                       (intmax_t)information->st_size);
    } else {
        result[0] = '\0';
    }
}

static void make_time_string(time_t modification_time,
                             char *result, size_t result_size)
{
    struct tm broken_down_time;

    if (localtime_r(&modification_time, &broken_down_time) == NULL ||
        strftime(result, result_size, "%Y-%m-%d %H:%M",
                 &broken_down_time) == 0) {
        (void)snprintf(result, result_size, "-");
    }
}

static void find_display_name(const char *path,
                              const char **name_start,
                              size_t *name_length)
{
    size_t end;
    size_t start;

    end = strlen(path);

    while (end > 1 && path[end - 1] == '/') {
        end--;
    }

    if (end == 1 && path[0] == '/') {
        *name_start = path;
        *name_length = 1;
        return;
    }

    start = end;

    while (start > 0 && path[start - 1] != '/') {
        start--;
    }

    *name_start = path + start;
    *name_length = end - start;
}

static int print_file_information(const char *path)
{
    struct stat information;
    char mode_string[11];
    char owner_name[64];
    char group_name[64];
    char size_string[32];
    char time_string[32];
    const char *display_name;
    size_t display_name_length;

    if (lstat(path, &information) == -1) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }

    make_mode_string(information.st_mode, mode_string);
    make_owner_name(information.st_uid,
                    owner_name, sizeof(owner_name));
    make_group_name(information.st_gid,
                    group_name, sizeof(group_name));
    make_size_string(&information, size_string, sizeof(size_string));
    make_time_string(information.st_mtime,
                     time_string, sizeof(time_string));
    find_display_name(path, &display_name, &display_name_length);

    printf("%-10s %4" PRIuMAX " %-12.12s %-12.12s %10s %-16s %.*s\n",
           mode_string,
           (uintmax_t)information.st_nlink,
           owner_name,
           group_name,
           size_string,
           time_string,
           (int)display_name_length,
           display_name);

    return 0;
}

int main(int argc, char *argv[])
{
    int status;
    int i;

    status = EXIT_SUCCESS;

    if (argc == 1) {
        if (print_file_information(".") == -1) {
            status = EXIT_FAILURE;
        }
    } else {
        for (i = 1; i < argc; i++) {
            if (print_file_information(argv[i]) == -1) {
                status = EXIT_FAILURE;
            }
        }
    }

    return status;
}
