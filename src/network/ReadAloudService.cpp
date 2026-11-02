#include "ReadAloudService.h"

#include <AppCapabilities.h>

ReadAloudService& ReadAloudService::getInstance() {
  static ReadAloudService instance;
  return instance;
}

#if CROSSINK_APP_CAP_READ_ALOUD && !defined(SIMULATOR)

#include <Logging.h>
#include <NimBLEDevice.h>

#include <atomic>

#include "CrossPointSettings.h"

namespace {

constexpr char SERVICE_UUID[] = "e8d61f0b-fed2-4705-95a9-e57756b33a14";
constexpr char TX_UUID[] = "48810e55-7a25-4a35-a6d2-6e07171779a6";
constexpr char RX_UUID[] = "3e8449f1-6b6c-48fe-8e3a-4ad9a135962c";
constexpr char DEFAULT_NAME[] = "CrossInk X4 Pro";
constexpr char PROTOCOL_VERSION[] = "1";

constexpr uint8_t CHUNK_FIRST = 0x01;
constexpr uint8_t CHUNK_LAST = 0x02;
constexpr uint16_t ATT_HEADER_BYTES = 3;
constexpr uint16_t DEFAULT_ATT_MTU = 23;
constexpr uint16_t REQUESTED_ATT_MTU = 517;
// One page plus its successor is a few KB; a queue this deep only fills when
// the phone stops reading notifications, and then old pages are worthless.
constexpr size_t MAX_OUTBOX_MESSAGES = 6;
constexpr size_t MAX_CHUNKS_PER_PUMP = 8;

NimBLEServer* server = nullptr;
NimBLECharacteristic* txCharacteristic = nullptr;

std::atomic<uint16_t> connHandle{BLE_HS_CONN_HANDLE_NONE};
std::atomic<uint16_t> attMtu{DEFAULT_ATT_MTU};
std::atomic<bool> subscribed{false};
std::atomic<bool> speaking{false};
// Set by the host task on subscribe; the main loop queues H and raises
// sessionStartForReader.
std::atomic<bool> helloPending{false};
std::atomic<bool> sessionStartForReader{false};
// Main loop only: H is queued for the current subscription, so the reader may
// send. Keeps B and P from going out ahead of H.
bool sessionOpen = false;
// Set by the host task on disconnect so the main loop drops the outbox.
std::atomic<bool> outboxResetPending{false};

portMUX_TYPE commandMux = portMUX_INITIALIZER_UNLOCKED;
constexpr size_t COMMAND_RING_SIZE = 8;
ReadAloudService::Command commandRing[COMMAND_RING_SIZE];
size_t commandHead = 0;
size_t commandCount = 0;

void pushCommand(const ReadAloudService::Command command) {
  portENTER_CRITICAL(&commandMux);
  if (commandCount == COMMAND_RING_SIZE) {
    // Drop the oldest: a phone that queued this many turns has moved on.
    commandHead = (commandHead + 1) % COMMAND_RING_SIZE;
    commandCount--;
  }
  commandRing[(commandHead + commandCount) % COMMAND_RING_SIZE] = command;
  commandCount++;
  portEXIT_CRITICAL(&commandMux);
}

// Phone messages are a type byte and at most one short field, so a small fixed
// buffer holds any valid one. Host task only.
char rxMessage[32];
size_t rxLength = 0;
bool rxOverflow = false;

void handleRxMessage(const char* message, const size_t length) {
  if (length == 0) return;
  switch (message[0]) {
    case 'N':
      pushCommand(ReadAloudService::Command::NextPage);
      break;
    case 'V':
      pushCommand(ReadAloudService::Command::PreviousPage);
      break;
    case 'R':
      pushCommand(ReadAloudService::Command::Resend);
      break;
    case 'S':
      speaking.store(length >= 3 && message[1] == ReadAloudService::FIELD_SEPARATOR && message[2] == '1');
      break;
    default:
      break;
  }
}

void resetSession() {
  subscribed.store(false);
  speaking.store(false);
  helloPending.store(false);
  outboxResetPending.store(true);
  rxLength = 0;
  rxOverflow = false;
}

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& connInfo) override {
    connHandle.store(connInfo.getConnHandle());
    attMtu.store(connInfo.getMTU());
    LOG_INF("RAL", "Phone connected (mtu %u)", connInfo.getMTU());
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, const int reason) override {
    connHandle.store(BLE_HS_CONN_HANDLE_NONE);
    attMtu.store(DEFAULT_ATT_MTU);
    resetSession();
    LOG_INF("RAL", "Phone disconnected (reason %d)", reason);
  }

  void onMTUChange(const uint16_t mtu, NimBLEConnInfo&) override {
    attMtu.store(mtu);
    LOG_DBG("RAL", "MTU %u", mtu);
  }
};

