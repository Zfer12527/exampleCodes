#pragma once  // 防止同一个 Wi-Fi 配置文件被重复包含。

namespace wifi_config {                    // 将 Wi-Fi 配置放入独立命名空间，避免名称冲突。
constexpr char SSID[] = "123";             // 设置 ESP32 需要连接的 Wi-Fi 名称。
constexpr char PASSWORD[] = "*********";  // 设置该 Wi-Fi 对应的连接密码。
}
