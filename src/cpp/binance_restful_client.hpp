
#ifndef BINANCE_RESTFUL_CLIENT_HPP
#define BINANCE_RESTFUL_CLIENT_HPP

// #include "base_client/restful_client.hpp"
#include "base_client/restful_client_async.hpp"
#include "boost/asio/io_context.hpp"
#include "spdlog/spdlog.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

using RequestParams = std::unordered_map<std::string, std::string>;

typedef std::map<std::string, std::string> Headers;

class BinanceRESTfulClient : public RESTfulCallbacks {
  std::string host_ = "fapi.binance.com";
  std::string port_ = "443";
  net::io_context &ioc_;
  ssl::context &ctx_;
  RequestParams params_;
  Headers headers_;

public:
  BinanceRESTfulClient(net::io_context &ioc, ssl::context &ctx) : ioc_(ioc), ctx_(ctx){};

  ~BinanceRESTfulClient(){};

  void set_headers(Headers headers) { headers_ = headers; }

  void set_rest_path(std::string host, std::string port) {
    host_ = host;
    port_ = port;
  }

  void restful_request(RequestMethod method, std::string target, std::function<void(const std::string &)> callback) {
    auto restful_client = std::make_shared<RESTfulClient>(*this, ioc_, ctx_, host_, port_, headers_);
    SPDLOG_DEBUG("restful_request thread_ target: {}", target);
    restful_client->request(method, target, callback);
    return;
  }

  void restful_request(RequestMethod method, std::string target,
                       std::function<void(const std::string &, const std::string &)> callback, std::string extra) {
    auto restful_client = std::make_shared<RESTfulClient>(*this, ioc_, ctx_, host_, port_, headers_);
    SPDLOG_DEBUG("restful_request thread_ target: {}", target);
    restful_client->request(method, target, callback, extra);
    return;
  }
};

#endif