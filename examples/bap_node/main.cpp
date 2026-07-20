#include <Arduino.h>
#include <Mesh.h>
#include <SPIFFS.h>
#include <helpers/IdentityStore.h>

#include "target.h"
#include "BapConfig.h"
#include "BapPacket.h"
#include "MyMesh.h"
#include "ArrivalDisplay.h"
#include "GatewayTask.h"
#include "BapSerialCLI.h"

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "0.1.0-bap"
#endif
#ifndef FIRMWARE_BUILD_DATE
  #define FIRMWARE_BUILD_DATE __DATE__
#endif

StdRNG fast_rng;
SimpleMeshTables tables;

static BapConfig g_config;
static MyMesh g_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables, g_config);
static ArrivalDisplay g_display(display);
static GatewayTask* g_gateway = nullptr;
static BapSerialCLI g_cli(g_config, g_mesh);

static void halt() { while (true) ; }

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

  // Splash
  if (display.begin()) {
    g_display.begin();
  }

  if (!radio_init()) {
    Serial.println("Radio init failed!");
    halt();
  }

  fast_rng.begin(radio_driver.getRngSeed());

  // Filesystem
  SPIFFS.begin(true);

  // Identity (mesh self_id) — load or generate
  IdentityStore store(SPIFFS, "/identity");
  if (!store.load("_main", g_mesh.self_id)) {
    g_mesh.self_id = radio_new_identity();
    int count = 0;
    while (count < 10 && (g_mesh.self_id.pub_key[0] == 0x00 ||
                          g_mesh.self_id.pub_key[0] == 0xFF)) {
      g_mesh.self_id = radio_new_identity();
      count++;
    }
    store.save("_main", g_mesh.self_id);
  }

  // Load BAP config (optional — defaults are fine on first boot)
  bool have_config = g_config.load();
  if (!have_config) {
    Serial.println("No /bap.cfg — using defaults. Use serial CLI to configure.");
  }

  // Wire display into mesh (for client RX path)
  g_mesh.setDisplay(&g_display);

  // Bring up mesh
  g_mesh.begin();  // mesh::Mesh::begin() takes no args (filesystem handled by app)

  Serial.println();
  Serial.print("BAP Node firmware ");
  Serial.print(FIRMWARE_VERSION);
  Serial.print(" (");
  Serial.print(FIRMWARE_BUILD_DATE);
  Serial.println(")");

  Serial.print("Node ID: ");
  {
    char hexbuf[PUB_KEY_SIZE * 2 + 1] = {0};
    mesh::Utils::toHex(hexbuf, g_mesh.self_id.pub_key, PUB_KEY_SIZE);
    Serial.println(hexbuf);
  }

  if (have_config) {
    Serial.print("Role: ");
    Serial.println(g_config.role == BapRole::GATEWAY ? "gateway" : "client");
  }

  // Role-specific setup
  if (g_config.role == BapRole::GATEWAY) {
    g_gateway = new GatewayTask(g_config, g_mesh, g_display);
    g_gateway->begin();
  } else {
    // Client shows splash until first packet
    if (g_config.client_stop > 0) {
      g_display.setStatus("Listening...");
    } else {
      g_display.setStatus("Not configured");
    }
  }

  g_cli.prompt();
  board.onBootComplete();
}

void loop() {
  g_cli.loop();
  g_mesh.loop();
  sensors.loop();
  g_display.loop();
  rtc_clock.tick();

  if (g_gateway) {
    g_gateway->loop();
  }
}
