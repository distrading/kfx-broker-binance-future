#include "marketdata_binance.h"
#include "trader_binance.h"

#include <kungfu/wingchun/extension.h>

KUNGFU_EXTENSION() {
  KUNGFU_DEFINE_MD(kungfu::wingchun::binance::MarketDataBinance);
  KUNGFU_DEFINE_TD(kungfu::wingchun::binance::TraderBinance);
}
