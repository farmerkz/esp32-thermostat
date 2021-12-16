#include <main.h>
#include "esp_task_wdt.h"

#ifdef RELEASE
#ifdef DEBUG
#undef DEBUG
#endif
#endif

#ifdef DEBUG
#include <logging.h>
#endif

// ====================================================================================
// Секция объявлений переменных
// ====================================================================================

// IPAddress srvip = IPAddress(MQTT_SERVER); // IP адрес MQTT сервера
char srvip[] = MQTT_SERVER;
WiFiUDP ntpUDP;                           // UDP протокол для NTP
WiFiUDP controlUDP;                       // UDP протокол для резервного управления
NTPClient timeClient(ntpUDP);             // NTP клиент для синхронизации времени
WiFiClient cl;                            // Клиент WiFi для mqtt клиента
MQTTClient mqtt;                          // mqtt клиент
Preferences preferences;                  // Для хранения настроек в EEPROM
OneWire oneWire(TERMO_DATA_1);            // Для работы с датчиками температуры
DallasTemperature sensors(&oneWire);      // Сенсоры температуры
RTC_DS3231 rtc;                           // Часы реального времени
EventGroupHandle_t eventGroup_1;          // Эвент-группа для разных флагов
xQueueHandle queueRelayLowTemp;           // Очередь для температуры включения реле
xQueueHandle queueRelayDelta;             // Очередь для температуры выключения реле
xQueueHandle queuePowerOn;                // Очередь для флага разрешения включения нагрева
xQueueHandle queueWorkSensor;             // Очередь для настроки рабочего сенсора
xQueueHandle queueTempSens0;              // Очередь для температуры датчика 0
xQueueHandle queueTempSens1;              // Очередь для температуры датчика 1
xQueueHandle queueSensRTC;                // Очередь для температуры RTC
xQueueHandle queueHeating;                // Очередь для признака включенного нагрева
xQueueHandle queueTopicReady;             // Признак топика, из которого пришли данные
TaskHandle_t taskTimeMonitor = NULL;      // Хэндл задачи мониторинга системного времени
TaskHandle_t taskGetTempSens = NULL;      // Хэндл задачи опроса датчиков DS18B20
TaskHandle_t taskGetRtcTemp = NULL;       // Хэндл задачи опроса датчика температуры RTC
TaskHandle_t taskRelayOnOff = NULL;       // Хэндл задачи управления реле
TaskHandle_t taskPrefSave = NULL;         // Хэндл задачи сохранения настроек в EEPROM
TaskHandle_t taskMonitoringWiFi = NULL;   // Хэндл задачи мониторинга WiFi
TaskHandle_t taskMonitoringMqtt = NULL;   // Хэндл задачи мониторинга MQTT сервера
TaskHandle_t taskSendToUdpServer = NULL;  // Хэндл задачи отправки данных на UDP сервер
TaskHandle_t taskPublishAllTopics = NULL; // Хэндл задачи публикации всех топиков

// Топики
char topicLowTemp[] = MQTT_CLIENT_ID "/" TOPIC_LOW_TEMP;
char topicDelta[] = MQTT_CLIENT_ID "/" TOPIC_DELTA;
char topicPowerOn[] = MQTT_CLIENT_ID "/" TOPIC_POWER_ON;
char topicWorkSensor[] = MQTT_CLIENT_ID "/" TOPIC_WORK_SENSOR;
char topicHeating[] = MQTT_CLIENT_ID "/" TOPIC_HEATING;
char topicSensor0[] = MQTT_CLIENT_ID "/" TOPIC_SENSOR0;
char topicSensor1[] = MQTT_CLIENT_ID "/" TOPIC_SENSOR1;
char topicSensorRtc[] = MQTT_CLIENT_ID "/" TOPIC_SENSOR_RTC;

// ====================================================================================
// Объявления для WEB-сервера OTA
// ====================================================================================

WebServer server(80);

const char *loginIndex =
    "<form name='loginForm'>"
    "<table width='20%' bgcolor='A09F9F' align='center'>"
    "<tr>"
    "<td colspan=2>"
    "<center><font size=4><b>ESP32 Login Page</b></font></center>"
    "<br>"
    "</td>"
    "<br>"
    "<br>"
    "</tr>"
    "<tr>"
    "<td>Username:</td>"
    "<td><input type='text' size=25 name='userid'><br></td>"
    "</tr>"
    "<br>"
    "<br>"
    "<tr>"
    "<td>Password:</td>"
    "<td><input type='Password' size=25 name='pwd'><br></td>"
    "<br>"
    "<br>"
    "</tr>"
    "<tr>"
    "<td><input type='submit' onclick='check(this.form)' value='Login'></td>"
    "</tr>"
    "</table>"
    "</form>"
    "<script>"
    "function check(form)"
    "{"
    "if(form.userid.value=='" OTA_USER "' && form.pwd.value=='" OTA_PASSWORD "')"
    "{"
    "window.open('/serverIndex')"
    "}"
    "else"
    "{"
    " alert('Error Password or Username')/*displays error message*/"
    "}"
    "}"
    "</script>";

/*
 * Server Index Page
 */