class TxCallbacks final : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& connInfo, const uint16_t subValue) override {
    const bool notifying = (subValue & 0x0001) != 0;
    if (notifying) {
      connHandle.store(connInfo.getConnHandle());
      outboxResetPending.store(true);
      helloPending.store(true);
      subscribed.store(true);
    } else {
      resetSession();
    }
    LOG_INF("RAL", "TX notifications %s", notifying ? "on" : "off");
  }
};

class RxCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const NimBLEAttValue& value = characteristic->getValue();
    if (value.size() < 1) return;
    const uint8_t flags = value[0];
    if (flags & CHUNK_FIRST) {
      rxLength = 0;
      rxOverflow = false;
    }
    const size_t payloadLength = value.size() - 1;
    if (rxLength + payloadLength > sizeof(rxMessage)) {
      rxOverflow = true;
    } else {
      memcpy(rxMessage + rxLength, value.data() + 1, payloadLength);
      rxLength += payloadLength;
    }
    if (flags & CHUNK_LAST) {
      if (!rxOverflow) handleRxMessage(rxMessage, rxLength);
      rxLength = 0;
      rxOverflow = false;
    }
  }
};

ServerCallbacks serverCallbacks;
TxCallbacks txCallbacks;
RxCallbacks rxCallbacks;

const char* advertisedName() { return SETTINGS.deviceName[0] != '\0' ? SETTINGS.deviceName : DEFAULT_NAME; }

}  // namespace

bool ReadAloudService::begin() {
  if (active) return true;

  const char* name = advertisedName();
  if (!NimBLEDevice::init(name)) {
    LOG_ERR("RAL", "NimBLE init failed");
    return false;
  }
  NimBLEDevice::setMTU(REQUESTED_ATT_MTU);

  server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks, /*deleteCallbacks=*/false);
  server->advertiseOnDisconnect(true);

  NimBLEService* service = server->createService(SERVICE_UUID);
  txCharacteristic = service->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  txCharacteristic->setCallbacks(&txCallbacks);
  NimBLECharacteristic* rx = service->createCharacteristic(RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(&rxCallbacks);
  // advertising->start() starts the GATT server, and with it the service.

  // The 128-bit service UUID fills most of the 31-byte advertisement, so the
  // name goes in the scan response.
  NimBLEAdvertisementData advertisement;
  advertisement.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advertisement.addServiceUUID(SERVICE_UUID);
  NimBLEAdvertisementData scanResponse;
  scanResponse.setName(name);
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setAdvertisementData(advertisement);
  advertising->setScanResponseData(scanResponse);
  if (!advertising->start()) {
    LOG_ERR("RAL", "Advertising failed to start");
    NimBLEDevice::deinit(true);
    server = nullptr;
    txCharacteristic = nullptr;
    return false;
  }

  active = true;
  LOG_INF("RAL", "Advertising as \"%s\"", name);
  return true;
}

