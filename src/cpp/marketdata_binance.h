#ifndef KUNGFU_BINANCE_MARKET_DATA_H
#define KUNGFU_BINANCE_MARKET_DATA_H

#include "binance_restful_client.hpp"
#include "binance_websocket_client.hpp"
#include "boost/asio/io_context.hpp"
#include "kungfu/longfist/types.h"
#include <kungfu/wingchun/broker/marketdata.h>
#include <kungfu/yijinjing/common.h>
#include <string>
#include <unordered_map>
using namespace kungfu::longfist;
using namespace kungfu::longfist::types;

namespace kungfu::wingchun::binance {

struct MDConfiguration {
  std::string market_type;
};

inline void from_json(const nlohmann::json &j, MDConfiguration &c) { j.at("market_type").get_to(c.market_type); }

class MarketDataBinance : public broker::MarketData, public BinanceWebsocketClient, public BinanceRESTfulClient {
public:
  explicit MarketDataBinance(broker::BrokerVendor &vendor);

  ~MarketDataBinance() override;

  bool subscribe(const std::vector<longfist::types::InstrumentKey> &instrument_keys) override;

  bool subscribe_all() override;
  bool subscribe_custom(const longfist::types::CustomSubscribe &custom_sub) override;
  bool unsubscribe(const std::vector<longfist::types::InstrumentKey> &instrument_keys) override;
  void on_band(const event_ptr &event) override;

  void on_ws_message(const std::string &sessionName, std::string &msg) override;
  void on_ws_close(const std::string &sessionName) override;
  void on_restful_message(const std::string &msg) override;

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

protected:
  void on_start() override;

  void pre_start() override;

private:
  MDConfiguration config_;
  std::string market_path_;
  kungfu::array<char, EXCHANGE_ID_LEN> exchange_id_;

  longfist::enums::InstrumentType instrument_type_;
  net::io_context ioc_;
  ssl::context ctx_{ssl::context::tlsv12_client};
  std::thread io_thread_;
  net::io_context::work work_;
  void runIoContext() { ioc_.run(); }
  std::unordered_map<std::string, std::string> kf_ba_instrument_map_;
  std::unordered_map<std::string, std::string> ba_kf_instrument_map_;
  std::unordered_map<std::string, longfist::types::Transaction> transaction_map_; // cache for quote last_price

  uint32_t transaction_band_uid_{};
  uint32_t tick_band_uid_{};
  yijinjing::journal::writer_ptr public_writer_;
  
};
} // namespace kungfu::wingchun::binance

#endif // KUNGFU_BINANCE_MARKET_DATA_H
