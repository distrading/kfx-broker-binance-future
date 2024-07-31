#ifndef WEBSOCKETCLIENT_HPP
#define WEBSOCKETCLIENT_HPP

#include "boost/asio/io_context.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>

#include <spdlog/spdlog.h>
#include <string>
#include <sys/types.h>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl;       // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
using Stream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

class WebsocketCallbacks {
public:
  // notifies of a connection failure - socket disconnect, problem passing message, socket closed etc
  virtual void on_ws_close(const std::string &sessionName) = 0;

  // a message received on the socket
  virtual void on_ws_message(const std::string &sessionName, std::string &msg) = 0;
};

class WebsocketClient : public std::enable_shared_from_this<WebsocketClient> {
  net::io_context &ioc_;
  ssl::context &ctx_;
  tcp::resolver resolver_;
  std::shared_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;
  beast::flat_buffer buffer_;
  std::string host_;
  std::string hostAndPort_;
  std::string port_;
  std::string handshake_target_;
  std::string text_;
  tcp::endpoint tcpEndPoint_;
  bool terminate_ = false;
  WebsocketCallbacks &callbacks_;
  std::thread workerThread_;
  std::thread monitThread_;
  std::string sessionIdenfitier_;

  std::mutex mutex_;
  std::condition_variable cv_;



public:
  explicit WebsocketClient(WebsocketCallbacks &callbacks, std::string &sessionIdenfitier, net::io_context &ioc,
                           ssl::context &ctx, std::string host, std::string port, char const *target)
      : callbacks_(callbacks), sessionIdenfitier_(sessionIdenfitier), ioc_(ioc), ctx_(ctx), resolver_(ioc_),
        host_(host), port_(port), handshake_target_(target), hostAndPort_() {}

  void ConnectAndReceive() {
    
    auto const tcpResolverResults = resolver_.resolve(host_, port_);

    tcpEndPoint_ = net::connect(get_lowest_layer(*ws_), tcpResolverResults);

    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
      throw beast::system_error(beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()),
                                "Failed to set SNI Hostname");
    }
    hostAndPort_ = host_ + ':' + std::to_string(tcpEndPoint_.port());

    ws_->next_layer().handshake(ssl::stream_base::client);

    ws_->set_option(websocket::stream_base::decorator([](websocket::request_type &req) {
      req.set(http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " websocket-client-async-ssl");
    }));

    ws_->handshake(hostAndPort_, handshake_target_);

    // 读取回调
    try {
      bool terminating = false;
      while (true) // expect to read continuously until a connection failure or manually terminated
      {
        if (!terminate_ || terminating) {
          ws_->read(buffer_);

          if (buffer_.size() > 0) {
            std::string bufferString = boost::beast::buffers_to_string(buffer_.data());
            callbacks_.on_ws_message(sessionIdenfitier_, bufferString);
          }
          buffer_.consume(buffer_.size()); // Clear the buffer
        }

      }
      // ws_->close(boost::beast::websocket::close_code::normal);

    } catch (beast::system_error const &se) {
      if (se.code() == websocket::error::closed) {
        SPDLOG_ERROR("{} socket was closed.", sessionIdenfitier_);
      } else {
        SPDLOG_ERROR("{} beast::exception:  {}", sessionIdenfitier_, se.code().message());
        SPDLOG_ERROR("last msg {}", boost::beast::buffers_to_string(buffer_.data()));
        buffer_.consume(buffer_.size());
      }
      throw;
    } catch (std::exception &ex) {
      SPDLOG_ERROR("std::exception: {} {} ", sessionIdenfitier_, ex.what());
      throw;
    }
  }

  void stop_ws_connection() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      terminate_ = true;
    }
    cv_.notify_all();
    if (workerThread_.joinable()) {
      workerThread_.join();
    }
    if (monitThread_.joinable()) {
      monitThread_.join();
    }

  }


  void workerThreadFunc() {
    while (!terminate_) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ws_ = std::make_shared<websocket::stream<beast::ssl_stream<tcp::socket>>>(ioc_, ctx_);
      }
      try {
        ConnectAndReceive();
      } catch (beast::system_error const &se) {
        SPDLOG_ERROR(" {} workerThreadFunc exception: {}", sessionIdenfitier_, se.code().message());
        std::this_thread::sleep_for(std::chrono::seconds(2)); // 重试
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ws_.reset();
      }
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return terminate_; });
    }

  }

  void monitThreadFunc() {
    while (true) {
      try {
        workerThread_.join();
        SPDLOG_INFO(" workerThread_ joined!");
      } catch (const std::exception& e) {
        SPDLOG_ERROR(" monitor Thread exception: {}", e.what());
        workerThread_ = std::thread([this] {
          workerThreadFunc();
        });
      }
    }
    SPDLOG_INFO(" start_ws_connection close");
  }

  void start_ws_connection() {
    workerThread_ = std::thread([this] {
      workerThreadFunc();
    });
    SPDLOG_TRACE("{} workerThread_ created!", sessionIdenfitier_);
    monitThread_ = std::thread([this] {
      monitThreadFunc();
    });
    SPDLOG_TRACE("{} monitThread_ created!", sessionIdenfitier_);

  }
};

#endif