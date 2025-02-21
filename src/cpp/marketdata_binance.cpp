#include "marketdata_binance.h"
#include "base_client/root_certificates.hpp"
#include "kungfu/longfist/enums.h"
#include "kungfu/longfist/types.h"
#include "kungfu/wingchun/common.h"
#include "spdlog/spdlog.h"
#include <cstdio>
#include <cstring>
#include "type_convert.h"

#include "rapidjson/document.h" // rapidjson's DOM-style API

using namespace kungfu::yijinjing;
using namespace kungfu::yijinjing::data;
using namespace kungfu::longfist::types;
using namespace kungfu::longfist::enums;
using json = nlohmann::json;
using namespace rapidjson;

namespace kungfu::wingchun::binance {
MarketDataBinance::MarketDataBinance(broker::BrokerVendor &vendor)
    : MarketData(vendor), ctx_(ssl::context::tlsv12_client), work_(ioc_),
      io_thread_(&MarketDataBinance::runIoContext, this), BinanceWebsocketClient(ioc_, ctx_),
      BinanceRESTfulClient(ioc_, ctx_) {
  KUNGFU_SETUP_LOG();
  SPDLOG_INFO("wait for http connect");
  std::this_thread::sleep_for(std::chrono::seconds(1));
}

MarketDataBinance::~MarketDataBinance() {
  SPDLOG_INFO(" ~MarketDataBinance");
  io_thread_.join();
}

void MarketDataBinance::on_start() {
  SPDLOG_INFO("MarketDataBinance on_start");

  update_broker_state(BrokerState::Ready);
  auto endpoint = market_path_ + "/v1/exchangeInfo";

  restful_request(RequestMethod::get, endpoint.c_str(),
                  std::bind(&MarketDataBinance::on_restful_message, this, std::placeholders::_1));
}

void MarketDataBinance::on_restful_message(const std::string &msg) {
  try {
    Document doc;
    doc.Parse(msg.c_str());
    if (doc.HasParseError()) {
      std::cerr << "Parse error: " << GetParseErrorFunc(doc.GetParseError()) << std::endl;
      return;
    }
    if (doc.HasMember("symbols")) {
      for (auto &item : doc["symbols"].GetArray()) {
        Instrument &instrument = public_writer_->open_data<Instrument>(0);

        from_binance(item, instrument);
        instrument.exchange_id = exchange_id_;
        instrument.instrument_type = instrument_type_;

        // instrument_id map kf: BTCUSD-PERP  binance: BTCUSD_PERP
        std::string instrument_id(item["symbol"].GetString());
        std::replace(instrument_id.begin(), instrument_id.end(), '_', '-');
        instrument.instrument_id = instrument_id.c_str();

        kf_ba_instrument_map_[instrument.instrument_id.to_string()] = item["symbol"].GetString();
        ba_kf_instrument_map_[item["symbol"].GetString()] = instrument.instrument_id.to_string();

        public_writer_->close_data();
      }
    }

  } catch (std::exception &ex) {
    SPDLOG_INFO("exception: {} ", ex.what());
    SPDLOG_INFO(msg);
  }
}

void MarketDataBinance::on_ws_message(const std::string &sessionName, std::string &msg) {
  // SPDLOG_DEBUG(msg);
  Document doc;
  ParseResult ok = doc.Parse(msg.c_str());

  if (!ok) {
    SPDLOG_ERROR("msg: {} code: {}", msg, ok.Code());
  }
  auto stream_name = doc["stream"].GetString();

  if (endswith(stream_name, "trade")) {
    // for 0 price/volume in trade
    auto price = std::stod(doc["data"]["p"].GetString());
    auto volume = std::stod(doc["data"]["q"].GetString());

    if (std::fabs(price) < 1e-9 || std::fabs(volume) < 1e-9) {
      // SPDLOG_WARN("0 in trade message {}", msg);
      return;
    }

    Transaction &transaction = public_writer_->open_data<Transaction>(0);

    transaction.price = price;
    transaction.volume = volume;
    transaction.exec_type = longfist::enums::ExecType::Trade;
    int64_t ts = doc["data"]["T"].GetInt64();
    transaction.data_time = ts * 1000000;
    bool is_buyer_maker = doc["data"]["m"].GetBool();
    transaction.side = is_buyer_maker ? longfist::enums::Side::Sell : longfist::enums::Side::Buy;

    transaction.instrument_id = binance_to_kf_instrument(doc["data"]["s"].GetString()).c_str();
    transaction.exchange_id = exchange_id_;
    transaction.instrument_type = instrument_type_;

    public_writer_->close_data();
    // SPDLOG_DEBUG("transaction: {}", transaction.to_string());
    transaction_map_[transaction.instrument_id] = transaction;

  } else if (endswith(stream_name, "depth10@100ms")) {

    Quote &quote = public_writer_->open_data<Quote>(0);
    from_binance(doc["data"], quote);

    quote.exchange_id = exchange_id_;
    quote.instrument_type = instrument_type_;
    quote.instrument_id = binance_to_kf_instrument(doc["data"]["s"].GetString()).c_str();

    // last_price from transaction cache
    auto transaction = transaction_map_.find(quote.instrument_id);
    if (transaction != transaction_map_.end()) {
      quote.last_price = transaction->second.price;
    }

    public_writer_->close_data();
    // SPDLOG_DEBUG("quote: {}", quote.to_string());
  } else if (endswith(stream_name, "bookTicker")) {
    Tick &tick = public_writer_->open_data<Tick>(0);
    from_binance(doc["data"], tick);

    tick.exchange_id = exchange_id_;
    tick.instrument_type = instrument_type_;

    tick.instrument_id = binance_to_kf_instrument(doc["data"]["s"].GetString()).c_str();

    public_writer_->close_data();
    // SPDLOG_DEBUG("tick: {}", tick.to_string());

  } else {
    SPDLOG_WARN("ws message not parse {}", msg);
  }
}

void MarketDataBinance::on_ws_close(const std::string &sessionName) {
  SPDLOG_DEBUG("websocket reconnect {}", sessionName);
  resubscribe_instrument(sessionName);
}

void MarketDataBinance::pre_start() {
  public_writer_ = get_public_writer();
  ctx_.set_verify_mode(ssl::verify_none);
  load_root_certificates(ctx_);
  config_ = nlohmann::json::parse(get_config());
  SPDLOG_INFO("config: {}", get_config());

  // set usd/coin market
  if (config_.market_type == "coin-market") {
    market_path_ = "/dapi";
    exchange_id_ = EXCHANGE_BINANCE_COIN_FUTURE;
    instrument_type_ = longfist::enums::InstrumentType::CryptoFuture;
    set_rest_path("dapi.binance.com", "443");
    set_ws_path("dstream.binance.com", "443");

  } else {
    market_path_ = "/fapi";
    exchange_id_ = EXCHANGE_BINANCE_USD_FUTURE;
    instrument_type_ = longfist::enums::InstrumentType::CryptoUFuture;
    set_rest_path("fapi.binance.com", "443");
    set_ws_path("fstream.binance.com", "443");
  }

  SPDLOG_INFO("MarketDataBinance pre_start");
}

// void MarketDataBinance::on_exit() { SPDLOG_INFO("MarketDataBinance on_exit"); }

bool MarketDataBinance::subscribe(const std::vector<longfist::types::InstrumentKey> &instrument_keys) {
  for (const auto &key : instrument_keys) {
    auto instrument = kf_to_binance_instrument(key.instrument_id.to_string());
    transform(instrument.begin(), instrument.end(), instrument.begin(), ::tolower);
    subscribe_instrument(instrument);
    // SPDLOG_TRACE("subscribe instrument_id: {}", instrument);
  }
  return true;
}

bool MarketDataBinance::subscribe_all() { return true; }

bool MarketDataBinance::subscribe_custom(const longfist::types::CustomSubscribe &custom_sub) {
  // SPDLOG_INFO("subscribe_custom");
  return false;
}

bool MarketDataBinance::unsubscribe(const std::vector<longfist::types::InstrumentKey> &instrument_keys) {
  for (const auto &key : instrument_keys) {

    auto instrument = kf_to_binance_instrument(key.instrument_id.to_string());
    transform(instrument.begin(), instrument.end(), instrument.begin(), ::tolower);
    unsubscribe_instrument(instrument);
    SPDLOG_INFO("unsubscribe_instrument_id: {}", key.to_string());
  }
  return true;
}

void MarketDataBinance::on_band(const event_ptr &event) { SPDLOG_INFO("on_band"); }

} // namespace kungfu::wingchun::binance
