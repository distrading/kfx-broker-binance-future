#ifndef RESTFULCLIENT_HPP
#define RESTFULCLIENT_HPP

#include "boost/asio/ssl/stream_base.hpp"
#include "boost/asio/ssl/verify_mode.hpp"
#include "boost/beast/http/string_body.hpp"
#include "root_certificates.hpp"

#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>

namespace beast = boost::beast;   // from <boost/beast.hpp>
namespace http = beast::http;     // from <boost/beast/http.hpp>
namespace net = boost::asio;      // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl; // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>
using RequestParams = std::unordered_map<std::string, std::string>;
using RequestMethod = http::verb;
typedef std::map<std::string, std::string> Headers;

class RESTfulCallbacks {
public:
  // a message received on the socket
  virtual void on_restful_message(const std::string &msg) = 0;
};

struct RequestMassage {
  RequestMethod method;
  std::string target;
};

class RESTfulClient : public std::enable_shared_from_this<RESTfulClient> {
  net::io_context &ioc_;
  ssl::context &ctx_;
  tcp::resolver resolver_;
  beast::ssl_stream<beast::tcp_stream> stream_;
  beast::flat_buffer buffer_; // (Must persist between reads)
  std::string host_;
  std::string port_;
  std::string hostAndPort_;

  http::request<http::empty_body> req_;
  http::response<http::string_body> res_;

  RESTfulCallbacks &callbacks_;
  // RequestParams params_;
  std::thread workerThread_;

  bool terminate_ = false;
  boost::asio::ip::basic_resolver_results<boost::asio::ip::tcp> results_;
  Headers extra_http_headers_;
  std::string extra_;
  std::function<void(const std::string &)> callback_method1_;
  std::function<void(const std::string &, const std::string &)> callback_method2_;
  RequestMassage req_msg_{};

public:
  explicit RESTfulClient(RESTfulCallbacks &callbacks, net::io_context &ioc, ssl::context &ctx, std::string host,
                         std::string port, Headers headers)
      : callbacks_(callbacks), ioc_(ioc), ctx_(ctx), resolver_(ioc), host_(host), port_(port), stream_(ioc, ctx),
        extra_http_headers_(headers) {}

  void ConnectREST() {
    SPDLOG_INFO("ConnectREST");
    SPDLOG_INFO(host_);

    results_ = resolver_.resolve(host_, port_);
  }

  void make_request() {
    SPDLOG_TRACE("make_request {}", req_msg_.target);
    // Set SNI Hostname (many hosts need this to handshake successfully)
    if (!SSL_set_tlsext_host_name(stream_.native_handle(), host_.c_str())) {
      beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
      std::cerr << ec.message() << "\n";
      return;
    }

    req_.version(11);
    req_.method(req_msg_.method);
    req_.target(req_msg_.target);
    req_.set(http::field::host, host_);
    req_.set(http::field::user_agent, "Boost Beast");

    for (auto &header : extra_http_headers_) {
      req_.insert(header.first, header.second);
    }

    resolver_.async_resolve(host_, port_, beast::bind_front_handler(&RESTfulClient::on_resolve, shared_from_this()));
  }

  void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    // SPDLOG_DEBUG("on_resolve");

    if (ec)
      return SPDLOG_ERROR("resolve {}", ec.message());

    // Set a timeout on the operation
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

    // Make the connection on the IP address we get from a lookup
    beast::get_lowest_layer(stream_).async_connect(
        results, beast::bind_front_handler(&RESTfulClient::on_connect, shared_from_this()));
  }

  void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
    // SPDLOG_DEBUG("on_connect");

    if (ec)
      return SPDLOG_ERROR("connect {}", ec.message());

    // Perform the SSL handshake
    stream_.async_handshake(ssl::stream_base::client,
                            beast::bind_front_handler(&RESTfulClient::on_handshake, shared_from_this()));
  }

  void on_handshake(beast::error_code ec) {
    // SPDLOG_DEBUG("on_handshake");

    if (ec)
      return SPDLOG_ERROR("on_handshake {}", ec.message());

    // Set a timeout on the operation
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

    // Send the HTTP request to the remote host
    http::async_write(stream_, req_, beast::bind_front_handler(&RESTfulClient::on_write, shared_from_this()));
  }
  void on_write(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec)
      return SPDLOG_ERROR("on_write {}", ec.message());

    // Receive the HTTP response
    http::async_read(stream_, buffer_, res_, beast::bind_front_handler(&RESTfulClient::on_read, shared_from_this()));
  }

  void on_read(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec)
      return SPDLOG_ERROR("on_read {}", ec.message());

    // callbacks_.on_restful_message(res_.body());

    if (extra_.empty()) {
      callback_method1_(res_.body());
    } else {
      callback_method2_(res_.body(), extra_);
    }

    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(5));

    // Gracefully close the stream
    stream_.async_shutdown(beast::bind_front_handler(&RESTfulClient::on_shutdown, shared_from_this()));
  }

  void on_shutdown(beast::error_code ec) {

    if (ec != net::ssl::error::stream_truncated)
      return SPDLOG_ERROR("on_shutdown {}", ec.message());

    // If we get here then the connection is closed gracefully
  }

  void request(RequestMethod method, std::string target, std::function<void(const std::string &)> callback) {
    callback_method1_ = callback;

    req_msg_.method = method;
    req_msg_.target = target;
    make_request();
    // workerThread_ = std::thread([this] { make_request(); });
  }

  void request(RequestMethod method, std::string target,
               std::function<void(const std::string &, const std::string &)> callback, std::string extra) {
    callback_method2_ = callback;
    extra_ = extra;
    req_msg_.method = method;
    req_msg_.target = target;
    make_request();
    // workerThread_ = std::thread([this] { make_request(); });
  }
};

#endif