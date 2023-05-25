#include <Arduino_CRC32.h>
#include <ArduinoJson.h>
#include <ArduinoHttpClient.h>
#include <MKRGSM.h>
#include <Arduino_PMIC.h>
#include "arduino_secrets.h"

enum POWER_STATE {
  POWER_ON = 1,
  POWER_OFF,
};

const int retries = 5;
const char pinnumber[] = SECRET_PINNUMBER;
const char gprs_apn[] = SECRET_GPRS_APN;
const char gprs_login[] = SECRET_GPRS_LOGIN;
const char gprs_password[] = SECRET_GPRS_PASSWORD;
const char backend_host[] = BACKEND_HOST;
const int backend_port = BACKEND_PORT;
const char user_agent[] = USER_AGENT;
const char content_type[] = "application/json";
const char api_path[] = "/api/power/state";
const unsigned long send_interval = 15000L;

Arduino_CRC32 crc32;
const uint32_t device_crc_id = crc32.calc((uint8_t const *)DEVICE_ID, strlen(DEVICE_ID));

static int cycles = 1;
static bool gprs_connected = false;
static unsigned long time_since_last_update = millis();

GSMClient client;
GPRS gprs;
GSM gsmAccess;
GSMScanner scannerNetworks;
HttpClient httpClient = HttpClient(client, backend_host, backend_port);

void setup() {
  Serial.begin(9600);
  delay(8000);

  println_info("Starting setup");

  scannerNetworks.begin();

  if (!PMIC.begin()) {
    println_info("Failed to initialize PMIC!");
  }

  if (!PMIC.setInputCurrentLimit(2.0)) {
    println_info("Error in set input current limit");
  }

  if (!PMIC.setInputVoltageLimit(3.88)) {
    println_info("Error in set input voltage limit");
  }

  if (!PMIC.setMinimumSystemVoltage(3.5)) {
    println_info("Error in set minimum system volage");
  }

  if (!PMIC.setChargeVoltage(4.2)) {
    println_info("Error in set charge volage");
  }

  if (!PMIC.setChargeCurrent(1)) {
    println_info("Error in set charge current");
  }

  println_info("Finished setup");
}

void loop() {
  println_info("Starting_loop...");

  while (!connect_gprs());

  int signal_strength = scannerNetworks.getSignalStrength().toInt();
  if (signal_strength > 33) {
    signal_strength = 0;
  }

  float battery_level = analogRead(ADC_BATTERY) * 3.3f / 1023.0f / 1.2f * (1.2f + 0.33f);
  float battery_percentage = map(battery_level, 3.6, 4.2, 0, 100);
  int signal_strength_percentage = map(signal_strength, 0, 33, 0, 100);

  show_telemetry(signal_strength, signal_strength_percentage, battery_level, battery_percentage, cycles);

  check_power_data(battery_percentage, signal_strength_percentage);
  if (cycles >= 30) {
    cycles = 0;
    shutdown_gprs();
  }

  println_info("Ending_loop...");
  cycles += 1;
  delay(10000);
}

void check_usb_mode(const float battery_percentage, const int network_level) {
  int usb_mode = PMIC.USBmode();
  switch (usb_mode) {
    case ADAPTER_PORT_MODE:
    case BOOST_MODE:
    case USB_HOST_MODE:
      try_update_state(POWER_ON, battery_percentage, network_level);
      break;
  }
}

void check_power_data(const float battery_percentage, const int network_level) {
  int charge_state = PMIC.chargeStatus();
  switch (charge_state) {
    case NOT_CHARGING:
      if ((millis() - time_since_last_update) >= send_interval) {
        try_update_state(POWER_OFF, battery_percentage, network_level);
      }
      break;
    case PRE_CHARGING:
    case FAST_CHARGING:
    case CHARGE_TERMINATION_DONE:
      if ((millis() - time_since_last_update) >= send_interval) {
        try_update_state(POWER_ON, battery_percentage, network_level);
      }
      break;
    default:
      if ((millis() - time_since_last_update) >= send_interval) {
        check_usb_mode(battery_percentage, network_level);
      }
      break;
  }
}

void try_update_state(const POWER_STATE state, const float battery_percentage, const int network_level) {
  int attempts = retries;
  println_info("Starting try update");
  while (attempts > 0) {
    if (send_post(state, battery_percentage, network_level)) {
      time_since_last_update = millis();

      break;
    }

    attempts -= 1;
  }
}

bool connect_gprs() {
  if (gprs_connected) {
    return true;
  }
  println_info("Starting_connect_gprs");
  bool connected = false;
  while (!connected) {
    if ((gsmAccess.begin(pinnumber) == GSM_READY) && (gprs.attachGPRS(gprs_apn, gprs_login, gprs_password) == GPRS_READY)) {
      connected = true;
    } else {
      println_info("Steel_connect_gprs...");
      delay(1000);
    }
  }

  gprs_connected = true;

  println_info("Ending_connect_gprs");

  return true;
}

void shutdown_gprs() {
  println_info("shutdown_gprs");
  gsmAccess.shutdown();
  gprs_connected = false;
}

bool send_post(const int state, const float battery_percentage, const int network_level) {
  if (!httpClient.connect(backend_host, backend_port)) {
    println_info("Connection-failure");
    httpClient.stop();
    delay(1000);

    return false;
  }

  println_info("Preparing_POST");

  StaticJsonDocument<128> json;
  json["device_id"] = device_crc_id;
  json["power_state"] = state;
  json["battery_level"] = battery_percentage;
  json["network_level"] = network_level;

  String buffer;
  serializeJson(json, buffer);

  httpClient.beginRequest();
  httpClient.post(api_path);
  httpClient.sendHeader(HTTP_HEADER_CONTENT_TYPE, content_type);
  httpClient.sendHeader(HTTP_HEADER_USER_AGENT, user_agent);
  httpClient.sendHeader(HTTP_HEADER_CONTENT_LENGTH, buffer.length());
  httpClient.beginBody();
  httpClient.print(buffer);
  httpClient.endRequest();

  int status_code = httpClient.responseStatusCode();
  String response = httpClient.responseBody();

  print_info("ResponseCode:");
  println_info(status_code);
  print_info("Response:");
  println_info(response);

  return status_code == 201 || status_code == 200;
}

void show_telemetry(const int signal_strength, const int network_level, const float battery_level, const float percentage, const int cycles) {
  print_info("BatteryVoltage:");
  println_info(battery_level);

  print_info("BatteryCapacity:");
  println_info(percentage);

  print_info("ChargeStatus:");
  println_info(PMIC.chargeStatus());

  print_info("Cycles:");
  println_info(cycles);

  print_info("NetworkSignal:");
  println_info(signal_strength);

  print_info("NetworkSignal%:");
  println_info(network_level);
}

void println_info(const char info[]) {
  if (Serial) {
    Serial.println(F(info));
  }
}

void println_info(const String &info) {
  if (Serial) {
    Serial.println(F(info.c_str()));
  }
}

void println_info(const int info) {
  if (Serial) {
    Serial.println(info);
  }
}

void println_info(const float info) {
  if (Serial) {
    Serial.println(info);
  }
}

void println_info(const bool info) {
  if (Serial) {
    Serial.println(info);
  }
}

void print_info(const char info[]) {
  if (Serial) {
    Serial.print(F(info));
  }
}