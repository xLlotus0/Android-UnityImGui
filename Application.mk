APP_ABI := arm64-v8a
APP_PLATFORM := android-25
APP_STL := c++_static
APP_OPTIM := release

APP_CPPFLAGS += -std=c++20 -fexceptions -frtti
APP_CFLAGS += -fvisibility=hidden -ffunction-sections -fdata-sections
APP_LDFLAGS += -Wl,--gc-sections -Wl,--strip-all -Wl,--exclude-libs,ALL
