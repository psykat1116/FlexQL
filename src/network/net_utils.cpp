#include <cstring>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include "network/net_utils.h"

bool Net_Send_All(int fd, const void *buf, size_t len)
{
    const char *p = reinterpret_cast<const char *>(buf);
    while (len > 0)
    {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }

    return true;
}

bool Net_Receive_All(int fd, void *buf, size_t len)
{
    char *p = reinterpret_cast<char *>(buf);
    while (len > 0)
    {
        ssize_t n = ::recv(fd, p, len, 0);
        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

bool Net_Send_Message(int fd, uint32_t type, const std::string &payload)
{
    MessageHeader hdr{type, (uint32_t)payload.size()};
    // Use writev() to send header + payload atomically in one syscall.
    // This eliminates one round-trip through the kernel per message and
    // avoids Nagle coalescing on the caller's side.
    struct iovec iov[2];
    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = const_cast<char *>(payload.data());
    iov[1].iov_len = payload.size();
    int nvecs = payload.empty() ? 1 : 2;
    ssize_t total = sizeof(hdr) + payload.size();
    ssize_t sent = ::writev(fd, iov, nvecs);
    if (sent == total)
        return true;
    // Partial write: fall back to net_sendAll for remainder
    return Net_Send_All(fd, &hdr, sizeof(hdr)) && (payload.empty() || Net_Send_All(fd, payload.data(), payload.size()));
}

std::string Net_Pack_Fields(const std::vector<std::string> &fields)
{
    std::string out;
    for (auto &f : fields)
    {
        out += f;
        out += '\0';
    }
    return out;
}

std::vector<std::string> Net_Unpack_Fields(const std::string &payload)
{
    size_t start = 0;
    std::vector<std::string> out;
    while (start < payload.size())
    {
        size_t end = payload.find('\0', start);
        if (end == std::string::npos)
            end = payload.size();
        out.push_back(payload.substr(start, end - start));
        start = end + 1;
    }
    return out;
}
