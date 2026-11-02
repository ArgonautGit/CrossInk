#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

// Read Aloud BLE peripheral: serves the open page's text to a phone app that
// speaks it, and takes page turns back. Protocol: docs/read-aloud-ble.md.
//
// The NimBLE implementation compiles only with CROSSINK_APP_CAP_READ_ALOUD;
// other boards link stubs, so callers need no #ifdefs. This header stays
// NimBLE-free.
//
// Threading: NimBLE callbacks run on the NimBLE host task and only touch
// atomics and the spinlock-guarded command ring. Everything else (send(),
// pump(), the outbox) belongs to the main loop.
class ReadAloudService {
 public:
  enum class Command : uint8_t { None, NextPage, PreviousPage, Resend };

  // Message types and the field separator from the protocol.
  static constexpr char MSG_HELLO = 'H';
  static constexpr char MSG_BOOK = 'B';
  static constexpr char MSG_PAGE = 'P';
  static constexpr char MSG_NO_BOOK = 'X';
  static constexpr char MSG_PROBLEM = 'E';
  static constexpr char FIELD_SEPARATOR = '\x1f';

  static ReadAloudService& getInstance();

  // Starts the BLE stack and advertising. Idempotent; false when the board
  // has no Read Aloud capability or NimBLE failed to start.
  bool begin();
  // Disconnects and releases the BLE stack.
  void end();

  bool isActive() const;
  // A phone is connected and subscribed to TX.
  bool isConnected() const;
  // The phone reports that it is speaking; the reader keeps the device awake.
  bool isSpeaking() const;

  // True once after each new subscription, after H has been queued. The reader
  // answers with B and P.
  bool consumeSessionStart();
  bool takeCommand(Command& out);

  // Queues one message for the phone. A queued, not yet started P is replaced
  // by a newer one, since only the page on screen matters.
  void send(std::string message);

  // Main loop: sends queued chunks while the stack accepts them.
  void pump();

 private:
  ReadAloudService() = default;

  std::deque<std::string> outbox;
  size_t sentBytes = 0;  // of outbox.front()
  bool active = false;
};

#define READ_ALOUD ReadAloudService::getInstance()
