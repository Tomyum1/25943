#include <sys/types.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

extern char *tzname[];

int main(void)
{
    time_t now;
    struct tm *sp;
    static char california_tz[] = "TZ=PST8";

    if (putenv(california_tz) != 0) {
        perror("putenv");
        return 1;
    }

    tzset();

    if (time(&now) == (time_t)-1) {
        perror("time");
        return 1;
    }

    sp = localtime(&now);
    if (sp == NULL) {
        perror("localtime");
        return 1;
    }

    printf("%02d/%02d/%04d %02d:%02d:%02d %s\n",
        sp->tm_mon + 1,
        sp->tm_mday,
        sp->tm_year + 1900,
        sp->tm_hour,
        sp->tm_min,
        sp->tm_sec,
        tzname[sp->tm_isdst > 0]);

    return 0;
}