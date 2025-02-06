
#ifndef BINANCE_WEBSOCKET_CLIENT_HPP
#define BINANCE_WEBSOCKET_CLIENT_HPP

// #include "base_client/websocket_client.hpp"
#include "base_client/websocket_client_async.hpp"
#include "boost/asio/io_context.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <map>
#include <string>

class BinanceWebsocketClient : public WebsocketCallbacks {
  std::string host_ = "fstream.binance.com";
  std::string port_ = "443";

  net::io_context &ioc_;
  ssl::context &ctx_;
  std::map<std::string, std::shared_ptr<WebsocketClient>> subscribe_map_;
  std::map<std::string, std::shared_ptr<WebsocketClient>> user_stream_;


public:
  BinanceWebsocketClient(net::io_context &ioc, ssl::context &ctx) : ioc_(ioc), ctx_(ctx){};

  ~BinanceWebsocketClient() {}

  void stop_ws_client() {
    for (auto &x : subscribe_map_) {
      x.second->stop_ws_connection();
      SPDLOG_INFO("stoping {} stream.", x.first);
    }
  }

  void set_ws_path(std::string host, std::string port) {
    host_ = host;
    port_ = port;
  }

  void subscribe_instrument(std::string instrument) {
    // std::string target = "/ws/" + instrument + "@depth10@500ms";
    std::string target =
        "/stream?streams=" + instrument + "@trade/" + instrument + "@depth10@100ms/" + instrument + "@bookTicker";
    if (subscribe_map_.find(instrument) == subscribe_map_.end()) {
      subscribe_map_[instrument] =
          std::make_shared<WebsocketClient>(*this, instrument, ioc_, ctx_, host_, port_, target.c_str());
      subscribe_map_[instrument]->strat_timer_check(30);
      subscribe_map_[instrument]->start_ws_connection();
      SPDLOG_TRACE("subscribe {} {} {}", host_, target, instrument);
    } else {
      SPDLOG_TRACE("{} already subscribed.", instrument);
    }
  }

  void resubscribe_instrument(std::string instrument) {
    // std::string target = "/ws/" + instrument + "@depth10@500ms";
    std::string target =
        "/stream?streams=" + instrument + "@trade/" + instrument + "@depth10@100ms/" + instrument + "@bookTicker";
    subscribe_map_[instrument] =
        std::make_shared<WebsocketClient>(*this, instrument, ioc_, ctx_, host_, port_, target.c_str());
    subscribe_map_[instrument]->strat_timer_check(30);
    subscribe_map_[instrument]->start_ws_connection();
    SPDLOG_TRACE("resubscribe {} {} {}", host_, target, instrument);

  }

  void unsubscribe_instrument(std::string instrument) {
    if (subscribe_map_.find(instrument) != subscribe_map_.end()) {
      subscribe_map_[instrument]->stop_ws_connection();
      subscribe_map_.erase(instrument);
    } else {
      SPDLOG_INFO("{} not found.", instrument);
    }
  }

  void subscribe_user_stream(std::string listenKey) {
    std::string target = "/ws/" + listenKey;
    user_stream_[listenKey] =
        std::make_shared<WebsocketClient>(*this, listenKey, ioc_, ctx_, host_, port_, target.c_str());
    user_stream_[listenKey]->strat_timer_check(600);
    user_stream_[listenKey]->start_ws_connection();
  }

  void unsubscribe_user_stream(std::string listenKey) {
    if (user_stream_.find(listenKey) != user_stream_.end()) {
      user_stream_[listenKey]->stop_ws_connection();
      user_stream_.erase(listenKey);
    } else {
      SPDLOG_INFO("{} not found.", listenKey);
    }
  }
  // void update_listenKey() {}
};
#endif