#ifndef WEBSOCKETCLIENT_HPP
#define WEBSOCKETCLIENT_HPP

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <chrono>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl;       // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
using Stream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

class WebsocketCallbacks {
public:
  virtual void on_ws_close(const std::string &sessionName) = 0;
  virtual void on_ws_message(const std::string &sessionName, std::string &msg) = 0;
};

class WebsocketClient : public std::enable_shared_from_this<WebsocketClient> {
  net::io_context &ioc_;
  ssl::context &ctx_;
  tcp::resolver resolver_;
  std::shared_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;
  beast::flat_buffer buffer_;
  std::string host_;
  std::string port_;
  std::string handshake_target_;
  WebsocketCallbacks &callbacks_;
  std::string sessionIdenfitier_;
  
  std::chrono::steady_clock::time_point lastMessageTime;
  std::chrono::seconds timeoutPeriod;
  std::thread timerThread;
  bool timerRunning;

public:
  explicit WebsocketClient(WebsocketCallbacks &callbacks, std::string &sessionIdenfitier, net::io_context &ioc,
                           ssl::context &ctx, std::string host, std::string port, char const *target)
      : callbacks_(callbacks), sessionIdenfitier_(sessionIdenfitier), ioc_(ioc), ctx_(ctx), resolver_(ioc_),
        host_(host), port_(port), handshake_target_(target) {}

  void start_ws_connection() {
    resolver_.async_resolve(host_, port_, beast::bind_front_handler(&WebsocketClient::on_resolve, shared_from_this()));
  }

  void stop_ws_connection() {
    if (ws_) {
      ws_->close(websocket::close_code::normal);
    }
    timerRunning = false;
    if (timerThread.joinable()) {
      timerThread.join();
    }
  }

  void strat_timer_check(int seconds) {
    startTimer(seconds);
  }



private:
  void startTimer(int seconds) {
    timeoutPeriod = std::chrono::seconds(seconds);
    lastMessageTime = std::chrono::steady_clock::now();
    timerRunning = true;
    timerThread = std::thread([this]() {
      while (timerRunning) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        if (std::chrono::steady_clock::now() - lastMessageTime > timeoutPeriod) {
          SPDLOG_ERROR("{} no msg receive error", sessionIdenfitier_);
          timerRunning = false;
          callbacks_.on_ws_close(sessionIdenfitier_);
        }
      }
    });
  }


  void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
      SPDLOG_ERROR("Resolve error: {}", ec.message());
      callbacks_.on_ws_close(sessionIdenfitier_);
      return;
    }

    ws_ = std::make_shared<websocket::stream<beast::ssl_stream<tcp::socket>>>(ioc_, ctx_);
    net::async_connect(get_lowest_layer(*ws_), results,
                       beast::bind_front_handler(&WebsocketClient::on_connect, shared_from_this()));
  }

  void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
    if (ec) {
      SPDLOG_ERROR("Connect error: {}", ec.message());
      callbacks_.on_ws_close(sessionIdenfitier_);
      return;
    }

    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
      ec = beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
      SPDLOG_ERROR("Failed to set SNI Hostname: {}", ec.message());
      callbacks_.on_ws_close(sessionIdenfitier_);
      return;
    }

    ws_->next_layer().async_handshake(
        ssl::stream_base::client, beast::bind_front_handler(&WebsocketClient::on_ssl_handshake, shared_from_this()));
  }

  void on_ssl_handshake(beast::error_code ec) {
    if (ec) {
      SPDLOG_ERROR("SSL Handshake error: {}", ec.message());
      callbacks_.on_ws_close(sessionIdenfitier_);
      return;
    }

    ws_->set_option(websocket::stream_base::decorator([](websocket::request_type &req) {
      req.set(http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " websocket-client-async-ssl");
    }));

    ws_->async_handshake(host_ + ':' + port_, handshake_target_,
                         beast::bind_front_handler(&WebsocketClient::on_handshake, shared_from_this()));
  }

  void on_handshake(beast::error_code ec) {
    if (ec) {
      SPDLOG_ERROR("Handshake error: {}", ec.message());
      callbacks_.on_ws_close(sessionIdenfitier_);
      return;
    }

    read_message();
  }

  void read_message() {
    ws_->async_read(buffer_, beast::bind_front_handler(&WebsocketClient::on_read, shared_from_this()));
  }

  void on_read(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec) {
      if (ec == websocket::error::closed) {
        SPDLOG_INFO("Socket was closed.");
      } else {
        SPDLOG_ERROR("Read error: {}", ec.message());
        SPDLOG_ERROR("last msg {}", boost::beast::buffers_to_string(buffer_.data()));
      }
      callbacks_.on_ws_close(sessionIdenfitier_);
      return;
    }

    lastMessageTime = std::chrono::steady_clock::now();

    std::string message = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());
    callbacks_.on_ws_message(sessionIdenfitier_, message);

    read_message();
  }
};

#endif