void ReadAloudService::end() {
  if (!active) return;
  active = false;
  NimBLEDevice::deinit(true);
  server = nullptr;
  txCharacteristic = nullptr;
  connHandle.store(BLE_HS_CONN_HANDLE_NONE);
  resetSession();
  outbox.clear();
  sentBytes = 0;
  outboxResetPending.store(false);
  sessionOpen = false;
  portENTER_CRITICAL(&commandMux);
  commandCount = 0;
  portEXIT_CRITICAL(&commandMux);
  LOG_INF("RAL", "Stopped");
}

bool ReadAloudService::isActive() const { return active; }

bool ReadAloudService::isConnected() const { return active && sessionOpen && subscribed.load(); }

bool ReadAloudService::isSpeaking() const { return isConnected() && speaking.load(); }

bool ReadAloudService::consumeSessionStart() { return sessionStartForReader.exchange(false); }

bool ReadAloudService::takeCommand(Command& out) {
  portENTER_CRITICAL(&commandMux);
  const bool available = commandCount > 0;
  if (available) {
    out = commandRing[commandHead];
    commandHead = (commandHead + 1) % COMMAND_RING_SIZE;
    commandCount--;
  }
  portEXIT_CRITICAL(&commandMux);
  return available;
}

void ReadAloudService::send(std::string message) {
  if (!isConnected() || message.empty()) return;
  if (message[0] == MSG_PAGE) {
    // Index 0 may be mid-send; anything after it has not started.
    for (size_t i = (sentBytes > 0 ? 1 : 0); i < outbox.size(); ++i) {
      if (!outbox[i].empty() && outbox[i][0] == MSG_PAGE) {
        outbox[i] = std::move(message);
        return;
      }
    }
  }
  if (outbox.size() >= MAX_OUTBOX_MESSAGES) {
    LOG_ERR("RAL", "Outbox full; dropping message '%c'", message[0]);
    return;
  }
  outbox.push_back(std::move(message));
}

void ReadAloudService::pump() {
  if (!active) return;

  if (outboxResetPending.exchange(false)) {
    outbox.clear();
    sentBytes = 0;
    sessionOpen = false;
  }
  if (helloPending.exchange(false)) {
    std::string hello;
    hello += MSG_HELLO;
    hello += FIELD_SEPARATOR;
    hello += PROTOCOL_VERSION;
    hello += FIELD_SEPARATOR;
    hello += advertisedName();
    outbox.push_front(std::move(hello));
    sentBytes = 0;
    sessionOpen = true;
    sessionStartForReader.store(true);
  }

  const uint16_t handle = connHandle.load();
  if (!subscribed.load() || handle == BLE_HS_CONN_HANDLE_NONE || !txCharacteristic) return;

  const uint16_t mtu = attMtu.load();
  const size_t maxPayload = (mtu > ATT_HEADER_BYTES + 1 ? mtu - ATT_HEADER_BYTES - 1 : 19);
  // Sized for the largest MTU NimBLE negotiates; static so the main-loop stack
  // stays small.
  static uint8_t chunk[REQUESTED_ATT_MTU];

  for (size_t sentChunks = 0; sentChunks < MAX_CHUNKS_PER_PUMP && !outbox.empty(); ++sentChunks) {
    const std::string& message = outbox.front();
    const size_t remaining = message.size() - sentBytes;
    const size_t payload = std::min({remaining, maxPayload, sizeof(chunk) - 1});
    uint8_t flags = 0;
    if (sentBytes == 0) flags |= CHUNK_FIRST;
    if (payload == remaining) flags |= CHUNK_LAST;
    chunk[0] = flags;
    memcpy(chunk + 1, message.data() + sentBytes, payload);
    if (!txCharacteristic->notify(chunk, payload + 1, handle)) {
      // Usually out of mbufs while earlier notifications drain; retry on the
      // next loop pass.
      return;
    }
    sentBytes += payload;
    if (sentBytes == message.size()) {
      outbox.pop_front();
      sentBytes = 0;
    }
  }
}

#elif CROSSINK_APP_CAP_READ_ALOUD  // SIMULATOR

