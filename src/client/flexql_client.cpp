#include "common/flexql.h"
#include "network/net_utils.h"
#include "common/flexql_protocol.h"

#include <netdb.h>
#include <unistd.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>

static char **Unpack_To_C(const std::string &payload, int *count)
{
    int n = 0;
    for (char c : payload)
        if (c == '\0')
            n++;

    *count = n;
    if (n == 0)
        return nullptr;

    int idx = 0;
    size_t start = 0;
    char **arr = static_cast<char **>(malloc((size_t)n * sizeof(char *)));
    for (size_t i = 0; i <= payload.size() && idx < n; i++)
    {
        if (i == payload.size() || payload[i] == '\0')
        {
            size_t len = i - start;
            arr[idx] = static_cast<char *>(malloc(len + 1));
            memcpy(arr[idx], payload.data() + start, len);
            arr[idx][len] = '\0';

            idx++;
            start = i + 1;
        }
    }

    return arr;
}

static void Free_C(char **arr, int count)
{
    if (!arr) return;
    for (int i = 0; i < count; i++)
        free(arr[i]);

    free(arr);
}

int flexql_open(const char *host, int port, FlexQL **db)
{
    if (!host || !db)
        return FLEXQL_ERROR;

    struct hostent *he = gethostbyname(host);
    if (!he)
        return FLEXQL_ERROR;

    int serverfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverfd < 0)
        return FLEXQL_ERROR;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (::connect(serverfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        ::close(serverfd);
        return FLEXQL_ERROR;
    }

    FlexQL *handle = new FlexQL();
    handle->serverfd = serverfd;
    handle->port = port;
    strncpy(handle->host, host, sizeof(handle->host) - 1);
    handle->host[sizeof(handle->host) - 1] = '\0';

    *db = handle;
    return FLEXQL_OK;
}

int flexql_close(FlexQL *db)
{
    if (!db)
        return FLEXQL_ERROR;

    ::close(db->serverfd);
    delete db;

    return FLEXQL_OK;
}

int flexql_exec(FlexQL *db, const char *sql, int (*callback)(void *, int, char **, char **), void *arg, char **errmsg)
{
    if (!db || !sql)
        return FLEXQL_ERROR;

    uint32_t sql_len = (uint32_t)strlen(sql);
    MessageHeader qhdr{MSG_QUERY, sql_len};

    if (!Net_Send_All(db->serverfd, &qhdr, sizeof(qhdr)) || (sql_len > 0 && !Net_Send_All(db->serverfd, sql, sql_len)))
    {
        if (errmsg)
            *errmsg = strdup("ERROR: Failed to send query");

        return FLEXQL_ERROR;
    }

    int col_count = 0;
    bool aborted = false;
    char **col_names = nullptr;
    while (true)
    {
        MessageHeader rhdr;
        if (!Net_Receive_All(db->serverfd, &rhdr, sizeof(rhdr)))
        {
            Free_C(col_names, col_count);
            if (errmsg)
                *errmsg = strdup("ERROR: Connection lost");
            
            return FLEXQL_ERROR;
        }

        std::string payload(rhdr.length, '\0');
        if (rhdr.length > 0 &&
            !Net_Receive_All(db->serverfd, payload.data(), rhdr.length))
        {
            Free_C(col_names, col_count);
            if (errmsg)
                *errmsg = strdup("ERROR: Truncated message");
            
            return FLEXQL_ERROR;
        }

        if (rhdr.type == MSG_ROW_HEADER)
        {
            Free_C(col_names, col_count);
            col_names = Unpack_To_C(payload, &col_count);
        }
        else if (rhdr.type == MSG_ROW)
        {
            if (!aborted && callback)
            {
                int val_count = 0;
                char **values = Unpack_To_C(payload, &val_count);
                int ret = callback(arg, col_count, values, col_names);
                Free_C(values, val_count);
                if (ret != 0)
                    aborted = true;
            }
        }
        else if (rhdr.type == MSG_STATUS)
        {
            Free_C(col_names, col_count);
            if (payload.rfind("ERROR", 0) == 0)
            {
                if (errmsg)
                    *errmsg = strdup(payload.c_str());
                
                return FLEXQL_ERROR;
            }

            return FLEXQL_OK;
        }
        else
        {
            Free_C(col_names, col_count);
            if (errmsg)
                *errmsg = strdup("ERROR: Unknown message type");
            
            return FLEXQL_ERROR;
        }
    }
}

void flexql_free(void *ptr) { free(ptr); }