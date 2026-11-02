#include <Arduino.h>      // 引入 Arduino 核心功能，例如 GPIO、串口、延时和 String。
#include <ArduinoOTA.h>   // 引入 ArduinoOTA，用于保留电脑主动推送固件的升级方式。
#include <HTTPClient.h>   // 引入 HTTP 客户端，用于读取服务器上的版本文件。
#include <Update.h>       // 引入底层 OTA 写入功能，用于设置并校验 firmware.md5。
#include <WiFi.h>         // 引入 ESP32 Wi-Fi 功能，用于连接无线网络。

#include "wifi_config.h"  // 从本地配置文件读取 Wi-Fi 名称和密码。

namespace {  // 使用匿名命名空间，避免本文件内的名称被其他源文件访问。
constexpr char OTA_HOSTNAME[] = "zfesp32";      // 设置手动 OTA 时设备在局域网中的主机名。
constexpr char OTA_PASSWORD[] = "zfpassword";  // 设置手动 OTA 的认证密码。

// 每次发布新固件时递增此版本号，并将服务器上的 firmware.version 更新为相同版本。
constexpr char APP_VERSION[] = "1.0.5";  // 定义当前固件版本，格式必须为“主版本.次版本.修订号”。
constexpr char UPDATE_VERSION_URL[] =     // 定义查询服务器最新版本号的地址。
    "http://192.168.1.112:8000/firmware.version";  // 指向只包含版本号的文本文件。
constexpr char UPDATE_MD5_URL[] =                   // 定义查询新固件 MD5 的地址。
    "http://192.168.1.112:8000/firmware.md5";      // 指向只包含 32 位小写 MD5 的文本文件。
constexpr char UPDATE_FIRMWARE_URL[] =              // 定义下载新固件的地址。
    "http://192.168.1.112:8000/firmware.bin";      // 指向由 PlatformIO 生成的固件文件。
constexpr uint16_t UPDATE_HTTP_TIMEOUT_MS = 5000;   // 设置各个 HTTP 请求的连接和读取超时为 5 秒。

constexpr uint8_t LED_PIN = 2;                    // 使用 D2 / GPIO2 板载 LED 指示设备状态。
constexpr unsigned long BLINK_INTERVAL_MS = 1000;  // 设置正常运行时 LED 每 1000 毫秒翻转一次。

unsigned long lastBlinkTime = 0;  // 保存上一次翻转 LED 时的毫秒时间戳。
bool ledState = false;            // 保存当前 LED 的逻辑电平状态。

bool parseVersion(String version, uint32_t parts[3]) {  // 将 x.y.z 版本字符串拆分成三个整数。
  version.trim();  // 删除版本字符串首尾可能存在的空格、回车和换行。
  if (version.startsWith("v") || version.startsWith("V")) {  // 允许服务器版本以 v 或 V 开头。
    version.remove(0, 1);  // 删除开头的版本前缀，只保留数字部分。
  }  // 结束可选版本前缀的处理。

  int position = 0;  // 记录当前解析到版本字符串中的哪个字符。
  for (size_t part = 0; part < 3; ++part) {  // 依次解析主版本、次版本和修订号。
    if (position >= version.length() || !isDigit(version[position])) {  // 要求每段至少以一个数字开始。
      return false;  // 格式不正确时立即返回解析失败。
    }  // 结束版本段起始字符检查。

    uint32_t value = 0;  // 保存当前版本段转换后的整数值。
    while (position < version.length() && isDigit(version[position])) {  // 连续读取当前版本段中的数字。
      value = value * 10U + static_cast<uint32_t>(version[position] - '0');  // 将十进制字符累加成整数。
      ++position;  // 移动到版本字符串的下一个字符。
    }  // 当前版本段的全部数字已经读取完成。
    parts[part] = value;  // 将解析结果写入调用者提供的数组。

    if (part < 2) {  // 前两个版本段后面必须存在英文句点分隔符。
      if (position >= version.length() || version[position] != '.') {  // 检查分隔符是否存在且正确。
        return false;  // 缺少句点或出现其他字符时返回解析失败。
      }  // 结束版本分隔符检查。
      ++position;  // 跳过句点，准备读取下一个版本段。
    }  // 最后一个版本段后面不需要句点。
  }  // 三个版本段已经全部解析。

  return position == version.length();  // 只有字符串被完整消费时才判定格式有效。
}  // 结束版本字符串解析函数。

bool isNewerVersion(const String &candidate, const char *current) {  // 判断服务器版本是否高于当前版本。
  uint32_t candidateParts[3] = {};  // 保存服务器版本的三个数字部分。
  uint32_t currentParts[3] = {};    // 保存当前固件版本的三个数字部分。
  if (!parseVersion(candidate, candidateParts) ||  // 尝试解析服务器返回的版本号。
      !parseVersion(String(current), currentParts)) {  // 同时解析当前固件版本号。
    return false;  // 任一版本格式错误时不执行升级。
  }  // 两个版本号均已成功解析。

  for (size_t i = 0; i < 3; ++i) {  // 按主版本、次版本、修订号的顺序逐项比较。
    if (candidateParts[i] != currentParts[i]) {  // 找到第一个不相等的版本部分。
      return candidateParts[i] > currentParts[i];  // 服务器对应部分更大时判定为新版本。
    }  // 相等时继续比较下一个版本部分。
  }  // 所有版本部分已经比较完毕。
  return false;  // 三个部分完全相等时没有新版本。
}  // 结束版本比较函数。

bool isValidMd5(const String &md5) {  // 检查 MD5 是否为 32 个小写十六进制字符。
  if (md5.length() != 32) {  // 标准 MD5 文本必须正好包含 32 个字符。
    return false;  // 长度不正确时拒绝继续升级。
  }  // MD5 长度检查结束。

  for (size_t i = 0; i < md5.length(); ++i) {  // 逐个检查 MD5 中的所有字符。
    const char value = md5[i];  // 读取当前位置的字符供后续判断。
    const bool isNumber = value >= '0' && value <= '9';  // 判断字符是否为数字 0 到 9。
    const bool isLowerHex = value >= 'a' && value <= 'f';  // 判断字符是否为小写字母 a 到 f。
    if (!isNumber && !isLowerHex) {  // 任何非十六进制字符都表示 MD5 文件无效。
      return false;  // 检测到无效字符后立即终止升级。
    }  // 当前字符有效，继续检查下一个字符。
  }  // 所有 MD5 字符已经检查完毕。

  return true;  // 长度和字符范围均正确，MD5 格式有效。
}  // 结束 MD5 格式检查函数。

void connectWiFi() {  // 连接配置文件中指定的无线网络。
  Serial.printf("Connecting to %s", wifi_config::SSID);  // 在串口打印正在连接的 Wi-Fi 名称。
  WiFi.mode(WIFI_STA);  // 将 ESP32 设置为无线终端模式，而不是热点模式。
  WiFi.begin(wifi_config::SSID, wifi_config::PASSWORD);  // 使用配置文件中的名称和密码开始连接。

  while (WiFi.status() != WL_CONNECTED) {  // 持续等待，直到 Wi-Fi 成功连接。
    ledState = !ledState;  // 等待期间翻转 LED 状态形成快速闪烁效果。
    digitalWrite(LED_PIN, ledState);  // 将新的状态输出到板载 LED 引脚。
    Serial.print('.');  // 每次等待时在串口输出一个点作为进度提示。
    delay(100);  // 暂停 100 毫秒，避免过于频繁地查询连接状态。
  }  // Wi-Fi 已经成功连接。

  digitalWrite(LED_PIN, LOW);  // 连接成功后先关闭板载 LED。
  Serial.println();  // 输出换行，使后续串口信息从新行开始。
  Serial.print("Wi-Fi connected, IP address: ");  // 打印连接成功提示及 IP 标签。
  Serial.println(WiFi.localIP());  // 打印路由器分配给 ESP32 的局域网 IP 地址。
}  // 结束 Wi-Fi 连接函数。

void checkForFirmwareUpdate() {  // 在设备启动时向本地服务器检查并安装新固件。
  Serial.printf("Checking for firmware update (current: %s)...\n", APP_VERSION);  // 打印当前版本和检查提示。

  WiFiClient versionClient;   // 创建获取版本文件所使用的底层 TCP 客户端。
  HTTPClient versionRequest;  // 创建用于获取 firmware.version 的 HTTP 请求对象。
  versionRequest.setConnectTimeout(UPDATE_HTTP_TIMEOUT_MS);  // 限制连接服务器的最长等待时间。
  versionRequest.setTimeout(UPDATE_HTTP_TIMEOUT_MS);  // 限制服务器响应数据的最长等待时间。
  versionRequest.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // 允许严格跟随版本文件的 HTTP 重定向。

  if (!versionRequest.begin(versionClient, UPDATE_VERSION_URL)) {  // 使用版本文件 URL 初始化 HTTP 请求。
    Serial.println("Unable to create update version request; continuing startup.");  // 记录请求初始化失败。
    return;  // 放弃本次检查并继续启动当前固件。
  }  // HTTP 版本请求已经初始化成功。

  const int statusCode = versionRequest.GET();  // 向服务器发送 GET 请求并保存 HTTP 状态码。
  if (statusCode != HTTP_CODE_OK) {  // 只有 HTTP 200 才表示成功取得版本文件。
    Serial.printf("Update server unavailable (HTTP %d); continuing startup.\n",  // 打印服务器错误提示。
                  statusCode);  // 将实际 HTTP 状态码填入提示信息。
    versionRequest.end();  // 释放当前 HTTP 请求占用的连接和内存。
    return;  // 放弃本次检查并继续运行现有固件。
  }  // 已经成功取得服务器版本文件。

  String latestVersion = versionRequest.getString();  // 将服务器响应正文读取为最新版本字符串。
  latestVersion.trim();  // 删除版本文件末尾常见的换行符和两端空白。
  versionRequest.end();  // 版本内容读取完毕后立即释放 HTTP 请求资源。

  uint32_t latestParts[3] = {};  // 创建临时数组，用于验证服务器版本格式。
  if (!parseVersion(latestVersion, latestParts)) {  // 检查服务器内容是否符合 x.y.z 格式。
    Serial.printf("Invalid server version '%s'; expected x.y.z.\n",  // 输出无效版本格式提示。
                  latestVersion.c_str());  // 将服务器返回的原始版本放进提示信息。
    return;  // 版本格式不安全时不下载任何固件。
  }  // 服务器版本格式验证通过。

  if (!isNewerVersion(latestVersion, APP_VERSION)) {  // 判断服务器版本是否真的更高。
    Serial.printf("Firmware is up to date (server: %s).\n",  // 提示当前固件无需升级。
                  latestVersion.c_str());  // 输出服务器上的版本号便于核对。
    return;  // 版本相同或更低时结束检查。
  }  // 已确认服务器上存在更新版本。

  Serial.printf("New firmware %s found; reading MD5...\n",  // 输出发现新固件并准备读取校验值的提示。
                latestVersion.c_str());  // 将目标版本号放进提示信息。

  WiFiClient md5Client;   // 创建获取 MD5 文件所使用的底层 TCP 客户端。
  HTTPClient md5Request;  // 创建用于获取 firmware.md5 的 HTTP 请求对象。
  md5Request.setConnectTimeout(UPDATE_HTTP_TIMEOUT_MS);  // 限制连接 MD5 文件的最长等待时间。
  md5Request.setTimeout(UPDATE_HTTP_TIMEOUT_MS);  // 限制读取 MD5 文件的最长等待时间。
  md5Request.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // 允许严格跟随 MD5 文件的 HTTP 重定向。

  if (!md5Request.begin(md5Client, UPDATE_MD5_URL)) {  // 使用 MD5 文件 URL 初始化 HTTP 请求。
    Serial.println("Unable to create firmware MD5 request; continuing startup.");  // 记录请求初始化失败。
    return;  // 无法取得期望 MD5 时不允许下载和安装固件。
  }  // HTTP MD5 请求已经初始化成功。

  const int md5StatusCode = md5Request.GET();  // 向服务器请求 firmware.md5 并保存 HTTP 状态码。
  if (md5StatusCode != HTTP_CODE_OK) {  // 只有 HTTP 200 才表示成功取得 MD5 文件。
    Serial.printf("Unable to download firmware.md5 (HTTP %d).\n",  // 输出 MD5 文件下载失败提示。
                  md5StatusCode);  // 将实际 HTTP 状态码填入提示信息。
    md5Request.end();  // 释放 MD5 HTTP 请求占用的连接和内存。
    return;  // 缺少可信校验值时停止升级并继续运行原固件。
  }  // 已经成功取得 MD5 文件。

  String expectedMd5 = md5Request.getString();  // 将服务器响应正文读取为期望的 MD5 字符串。
  expectedMd5.trim();  // 删除 MD5 文件首尾可能存在的空格、回车和换行。
  expectedMd5.toLowerCase();  // 将大写十六进制字母统一转换为小写，避免大小写比较失败。
  md5Request.end();  // MD5 内容读取完毕后立即释放 HTTP 请求资源。

  if (!isValidMd5(expectedMd5)) {  // 验证 MD5 是否为 32 个小写十六进制字符。
    Serial.printf("Invalid firmware MD5: '%s'.\n", expectedMd5.c_str());  // 输出无效的 MD5 内容便于排查。
    return;  // MD5 格式错误时拒绝下载和安装固件。
  }  // firmware.md5 文件的格式已经验证通过。

  Serial.printf("Expected firmware MD5: %s\n", expectedMd5.c_str());  // 打印即将用于完整性校验的 MD5。
  Serial.println("Downloading new firmware...");  // 提示即将开始下载并写入 OTA 分区。

  WiFiClient firmwareClient;   // 创建下载 firmware.bin 所使用的底层 TCP 客户端。
  HTTPClient firmwareRequest;  // 创建用于下载 firmware.bin 的 HTTP 请求对象。
  firmwareRequest.setConnectTimeout(UPDATE_HTTP_TIMEOUT_MS);  // 限制连接固件文件的最长等待时间。
  firmwareRequest.setTimeout(UPDATE_HTTP_TIMEOUT_MS);  // 限制固件数据两次接收之间的最长等待时间。
  firmwareRequest.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // 允许严格跟随固件文件的 HTTP 重定向。

  if (!firmwareRequest.begin(firmwareClient, UPDATE_FIRMWARE_URL)) {  // 使用固件 URL 初始化 HTTP 请求。
    Serial.println("Unable to create firmware request; continuing startup.");  // 记录固件请求初始化失败。
    return;  // 无法创建下载请求时继续运行原固件。
  }  // HTTP 固件请求已经初始化成功。

  const int firmwareStatusCode = firmwareRequest.GET();  // 向服务器请求 firmware.bin 并保存 HTTP 状态码。
  if (firmwareStatusCode != HTTP_CODE_OK) {  // 只有 HTTP 200 才表示服务器允许下载固件。
    Serial.printf("Unable to download firmware.bin (HTTP %d).\n",  // 输出固件下载失败提示。
                  firmwareStatusCode);  // 将实际 HTTP 状态码填入提示信息。
    firmwareRequest.end();  // 释放固件 HTTP 请求占用的连接和内存。
    return;  // 下载失败时继续运行原固件。
  }  // 固件服务器已经返回成功状态。

  const int firmwareSize = firmwareRequest.getSize();  // 从 Content-Length 响应头取得固件字节数。
  if (firmwareSize <= 0) {  // OTA 写入必须知道一个有效且大于零的固件大小。
    Serial.println("Invalid firmware size or missing Content-Length.");  // 提示服务器没有提供有效文件长度。
    firmwareRequest.end();  // 释放固件 HTTP 请求资源。
    return;  // 文件大小不可信时拒绝写入 OTA 分区。
  }  // 固件大小已经验证为正数。

  Update.onProgress([](size_t progress, size_t total) {  // 注册底层 OTA 写入进度回调。
    if (total == 0) {  // 防止总字节数异常时发生除零错误。
      return;  // 没有有效总长度时跳过本次进度计算。
    }  // 总字节数有效，可以安全计算百分比。

    static int lastPercent = -1;  // 记住上一次打印的百分比，避免重复输出。
    const int percent = static_cast<int>(progress * 100U / total);  // 将已写入字节数换算为整数百分比。
    if (percent != lastPercent && (percent % 5 == 0 || percent == 100)) {  // 每增加 5% 或完成时输出一次。
      lastPercent = percent;  // 保存本次输出的百分比供下次回调比较。
      Serial.printf("Online OTA progress: %d%%\n", percent);  // 在串口打印当前 OTA 写入进度。
    }  // 结束进度输出条件。
  });  // 完成底层 OTA 进度回调注册。

  if (!Update.begin(static_cast<size_t>(firmwareSize), U_FLASH, LED_PIN, HIGH)) {  // 准备非运行中的 OTA 分区并启用 LED 指示。
    Serial.printf("Unable to begin OTA: %s\n", Update.errorString());  // 输出 OTA 分区初始化错误。
    firmwareRequest.end();  // 释放固件 HTTP 请求资源。
    return;  // 无法准备 OTA 分区时继续运行原固件。
  }  // OTA 分区已经准备好接收新固件。

  if (!Update.setMD5(expectedMd5.c_str())) {  // 把 firmware.md5 设置为写入结束时必须匹配的值。
    Serial.println("Unable to set expected firmware MD5.");  // 提示 MD5 无法交给底层更新器。
    Update.abort();  // 中止本次 OTA 并保持新分区不可启动。
    firmwareRequest.end();  // 释放固件 HTTP 请求资源。
    return;  // 设置校验值失败时继续运行原固件。
  }  // 底层更新器已经保存期望的 MD5。

  const size_t written =  // 保存实际写入 OTA 分区的字节数。
      Update.writeStream(*firmwareRequest.getStreamPtr());  // 边接收网络数据边写入非运行中的 OTA 分区。
  firmwareRequest.end();  // 固件流写入结束后释放 HTTP 连接和相关内存。

  if (written != static_cast<size_t>(firmwareSize)) {  // 检查实际写入量是否等于服务器声明的文件长度。
    Serial.printf("Incomplete firmware download: %u/%d bytes.\n",  // 输出已写入字节数和期望字节数。
                  static_cast<unsigned int>(written), firmwareSize);  // 将两个长度值填入错误信息。
    Update.abort();  // 中止本次 OTA，确保不完整固件不会被设为启动分区。
    return;  // 保持现有固件运行。
  }  // 固件已经完整写入 OTA 分区。

  if (!Update.end()) {  // 完成写入并自动比较实际 MD5 与 firmware.md5。
    Serial.printf("OTA verification failed: %s\n", Update.errorString());  // 输出包括 MD5 不匹配在内的校验错误。
    return;  // 校验失败时不会激活新分区，继续运行原固件。
  }  // 固件写入和 MD5 校验均已成功。

  if (!Update.isFinished()) {  // 再次确认底层更新器已经处理完所有固件数据。
    Serial.println("OTA did not finish; the existing firmware remains active.");  // 提示 OTA 状态异常。
    return;  // 状态不完整时不主动重启设备。
  }  // OTA 更新器确认整个流程已经完成。

  Serial.println("Firmware MD5 verified successfully.");  // 明确记录 firmware.md5 校验已经通过。
  Serial.println("Online OTA complete; restarting...");  // 提示设备即将重启进入新固件。
  delay(500);  // 留出时间让最后两行串口日志完整发送。
  ESP.restart();  // 重启 ESP32，并由引导程序启动刚刚验证通过的新分区。
}  // 结束在线固件检查和升级函数。

void setupOTA() {  // 配置电脑主动向 ESP32 推送固件的备用 OTA 服务。
  ArduinoOTA.setHostname(OTA_HOSTNAME);  // 设置网络中识别设备所使用的 OTA 主机名。
  ArduinoOTA.setPassword(OTA_PASSWORD);  // 设置手动 OTA 上传时必须提供的密码。

  ArduinoOTA.onStart([]() {  // 注册手动 OTA 开始时执行的回调。
    const char *type =  // 创建字符串指针，用来说明本次更新的内容类型。
        ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem";  // 区分固件更新和文件系统更新。
    Serial.printf("OTA update started: %s\n", type);  // 在串口打印本次更新的内容类型。
    digitalWrite(LED_PIN, HIGH);  // 点亮板载 LED，表示手动 OTA 正在进行。
  });  // 完成手动 OTA 开始回调注册。

  ArduinoOTA.onEnd([]() {  // 注册手动 OTA 成功结束时执行的回调。
    digitalWrite(LED_PIN, LOW);  // 升级完成后关闭板载 LED。
    Serial.println("\nOTA update complete, restarting...");  // 打印升级完成及即将重启的提示。
  });  // 完成手动 OTA 结束回调注册。

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {  // 注册手动 OTA 进度回调。
    const unsigned int percent = total == 0 ? 0 : progress * 100U / total;  // 安全地计算当前完成百分比。
    Serial.printf("Progress: %u%%\r", percent);  // 在同一串口行中持续刷新上传进度。
  });  // 完成手动 OTA 进度回调注册。