const char *serverIndex =
    "<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>"
    "<form method='POST' action='#' enctype='multipart/form-data' id='upload_form'>"
    "<input type='file' name='update'>"
    "<input type='submit' value='Update'>"
    "</form>"
    "<div id='prg'>progress: 0%</div>"
    "<script>"
    "$('form').submit(function(e){"
    "e.preventDefault();"
    "var form = $('#upload_form')[0];"
    "var data = new FormData(form);"
    " $.ajax({"
    "url: '/update',"
    "type: 'POST',"
    "data: data,"
    "contentType: false,"
    "processData:false,"
    "xhr: function() {"
    "var xhr = new window.XMLHttpRequest();"
    "xhr.upload.addEventListener('progress', function(evt) {"
    "if (evt.lengthComputable) {"
    "var per = evt.loaded / evt.total;"
    "$('#prg').html('progress: ' + Math.round(per*100) + '%');"
    "}"
    "}, false);"
    "return xhr;"
    "},"
    "success:function(d, s) {"
    "console.log('success!')"
    "},"
    "error: function (a, b, c) {"
    "}"
    "});"
    "});"
    "</script>";

bool beginOTA = false;

// ====================================================================================

// ====================================================================================
// Секция кода
// ====================================================================================

// ====================================================================================
// Подпрограммы
// ====================================================================================

/** @brief Callback функция обработки топика 
 * с mqtt сервера
 * 
 * @param topic имя топика
 * @param payloaf сотержимое топика
 * 
 * @return устанавливает значение очереди queueTopicReady в значения:
 * NO_DATA, LOW_TEMP, DELTA, POWER_ON, WORK_SENSOR.
 * Принятые из топика данные сохраняются в очередях.
 */
void mqttCallback(String &topic, String &payload)
{
  uint8_t _topic;
  float _topicData;
  uint16_t _topicDataUint;
  bool _powerOn;

  if (strcmp(topic.c_str(), topicLowTemp) == 0) // Температура включения реле
  {
    _topicData = atof(payload.c_str());
    xQueueOverwrite(queueRelayLowTemp, &_topicData);
    _topic = LOW_TEMP;
  }
  else if (strcmp(topic.c_str(), topicDelta) == 0) // Температура выключения реде (дельта)
  {
    _topicData = abs(atof(payload.c_str()));
    xQueueOverwrite(queueRelayDelta, &_topicData);
    _topic = DELTA;
  }
  else if (strcmp(topic.c_str(), topicPowerOn) == 0) // Разрешение нагрева
  {
    _topicData = abs(atof(payload.c_str()));
    if (_topicData == 0)
    {
      _powerOn = false;
    }
    else
    {
      _powerOn = true;
    }
    xQueueOverwrite(queuePowerOn, &_powerOn);
    _topic = POWER_ON;
  }
  else if (strcmp(topic.c_str(), topicWorkSensor) == 0) // Номер рабочего сенсора
  {
    _topicDataUint = (uint16_t)abs(atof(payload.c_str()));

    if (_topicDataUint >= 2)
    {
      _topicDataUint = 1;
    }
    else
    {
      _topicDataUint = 0;
    }
    xQueueOverwrite(queueWorkSensor, &_topicDataUint);
    _topic = WORK_SENSOR;
  }
  else
  {
    _topic = NO_DATA;
  }

  xQueueOverwrite(queueTopicReady, &_topic);

} // mqttCallback()

// ------------------------------------------------------------------------------------

/**
 * @brief Сохранение настроек в EEPROM
 * 
 * @param pvParameters 
 */
void prefSave(void *pvParameters)
{
  uint8_t _topic;
  float _topicData;
  uint16_t _topicDataUint;
  bool _powerOn;
  while (1)
  {
    xQueueReceive(queueTopicReady, &_topic, portMAX_DELAY);
    switch (_topic)
    {
    case LOW_TEMP:
      xQueuePeek(queueRelayLowTemp, &_topicData, portMAX_DELAY);
      preferences.putFloat(PREF_LOW_TEMP, _topicData);
      break;
    case DELTA:
      xQueuePeek(queueRelayDelta, &_topicData, portMAX_DELAY);
      preferences.putFloat(PREF_DELTA, _topicData);
      break;
    case POWER_ON:
      xQueuePeek(queuePowerOn, &_powerOn, portMAX_DELAY);
      preferences.putBool(PREF_POWER_ON, _powerOn);
      break;
    case WORK_SENSOR:
      xQueuePeek(queueWorkSensor, &_topicDataUint, portMAX_DELAY);
      preferences.putUShort(PREF_WORK_SENSOR, _topicDataUint);
      break;
    case NO_DATA:
      break;
    default:
      break;
    }
    delay(10);
  }
} // prefSave()

// ------------------------------------------------------------------------------------

/** @brief Callback обработки события WiFi SYSTEM_EVENT_STA_CONNECTED
 * 
 * @param event
 * @param info
 * 
 * @return сбрасывает флаг wifiOnline
 */
void WiFiStationConnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
#ifdef DEBUG
  LOG_NOTICE("WIFI", "Station Connected");
#endif
  xEventGroupClearBits(eventGroup_1, WIFI_ONLINE);
} // WiFiStationConnected()

// ------------------------------------------------------------------------------------

/** @brief Callback обработки события WiFi SYSTEM_EVENT_STA_GOT_IP
 * 
 * @param event
 * @param info
 * 
 * @return устаналивает флаг wifiOnline
 */
void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info)
{
#ifdef DEBUG
  LOG_NOTICE("WIFI", "WiFI got IP address: " << WiFi.localIP());
#endif
  xEventGroupSetBits(eventGroup_1, WIFI_ONLINE);
} // WiFiGotIP()

// ------------------------------------------------------------------------------------

/** @brief Callback обработки событий WiFi SYSTEM_EVENT_STA_DISCONNECTED
 * и SYSTEM_EVENT_STA_LOST_IP
 * 
 * @param event
 * @param info
 * 
 * @return сбрасывает флаг wifiOnline
 */
