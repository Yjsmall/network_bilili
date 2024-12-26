#include <algorithm>
#include <fmt/format.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

int check_error(const char *msg, int res) {
    if (res == -1) {
        fmt::println("{}: {}", msg, strerror(errno));
        throw;
    }
    return res;
}

size_t check_error(const char *msg, ssize_t res) {
    if (res == -1) {
        fmt::println("{}: {}", msg, strerror(errno));
        throw;
    }
    return res;
}

#define CHECK_CALL(func, ...) check_error(#func, func(__VA_ARGS__))

struct http_header_parser {
    std::string m_header;
    std::string m_body;
    size_t      content_length    = 0;
    bool        m_body_finished   = false;
    bool        m_header_finished = false;

    [[nodiscard]] bool hdeader_finished() { return m_header_finished; }

    [[nodiscard]] bool need_more_chunks() const { return !m_body_finished; }

    void _extract_header() {
        size_t pos = m_header.find("\r\n");
        while (pos != std::string::npos) {
            pos += 2;
            // 可能为npos
            size_t next_pos = m_header.find("\r\n", pos);
            size_t line_len = std::string::npos;
            if (next_pos != std::string::npos) {
                line_len = next_pos - pos;
            }
            std::string line  = m_header.substr(pos, line_len);
            size_t      colon = line.find(": ");
            if (colon != std::string::npos) {
                // each line has key:val
                std::string key   = line.substr(0, colon);
                std::string value = line.substr(colon + 2);
                // convert to lower character
                std::transform(key.begin(), key.end(), key.begin(), [](char c) {
                    if ('A' <= c && c <= 'Z') {
                        c += 'a' - 'A';
                    }
                    return c;
                });
                if (key == "content-length") {
                    content_length = std::stoi(value);
                }
            }
            pos = next_pos;
        }
    }

    void push_chunk(std::string_view chunk) {
        if (!m_header_finished) {
            m_header.append(chunk);
            // 找到了,就说明头以及结束了
            size_t header_len = m_header.find("\r\n\r\n");
            if (header_len != std::string::npos) {
                m_header_finished = true;
                m_body            = m_header.substr(header_len);
                m_header.resize(header_len);
                size_t body_len = 0;
                if (m_body.size() >= body_len) {
                    m_body_finished = true;
                }
            }
        } else {
            m_body.append(chunk);
        }
    }
};

struct socket_address_fatptr {
    struct sockaddr *m_addr;
    socklen_t        m_addrlen;
};

struct socket_address_storage {
    union {
        struct sockaddr         m_addr;
        struct sockaddr_storage m_addr_storage;
    };
    socklen_t m_addrlen = sizeof(struct sockaddr_storage);

    operator socket_address_fatptr() { return {&m_addr, m_addrlen}; }
};

struct address_resolved_entry {
    struct addrinfo *m_curr = nullptr;

    socket_address_fatptr get_address() const {
        return {m_curr->ai_addr, m_curr->ai_addrlen};
    }

    int create_socket() const {
        int sockfd = CHECK_CALL(socket, m_curr->ai_family, m_curr->ai_socktype,
                                m_curr->ai_protocol);
        return sockfd;
    }

    int create_socket_and_bind() const {
        int                   sockfd     = create_socket();
        socket_address_fatptr serve_addr = get_address();
        int                   on         = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
        CHECK_CALL(bind, sockfd, serve_addr.m_addr, serve_addr.m_addrlen);
        CHECK_CALL(listen, sockfd, SOMAXCONN);
        return sockfd;
    }

    [[nodiscard]] bool next_entry() {
        m_curr = m_curr->ai_next;
        if (m_curr == nullptr) {
            return false;
        }
        return true;
    }
};

struct address_resolver {
    struct addrinfo *m_head = nullptr;

    address_resolved_entry resolve(std::string const &name,
                                   std::string const &service) {
        int err = getaddrinfo(name.c_str(), service.c_str(), NULL, &m_head);
        if (err != 0) {
            fmt::println("{} {}: {}", name, service, gai_strerror(err));
            throw;
        }
        return {m_head};
    }

    address_resolved_entry get_first_entry() { return {m_head}; }

    address_resolver() = default;

    address_resolver(address_resolver &&that) : m_head(that.m_head) {
        that.m_head = nullptr;
    }

    ~address_resolver() {
        if (m_head) {
            freeaddrinfo(m_head);
        }
    }
};

std::vector<std::thread> pool;

int main(int argc, char *argv[]) {
    address_resolver resolver;
    fmt::println("connection .... localhost");
    // 0-1024 must use sudo permission
    resolver.resolve("localhost", "8080");
    auto entry    = resolver.get_first_entry();
    int  listenfd = entry.create_socket_and_bind();
    CHECK_CALL(listen, listenfd, SOMAXCONN);
    while (true) {
        socket_address_storage addr;
        int                    connid =
            CHECK_CALL(accept, listenfd, &addr.m_addr, &addr.m_addrlen);

        pool.push_back(std::thread([connid] {
            char               buf[1024];
            http_header_parser req_parse;
            do {
                size_t n = CHECK_CALL(read, connid, buf, sizeof(buf));
                req_parse.push_chunk(std::string_view(buf, n));
            } while (req_parse.need_more_chunks());

            auto req = req_parse.m_header;
            fmt::println("我的接收: {}", req);

            std::string res =
                "HTTP/1.1 200 OK\r\nServer: co_http\r\nConnection: "
                "close\r\nContent-length: 9\r\n\r\nHelloword";

            fmt::println("我的反馈是: {}", res);
            CHECK_CALL(write, connid, res.data(), res.size());
            close(connid);
        }));
    }
    for (auto &t : pool) {
        t.join();
    }

    return 0;
}