  ArduinoOTA.onError([](ota_error_t error) {  // 注册手动 OTA 发生错误时执行的回调。
    digitalWrite(LED_PIN, LOW);  // 出错后关闭 LED，结束升级指示状态。
    Serial.printf("\nOTA error [%u]: ", error);  // 打印 OTA 错误编号。

    switch (error) {  // 根据错误编号选择更具体的错误说明。
      case OTA_AUTH_ERROR:  // 处理 OTA 密码验证失败。
        Serial.println("authentication failed");  // 输出认证失败提示。
        break;  // 结束当前错误分支。
      case OTA_BEGIN_ERROR:  // 处理无法开始写入更新的错误。
        Serial.println("begin failed");  // 输出初始化失败提示。
        break;  // 结束当前错误分支。
      case OTA_CONNECT_ERROR:  // 处理 OTA 网络连接失败。
        Serial.println("connection failed");  // 输出连接失败提示。
        break;  // 结束当前错误分支。
      case OTA_RECEIVE_ERROR:  // 处理接收固件数据失败。
        Serial.println("receive failed");  // 输出接收失败提示。
        break;  // 结束当前错误分支。
      case OTA_END_ERROR:  // 处理结束固件写入时发生的错误。
        Serial.println("end failed");  // 输出结束阶段失败提示。
        break;  // 结束当前错误分支。
      default:  // 处理框架未明确列出的其他 OTA 错误。
        Serial.println("unknown error");  // 输出未知错误提示。
        break;  // 结束默认错误分支。
    }  // 所有 OTA 错误类型处理完毕。
  });  // 完成手动 OTA 错误回调注册。

