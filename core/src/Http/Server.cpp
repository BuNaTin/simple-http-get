#include <Http/Server.h>

#include <arpa/inet.h>
#include <cstdlib>
#include <functional>
#include <future>
#include <netdb.h>
#include <numeric>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <iostream>

namespace {

void workWithClient(
        const std::vector<
                std::pair<std::string, http_get::Server::handler_t>>
                &handlers,
        i32 client_desc);

/**
 * @brief read TCP package
 *
 * @param client_desc - socket descriptor
 * @param data - where to read
 * @param size - max buffer size
 * @param read_size - size of read data
 * @return true
 * @return false
 */
bool readRequest(i32 client_desc,
                 u8 *data,
                 const i64 size,
                 i64 &read_size);
bool readContinue(i32 client_desc, u8 *data, const i64 size);

bool sendContinue(i32 client_desc);
bool sendNotImplemented(i32 client_desc);

bool sendResponse(i32 client_desc, const http_get::Response &resp);

/**
 * @brief Send long response payload stored in file
 *
 * @param client_desc
 * @param data
 * @param size
 * @param read_from - filename
 * @return true
 * @return false
 */
bool sendResponse(i32 client_desc,
                  u8 *data,
                  const i64 size,
                  std::ifstream &read_from);

// implementation

void workWithClient(
        const std::vector<
                std::pair<std::string, http_get::Server::handler_t>>
                &handlers,
        i32 client_desc) {
    const i64 size = 32 * 1024;
    u8 buffer[size] = {0};
    i64 read_size;
    if (!readRequest(client_desc, buffer, size, read_size)) return;
    u8 *end = buffer + read_size;

    std::unique_ptr<http_get::Request> cur_req =
            std::make_unique<http_get::Request>(buffer, end);
    if (cur_req->needContinue()) {
        i64 need_size = cur_req->payloadSize();
        if (!sendContinue(client_desc)) return;
        while (need_size > 0 &&
               readRequest(client_desc, buffer, size, read_size) &&
               read_size) {
            need_size -= read_size;
            cur_req->append(buffer, buffer + read_size);
        }
    }
    auto handler_filter = [query = cur_req->query()](const auto &pair) {
        return utils::fmt::cmp(pair.first, query);
    };
    auto p_handler = std::find_if(
            handlers.begin(), handlers.end(), handler_filter);
    // did not find any handler
    if (p_handler == handlers.end()) {
        return (void)sendNotImplemented(client_desc);
    }
    http_get::Response response = p_handler->second(cur_req->fin());
    if (!sendResponse(client_desc, response) ||
        !response.needContinue()) {
        return;
    }
    // data is big & stored in file
    std::ifstream read_from_stream(response.readFrom());
    while (sendResponse(client_desc, buffer, size, read_from_stream)) {
        ; // do literally nothing, just send data
    }
}

bool readRequest(i32 client_desc,
                 u8 *data,
                 const i64 size,
                 i64 &read_size) {
    read_size = recv(client_desc, data, size, 0);
    if (read_size < 0) {
        std::cerr << "HttpServer couldn't receive message from client "
                  << client_desc << std::endl;
        return false;
    }
    return true;
}

bool readContinue(i32 client_desc, u8 *data, const i64 size) {
    int read_size = recv(client_desc, data, size, 0);
    if (read_size < 0) {
        std::cerr << "HttpServer couldn't receive message from client "
                  << client_desc << std::endl;
        return false;
    }
    if (std::string(data, data + read_size)
                .find("HTTP/1.1 100 Continue") == std::string::npos) {
        std::cerr << "No continue from client " << client_desc
                  << std::endl;
        return false;
    }
    return true;
}

bool sendContinue(i32 client_desc) {
    const i64 size = 50;
    u8 data[size] =
            "HTTP/1.1 100 Continue\r\nConnection: keep-alive\r\n\r\n";

    if (send(client_desc, data, size, 0) < 0) {
        std::cerr << "Can't send 100-continue to " << client_desc
                  << std::endl;
        return false;
    }
    return true;
}

bool sendNotImplemented(i32 client_desc) {
    const i64 size = 33;
    u8 data[size] = "HTTP/1.1 501 Not Implemented\r\n\r\n";

    if (send(client_desc, data, size, 0) < 0) {
        std::cerr << "Can't send 501-not-implemented to " << client_desc
                  << std::endl;
        return false;
    }
    return true;
}

bool sendResponse(i32 client_desc, const http_get::Response &resp) {
    const std::string &data = resp.create();
    if (send(client_desc, data.c_str(), data.size(), 0) < 0) {
        std::cerr << "Can't send response to " << client_desc
                  << std::endl;
        return false;
    }
    return true;
}

bool sendResponse(i32 client_desc,
                  u8 *data,
                  const i64 size,
                  std::ifstream &read_from) {
    std::fill(data, data + size, '\0');
    read_from.read((char *)data, size);
    if (read_from.gcount() == 0) {
        return false;
    }
    if (send(client_desc, data, size, 0) < 0) {
        std::cerr << "Can't send response to " << client_desc
                  << std::endl;
        return false;
    }
    return true;
}

} // namespace