void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
#ifdef DEBUG
  LOG_NOTICE("WIFI", "WiFi lost connection. Reason: " << info.disconnected.reason);
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  xEventGroupClearBits(eventGroup_1, WIFI_ONLINE);
} // WiFiStationDisconnected()

// ------------------------------------------------------------------------------------

/** @brief Синхронизация системного времени с NTP сервером
 * 
 */
bool ntpGetDateTime(void)
{
  timeval _tv; // Системное время
  timeClient.begin();
  timeClient.setTimeOffset(OFFSET_TZ);
  uint8_t i = 0;
  while (!timeClient.update())
  {
    timeClient.forceUpdate();
    i++;
    if (i > 100)
    {
      timeClient.end();
      return false;
    }
    delay(100);
  }
  _tv.tv_sec = timeClient.getEpochTime();
  _tv.tv_usec = 0;
  settimeofday(&_tv, NULL);
  xEventGroupSetBits(eventGroup_1, SYSTEM_TIME_GOOD);
  rtc.adjust(DateTime(timeClient.getFormattedDate().c_str()));
#ifdef DEBUG
  LOG_NOTICE("NTP", "Установлено системное время от NTP");
  LOG_NOTICE("NTP", "Установлено время в RTC от NTP");
#endif
  timeClient.end();
  return true;
} // ntpGetDateTime()

// ------------------------------------------------------------------------------------

/** @brief Переключение реле и сохранение флага в очереди queueHeating
 * 
 * @param on флаг включение/выключение
 */
void RelaySW(bool on)
{
  if (on)
  {
    digitalWrite(RELAY_1, HIGH);
    digitalWrite(RELAY_2, HIGH);
  }
  else
  {
    digitalWrite(RELAY_1, LOW);
    digitalWrite(RELAY_2, LOW);
  }
  xQueueOverwrite(queueHeating, &on);
} // RelaySW()

// ------------------------------------------------------------------------------------

/** @brief Подписывает на топики на MQTT сервере
 * 
 * @return true/false
 * */
bool subscribeTopics(void)
{
  if (mqtt.subscribe(topicLowTemp, 2) &&  // Температура включения
      mqtt.subscribe(topicDelta, 2) &&    // Температура выключения
      mqtt.subscribe(topicPowerOn, 2) &&  // Разрешение нагрева
      mqtt.subscribe(topicWorkSensor, 2)) // Рабочий датчик температуры
  {
    return true;
  }
  else
  {
    return false;
  }
} // subscribeTopics()

// ------------------------------------------------------------------------------------

/** @brief Публикация всех топиков на MQTT сервер
 * 
 */
void publishAllTopics(void *pvParameters)
{
  uint8_t _msgLen;            // Длина сообщения
  char _msgBuf[MQTT_MAX_LEN]; // Буфер сообщения
  float _tmpValFloat;         // Временная переменная для float значений
  bool _tmpValBool;           // Временная переменная для логических значений
  uint16_t _workSensor;       // Временная - рабочий датчик температуры
  EventBits_t _eventBits;     // Флаги

  while (1)
  {
    delay(500);
    _eventBits = xEventGroupGetBits(eventGroup_1);

    if (((_eventBits & WIFI_ONLINE) != 0) &&
        ((_eventBits & MQTT_ONLINE) != 0))
    {
      mqtt.loop();
      delay(10);
      if (mqtt.connected())
      {
        xEventGroupSetBits(eventGroup_1, MQTT_ONLINE);

        // 1. Температура включения реле
        // -----------------------------
        xQueuePeek(queueRelayLowTemp, &_tmpValFloat, portMAX_DELAY);
        _msgLen = sprintf(_msgBuf, "%f\n", _tmpValFloat);
        mqtt.publish(topicLowTemp, _msgBuf, _msgLen, false, 2);
        delay(10);
        if (!mqtt.connected())
        {
          xEventGroupClearBits(eventGroup_1, MQTT_ONLINE);
          continue;
        }

        // 2. Температура выключения (дельта) реле
        // ---------------------------------------
        xQueuePeek(queueRelayDelta, &_tmpValFloat, portMAX_DELAY);
        _msgLen = sprintf(_msgBuf, "%f\n", _tmpValFloat);
        mqtt.publish(topicDelta, _msgBuf, _msgLen, false, 2);
        delay(10);
        if (!mqtt.connected())
        {
          xEventGroupClearBits(eventGroup_1, MQTT_ONLINE);
          continue;
        }

        // 3. Разрешение включения реле
        // ----------------------------
        xQueuePeek(queuePowerOn, &_tmpValBool, portMAX_DELAY);
        mqtt.publish(topicPowerOn, _tmpValBool ? "1" : "0", false, 2);
        delay(10);
        if (!mqtt.connected())
        {
          xEventGroupClearBits(eventGroup_1, MQTT_ONLINE);
          continue;
        }

        // 4. Рабочий датчик
        // -----------------
        xQueuePeek(queueWorkSensor, &_workSensor, portMAX_DELAY);
        _msgLen = sprintf(_msgBuf, "%d\n", (_workSensor + 1));
        mqtt.publish(topicWorkSensor, _msgBuf, _msgLen, false, 2);
        delay(10);
        if (!mqtt.connected())
        {
          xEventGroupClearBits(eventGroup_1, MQTT_ONLINE);
          continue;
        }

        // 5. Температура датчика 0
        // ------------------------
        xQueuePeek(queueTempSens0, &_tmpValFloat, portMAX_DELAY);
        _msgLen = sprintf(_msgBuf, "%f\n", _tmpValFloat);
        mqtt.publish(topicSensor0, _msgBuf, _msgLen, false, 2);
        delay(10);
        if (!mqtt.connected())
        {
          xEventGroupClearBits(eventGroup_1, MQTT_ONLINE);
          continue;
        }

        // 6. Температура датчика 1
        // ------------------------
        xQueuePeek(queueTempSens1, &_tmpValFloat, portMAX_DELAY);
        _msgLen = sprintf(_msgBuf, "%f\n", _tmpValFloat);
        mqtt.publish(topicSensor1, _msgBuf, _msgLen, false, 2);
        delay(10);
        if (!mqtt.connected())
        {
          xEventGroupClearBits(eventGroup_1, MQTT_ONLINE);
          continue;
        }

        // 7. Температура датчка RTC
        // -------------------------
        xQueuePeek(queueSensRTC, &_tmpValFloat, portMAX_DELAY);
        _msgLen = sprintf(_msgBuf, "%f\n", _tmpValFloat);
        mqtt.publish(topicSensorRtc, _msgBuf, _msgLen, false, 2);
        delay(10);
        if (!mqtt.connected())
        {
          xEventGroupClearBits(eventGroup_1, MQTT_ONLINE);
          continue;
        }

        // 8. Признак включенного реле
        // ---------------------------
        xQueuePeek(queueHeating, &_tmpValBool, portMAX_DELAY);
        mqtt.publish(topicHeating, _tmpValBool ? "1" : "0", false, 2);
        delay(10);
        if (!mqtt.connected())
        {
          xEventGroupClearBits(eventGroup_1, MQTT_ONLINE);
          continue;
        }
      }
      else
      {
        xEventGroupClearBits(eventGroup_1, MQTT_ONLINE);
      }
    }
  }
} // publishAllTopics()

