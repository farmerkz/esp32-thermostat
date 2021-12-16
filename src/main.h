#ifndef __my_main_h__
#define __my_main_h__

#include <Arduino.h>
#include "WiFi.h"
#include <WiFiClient.h>
#include "freertos/task.h"
#include "RTClib.h"
#include <Wire.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <DallasTemperature.h>
#include <MQTT.h>
#include <Preferences.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>

// Учетные данные для WiFi, MQTT сервера, OTA и прочее

#include <account_thermostat.h>
#ifndef __ACCOUNTINFO__
#define WIFI_SSID "ssid"
#define WIFI_PASS "wifipassword"
#define WIFI_HOSTNAME "hostname"
#define MQTT_SERVER "192.168.0.1"
#define MQTT_PORT 1883
#define MQTT_CLIENT_ID "clientid"
#define MQTT_USER "user"
#define MQTT_PASSWORD "password"
#define OTA_HOSTNAME "hostname"
#define OTA_USER "user"
#define OTA_PASSWORD "password"
#define UDP_REMOTE_IP "192.168.1.1"
#define UDP_REMOTE_PORT 6000
#define UDP_LOCAL_PORT 6000
#endif

#define RELAY_1 16               // Пин Реле 1
#define RELAY_2 17               // Пин Реле 2
#define TERMO_DATA_1 4           // Шина датчиков 18B20
#define RESOLUTION 12            // Разрядность для датчиков температуры
#define RELAY_ON true            // Включение реле
#define RELAY_OFF false          // Выключение реле
#define DEFAULT_LOW_TEMP 5       // Значение по умолчанию температуры включения
#define DEFAULT_DELTA 1          // Значение по умолчанию температуры выключения
#define DEFAULT_POWER_ON true    // Значение по умолчанию разрешения нагрева
#define DEFAULT_WORK_SENSOR 0    // Номер рабочего датчика температуры по умолчанию
#define DEFAULT_TEMP_SENS_0 6    // Значение по умолчанию температуры датчика 0
#define DEFAULT_TEMP_SENS_1 6    // Значение по умолчанию температуры датчика 1
#define DEFAULT_TEMP_SENS_RTC 6  // Значение по умолчанию температуры датчика RTC
#define DEFAULT_HEATING_ON false // Значение по умолчанию разрешения включения нагрева
#define MQTT_MAX_LEN 127         // Максимальная длина сообщения  MQTT
#define OFFSET_TZ 3600 * 6       // Смещение таймзоны
#define TEMP_BAD_SENSOR -30.0    // Нижняя граница возможной температуры сенсоров
#define DATE_BUF_SIZE 80         // Размер массива для форматированной даты

// Топики

#define TOPIC_LOW_TEMP "RelayLowTemp"  // Топик для настройки температуры включения (подписка)
#define TOPIC_DELTA "RelayDelta"       // Топик для настройки температуры выключения (подписка)
#define TOPIC_POWER_ON "PowerOn"       // Топик управления включением - выключением нагрева (подписка)
#define TOPIC_WORK_SENSOR "WorkSensor" // Топик для указания номера рабочего датчика температуры (подписка)
#define TOPIC_HEATING "Heating"        // Топик для индикации включения нагрева (без подписки)
#define TOPIC_SENSOR0 "Sensor0"        // Температура датчика 0 (без подписки)
#define TOPIC_SENSOR1 "Sensor1"        // Температура датчика 1 (без подписки)
#define TOPIC_SENSOR_RTC "SensorRTC"   // Температура датчика RTC (без подписки)

// ключи для Preferences

#define PREF_LOW_TEMP "RelayLowTemp"  // Ключ для сохранения температуры включения
#define PREF_DELTA "RelayDelta"       // Ключ для сохранения температуры выключения
#define PREF_POWER_ON "PowerOn"       // Ключ для сохранения разрешения на включение нагрева
#define PREF_WORK_SENSOR "WorkSensor" // Ключ для сохранения номера рабочего сенсора

// Различные флаги

#define RTC_PRESENT (1 << 1)      // Обнаружен RTC
#define RTC_GOOD (1 << 3)         // RTC обнаружено и время действительно или синхронизировано с NTP
#define SYSTEM_TIME_GOOD (1 << 5) // Системное время синхронизировано с NTP сервером
#define MQTT_ONLINE (1 << 6)      // MQTT сервер онлайн
#define WIFI_ONLINE (1 << 7)      // WiFi Доступен
#define SENSOR_0_GOOD (1 << 8)    // Значение датчика 0 действительное
#define SENSOR_1_GOOD (1 << 9)    // Значение датчика 1 действительное

enum
{
    NO_DATA,    // Данных с mqtt сервера не поступало
    LOW_TEMP,   // Прочитан топик RelayLowTemp
    DELTA,      // Прочитан топик RelayDelta
    POWER_ON,   // Прочитан топик PowerOn
    WORK_SENSOR // Прочитан топик рабочего сенсора
};

// Объявление подпрограмм

void mqttCallback(String &topic, String &payload);
void prefSave(void *pvParameters);
void WiFiStationConnected(WiFiEvent_t event, WiFiEventInfo_t info);
void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info);
void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info);
bool ntpGetDateTime(void);
void RelaySW(bool on);
bool subscribeTopics(void);
void publishAllTopics(void *pvParameters);
void sendToUdpServer(void *pvParameters);
void sendGelfEvent(const char *_topic, float _payload);
void sendGelfEvent(const char *_topic, uint16_t _payload);
void sendGelfEvent(const char *_topic, bool _payload);
void timeMonitor(void *pvParameters);
void getTempSens(void *pvParameters);
void getRtcTemp(void *pvParameters);
void relayOnOff(void *pvParameters);
bool mqttConnecting(void);
void monitoringWiFi(void *pvParameters);
void monitoringMqtt(void *pvParameters);
void setSystemTime(void);
void getSystemTime(char *_buf);

#endif
