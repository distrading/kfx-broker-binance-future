#ifndef KUNGFU_BINANCE_TYPE_CONVERT_H
#define KUNGFU_BINANCE_TYPE_CONVERT_H

#include "kungfu/longfist/enums.h"
#include "kungfu/longfist/types.h"
#include <cstdint>
#include <kungfu/longfist/longfist.h>
#include <kungfu/wingchun/common.h>
#include <kungfu/yijinjing/time.h>
// #include <nlohmann/json.hpp>
#include "rapidjson/document.h" // rapidjson's DOM-style API
#include <string>

using namespace kungfu::longfist;
using namespace kungfu::longfist::enums;
using namespace kungfu::longfist::types;
// using json = nlohmann::json;
using namespace rapidjson;

namespace kungfu::wingchun::binance {

inline void from_binance(const Value &symbol, Instrument &instrument) {
  auto pair = symbol["pair"].GetString();
  memcpy(instrument.product_id, pair, strlen(pair));

  for (auto &item : symbol["filters"].GetArray()) {
    if (string_equals(item["filterType"].GetString(), "PRICE_FILTER")) {
      instrument.price_tick = std::stod(item["tickSize"].GetString());
    }
    if (string_equals(item["filterType"].GetString(), "LOT_SIZE")) {
      instrument.quantity_unit = std::stod(item["stepSize"].GetString());
    }
  }
}

inline void from_binance(const Value &data, Quote &quote) {
  ///十档申买价
  double bid[10];
  ///十档申卖价
  double ask[10];
  ///十档申买量
  double bid_qty[10];
  ///十档申卖量
  double ask_qty[10];

  int i = 0;
  for (const auto &item : data["b"].GetArray()) {
    bid[i] = std::stod(item[0].GetString());
    bid_qty[i] = std::stod(item[1].GetString());
    i++;
  }
  int j = 0;

  for (const auto &item : data["a"].GetArray()) {
    ask[j] = std::stod(item[0].GetString());
    ask_qty[j] = std::stod(item[1].GetString());
    j++;
  }
  // quote.last_price = (bid[0] + ask[0]) / 2;
  int64_t ts = data["T"].GetInt64();
  quote.data_time = ts * 1000000;

  // quote.instrument_id = data["s"].GetString();
  memcpy(quote.ask_price, ask, sizeof(quote.ask_price));
  memcpy(quote.bid_price, bid, sizeof(quote.bid_price));
  memcpy(quote.ask_volume, ask_qty, sizeof(quote.ask_price));
  memcpy(quote.bid_volume, bid_qty, sizeof(quote.bid_price));
}

inline void from_binance(const Value &data, Transaction &transaction) {
  // std::string instrument_id(data["s"].GetString());
  // std::replace(instrument_id.begin(), instrument_id.end(), '_', '-');

  // transaction.instrument_id = instrument_id.c_str();
  transaction.price = std::stod(data["p"].GetString());
  transaction.volume = std::stod(data["q"].GetString());
  transaction.exec_type = longfist::enums::ExecType::Trade;
  int64_t ts = data["T"].GetInt64();
  transaction.data_time = ts * 1000000;
  bool is_buyer_maker = data["m"].GetBool();
  transaction.side = is_buyer_maker ? longfist::enums::Side::Sell : longfist::enums::Side::Buy;
}

inline void from_binance(const Value &data, Tick &tick) {
  // std::string instrument_id(data["s"].GetString());
  // std::replace(instrument_id.begin(), instrument_id.end(), '_', '-');

  // tick.instrument_id = instrument_id.c_str();
  tick.ask_price = std::stod(data["a"].GetString());
  tick.ask_volume = std::stod(data["A"].GetString());
  tick.bid_price = std::stod(data["b"].GetString());
  tick.bid_volume = std::stod(data["B"].GetString());

  int64_t ts = data["T"].GetInt64();

  tick.data_time = ts * 1000000;
}
} // namespace kungfu::wingchun::binance

#endif // KUNGFU_BINANCE_TYPE_CONVERT_H
