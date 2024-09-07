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
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
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
  ssl::stream<tcp::socket> stream_;
  beast::flat_buffer buffer_; // (Must persist between reads)
  std::string host_;
  std::string port_;
  std::string hostAndPort_;

  http::request<http::empty_body> req_;
  http::response<http::string_body> res_;

  RESTfulCallbacks &callbacks_;

  bool terminate_ = false;
  boost::asio::ip::basic_resolver_results<boost::asio::ip::tcp> results_;
  Headers extra_http_headers_;
  RequestMassage msg_{};
  std::string extra_;
  std::function<void(const std::string &)> callback_method1_;
  std::function<void(const std::string &, const std::string &)> callback_method2_;

public:
  explicit RESTfulClient(RESTfulCallbacks &callbacks, net::io_context &ioc, ssl::context &ctx, std::string host,
                         std::string port, Headers headers)
      : callbacks_(callbacks), ioc_(ioc), ctx_(ctx), resolver_(ioc), host_(host), port_(port), stream_(ioc, ctx),
        extra_http_headers_(headers) {}

  http::response<http::string_body> do_request() {
    results_ = resolver_.resolve(host_, port_);
    net::connect(stream_.next_layer(), results_.begin(), results_.end());
    stream_.set_verify_callback(ssl::host_name_verification(host_));
    if (!SSL_set_tlsext_host_name(stream_.native_handle(), host_.c_str())) {
      boost::system::error_code ec{static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category()};
      throw boost::system::system_error{ec};
    }
    stream_.handshake(ssl::stream_base::client);
    SPDLOG_DEBUG(msg_.target);
    http::request<http::string_body> req{msg_.method, msg_.target, 11};
    req.set(http::field::host, host_);
    req.set(http::field::user_agent, "Boost Beast");

    for (auto &header : extra_http_headers_) {
      req.insert(header.first, header.second);
    }
    http::write(stream_, req);

    http::response<http::string_body> res;
    http::read(stream_, buffer_, res);
    return res;
  }

  void request(RequestMethod method, std::string target, std::function<void(const std::string &)> callback) {
    callback_method1_ = callback;
    msg_.method = method;
    msg_.target = target;
    auto res = do_request();
    callback_method1_(res.body());
  }

  void request(RequestMethod method, std::string target,
               std::function<void(const std::string &, const std::string &)> callback, std::string extra) {
    extra_ = extra;
    callback_method2_ = callback;
    msg_.method = method;
    msg_.target = target;
    auto res = do_request();
    callback_method2_(res.body(), extra_);
  }
};

#endif