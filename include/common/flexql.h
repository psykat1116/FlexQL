#pragma once

#define FLEXQL_OK 0
#define FLEXQL_ERROR 1

typedef struct
{
    long long rows = 0;
} QueryStats;

typedef struct
{
    int port;
    int serverfd;
    char host[256];
} FlexQL;

void flexql_free(void *ptr);
int flexql_close(FlexQL *db);
int flexql_open(const char *host, int port, FlexQL **db);
int flexql_exec(FlexQL *db, const char *sql, int (*callback)(void *, int, char **, char **), void *arg, char **errmsg);