#ifndef KUNGFU_BINANCE_EXT_TRADER_H
#define KUNGFU_BINANCE_EXT_TRADER_H

#include "kungfu/common.h"
#include "kungfu/wingchun/broker/broker.h"
#include <kungfu/wingchun/broker/trader.h>
#include <string>

#include "binance_restful_client.hpp"
#include "binance_websocket_client.hpp"
#include "buffer_data.h"

using namespace kungfu::longfist;
using namespace kungfu::longfist::types;

namespace kungfu::wingchun::binance {

struct TDConfiguration {
  std::string account_id;
  std::string market_type;
  std::string api_key;
  std::string api_secret;
  bool testnet;
  bool sync_external_order;
};

inline void from_json(const nlohmann::json &j, TDConfiguration &c) {
  j.at("account_id").get_to(c.account_id);
  j.at("market_type").get_to(c.market_type);
  j.at("api_key").get_to(c.api_key);
  j.at("api_secret").get_to(c.api_secret);
  j.at("testnet").get_to(c.testnet);
  j.at("sync_external_order").get_to(c.sync_external_order);
}

class TraderBinance : public broker::Trader, public BinanceWebsocketClient, public BinanceRESTfulClient {
public:
  explicit TraderBinance(broker::BrokerVendor &vendor);

  ~TraderBinance() override;

  [[nodiscard]] longfist::enums::AccountType get_account_type() const override {
    return longfist::enums::AccountType::Future;
  }

  void pre_start() override;
  void on_start() override;
  void on_exit() override;

  bool insert_order(const event_ptr &event) override;
  bool cancel_order(const event_ptr &event) override;
  bool req_position() override;
  bool req_account() override;
  bool req_history_order(const event_ptr &event) override;
  bool req_history_trade(const event_ptr &event) override;
  void on_recover() override;

  void query_listenkey();
  void query_account();

  // web callbacks
  void on_ws_message(const std::string &sessionName, std::string &msg) override;
  void on_ws_close(const std::string &sessionName) override;
  void on_restful_message(const std::string &msg) override;

  bool on_custom_event(const event_ptr &event) override;

  void sign_request(std::string &target, RequestParams params);

  // rest callbacks
  void on_send_order(const std::string &msg, const std::string &extra);
  void on_query_account(const std::string &msg);
  void on_cancel_order(const std::string &msg);
  void on_query_listenkey(const std::string &msg);
  void on_renew_listenkey(const std::string &msg);

  // custom websocket callbacks
  bool custom_on_ws_event(const event_ptr &event);

  // custom rest callbacks
  bool custom_on_send_order_event(const event_ptr &event);
  bool custom_on_query_account_event(const event_ptr &event);
  bool custom_on_cancel_order_event(const event_ptr &event);
  bool custom_on_query_listenkey_event(const event_ptr &event);
  bool custom_on_renew_listenkey_event(const event_ptr &event);

  // instruments
  std::string binance_to_kf_instrument(std::string ba_instrument) {
    auto instrument_iter = ba_kf_instrument_map_.find(ba_instrument);
    std::string instrument = instrument_iter != ba_kf_instrument_map_.end() ? instrument_iter->second : ba_instrument;
    return instrument;
  }
  std::string kf_to_binance_instrument(std::string kf_instrument) {
    auto instrument_iter = kf_ba_instrument_map_.find(kf_instrument);
    std::string instrument = instrument_iter != kf_ba_instrument_map_.end() ? instrument_iter->second : kf_instrument;
    return instrument;
  }

private:
  TDConfiguration config_{};
  std::string market_path_;
  std::string ep_version_;
  kungfu::array<char, EXCHANGE_ID_LEN> exchange_id_;
  longfist::enums::InstrumentType instrument_type_;

  net::io_context ioc_;
  ssl::context ctx_{ssl::context::sslv23_client};
  std::thread io_thread_;
  void runIoContext() {
    while (true) {
      if (!ioc_.run_one()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
  }

  inline static std::shared_ptr<TraderBinance> td_;

  std::unordered_map<uint64_t, uint64_t> map_kf_to_binance_order_id_;
  std::unordered_map<uint64_t, uint64_t> map_binance_to_kf_order_id_;

  std::unordered_map<std::string, std::string> kf_ba_instrument_map_;
  std::unordered_map<std::string, std::string> ba_kf_instrument_map_;

  void generate_trade(const longfist::types::Order &order, uint32_t dest_id);

  bool verify_order(longfist::types::Order &order);

  void trigger_start(uint64_t trigger_id);

  void cancel_order(uint64_t order_id);
};

} // namespace kungfu::wingchun::binance

#endif