// ------------------------------------------------------------------------------------

/** @brief Отправляем все данные на UDP сервер
 * 
 */
void sendToUdpServer(void *pvParameters)
{
  float _tmpValFloat;   // Временная переменная для float значений
  bool _tmpValBool;     // Временная переменная для логических значений
  uint16_t _workSensor; // Временная - рабочий датчик температуры
  while (1)
  {
    // 1. Температура включения реле
    // -----------------------------
    xQueuePeek(queueRelayLowTemp, &_tmpValFloat, portMAX_DELAY);
    sendGelfEvent(TOPIC_LOW_TEMP, _tmpValFloat);
    delay(10);

    // 2. Температура выключения (дельта) реле
    // ---------------------------------------
    xQueuePeek(queueRelayDelta, &_tmpValFloat, portMAX_DELAY);
    sendGelfEvent(TOPIC_DELTA, _tmpValFloat);
    delay(10);

    // 3. Разрешение включения реле
    // ----------------------------
    xQueuePeek(queuePowerOn, &_tmpValBool, portMAX_DELAY);
    sendGelfEvent(TOPIC_POWER_ON, _tmpValBool);
    delay(10);

    // 4. Рабочий датчик
    // -----------------
    xQueuePeek(queueWorkSensor, &_workSensor, portMAX_DELAY);
    sendGelfEvent(TOPIC_WORK_SENSOR, uint16_t(_workSensor + 1));
    delay(10);

    // 5. Температура датчика 0
    // ------------------------
    xQueuePeek(queueTempSens0, &_tmpValFloat, portMAX_DELAY);
    sendGelfEvent(TOPIC_SENSOR0, _tmpValFloat);
    delay(10);

    // 6. Температура датчика 1
    // ------------------------
    xQueuePeek(queueTempSens1, &_tmpValFloat, portMAX_DELAY);
    sendGelfEvent(TOPIC_SENSOR1, _tmpValFloat);
    delay(10);

    // 7. Температура датчка RTC
    // -------------------------
    xQueuePeek(queueSensRTC, &_tmpValFloat, portMAX_DELAY);
    sendGelfEvent(TOPIC_SENSOR_RTC, _tmpValFloat);
    delay(10);

    // 8. Признак включенного реле
    // ---------------------------
    xQueuePeek(queueHeating, &_tmpValBool, portMAX_DELAY);
    sendGelfEvent(TOPIC_HEATING, _tmpValBool);
    delay(430);
  }
} // sendToUdpServer()

// ------------------------------------------------------------------------------------
/**
 * @brief Формирование и отправка данных в формате GELF UDP
 * 
 * @param _topic имя топика
 * @param _payload значение
 */
void sendGelfEvent(const char *_topic, float _payload)
{
  char _time[DATE_BUF_SIZE];
  getSystemTime(_time);
  controlUDP.beginPacket(UDP_REMOTE_IP, UDP_REMOTE_PORT);
  controlUDP.printf("{\"version\":\"1.1\",");
  controlUDP.printf("\"host\":\"%s\",", MQTT_CLIENT_ID);
  controlUDP.printf("\"short_message\":\"%s = %.2f\",", _topic, _payload);
  controlUDP.printf("\"topic\":\"%s\",", _topic);
  controlUDP.printf("\"payload\":\"%.2f\",", _payload);
  controlUDP.printf("\"type\":\"float\",");
  controlUDP.printf("\"_localtime\":\"%s\"}", _time);
  controlUDP.endPacket();
} // sendGelfEvent()