// The simulator has no radio. It stands in a phone that connects as soon as
// the reader starts the service, prints every message it receives, and, with
// CROSSINK_SIMULATOR_READ_ALOUD_TURNS=n, answers the first n pages with N as
// if it had finished speaking them, CROSSINK_SIMULATOR_READ_ALOUD_SPEAK_MS
// (default 1500) after each.

#include <Arduino.h>

#include <cstdio>
#include <cstdlib>

namespace {

constexpr unsigned long DEFAULT_SIM_SPEAK_MS = 1500;

bool simConnected = false;
bool simHelloPending = false;
bool simSessionStart = false;
int simTurnsLeft = 0;
unsigned long simSpeakMs = DEFAULT_SIM_SPEAK_MS;
bool simTurnPending = false;
unsigned long simTurnAtMs = 0;

void printMessage(const std::string& message) {
  std::string printable;
  printable.reserve(message.size());
  for (const char c : message) {
    if (c == ReadAloudService::FIELD_SEPARATOR)
      printable += " | ";
    else if (c == '\n')
      printable += " // ";
    else
      printable += c;
  }
  std::printf("[RAL] phone <- %s\n", printable.c_str());
  std::fflush(stdout);
}

}  // namespace

bool ReadAloudService::begin() {
  if (active) return true;
  active = true;
  simConnected = true;
  simHelloPending = true;
  const char* turns = std::getenv("CROSSINK_SIMULATOR_READ_ALOUD_TURNS");
  simTurnsLeft = turns ? std::max(0, std::atoi(turns)) : 0;
  const char* speakMs = std::getenv("CROSSINK_SIMULATOR_READ_ALOUD_SPEAK_MS");
  simSpeakMs = speakMs ? static_cast<unsigned long>(std::max(0, std::atoi(speakMs))) : DEFAULT_SIM_SPEAK_MS;
  std::printf("[RAL] simulated phone connected\n");
  return true;
}

void ReadAloudService::end() {
  active = false;
  simConnected = false;
  simTurnPending = false;
}

bool ReadAloudService::isActive() const { return active; }
bool ReadAloudService::isConnected() const { return active && simConnected && !simHelloPending; }
// Only a phone that turns pages counts as speaking, so a listen-only run keeps
// the normal idle loop timing the smoke test's input script is paced by.
bool ReadAloudService::isSpeaking() const { return isConnected() && (simTurnsLeft > 0 || simTurnPending); }

bool ReadAloudService::consumeSessionStart() {
  const bool started = simSessionStart;
  simSessionStart = false;
  return started;
}

bool ReadAloudService::takeCommand(Command& out) {
  if (!simTurnPending || millis() < simTurnAtMs) return false;
  simTurnPending = false;
  out = Command::NextPage;
  std::printf("[RAL] phone -> N\n");
  return true;
}

void ReadAloudService::send(std::string message) {
  if (!isConnected() || message.empty()) return;
  printMessage(message);
  if (message[0] == MSG_PAGE && simTurnsLeft > 0) {
    simTurnsLeft--;
    simTurnPending = true;
    simTurnAtMs = millis() + simSpeakMs;
  }
}

void ReadAloudService::pump() {
  if (!active || !simHelloPending) return;
  simHelloPending = false;
  std::string hello;
  hello += MSG_HELLO;
  hello += FIELD_SEPARATOR;
  hello += "1";
  hello += FIELD_SEPARATOR;
  hello += "CrossInk Simulator";
  printMessage(hello);
  simSessionStart = true;
}

#else  // !CROSSINK_APP_CAP_READ_ALOUD

bool ReadAloudService::begin() { return false; }
void ReadAloudService::end() {}
bool ReadAloudService::isActive() const { return false; }
bool ReadAloudService::isConnected() const { return false; }
bool ReadAloudService::isSpeaking() const { return false; }
bool ReadAloudService::consumeSessionStart() { return false; }
bool ReadAloudService::takeCommand(Command&) { return false; }
void ReadAloudService::send(std::string) {}
void ReadAloudService::pump() {}

#endif