  ArduinoOTA.begin();  // 启动手动 OTA 网络服务，开始监听上传请求。
  Serial.printf("OTA ready: %s.local\n", OTA_HOSTNAME);  // 打印可用于访问设备的局域网主机名。
}  // 结束手动 OTA 配置函数。
}  // namespace：结束仅供本文件使用的匿名命名空间。

void setup() {  // Arduino 启动函数，设备每次开机或复位后执行一次。
  pinMode(LED_PIN, OUTPUT);  // 将板载 LED 引脚配置为数字输出模式。
  digitalWrite(LED_PIN, LOW);  // 初始化时先关闭板载 LED。

  Serial.begin(115200);  // 以 115200 波特率启动串口，供日志和调试使用。
  delay(500);  // 等待串口和硬件稳定，避免丢失最早的启动日志。

  connectWiFi();  // 连接配置文件中指定的 Wi-Fi 网络。
  checkForFirmwareUpdate();  // 联网后立即检查本地服务器是否有新固件。
  setupOTA();  // 在线检查结束后启动备用的手动 OTA 服务。
}  // 启动初始化流程全部完成。

void loop() {  // Arduino 主循环，初始化完成后持续重复执行。
  ArduinoOTA.handle();  // 处理可能到达的手动 OTA 网络请求。

  const unsigned long now = millis();  // 读取设备启动至今经过的毫秒数。
  if (now - lastBlinkTime >= BLINK_INTERVAL_MS) {  // 使用非阻塞方式判断是否到达闪烁间隔。
    lastBlinkTime = now;  // 保存本次 LED 翻转的时间。
    ledState = !ledState;  // 将 LED 状态在亮和灭之间切换。
    digitalWrite(LED_PIN, ledState);  // 把新状态输出到板载 LED 引脚。
  }  // 本次 LED 定时处理结束。
}  // 本轮主循环结束，Arduino 框架会立即再次调用。