namespace http_get { inline namespace Http {

class ServerImpl final : public Server {
public:
    bool start() noexcept override;

    Server *addHandler(const std::string &pattern,
                       Server::handler_t &&handler) noexcept override;

    // constructors
public:
    ServerImpl(i32 fd, std::atomic_flag &stop)
            : m_stop(stop), m_fd(fd) {}
    ~ServerImpl() = default;

private:
    std::vector<std::pair<std::string, handler_t>> m_data;
    std::atomic_flag &m_stop;
    i32 m_fd;
};

std::unique_ptr<Server> Server::create(const Server::Settings &settings,
                                       std::atomic_flag &m_stop) {
    int socket_desc;
    struct sockaddr_in server_addr;

    socket_desc = socket(AF_INET, SOCK_STREAM, 0);

    if (socket_desc < 0) {
        std::cerr << "HttpServer: could not create socket"
                  << std::endl;
        return nullptr;
    }
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(settings.port);
    server_addr.sin_addr.s_addr = inet_addr(settings.address.c_str());
    int reuseaddr = 1;

    if (setsockopt(socket_desc,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &reuseaddr,
                   sizeof(int)) < 0) {
        std::cerr << "HttpServer: Could not reuse port"
                  << std::endl;
        return nullptr;
    }

    if (bind(socket_desc,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        std::cerr << "HttpServer: Couldn't bind to the port"
                  << std::endl;
        return nullptr;
    }
    return std::make_unique<ServerImpl>(socket_desc, m_stop);
}

bool ServerImpl::start() noexcept {
    if (m_fd == -1) {
        std::cerr << "Wrong fd" << std::endl;
        return false;
    }

    if (listen(m_fd, 1) < 0) {
        std::cerr << "HttpServer: err while listening"
                  << std::endl;
        return false;
    }

    int client_sock, client_size;
    struct sockaddr_in client_addr;
    client_size = sizeof(client_addr);

    struct timeval timeout;
    fd_set dummy;

    std::cout << "HttpServer listening" << std::endl;
    while (m_stop.test_and_set(std::memory_order_acquire)) {
        FD_ZERO(&dummy);
        FD_SET(m_fd, &dummy);
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        // std::cout << "HttpServer - tik" << std::endl;
        if (select(m_fd + 1, &dummy, NULL, NULL, &timeout) <= 0) {
            continue;
        };
        client_sock = accept(m_fd,
                             (struct sockaddr *)&client_addr,
                             (socklen_t *)&client_size);
        if (client_sock < 0) {
            std::cerr << "HttpServer couldnot accept" << std::endl;
            return false;
        }
        // std::cout << "Client number_" << client_sock << std::endl;
        workWithClient(m_data, client_sock);
        if (close(client_sock) < 0) {
            std::cerr << "HttpServer could not close client "
                      << client_sock << std::endl;
        }
    }
    m_stop.clear();
    return true;
}

Server *ServerImpl::addHandler(const std::string &pattern,
                               Server::handler_t &&handler) noexcept {
    m_data.push_back({pattern, std::move(handler)});
    return this;
}

}} // namespace http_get::Http