void sendGelfEvent(const char *_topic, uint16_t _payload)
{
  char _time[DATE_BUF_SIZE];
  getSystemTime(_time);
  controlUDP.beginPacket(UDP_REMOTE_IP, UDP_REMOTE_PORT);
  controlUDP.printf("{\"version\":\"1.1\",");
  controlUDP.printf("\"host\":\"%s\",", MQTT_CLIENT_ID);
  controlUDP.printf("\"short_message\":\"%s = %d\",", _topic, _payload);
  controlUDP.printf("\"topic\":\"%s\",", _topic);
  controlUDP.printf("\"payload\":\"%d\",", _payload);
  controlUDP.printf("\"type\":\"int\",");
  controlUDP.printf("\"_localtime\":\"%s\"}", _time);
  controlUDP.endPacket();
} // sendGelfEvent()

void sendGelfEvent(const char *_topic, bool _payload)
{
  char _time[DATE_BUF_SIZE];
  getSystemTime(_time);
  controlUDP.beginPacket(UDP_REMOTE_IP, UDP_REMOTE_PORT);
  controlUDP.printf("{\"version\":\"1.1\",");
  controlUDP.printf("\"host\":\"%s\",", MQTT_CLIENT_ID);
  controlUDP.printf("\"short_message\":\"%s = %s\",", _topic, _payload ? "true" : "false");
  controlUDP.printf("\"topic\":\"%s\",", _topic);
  controlUDP.printf("\"payload\":\"%s\",", _payload ? "true" : "false");
  controlUDP.printf("\"type\":\"bool\",");
  controlUDP.printf("\"_localtime\":\"%s\"}", _time);
  controlUDP.endPacket();
} // sendGelfEvent()

// ------------------------------------------------------------------------------------

/** @brief Мониторинг системного времени. Ожидает синхронизации с NTP сервером,
 * устанавливает время в RTC. Выставляет флаги обновления времени в RTC и 
 * синхронизации с NTP сервером.
 * 
 * @param pvParameters стандартный параметр задачи FreeRTOS
 */
void timeMonitor(void *pvParameters)
{

  delay(10000);

  while (1)
  {
    xEventGroupWaitBits(eventGroup_1, WIFI_ONLINE, pdFALSE, pdFALSE, portMAX_DELAY);
    ntpGetDateTime();
    delay(20000);
  }
} // timeMonitor()

// ------------------------------------------------------------------------------------

/** @brief Запрашиваем и читаем температуру двух датчиков DS18B20. Рузультат сохраняем
 * в очередях queueTempSens0 и queueTempSens1.
 * Если температура меньше TEMP_BAD_SENSOR, вероятнее всего датчик неисправен либо отсутсвует, либо
 * ошибка чтения, в этом случаем снимаем для данного датчика флаг валидности температуры
 * 
 * @param pvParameters
 */
void getTempSens(void *pvParameters)
{
  float _tempSens0 = 0;
  float _tempSens1 = 0;
  xEventGroupClearBits(eventGroup_1, SENSOR_0_GOOD | SENSOR_1_GOOD);
  while (1)
  {
    sensors.setWaitForConversion(false);
    sensors.requestTemperatures();
    sensors.setWaitForConversion(true);
    delay(800);
    _tempSens0 = sensors.getTempCByIndex(0);
    _tempSens0 = sensors.getTempCByIndex(0);
    xQueueOverwrite(queueTempSens0, &_tempSens0);
    _tempSens1 = sensors.getTempCByIndex(1);
    xQueueOverwrite(queueTempSens1, &_tempSens1);
    if (_tempSens0 >= TEMP_BAD_SENSOR)
    {
      xEventGroupSetBits(eventGroup_1, SENSOR_0_GOOD);
    }
    else
    {
      xEventGroupClearBits(eventGroup_1, SENSOR_0_GOOD);
    }
    if (_tempSens1 >= TEMP_BAD_SENSOR)
    {
      xEventGroupSetBits(eventGroup_1, SENSOR_1_GOOD);
    }
    else
    {
      xEventGroupClearBits(eventGroup_1, SENSOR_1_GOOD);
    }
    delay(10);
  }
} // getTempSens()

// ------------------------------------------------------------------------------------

/** @brief Читаем температуру датчика в RTC. Результат сохраняем в очереди
 * queueSensRTC
 * 
 * @param pvParameters
 */
void getRtcTemp(void *pvParameters)
{
  float _tempSens = 0;
  while (1)
  {
    xEventGroupWaitBits(eventGroup_1, RTC_PRESENT, pdFALSE, pdFALSE, portMAX_DELAY);
#ifdef DEBUG
    LOG_NOTICE("SETUP", "Снимаем температуру RTC");
#endif
    _tempSens = rtc.getTemperature();
    xQueueOverwrite(queueSensRTC, &_tempSens);
    delay(1000);
  }
} // getRtcTemp()

// ------------------------------------------------------------------------------------
/** @brief Проверка разрешения включения нагрева, текущей температуры рабочего датчика
 * температуры, флага валидности температуры рабочего датчика и по результатам включение 
 * или выключение реле. В очередь пишем состояние реле
 * 
 * @param pvParameters
 */
