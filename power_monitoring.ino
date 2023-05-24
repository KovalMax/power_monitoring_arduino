#include <Arduino_CRC32.h>
#include <ArduinoJson.h>
#include <ArduinoHttpClient.h>
#include <MKRGSM.h>
#include <Arduino_PMIC.h>
#include "arduino_secrets.h"

enum POWER_STATE {
  DEFAULT = 1,
  POWER_ON,
  POWER_OFF,
};

const char pinnumber[] = SECRET_PINNUMBER;
const char gprs_apn[] = SECRET_GPRS_APN;
const char gprs_login[] = SECRET_GPRS_LOGIN;
const char gprs_password[] = SECRET_GPRS_PASSWORD;
const char device_identity[] = DEVICE_ID;
const char backend_host[] = BACKEND_HOST;
const int  backend_port = BACKEND_PORT;


GSMClient client;
GPRS gprs;
GSM gsmAccess;
Arduino_CRC32 crc32;
HttpClient httpClient = HttpClient(client, backend_host, backend_port);

volatile POWER_STATE power_status = DEFAULT;
volatile int cycles = 0;
static bool gprs_connected = false;
static const uint32_t device_crc_id = crc32.calc((uint8_t const *)device_identity, strlen(device_identity));
const int retries = 10;

void setup() {
  Serial.begin(9600);
  delay(8000);

  println_info("Starting setup");

  if (!PMIC.begin()) {
    println_info("Failed to initialize PMIC!");
    while (1);
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
  cycles += 1;
  float battery_level = analogRead(ADC_BATTERY) * 3.3f / 1023.0f / 1.2f * (1.2f+0.33f);
  float percentage = map(battery_level, 3.6, 4.2, 0, 100);
  println_info("Starting_loop...");
  print_info("BatteryVoltage:");
  println_info(battery_level);
  print_info("BatteryCapacity:");
  println_info(percentage);
  print_info("PowerState:");
  println_info(power_status);
  print_info("ChargeStatus:");
  println_info(PMIC.chargeStatus());
  print_info("Cycles:");
  println_info(cycles);
  
  while (!connect_gprs());
  check_power_data(percentage);
  if (cycles % 20 == 0) {
    cycles = 0;
    shutdown_gprs();
  }

  println_info("Ending_loop...");
  delay(10000);
}

void check_usb_mode(float battery_percentage) {
  int usb_mode = PMIC.USBmode();
  switch (usb_mode) {
    case ADAPTER_PORT_MODE:
    case BOOST_MODE:
    case USB_HOST_MODE:
      println_info("Power-ON");
      try_update_state(POWER_ON,  battery_percentage);
      break;
  }
}

void check_power_data(float battery_percentage) {
  int charge_state = PMIC.chargeStatus();
  switch (charge_state) {
    case NOT_CHARGING:
      if (power_status != POWER_OFF) {
        println_info("Power-OFF");
        try_update_state(POWER_OFF, battery_percentage);
      }
      break;
    case PRE_CHARGING:
    case FAST_CHARGING:
    case CHARGE_TERMINATION_DONE:
      if (power_status != POWER_ON) {
        println_info("Power-ON");
        try_update_state(POWER_ON, battery_percentage);
      }
      break;
    default:
      if (power_status != POWER_ON) {
        check_usb_mode(battery_percentage);
      }
      break;
  }
}

void shutdown_gprs() {
  println_info("shutdown_gprs");
  gsmAccess.shutdown();
  gprs_connected = false;
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

void try_update_state(const POWER_STATE state, const float battery_percentage) {
  int attempts = retries;
  println_info("Starting try update");
  while (attempts > 0) {
    print_info("Attempt:");
    println_info(attempts);
    if (send_post(state, battery_percentage)) {
      power_status = state;

      break;
    }

    attempts -= 1;
  }
}

bool send_post(const int state, const float battery_percentage) {
  if (!httpClient.connect(backend_host, backend_port)) {
    println_info("Connection-failure");
    delay(1000);
    httpClient.stop();

    return false;
  }
  
  println_info("Preparing_POST");
  
  StaticJsonDocument<128> json;
  json["device_id"] = device_crc_id;
  json["power_state"] = state;
  json["battery_level"] = battery_percentage;

  String buffer;
  serializeJson(json, buffer);
  
  httpClient.beginRequest();
  httpClient.post("/api/power/state");
  httpClient.sendHeader(HTTP_HEADER_CONTENT_TYPE, "application/json");
  httpClient.sendHeader(HTTP_HEADER_USER_AGENT, "PowerMonitoring (MKR-GSM)");
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