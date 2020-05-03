#include <HTTP/Client.h>
#include <Logger.h>
#include <Utils.h>

#include <sstream>
#include <algorithm>

HTTPClient::HTTPClient(const std::string& server_host, int server_port) :
    _unresolved_host(server_host), _port(server_port), _address{}
{
    SetupSystemHeaders();
}

SOCKET HTTPClient::Connect()
{
    SOCKET sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    int ret = connect(sockfd, &_address, sizeof(_address));
    if (ret == SOCKET_ERROR) {
        closesocket(sockfd);
        return INVALID_SOCKET;
    }

    return sockfd;
}

void HTTPClient::Disconnect(SOCKET sockfd)
{
    closesocket(sockfd);
}

ECode HTTPClient::Send(SOCKET sockfd, const std::string& request)
{
    int buf_idx = 0;
    int remaining_bytes = static_cast<int>(request.size());
    int sent_bytes;

    while (remaining_bytes) {
        sent_bytes = send(sockfd, &request[buf_idx], remaining_bytes, 0);
        if (sent_bytes == SOCKET_ERROR) {
            return ECode::SOCKET_SEND;
        }

        buf_idx += sent_bytes;
        remaining_bytes -= sent_bytes;
    }

    return ECode::OK;
}

ECode HTTPClient::Receive(SOCKET sockfd, HTTPResponse& response)
{
    char buffer[256];
    int recv_bytes;

    response.Reset();
    while (1) {
        recv_bytes = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
        if (recv_bytes == -1) {
            return ECode::SOCKET_RECV;
        }

        buffer[recv_bytes] = 0;
        response._raw.append(buffer);

        if (recv_bytes != sizeof(buffer) - 1) {
            break;
        }
    }

    return ParseResponse(response);
}

ECode HTTPClient::Request(
    const std::string& method, const std::string& path, const Map& query_params, const std::string& data,
    const std::string& content_type, const Map& user_headers, const Map& user_cookies)
{
    ECode err;
    SOCKET sockfd;
    std::string request;
    HTTPResponse response;
    Map merged_headers = user_headers;
    Map merged_cookies = user_cookies;

    merged_headers.insert(_system_headers.begin(), _system_headers.end());
    merged_cookies.insert(_system_cookies.begin(), _system_cookies.end());
    request = std::move(FormatRequest(method, path, query_params, data, content_type, merged_headers, merged_cookies));

    sockfd = Connect();
    if (sockfd == INVALID_SOCKET) {
        LOG_ERROR("Couldn't connect to HTTP server.");
        return ECode::SOCKET_CONNECT;
    }

    err = Send(sockfd, request);
    if (err != ECode::OK) {
        LOG_ERROR("Couldn't send HTTP request, errcode: {}", err);
        Disconnect(sockfd);
        return err;
    }

    err = Receive(sockfd, response);
    if (err != ECode::OK) {
        LOG_ERROR("Couldn't receive HTTP response, errcode: {}", err);
        Disconnect(sockfd);
        return err;
    }

    LOG_DEBUG("Raw response:\n{}", response.GetRaw());

    // update cookies
    for (const auto kv : response.GetCookies()) {
        _system_cookies[kv.first] = kv.second;
    }

    Disconnect(sockfd);
    return ECode::OK;
}

std::string HTTPClient::FormatRequest(
    const std::string& method, const std::string& path, const Map& query_params, const std::string& data,
    const std::string& content_type, const Map& headers, const Map& cookies)
{
    std::string request;
    std::string query_string;

    if (query_params.size()) {
        query_string = "?";

        for (const auto& kv : query_params) {
            query_string += fmt::format("{}={}&", kv.first, kv.second);
        }
    }

    // request type + path-query + HTTP version
    request = fmt::format("{} {}{} {}\r\n", method, path, query_string, HTTP_VERSION);

    // headers
    for (const auto& kv : headers) {
        request += fmt::format("{}: {}\r\n", kv.first, kv.second);
    }

    // cookies
    if (cookies.size()) {
        request += "cookie: ";
        for (const auto& kv : cookies) {
            request += fmt::format("{}={};", kv.first, kv.second);
        }
        request += "\r\n";
    }

    // data headers
    if (data.size()) {
        request += fmt::format("content-length: {}\r\n", data.length());
        request += fmt::format("content-type: {}\r\n", content_type);
    }

    request += "\r\n";

    // data
    if (data.size()) {
        request += data;
    }

    return request;
}

