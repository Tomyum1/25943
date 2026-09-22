#include <sys/types.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

int main(void)
{
    time_t now;
    struct tm *sp;

    static char pst_tz[] = "TZ=PST8";
    static char pdt_tz[] = "TZ=PDT7";

    time(&now);

    putenv(pst_tz);
    tzset();
    sp = localtime(&now);

    printf("PST: %02d/%02d/%04d %02d:%02d:%02d\n",
        sp->tm_mon + 1,
        sp->tm_mday,
        sp->tm_year + 1900,
        sp->tm_hour,
        sp->tm_min,
        sp->tm_sec);

    putenv(pdt_tz);
    tzset();
    sp = localtime(&now);

    printf("PDT: %02d/%02d/%04d %02d:%02d:%02d\n",
        sp->tm_mon + 1,
        sp->tm_mday,
        sp->tm_year + 1900,
        sp->tm_hour,
        sp->tm_min,
        sp->tm_sec);

    return 0;
}