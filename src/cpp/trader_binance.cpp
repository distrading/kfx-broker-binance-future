#include "trader_binance.h"
#include "base_client/restful_client.hpp"
#include "binance_restful_client.hpp"
#include "boost/asio/ssl/verify_mode.hpp"
#include "buffer_data.h"
#include "kungfu/common.h"
#include "kungfu/longfist/enums.h"
#include "kungfu/wingchun/broker/broker.h"
#include "kungfu/wingchun/broker/trader.h"

#include "base_client/root_certificates.hpp"
#include "kungfu/wingchun/common.h"
#include "spdlog/common.h"
#include "spdlog/spdlog.h"
#include "type_convert.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "rapidjson/document.h" // rapidjson's DOM-style API

using namespace kungfu::yijinjing::data;
using namespace kungfu::yijinjing;
using namespace rapidjson;

namespace kungfu::wingchun::binance {

const std::unordered_map<std::string, OrderStatus> orderStatusLookup = {
    {"NEW", OrderStatus::Submitted},
    {"PARTIALLY_FILLED", OrderStatus::PartialFilledActive},
    {"FILLED", OrderStatus::Filled},
    {"CANCELED", OrderStatus::Cancelled},
    {"EXPIRED", OrderStatus::Error}};

inline std::string b2a_hex(char *byte_arr, const int n) noexcept {
  const static std::string HexCodes = "0123456789abcdef";
  std::string HexString;
  for (int i = 0; i < n; ++i) {
    unsigned char BinValue = byte_arr[i];
    HexString += HexCodes[(BinValue >> 4) & 0x0F];
    HexString += HexCodes[BinValue & 0x0F];
  }
  return HexString;
}

inline std::string createSignature(const std::string &key, const std::string &data) noexcept {
  std::string hash;
  if (unsigned char *digest = HMAC(EVP_sha256(), key.c_str(), static_cast<int>(key.size()),
                                   (unsigned char *)data.c_str(), data.size(), NULL, NULL);
      digest) {
    hash = b2a_hex((char *)digest, 32);
  }
  return hash;
}

inline Document parse_json(const std::string &msg) noexcept {
  Document doc;
  try {
    doc.Parse(msg.c_str());
    if (doc.HasParseError()) {
      SPDLOG_WARN("Parse error: {} {}", doc.GetParseError(), msg);
    }
  } catch (std::exception &ex) {
    SPDLOG_WARN("exception: {} raw msg: {}", ex.what(), msg);
  }
  return doc;
}

TraderBinance::TraderBinance(broker::BrokerVendor &vendor)
    : Trader(vendor), ctx_(ssl::context::sslv23_client), io_thread_(&TraderBinance::runIoContext, this),
      BinanceWebsocketClient(ioc_, ctx_), BinanceRESTfulClient(ioc_, ctx_) {
  KUNGFU_SETUP_LOG();

  SPDLOG_INFO("wait for http connect");
  std::this_thread::sleep_for(std::chrono::seconds(1));
  td_ = std::shared_ptr<TraderBinance>(this);

  SPDLOG_INFO("construct TraderBinance");
}

TraderBinance::~TraderBinance() {
  SPDLOG_INFO(" ~TraderBinance");
  // stop_ws_client();
}

void TraderBinance::sign_request(std::string &target, RequestParams params) {
  std::ostringstream pathWithParams;
  for (auto &param : params) {
    pathWithParams << std::move(param.first) << "=" << std::move(param.second) << "&";
  }
  pathWithParams << "timestamp="
                 << std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
  auto pathWithoutSig = pathWithParams.str();
  target =
      target + "?" + std::move(pathWithoutSig) + "&signature=" + createSignature(config_.api_secret, pathWithoutSig);
}

void TraderBinance::on_query_account(const std::string &msg) {
  // SPDLOG_DEBUG("on_query_account {}", msg);
  auto &rest_message =
      get_thread_writer()->open_custom_data<BufferBinanceRestReport>(BinanceRestAccountReportType, now());
  rest_message.message = msg.c_str();
  get_thread_writer()->close_data();
}

void TraderBinance::on_query_listenkey(const std::string &msg) {
  auto doc = parse_json(msg);
  if (doc.HasMember("code")) {
    SPDLOG_WARN("on_query_listenkey error {}", msg);
    add_timer(time::now_in_nano() + int64_t(20 * time_unit::NANOSECONDS_PER_SECOND),
              [&](const auto &) { query_listenkey(); });

    return;
  }

  subscribe_user_stream(doc["listenKey"].GetString());
  SPDLOG_DEBUG("subscribe_user_stream listenkey {}", msg);
}

void TraderBinance::on_send_order(const std::string &msg, const std::string &extra) {
  // only use error message from restful api
  auto &rest_message =
      get_thread_writer()->open_custom_data<BufferBinanceRestOrderReport>(BinanceRestOrderReportType, now());
  rest_message.message = msg.c_str();
  rest_message.extra = extra.c_str();
  get_thread_writer()->close_data();
  SPDLOG_DEBUG("on_send_order {} {}", msg, extra);
}

void TraderBinance::on_ws_message(const std::string &sessionName, std::string &msg) {
  SPDLOG_DEBUG(msg);
  auto &ws_message =
      get_thread_writer()->open_custom_data<BufferBinanceWebSocketReport>(BinanceWebSocketReportType, now());
  ws_message.message = msg.c_str();
  get_thread_writer()->close_data();
}

void TraderBinance::on_cancel_order(const std::string &msg) { SPDLOG_INFO("on_cancel_order {}", msg); }

void TraderBinance::on_renew_listenkey(const std::string &msg) { SPDLOG_INFO("on_renew_listenkey {}", msg); }

void TraderBinance::on_restful_message(const std::string &msg) { SPDLOG_INFO(msg); }

bool TraderBinance::on_custom_event(const event_ptr &event) {
  SPDLOG_TRACE("msg_type: {}", event->msg_type());
  switch (event->msg_type()) {
  case BinanceWebSocketReportType:
    return custom_on_ws_event(event);
  case BinanceRestOrderReportType:
    return custom_on_send_order_event(event);
  case BinanceRestAccountReportType:
    return custom_on_query_account_event(event);
  }
  return false;
}

bool TraderBinance::custom_on_query_account_event(const event_ptr &event) {

  const auto *rest_message = reinterpret_cast<const BufferBinanceRestReport *>(event->data_address());
  auto msg = rest_message->message;
  // SPDLOG_INFO(msg);
  auto doc = parse_json(msg);
  if (doc.HasMember("code")) {
    SPDLOG_WARN("on_query_account error {}", msg);
    return false;
  }

  // account asset to position
  for (auto &asset : doc["assets"].GetArray()) {
    auto balance = std::stod(asset["walletBalance"].GetString());
    if (is_greater(balance, 0.0)) {
      Position &asset_pos = get_position_writer()->open_data<Position>(0);

      asset_pos.instrument_id = asset["asset"].GetString();
      asset_pos.instrument_type = instrument_type_;
      asset_pos.exchange_id = exchange_id_;
      asset_pos.volume = balance;
      get_position_writer()->close_data();
      SPDLOG_DEBUG("asset_pos {}", asset_pos.to_string());
      // enable_positions_sync();
    }
  }

  // open position
  for (auto &pos : doc["positions"].GetArray()) {
    auto volume = std::stod(pos["positionAmt"].GetString());

    std::string instrument_id(pos["symbol"].GetString());
    std::replace(instrument_id.begin(), instrument_id.end(), '_', '-');

    // SPDLOG_DEBUG("symbol {} volume {}", instrument_id, volume);

    // instrument_id map kf: BTCUSD-PERP  binance: BTCUSD_PERP
    kf_ba_instrument_map_[instrument_id] = pos["symbol"].GetString();
    ba_kf_instrument_map_[pos["symbol"].GetString()] = instrument_id;

    if (is_greater(abs(volume), 0.0)) {

      Position &position = get_position_writer()->open_data<Position>(0);

      position.instrument_id = instrument_id.c_str();
      position.instrument_type = instrument_type_;
      position.exchange_id = exchange_id_;

      auto direction = pos["positionSide"].GetString();

      // BOTH LONG SHORT
      if (string_equals(direction, "SHORT")) {
        position.direction = longfist::enums::Direction::Short;
        position.volume = abs(volume);

      } else {
        position.direction = longfist::enums::Direction::Long;
        position.volume = volume;
      }
      position.avg_open_price = std::stod(pos["entryPrice"].GetString());
      // position.volume = volume;
      position.unrealized_pnl = std::stod(pos["unrealizedProfit"].GetString());
      get_position_writer()->close_data();
      SPDLOG_DEBUG("position {}", position.to_string());
    }
  }

  PositionEnd &end = get_position_writer()->open_data<PositionEnd>(0);
  end.holder_uid = get_home_uid();
  get_position_writer()->close_data();
  enable_positions_sync();

  // SPDLOG_INFO(msg);

  // account asset
  // coin base market no account asset
  if (config_.market_type == "coin-market") {
    return true;
  }
  Asset &asset = get_asset_writer()->open_data<Asset>(0);
  asset.unrealized_pnl = std::stod(doc["totalUnrealizedProfit"].GetString()); // 持仓未实现盈亏总额
  asset.market_value = std::stod(doc["totalCrossWalletBalance"].GetString()); // 全仓账户余额

  asset.avail = std::stod(doc["availableBalance"].GetString());         // 可用余额
  asset.total_asset = std::stod(doc["totalWalletBalance"].GetString()); // 账户总余额
  asset.margin = std::stod(doc["totalInitialMargin"].GetString());      // 当前所需起始保证金总额
  asset.holder_uid = get_home()->uid;
  asset.update_time = yijinjing::time::now_in_nano();

  SPDLOG_DEBUG("account asset {}", asset.to_string());

  get_asset_writer()->close_data();
  enable_asset_sync();
  return true;
}

bool TraderBinance::custom_on_send_order_event(const event_ptr &event) {
  const auto *rest_message = reinterpret_cast<const BufferBinanceRestOrderReport *>(event->data_address());
  auto msg = rest_message->message;
  auto extra = rest_message->extra;

  auto doc = parse_json(msg);
  if (doc.HasMember("code")) {
    SPDLOG_WARN("on_send_order error {}", msg);
    auto &order_state = get_order(std::stoull(extra));
    order_state.data.status = OrderStatus::Error;
    order_state.data.error_id = doc["code"].GetInt();
    order_state.data.error_msg = doc["msg"].GetString();
    order_state.data.update_time = now();
    try_write_to(order_state.data, order_state.dest);
  }
  return true;
}

bool TraderBinance::custom_on_ws_event(const event_ptr &event) {
  const auto *ws_message = reinterpret_cast<const BufferBinanceWebSocketReport *>(event->data_address());
  auto msg = ws_message->message;
  SPDLOG_DEBUG(msg);

  auto doc = parse_json(msg);
  auto ba_event_type = doc["e"].GetString();

  // renew listenKey
  if (string_equals(ba_event_type, "listenKeyExpired")) {

    // unsubscribe_user_stream(doc["listenKey"].GetString());
    std::string target_listenKey = market_path_ + "/v1/listenKey";

    sign_request(target_listenKey, {});
    restful_request(RequestMethod::post, target_listenKey.c_str(),
                    std::bind(&TraderBinance::on_query_listenkey, this, std::placeholders::_1));
  }

  // account position update use get_public_writer
  if (string_equals(ba_event_type, "ACCOUNT_UPDATE")) {
    // account asset to position
    auto writer = get_public_writer();

    for (auto &asset : doc["a"]["B"].GetArray()) {
      auto balance = std::stod(asset["wb"].GetString());
      if (is_greater(balance, 0.0)) {
        Position &asset_pos = writer->open_data<Position>(location::SYNC);

        asset_pos.instrument_id = binance_to_kf_instrument(asset["a"].GetString()).c_str();
        asset_pos.instrument_type = instrument_type_;
        asset_pos.exchange_id = exchange_id_;
        asset_pos.volume = balance;
        asset_pos.update_time = now();
        SPDLOG_INFO("ws asset_pos update {}", asset_pos.to_string());
        writer->close_data();
      }
    }

    // open position
    for (auto &pos : doc["a"]["P"].GetArray()) {
      auto volume = std::stod(pos["pa"].GetString());

      Position &position = writer->open_data<Position>(location::SYNC);

      position.instrument_id = binance_to_kf_instrument(pos["s"].GetString()).c_str();
      position.instrument_type = instrument_type_;
      position.exchange_id = exchange_id_;

      auto direction = pos["ps"].GetString();

      // BOTH LONG SHORT
      if (string_equals(direction, "SHORT")) {
        position.direction = longfist::enums::Direction::Short;
        position.volume = abs(volume);

      } else {
        position.direction = longfist::enums::Direction::Long;
        position.volume = volume;
      }
      position.avg_open_price = std::stod(pos["ep"].GetString());
      // position.volume = volume;
      position.unrealized_pnl = std::stod(pos["up"].GetString());
      position.realized_pnl = std::stod(pos["cr"].GetString());
      position.margin = std::stod(pos["iw"].GetString());
      // position.last_price = std::stod(pos["bep"].GetString());
      position.update_time = now();
      SPDLOG_DEBUG("ws position update {}", position.to_string());

      writer->close_data();
    }
    PositionEnd &end = writer->open_data<PositionEnd>(location::SYNC);
    end.holder_uid = get_home_uid();
    writer->close_data();
    enable_positions_sync();
  }

  // order trade update
  if (string_equals(ba_event_type, "ORDER_TRADE_UPDATE")) {

    auto clientOrderId = doc["o"]["c"].GetString();

    if (startswith(clientOrderId, "autoclose-")) {
      // 系统强平订单
    } else if (startswith(clientOrderId, "adl_autoclose")) {
      // ADL自动减仓订单
    } else if (startswith(clientOrderId, "settlement_autoclose-")) {
      // 下架或交割的结算订单
    } else {
      uint64_t kf_order_id;
      uint64_t ba_order_id = doc["o"]["i"].GetInt64();
      try {
        kf_order_id = std::stoull(clientOrderId);
      } catch (const std::invalid_argument &e) {
        SPDLOG_INFO("external order ba_order_id {}, clientOrderId={}", ba_order_id, clientOrderId);
        return false;
      }

      auto &order_state = get_order(kf_order_id);

      // SPDLOG_INFO("order_state  {}", order_state.data.to_string());

      order_state.data.external_order_id = std::to_string(ba_order_id).c_str();
      order_state.data.volume_left = std::stod(doc["o"]["q"].GetString()) - std::stod(doc["o"]["z"].GetString());

      auto ba_event_status = doc["o"]["x"].GetString(); // 事件的具体执行类型
      auto ba_order_status = doc["o"]["X"].GetString(); // 订单状态

      if (string_equals(ba_order_status, "NEW")) {
        map_binance_to_kf_order_id_.emplace(ba_order_id, kf_order_id);
        map_kf_to_binance_order_id_.emplace(kf_order_id, ba_order_id);
        order_state.data.status = OrderStatus::Submitted;

      } else if (endswith(ba_order_status, "FILLED")) {
        // filled or PartialFilled
        auto writer = get_writer(order_state.dest);
        Trade &trade = writer->open_data<Trade>(now());
        auto ts = doc["o"]["T"].GetInt64();

        trade.trade_id = writer->current_frame_uid();
        ;
        trade.external_trade_id = std::to_string(doc["o"]["t"].GetInt64()).c_str();
        // trade.order_id = kf_order_id;
        // trade.external_trade_id = order_state.data.external_order_id;
        trade.trade_time = ts * 1000000;
        trade.price = std::stod(doc["o"]["L"].GetString());
        trade.volume = std::stod(doc["o"]["l"].GetString());

        trade.order_id = order_state.data.order_id;
        strcpy(trade.external_order_id, order_state.data.external_order_id);
        strcpy(trade.instrument_id, order_state.data.instrument_id);
        strcpy(trade.exchange_id, order_state.data.exchange_id);
        trade.instrument_type = order_state.data.instrument_type;
        trade.side = order_state.data.side;
        trade.offset = order_state.data.offset;
        trade.hedge_flag = order_state.data.hedge_flag;
        writer->close_data();

        order_state.data.commission = std::stod(doc["o"]["n"].GetString());

        order_state.data.volume_left = order_state.data.volume - std::stod(doc["o"]["z"].GetString());
        order_state.data.status = orderStatusLookup.at(ba_order_status);

      } else if (string_equals(ba_order_status, "CANCELED")) {
        order_state.data.status = OrderStatus::Cancelled;

      } else if (string_equals(ba_order_status, "EXPIRED")) {
        order_state.data.status = OrderStatus::Error;

      } else {
        SPDLOG_WARN("order_state: {} not convert", ba_order_status);
      }
      order_state.data.update_time = now();

      try_write_to(order_state.data, order_state.dest);
    }
  }
  return true;
}

void TraderBinance::on_ws_close(const std::string &sessionName) {
  unsubscribe_user_stream(sessionName);
  query_listenkey();
  SPDLOG_WARN("websocket session {} closed! reconnect", sessionName);
}

void TraderBinance::query_listenkey() {
  std::string target_listenKey = market_path_ + "/v1/listenKey";

  sign_request(target_listenKey, {});

  restful_request(RequestMethod::post, target_listenKey.c_str(),
                  std::bind(&TraderBinance::on_query_listenkey, this, std::placeholders::_1));
}

void TraderBinance::query_account() {
  std::string target_account = market_path_ + ep_version_ + "/account";
  sign_request(target_account, {});
  restful_request(RequestMethod::get, target_account.c_str(),
                  std::bind(&TraderBinance::on_query_account, this, std::placeholders::_1));
}

void TraderBinance::pre_start() {
  ctx_.set_verify_mode(ssl::verify_none);
  load_root_certificates(ctx_);
  // io_thread_([ioc_]() {
  //       ioc_.run();
  // });
  // ioc_.run();
  config_ = nlohmann::json::parse(get_config());
  SPDLOG_INFO("config: {}", get_config());

  // set usd/coin market
  if (config_.market_type == "coin-market") {
    market_path_ = "/dapi";
    ep_version_ = "/v1";
    exchange_id_ = EXCHANGE_BINANCE_COIN_FUTURE;
    instrument_type_ = longfist::enums::InstrumentType::CryptoFuture;

    set_rest_path("dapi.binance.com", "443");
    set_ws_path("dstream.binance.com", "443");
    if (config_.testnet) {
      set_rest_path("testnet.binancefuture.com", "443");
      set_ws_path("dstream.binancefuture.com", "443");
      SPDLOG_INFO("use coin market testnet");
    }

  } else {
    market_path_ = "/fapi";
    ep_version_ = "/v2";
    exchange_id_ = EXCHANGE_BINANCE_USD_FUTURE;
    instrument_type_ = longfist::enums::InstrumentType::CryptoUFuture;
    set_rest_path("fapi.binance.com", "443");
    set_ws_path("fstream.binance.com", "443");
    if (config_.testnet) {
      set_rest_path("testnet.binancefuture.com", "443");
      set_ws_path("fstream.binancefuture.com", "443");
      SPDLOG_INFO("use usd market testnet");
    }
  }

  // set api key header
  Headers headers;
  headers["X-MBX-APIKEY"] = config_.api_key;
  set_headers(headers);

  // get listen key
  query_listenkey();
  // get init account&position
  query_account();

  SPDLOG_INFO("Connecting Binance");
  update_broker_state(BrokerState::Connected);
}

void TraderBinance::on_start() {
  SPDLOG_INFO("TraderBinance on_start");
  update_broker_state(BrokerState::Ready);
}

void TraderBinance::on_exit() { SPDLOG_INFO("TraderBinance on_exit"); }

bool TraderBinance::insert_order(const event_ptr &event) {

  const OrderInput &input = event->data<OrderInput>();
  SPDLOG_DEBUG("OrderInput: {}", input.to_string());

  if (!has_writer(event->source())) {
    SPDLOG_ERROR("insert_order: not find id:{}", event->source());
    return false;
  }

  // local order
  auto nano = yijinjing::time::now_in_nano();
  auto writer = get_writer(event->source());
  Order &order = writer->open_data<Order>(event->gen_time());
  order_from_input(input, order);
  order.status = OrderStatus::Pending;
  order.insert_time = nano;
  order.update_time = nano;
  writer->close_data();

  // make order request
  RequestParams params;
  params["symbol"] = kf_to_binance_instrument(input.instrument_id.to_string());

  if (input.offset == Offset::Open) {
    if (input.side == Side::Buy) {
      params["side"] = "BUY";
      params["positionSide"] = "LONG";
    } else {
      params["side"] = "SELL";
      params["positionSide"] = "SHORT";
    }
  } else {
    if (input.side == Side::Buy) {
      params["side"] = "BUY";
      params["positionSide"] = "SHORT";
    } else {
      params["side"] = "SELL";
      params["positionSide"] = "LONG";
    }
  }

  params["type"] = input.price_type == PriceType::Limit ? "LIMIT" : "MARKET";

  params["quantity"] = std::to_string(input.volume);
  params["newClientOrderId"] = std::to_string(order.order_id);

  if (input.price_type == PriceType::Limit) {
    params["timeInForce"] = "GTC";
    params["price"] = std::to_string(input.limit_price);
  }

  std::string target = market_path_ + "/v1/order";
  sign_request(target, params);

  restful_request(RequestMethod::post, target.c_str(),
                  std::bind(&TraderBinance::on_send_order, this, std::placeholders::_1, std::placeholders::_2),
                  std::to_string(order.order_id));

  return true;
}

bool TraderBinance::cancel_order(const event_ptr &event) {
  SPDLOG_INFO("TraderBinance cancel_order");
  const auto &action = event->data<OrderAction>();
  auto &order_state = get_order(action.order_id);
  auto order_id_iter = map_kf_to_binance_order_id_.find(action.order_id);
  if (order_id_iter == map_kf_to_binance_order_id_.end()) {
    SPDLOG_ERROR("failed to cancel order {}, can't find related binance order id", action.order_id);
    return false;
  }

  if (not has_order(action.order_id)) {
    SPDLOG_ERROR("no order_id {} in orders_", action.order_id);
    return false;
  }

  SPDLOG_INFO("sending cancel {}", order_state.data.to_string());
  RequestParams params;

  params["symbol"] = kf_to_binance_instrument(order_state.data.instrument_id.to_string());
  params["orderId"] = order_state.data.external_order_id.to_string();
  std::string target = market_path_ + "/v1/order";
  sign_request(target, params);

  restful_request(RequestMethod::delete_, target.c_str(),
                  std::bind(&TraderBinance::on_cancel_order, this, std::placeholders::_1));

  return true;
}

bool TraderBinance::req_position() {
  // SPDLOG_INFO("TraderBinance req_position");
  return true;
}

bool TraderBinance::req_account() {
  query_account();
  return true;
}

bool TraderBinance::req_history_order(const event_ptr &event) {
  SPDLOG_INFO("TraderBinance req_history_order");
  return true;
}

bool TraderBinance::req_history_trade(const event_ptr &event) {
  SPDLOG_INFO("TraderBinance req_history_trade");
  return true;
}

void TraderBinance::on_recover() {
  SPDLOG_INFO("on_recover begin orders_ size={}, trades_ size ={} ", get_orders().size(), get_trades().size());
  // for (auto& pair : get_orders()) {
  //     const std::string str_external_order_id = pair.second.data.external_order_id.to_string();
  //     if (not str_external_order_id.empty()) {
  //         uint64_t order_id = pair.first;
  //         SPDLOG_INFO("on_recover str_external_order_id={}, order_id={}", str_external_order_id, order_id);
  //         map_kf_to_binance_order_id_.emplace(order_id, str_external_order_id);
  //         map_binance_to_kf_order_id_.emplace(str_external_order_id, order_id);

  //     }
  // }
}

} // namespace kungfu::wingchun::binance