void relayOnOff(void *pvParameters)
{
  float _relayLowTemp = 0;   // Температура включения реле (читаем из очереди)
  float _relayUpperTemp = 0; // Температура выключения реде (читаем дельту из очереди и пересчитываем)
  bool _powerOn = false;     // Разрешение включения нагрева (читаем из очереди)
  uint16_t _workSensor = 0;  // Номер рабочего сенсора (читаем из очереди)
  float _tempSens0;          // Температура датчика 0 (читаем из очереди)
  float _tempSens1;          // Температура датчика 1 (читаем из очереди)
  bool _heatingOn = false;   // Нагрев включен (пишем в очередь)
  EventBits_t _eventBits;    // Для флагов, нам нужны флаги валидности температуры

  while (1)
  {
    delay(500);
    xQueuePeek(queueRelayLowTemp, &_relayLowTemp, portMAX_DELAY);
    xQueuePeek(queueRelayDelta, &_relayUpperTemp, portMAX_DELAY);
    xQueuePeek(queuePowerOn, &_powerOn, portMAX_DELAY);
    xQueuePeek(queueWorkSensor, &_workSensor, portMAX_DELAY);
    xQueuePeek(queueTempSens0, &_tempSens0, portMAX_DELAY);
    xQueuePeek(queueTempSens1, &_tempSens1, portMAX_DELAY);

    _relayUpperTemp = _relayLowTemp + _relayUpperTemp;
    _eventBits = xEventGroupGetBits(eventGroup_1); // Загружаем флаги, нужны флаги валидности температуры

    if (_powerOn)
    {
      if ((_workSensor == 0) && ((_eventBits & SENSOR_0_GOOD) != 0))
      {
        if (_tempSens0 <= _relayLowTemp)
        {
          RelaySW(RELAY_ON);
          _heatingOn = true;
        }
        else if (_tempSens0 > _relayUpperTemp)
        {
          RelaySW(RELAY_OFF);
          _heatingOn = false;
        }
      }
      else if ((_workSensor == 1) && ((_eventBits & SENSOR_1_GOOD) != 0))
      {
        if (_tempSens1 <= _relayLowTemp)
        {
          RelaySW(RELAY_ON);
          _heatingOn = true;
        }
        else if (_tempSens1 > _relayUpperTemp)
        {
          RelaySW(RELAY_OFF);
          _heatingOn = false;
        }
      }
      else
      {
        RelaySW(RELAY_OFF);
        _heatingOn = false;
      }
    }
    else
    {
      RelaySW(RELAY_OFF);
      _heatingOn = false;
    }
    xQueueOverwrite(queueHeating, &_heatingOn);
  }
}

// ------------------------------------------------------------------------------------
/** @brief Подключение к mqtt серверу, если удачно - подписываемся на топики
 * 
 * @return true/false
 */
bool mqttConnecting(void)
{
#ifdef DEBUG
  LOG_NOTICE("MQTT", "Заходим в mqttConnecting");
#endif
  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD))
  {
#ifdef DEBUG
    LOG_NOTICE("MQTT", "mqttConnecting - подключились");
#endif
    delay(50);         //
    subscribeTopics(); // Подписываемся на топики
    return true;       // MQTT сервер доступен
  }                    //
  else                 //
  {
#ifdef DEBUG
    LOG_NOTICE("MQTT", "mqttConnecting - MQTT сервер не доступен");
#endif            //
    return false; // MQTT сервер не доступен
  }               //
} // mqttConnecting()

// ------------------------------------------------------------------------------------
/**
 * @brief Мониторинг подключения к WiFi. Если сброшен флаг wifiOnline - пытаемся 
 * подключиться.
 * 
 * @param pvParameters 
 */
void monitoringWiFi(void *pvParameters)
{
  EventBits_t _eventBits;
  while (1)
  {
    _eventBits = xEventGroupGetBits(eventGroup_1);
    if ((_eventBits & WIFI_ONLINE) == 0)
    {
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      delay(3000);
    }
    else
    {
      delay(100);
    }
  }
}

/**
 * @brief Мониторинг подключения к серверу MQTT. Если сброшен флаг .... - пытаемся 
 * подключиться
 * 
 * @param pvParameters
 */
void monitoringMqtt(void *pvParameters)
{
  EventBits_t _eventBits;
  while (1)
  {
    _eventBits = xEventGroupGetBits(eventGroup_1);
    if ((_eventBits & MQTT_ONLINE) == 0)
    {
      if (mqttConnecting())
      {
        xEventGroupSetBits(eventGroup_1, MQTT_ONLINE);
      }
      else
      {
        xEventGroupClearBits(eventGroup_1, MQTT_ONLINE);
      }
    }
    delay(100);
  }
}

/**
 * @brief Установка системного времени из RTC
 * 
 */
void setSystemTime(void)
{
  timeval _tv;
  tm _tm;
  DateTime _now;

  _now = rtc.now();
  _tm.tm_year = _now.year() - 1900;
  _tm.tm_mon = _now.month() - 1;
  _tm.tm_mday = _now.day();
  _tm.tm_hour = _now.hour();
  _tm.tm_min = _now.minute();
  _tm.tm_sec = _now.second();
  _tm.tm_isdst = 0;
  _tv.tv_sec = mktime(&_tm);
  _tv.tv_usec = 0;
  settimeofday(&_tv, NULL);
  xEventGroupSetBits(eventGroup_1, SYSTEM_TIME_GOOD);
#ifdef DEBUG
  LOG_NOTICE("SYSTIME", "Установлено системное время из RTC");
#endif
}

void getSystemTime(char *_buf)
{
  char _tmpbuf[DATE_BUF_SIZE];
  time_t _now;
  tm _tm;
  time(&_now);
  localtime_r(&_now, &_tm);
  strftime(_tmpbuf, sizeof(_tmpbuf), "%F %T", &_tm);
  strcpy(_buf, _tmpbuf);
}

// ------------------------------------------------------------------------------------

// ====================================================================================
// Основной блок
// ====================================================================================

