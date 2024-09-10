#ifndef BINANCE_BUFFER_DATA_H
#define BINANCE_BUFFER_DATA_H

#include <kungfu/longfist/enums.h>
#include <kungfu/longfist/longfist.h>
#include <string>

namespace kungfu::wingchun::binance {
using namespace kungfu::longfist::types;
using namespace kungfu::longfist::enums;

static constexpr int32_t BinanceWebSocketReportType = 88880000;
static constexpr int32_t BinanceRestReportType = 88880001;
static constexpr int32_t BinanceRestOrderReportType = 88880002;
static constexpr int32_t BinanceRestAccountReportType = 88880003;
static constexpr int32_t BinanceRestAllOrderReportType = 88880004;

struct BufferBinanceMessage {
  std::string message;
  std::string to_string() const { return message; }
};


struct BufferBinanceRestOrderReport {
  char message[1024];
  char extra[64];
};


} // namespace kungfu::wingchun::binance

#endif