ECode HTTPClient::ParseResponse(HTTPResponse& response)
{
    enum {
        STATUS,
        HEADERS,
        BODY
    };

    auto lines = Utils::Split(response._raw, "\r\n");
    int state = STATUS;
    size_t content_length = 0;

    for (const auto& line : lines) {
        switch (state) {
            case STATUS: {
                std::stringstream ss(line);
                ss >> response._protover >> response._code >> response._status;

                state = HEADERS;
                break;
            }
            case HEADERS: {
                if (line.length() == 0) {
                    state = BODY;
                }
                else {
                    auto pos = line.find(':');
                    if (pos != std::string::npos) {
                        std::string key = line.substr(0, pos);
                        std::string val = line.substr(pos + 2);
                    
                        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                        if (key != "set-cookie") {
                            response._headers[key] = val;

                            if (key == "content-length") {
                                content_length = std::atoi(val.c_str());
                            }
                        }
                        else {
                            pos = val.find("=");

                            if (pos != std::string::npos) {
                                std::string cookie_key = val.substr(0, pos);
                                std::string cookie_val = val.substr(pos + 1);

                                pos = cookie_val.find(';');
                                if (pos != std::string::npos) {
                                    cookie_val.erase(pos);
                                }

                                response._cookies[cookie_key] = cookie_val;
                            }
                        }
                    }
                }
                break;
            }
            case BODY: {
                response._data += line;
                if (response._data.length() < content_length) {
                    response._data += "\r\n";
                }
                break;
            }
        }
    }
    
    return ECode::OK;
}

ECode HTTPClient::ResolveHost()
{
    ECode err = ECode::HOST_NORESULT;
    int ret;

    struct addrinfo* result = nullptr;
    struct addrinfo hints {};

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    ret = getaddrinfo(_unresolved_host.c_str(), std::to_string(_port).c_str(), &hints, &result);
    if (ret != 0) {
        return ECode::HOST_ADDRINFO;
    }

    for (struct addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        if (ptr->ai_family == AF_INET && ptr->ai_socktype == SOCK_STREAM && ptr->ai_protocol == IPPROTO_TCP) {

            memcpy(&_address, ptr->ai_addr, sizeof(struct sockaddr));
            err = ECode::OK;
            break;
        }
    }

    freeaddrinfo(result);
    return err;
}

void HTTPClient::SetupSystemHeaders()
{
    _system_headers["host"] = fmt::format("{}:{}", _unresolved_host, _port);
}

ECode HTTPClient::GlobalStartup()
{
#ifdef _WIN32
    WORD wVersionRequested;
    WSADATA wsaData;
    int err;

    wVersionRequested = MAKEWORD(2, 2);

    err = WSAStartup(wVersionRequested, &wsaData);
    if (err != 0) {
        LOG_ERROR("WSAStartup failed with error: {}", err);
        return ECode::WSA_STARTUP;
    }

    if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2) {
        LOG_ERROR("Could not find a usable version of Winsock.dll");
        WSACleanup();
        return ECode::WSA_STARTUP;
    }

    LOG_DEBUG("The Winsock 2.2 dll was found okay");
#endif
    return ECode::OK;
}

ECode HTTPClient::GlobalShutdown()
{
#ifdef _WIN32
    WSACleanup();
#endif
    return ECode::OK;
}