void setup()
{
#ifdef DEBUG
  Serial.begin(115200);
#endif
  float relayLowTemp = DEFAULT_LOW_TEMP;     // Температура включения реле
  float relayDelta = DEFAULT_DELTA;          // Температура выключения реле
  bool powerOn = DEFAULT_POWER_ON;           // Флаг разрешения включения нагрева
  uint16_t workSensor = DEFAULT_WORK_SENSOR; // Номер сенсора, который используется для управления реле
  float tempSens0 = DEFAULT_TEMP_SENS_0;     // Температура датчика 0
  float tempSens1 = DEFAULT_TEMP_SENS_1;     // Температура датчика 1
  float tempRTC = DEFAULT_TEMP_SENS_RTC;     // Температура датчика RTC
  bool heatingOn = DEFAULT_HEATING_ON;       // Флаг включенного нагрева
  uint8_t topicStatus = NO_DATA;             // Признак топика, из которого данные пришли

  // Создаем эвент-группы, очереди, мьютексы
  eventGroup_1 = xEventGroupCreate();                        // Event группа 1
  queueRelayLowTemp = xQueueCreate(1, sizeof(relayLowTemp)); // Очередь температуры включения реле
  queueRelayDelta = xQueueCreate(1, sizeof(relayDelta));     // Очередь температуры выключения реле
  queuePowerOn = xQueueCreate(1, sizeof(powerOn));           // Очередь признака разрешения нагрева
  queueWorkSensor = xQueueCreate(1, sizeof(workSensor));     // Очередь номера рабочего сенсора
  queueTempSens0 = xQueueCreate(1, sizeof(tempSens0));       // Очередь для температуры сенсора 0
  queueTempSens1 = xQueueCreate(1, sizeof(tempSens1));       // Очередь для температуры сенсора 1
  queueSensRTC = xQueueCreate(1, sizeof(tempRTC));           // Очередь для температуры RTC
  queueHeating = xQueueCreate(1, sizeof(heatingOn));         // Очередь признака включенного нагрева
  queueTopicReady = xQueueCreate(1, sizeof(topicStatus));    // Очередь признака топика, из которого пришли даные

  // Инициируем эвент-группы и очереди
  xEventGroupClearBits(eventGroup_1, (uint32_t)0x0FFFFFF); // Обнуляем все флаги
  xQueueOverwrite(queueRelayLowTemp, &relayLowTemp);       // Температура включения, по умолчанию
  xQueueOverwrite(queueRelayDelta, &relayDelta);           // Температура выключения, по умолчанию
  xQueueOverwrite(queuePowerOn, &powerOn);                 // Разрешение включения нагрева, выключено
  xQueueOverwrite(queueWorkSensor, &workSensor);           // Рабочий датчик, по умолчанию
  xQueueOverwrite(queueTempSens0, &tempSens0);             // Температура датчика 0 (0)
  xQueueOverwrite(queueTempSens1, &tempSens1);             // Температура датчика 1 (0)
  xQueueOverwrite(queueSensRTC, &tempRTC);                 // Температура RTC (0)
  xQueueOverwrite(queueHeating, &heatingOn);               // Признак включенного нагрева (false)
  xQueueOverwrite(queueTopicReady, &topicStatus);          // Флаг готовности данных из топиков

  // Инициируем пины для управления реле
  pinMode(RELAY_1, OUTPUT);
  pinMode(RELAY_2, OUTPUT);

  // Выключаем нагрузку
  RelaySW(RELAY_OFF);

  // Сбрасывем WiFi
  WiFi.disconnect(true);

  // Инициируем область настроек в EEPROM
  preferences.begin("SavedPrefsT", false);

  // Проверяем наличие ключей для хранения настроек и, при необходимости,
  // создаем их и записываем туда значение по умолчанию
  if (!preferences.isKey(PREF_LOW_TEMP))
  {
    preferences.putFloat(PREF_LOW_TEMP, DEFAULT_LOW_TEMP);
  }
  if (!preferences.isKey(PREF_DELTA))
  {
    preferences.putFloat(PREF_DELTA, DEFAULT_DELTA);
  }
  if (!preferences.isKey(PREF_POWER_ON))
  {
    preferences.putBool(PREF_POWER_ON, DEFAULT_POWER_ON);
  }
  if (!preferences.isKey(PREF_WORK_SENSOR))
  {
    preferences.putUShort(PREF_WORK_SENSOR, DEFAULT_WORK_SENSOR);
  }

  // Читаем настроки из EEPROM
  relayLowTemp = preferences.getFloat(PREF_LOW_TEMP, DEFAULT_LOW_TEMP);
  relayDelta = preferences.getFloat(PREF_DELTA, DEFAULT_DELTA);
  powerOn = preferences.getBool(PREF_POWER_ON, DEFAULT_POWER_ON);
  workSensor = preferences.getUShort(PREF_WORK_SENSOR, DEFAULT_WORK_SENSOR);

  // Инициируем очереди данными из EEPROM
  xQueueOverwrite(queueRelayLowTemp, &relayLowTemp); // Температура включения, из EEPROM
  xQueueOverwrite(queueRelayDelta, &relayDelta);     // Температура выключения, из EEPROM
  xQueueOverwrite(queuePowerOn, &powerOn);           // Разрешение включения нагрева, из EEPROM
  xQueueOverwrite(queueWorkSensor, &workSensor);     // Рабочий датчик, из EEPROM

  // Инициируем WiFi, устанавливаем имя хоста, определяем подпрограммы
  // обработки событий WiFi
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.onEvent(WiFiStationConnected, SYSTEM_EVENT_STA_CONNECTED);
  WiFi.onEvent(WiFiGotIP, SYSTEM_EVENT_STA_GOT_IP);
  WiFi.onEvent(WiFiStationDisconnected, SYSTEM_EVENT_STA_DISCONNECTED);
  WiFi.onEvent(WiFiStationDisconnected, SYSTEM_EVENT_STA_LOST_IP);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  delay(3000);

  // Инициируем датчики температуры, устанавливаем разрешение сенсовров
  sensors.begin();
  delay(10);
  sensors.setResolution(RESOLUTION);

  // Проверяем наличие RTC и его статус
  if (!rtc.begin())
  {
    xEventGroupClearBits(eventGroup_1, RTC_PRESENT); // RTC не обнаружено
    xEventGroupClearBits(eventGroup_1, RTC_GOOD);
#ifdef DEBUG
    LOG_NOTICE("SETUP", "RTC не обнаружен");
#endif
    tempRTC = (float)TEMP_BAD_SENSOR;
    xQueueOverwrite(queueSensRTC, &tempRTC);
  }
  else if (rtc.lostPower())
  {
    xEventGroupSetBits(eventGroup_1, RTC_PRESENT);
    xEventGroupClearBits(eventGroup_1, RTC_GOOD); // RTC обнаружено, время НЕ действительно
#ifdef DEBUG
    LOG_NOTICE("SETUP", "RTC обнаружен, время не действительно");
#endif
    setSystemTime();
  }
  else
  {
    xEventGroupSetBits(eventGroup_1, RTC_PRESENT);
    xEventGroupSetBits(eventGroup_1, RTC_GOOD); // RTC обнаружено, время действительно
#ifdef DEBUG
    LOG_NOTICE("SETUP", "RTC обнаружен, время действительно");
#endif
    setSystemTime();
  }

  // Инициируем MQTT сервер
  mqtt.begin(srvip, MQTT_PORT, cl);
  mqtt.setKeepAlive(10);
  mqtt.onMessage(mqttCallback);
  mqtt.setTimeout(500);

  // ------------------------------------------------------------------------------------
  // Инициализация WEB-сервера и OTA
  // ------------------------------------------------------------------------------------

  if (!MDNS.begin(OTA_HOSTNAME))
  {
#ifdef DEBUG
    LOG_NOTICE("SETUP", "Ошибка инициализации OTA");
#endif
  }

  server.on("/", HTTP_GET, []()
            {
              server.sendHeader("Connection", "close");
              server.send(200, "text/html", loginIndex);
            });
  server.on("/serverIndex", HTTP_GET, []()
            {
              server.sendHeader("Connection", "close");
              server.send(200, "text/html", serverIndex);
            });
  /*handling uploading firmware file */
  server.on(
      "/update", HTTP_POST, []()
      {
        server.sendHeader("Connection", "close");
        server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        ESP.restart();
      },
      []()
      {
        HTTPUpload &upload = server.upload();
        disableLoopWDT();
        if (upload.status == UPLOAD_FILE_START)
        {
          // Serial.printf("Update: %s\n", upload.filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN))
          { //start with max available size
            // Update.printError(Serial);
          }
        }
        else if (upload.status == UPLOAD_FILE_WRITE)
        {
          /* flashing firmware to ESP*/
          if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
          {
            // Update.printError(Serial);
          }
        }
        else if (upload.status == UPLOAD_FILE_END)
        {
          if (Update.end(true))
          { //true to set the size to the current progress
            // Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
          }
          else
          {
            // Update.printError(Serial);
          }
        }
        enableLoopWDT();
      });
  server.begin();

  // ------------------------------------------------------------------------------------

  //Создаем задачи

  // Задача мониторинга системного времени
  xTaskCreatePinnedToCore(
      timeMonitor,
      "SytemTime",
      4000,
      NULL,
      2,
      &taskTimeMonitor,
      1);

  // Задача опроса датчиков температуры DS18B20
  xTaskCreatePinnedToCore(
      getTempSens,
      "GetTempuratureFromSensors",
      4000,
      NULL,
      4,
      &taskGetTempSens,
      1);

  // Ожидаем секунду, чтобы выполнился первый цикл чтения температуры датчиков DS18B20
  delay(1000);

  // Разрешаем WDT основного цикла
  enableLoopWDT();

  // Задача опроса датчиков температуры RTC
  xTaskCreatePinnedToCore(
      getRtcTemp,
      "GetTemperatureFromRTC",
      4000,
      NULL,
      2,
      &taskGetRtcTemp,
      1);

  // Задача управления реле
  xTaskCreatePinnedToCore(
      relayOnOff,
      "Relay Control",
      4000,
      NULL,
      3,
      &taskRelayOnOff,
      1);

  // Задача сохранения настроек в EEPROM
  xTaskCreatePinnedToCore(
      prefSave,
      "Save_Preferences",
      4000,
      NULL,
      2,
      &taskPrefSave,
      1);

  // Задача мониторинга WiFi
  xTaskCreatePinnedToCore(
      monitoringWiFi,
      "WiFi monitoring",
      4000,
      NULL,
      2,
      &taskMonitoringWiFi,
      1);

  // Задача мониторинга MQTT
  xTaskCreatePinnedToCore(
      monitoringMqtt,
      "MQTT monitoring",
      4000,
      NULL,
      2,
      &taskMonitoringMqtt,
      1);

  // Задача отправки данных на UDP сервер
  xTaskCreatePinnedToCore(
      sendToUdpServer,
      "Send data to UDP",
      8000,
      NULL,
      2,
      &taskSendToUdpServer,
      1);
  xTaskCreatePinnedToCore(
      publishAllTopics,
      "Publish all topics",
      4000,
      NULL,
      2,
      &taskPublishAllTopics,
      1);
} // setup()

void loop()
{
  server.handleClient();
  delay(100);
}