#ifndef WEBSOCKETCLIENT_HPP
#define WEBSOCKETCLIENT_HPP

#include "boost/asio/io_context.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <iostream>

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
  std::string sessionIdenfitier_;

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
        } else { // termination has been requested
          ws_->close(boost::beast::websocket::close_code::normal);
          terminating = true; // we could break out of while here, but then we would not read any final message from
                              // cpty after we send close?
        }
      }
    } catch (beast::system_error const &se) {
      if (se.code() == websocket::error::closed) {
        SPDLOG_INFO("{} socket was closed.",sessionIdenfitier_);
      } else {
        SPDLOG_INFO("exception: {} {}",sessionIdenfitier_, se.code().message());
      }
      callbacks_.on_ws_close(sessionIdenfitier_);
    } catch (std::exception &ex) {
      SPDLOG_ERROR("exception: {} {} ",sessionIdenfitier_, ex.what());
      callbacks_.on_ws_close(sessionIdenfitier_);
    }
  }

  void stop_ws_connection() {
    terminate_ = true;
    if (workerThread_.joinable()) {
      workerThread_.join();
    }
  }

  void start_ws_connection() {
    workerThread_ = std::thread([this] {
      while (!terminate_) {

        ws_ = std::make_shared<websocket::stream<beast::ssl_stream<tcp::socket>>>(ioc_, ctx_);

        try {
          // initalizes websocket and polls continuously for messages until socket closed or an exception occurs
          ConnectAndReceive();
        }
        // catches network or ssl handshake errors in attemting to establish the websocket
        catch (beast::system_error const &se) {

          callbacks_.on_ws_close(sessionIdenfitier_); // notify failed connection

          SPDLOG_INFO("exception: {}", se.code().message());
        }

        ws_.reset();
      }
      SPDLOG_INFO("ws Stopped.");
    });
  }
};

#endif