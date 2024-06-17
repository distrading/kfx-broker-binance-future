# Binance Future for Kungfu

reference: https://github.com/liujiaecit/kfx-broker-okx

### Start

Prerequisites:

cmake

windows:
    Visual Studio

Linux:
    g++ 11

#### Dependencies

```
boost v1.7.9
rapidjson v1.1.0
openssl v3.0 
```

dependencies path like:

```
__kungfulibs__/boost/v1.7.9/include/boost
__kungfulibs__/openssl/v3.0/lib
```

#### run build

```
kfs extension build/clean
```

#### install to Kungfu

copy dist/binancefutrue to Kungfu/resources/app/kungfu-extensions

Enjoy!
