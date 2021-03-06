#include <stdexcept>
#include <memory>
#include <new>
#include <iostream>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include "easylogging++.h"
#include "fd_handler.h"
#include "server.h"

namespace thanos {

Server::Server() : 
        _conf(), _listenfd(-1), _epollfd(-1), 
        _connections(), _threadpool(), _is_running(false), 
        _port(-1), _backlog(-1), _max_events(-1) {}

Server::~Server() = default;

bool Server::_listen_at_port() {
    _listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (_listenfd == -1) {
        LOG(WARNING) << "[Server::_listen_at_port]: socket failed";
        return false;
    }

    sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_family = AF_INET;
    address.sin_port = htons(_port);

    if (bind(_listenfd, reinterpret_cast<sockaddr *>(&address), 
            sizeof(address)) == -1) {
        LOG(WARNING) << "[Server::_listen_at_port]: bind failed";
        return false;
    }

    if (listen(_listenfd, _backlog) == -1) {
        LOG(WARNING) << "[Server::_listen_at_port]: listen failed";
        return false;
    }
    LOG(INFO) << "listen at port:" << _port;
    return true;
}

bool Server::_close_connection(int fd) {
    if (_connections.find(fd) == _connections.end() ||
            _connections[fd] == nullptr) {
        LOG(WARNING) << "[Server::_close_connection]: cannot find fd:" << fd;
        return false;
    }
    if (fd == _listenfd || fd == _epollfd) {
        LOG(WARNING) << "[Server::_close_connection]: should not close listenfd or"
                << " epollfd here";
        return false;
    }
    if (!_connections[fd]->connection_close()) {
        LOG(WARNING) << "[Server::_close_connection]: connection close failed";
        return false;
    }
    delete _connections[fd];
    _connections[fd] = nullptr;
}

bool Server::init(const std::string& conf_path) {
    // 1、对初始化工具_conf本身进行初始化
    if (!_conf.init_config(conf_path)) {
        LOG(WARNING) << "[Server::init]: init _conf failed";
        return false;
    }

    // 2、解析配置键值对
    try {
        _port = _conf["PORT"].to_int32();
        _backlog = _conf["BACKLOG"].to_int32();
        _max_events = _conf["MAX_EVENTS"].to_int32();
        _max_thread_num = _conf["MAX_THREAD_NUM"].to_int32();
        _max_requests = _conf["MAX_REQUESTS"].to_int32();
        _root_dir = _conf["ROOT_DIR"].to_string();
    } catch (std::exception err) {
        LOG(WARNING) << err.what();
        return false;
    }

    // 3、初始化线程池
    if (!_threadpool.init(_max_thread_num, _max_requests)) {
        LOG(WARNING) << "[Server::init]: _threadpool.init failed";
        return false;
    }

    // 4、初始化Connection的根目录
    Connection::init_root_dir(_root_dir);
    
    return true;
}

bool Server::uninit() {
    for (auto it = _connections.begin(); it != _connections.end(); ++it) {
        if (it->second != nullptr) {
            delete (it->second);
        }
    }

    if (_listenfd != -1 && close(_listenfd) == -1) {
        LOG(WARNING) << "[Server::uninit]: close _listenfd failed";
        return false;
    }
    if (_epollfd != -1 && close(_epollfd) == -1) {
        LOG(WARNING) << "[Server::uninit]: close _epollfd failed";
        return false;
    }
    LOG(INFO) << "[Server::uninit]: clear resource success";
    return true;
}

void Server::ask_to_quit() {
    _is_running = false;
    _threadpool.ask_to_quit();
}

bool Server::run() {
    if (!_listen_at_port()) {
        LOG(WARNING) << "[Server::run]: _listen_at_port failed";
        return false;
    }

    epoll_event* events = new (std::nothrow) epoll_event[_max_events];
    if (events == nullptr) {
        LOG(WARNING) << "[Server::run]: new expection";
        return false;
    }

    std::shared_ptr<epoll_event> events_ptr_guard(events);

    // 在kernel-2.6.8之后的版本中epoll_create参数是被忽略的
    _epollfd = epoll_create(1024);
    if (_epollfd == -1) {
        LOG(WARNING) << "[Server::run]: epoll_create failed";
        return false;
    }

    Connection::set_epollfd(_epollfd);
    
    if (!FdHandler::add_fd(_epollfd, _listenfd, false)) {
        LOG(WARNING) << "[Server::run]: add_fd failed";
        return false;
    }

    _is_running = true;

    while (_is_running) {
        int event_num = epoll_wait(_epollfd, events, _max_events, -1);
        if (event_num == -1 && errno != EINTR) {
            LOG(WARNING) << "[Server::run]: epoll_wait failed";
            return false;
        }

        for (int i = 0; i < event_num; ++i) {
            if (!_handle_event(events[i])) {
                LOG(WARNING) << "[Server::run]: _handle_event failed";
                return false;
            }
        }
    }
    return true;
}

bool Server::_handle_event(const epoll_event& ev) {
    if (ev.data.fd == _listenfd) {
        // 新的连接
        if (_is_running) {
            if (!_handle_accept()) {
                LOG(WARNING) << "[Server::_handle_event]: _handle_accept failed";
                return false;
            }
        } else {
            // 新的连接过来的时候有可能已经停止服务
            LOG(INFO) << "[Server::_handle_event]: server has been closed "
                    << "reject new connection. fd = " << ev.data.fd;
        }
    } else if (ev.events & EPOLLIN) {
        // 发生可读事件
        if (!_handle_readable(ev)) {
            LOG(WARNING) << "[Server::_handle_event]: _handle_readable failed";
            return false;
        }
    } else if (ev.events & EPOLLOUT) {
        // 发生可写事件
        if (!_handle_writable(ev)) {
            LOG(WARNING) << "[Server::_handle_event]: _handle_writable failed";
            return false;
        }
    } else if (ev.events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        if (!_close_connection(ev.data.fd)) {
            LOG(WARNING) << "[Server::_handle_event]: _close_connection";
            return false;
        }
    } else {
        // 出错
        LOG(WARNING) << "[Server::_handle_event]: unkown event happened";
        return false;
    }
    return true;
}

bool Server::_handle_accept() {
    sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int connfd = accept(_listenfd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
    if (connfd == -1) {
        LOG(WARNING) << "[Server::_handle_accept]: accept failed";
        return false;
    }


    // 初始化该描述符对应的连接，如果不存在则新建
    if (_connections.find(connfd) == _connections.end() ||
            _connections[connfd] == nullptr) {
        // 描述符无对应的连接，或者连接池中无该描述符
        // 使用prototype模式来实例化
        _connections[connfd] = Connection::new_instance();
        if (_connections[connfd] == nullptr) {
            LOG(WARNING) << "[Server::_handle_accept]: alloc memory failed";
            return false;
        }
    } else {
        // 连接池中存在该描述符对应的连接, 就将其关闭
        _connections[connfd]->connection_close(); 
    }

    _connections[connfd]->set_connfd(connfd); 
    if (!FdHandler::add_fd(_epollfd, connfd, true)) {
        LOG(WARNING) << "[Server::_handle_accept]: add_fd failed";
        return false;
    }
    return true;
}

bool Server::_handle_readable(const epoll_event& ev) {
    int sockfd = ev.data.fd;
    // 判断发生事件的描述符是否在连接池当中
    if (_connections.find(sockfd) == _connections.end() ||
            _connections[sockfd] == nullptr) {
        LOG(WARNING) << "[Server::_handle_readable]: can not find fd:" <<
                sockfd << " in connection pool";
        return false;
    }

    if (!_connections[sockfd]->connection_read()) {
        LOG(WARNING) << "[Server::_handle_readable]: connection read failed";
        if (!_close_connection(sockfd)) {
            LOG(WARNING) << "[Server::_handle_readable]: _close_connection failed";
        }
        return false;
    }
    if (!_threadpool.append(_connections[sockfd])) {
        LOG(WARNING) << "[Server::_handle_readable]: threadpool add_connection failed";
        if (!_close_connection(sockfd)) {
            LOG(WARNING) << "[Server::_handle_readable]: _close_connection failed";
        }
        return false;
    }
    return true;
}
    
bool Server::_handle_writable(const epoll_event& ev) {
    int sockfd = ev.data.fd;
    if (_connections.find(sockfd) == _connections.end() ||
            _connections[sockfd] == nullptr) {
        LOG(WARNING) << "[Server::_handle_writable]: can not find fd:" <<
                sockfd << " in connection pool";
        return false;
    }

    if (!_connections[sockfd]->connection_write()) {
        LOG(WARNING) << "[Server::_handle_writable]: connection write failed";
        if (!_close_connection(sockfd)) {
            LOG(WARNING) << "[Server::_handle_writable]: _close_connection failed";
        }
        return false;
    }

    /*
    if (!_connections[sockfd]->connection_close()) {
        LOG(WARNING) << "[Server::_handle_writable]: connection close failed";
        return false;
    }
    */
    return true;
}

} // namepace thanos
