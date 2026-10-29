#pragma once

#include <atomic>

#include "AudioTools.h"
#include "AudioTools/Concurrency.h"
#include "ModemStream.h"

#if defined(RP2040) && !defined(USE_FREERTOS)
#error "ModemStreamRTOS on RP2040: select Tools > Operating System > FreeRTOS SMP and #include <FreeRTOS.h> before AudioTools.h"
#endif

namespace tinymodem {

/**
 * @brief BasicModemStream which receives in a separate task (FreeRTOS on ESP32
 * and RP2040, a std::thread on the desktop): the task reads the audio input
 * and decodes it, so the decoding does not block your loop() and no audio is
 * lost while loop() is busy. available(), read() and peek() only access the
 * received data and can be called from any task.
 *
 * Transmitting (write(), print(), flush()) happens in the calling task like
 * in the BasicModemStream. The status output (setStatusOutput()) is written by
 * the receive task.
 * @tparam Base the BasicModemStream (e.g. ModemStream or MFSKStream)
 * @copyright GPLv3
 */
template <class Base>
class BasicModemStreamRTOS : public Base {
 public:
  using Base::Base;
  using Config = typename Base::ConfigType;

  ~BasicModemStreamRTOS() { end(); }

  /// Defines the receive task parameters: call before begin(). The stack
  /// size is in bytes on ESP32 and in words on other FreeRTOS platforms.
  void setTaskConfig(int stack_size, int priority = 1, int core = -1) {
    task_stack_size = stack_size;
    task_priority = priority;
    task_core = core;
  }

  /// Starts the modem and the receive task: the input is only read while
  /// less than rx_buffer_size decoded bytes are waiting to be read
  bool begin(Config cfg, int rx_buffer_size = 256) {
    end();
    if (!Base::begin(cfg, rx_buffer_size)) return false;
    if (this->p_audio_in == nullptr) return true;
    // the decoder runs in the receive task: protect the receive queue
    locked_queue.p_queue = &this->rx_queue;
    locked_queue.p_mutex = &mutex;
    this->decoder.setOutput(locked_queue);
    if (!task_created) {
      if (!task.create("tinymodem-rx", task_stack_size, task_priority,
                       task_core)) {
        LOGE("BasicModemStreamRTOS: could not create the receive task");
        return false;
      }
      task_created = true;
    }
    is_idle = false;
    is_running = true;
    if (!task.begin([this]() { receive(); })) {
      LOGE("BasicModemStreamRTOS: could not start the receive task");
      is_running = false;
      return false;
    }
    is_task_active = true;
    return true;
  }

  /// Stops the receive task and the modem
  void end() {
    if (is_task_active) {
      // let the task complete the current step before we stop it
      is_running = false;
      while (!is_idle) delay(1);
      task.end();
      is_task_active = false;
    }
    Base::end();
  }

  int available() override {
    audio_tools::LockGuard guard(mutex);
    return this->rx_queue.available();
  }

  int read() override {
    audio_tools::LockGuard guard(mutex);
    return this->rx_queue.read();
  }

  int peek() override {
    audio_tools::LockGuard guard(mutex);
    return this->rx_queue.peek();
  }

  /// Not needed: the receive task processes the input
  size_t update() { return 0; }

  /// true while the receive task is running
  bool isRunning() { return is_task_active; }

 protected:
#ifdef ESP32
  static constexpr int DEFAULT_STACK_SIZE = 8 * 1024;  // bytes
#else
  static constexpr int DEFAULT_STACK_SIZE = 2 * 1024;  // words
#endif
  using Queue = typename Base::ReceiveQueue;

  /// Print which writes the decoded data into the queue with a lock
  struct LockedQueue : public Print {
    Queue* p_queue = nullptr;
    audio_tools::MutexBase* p_mutex = nullptr;
    size_t write(uint8_t ch) override {
      audio_tools::LockGuard guard(p_mutex);
      return p_queue->write(ch);
    }
    size_t write(const uint8_t* data, size_t len) override {
      audio_tools::LockGuard guard(p_mutex);
      for (size_t j = 0; j < len; j++) p_queue->write(data[j]);
      return len;
    }
  };

  audio_tools::Task task;
  audio_tools::Mutex mutex;
  LockedQueue locked_queue;
  int task_stack_size = DEFAULT_STACK_SIZE;
  int task_priority = 1;
  int task_core = -1;
  bool task_created = false;
  bool is_task_active = false;
  std::atomic<bool> is_running{false};
  std::atomic<bool> is_idle{false};

  /// One iteration of the receive task
  void receive() {
    if (!is_running) {
      is_idle = true;
      delay(10);
      return;
    }
    if (readInputLocked() == 0) delay(1);
  }

  /// Reads the available audio input and feeds it to the decoder
  size_t readInputLocked() {
    {
      // stop reading the input while the received data is not consumed
      audio_tools::LockGuard guard(mutex);
      if (this->rx_queue.available() >= this->rx_limit) return 0;
    }
    return this->readInput();
  }
};

/**
 * @brief ModemStream (FSK) which receives in a separate task. See
 * BasicModemStreamRTOS.
 * @copyright GPLv3
 */
class ModemStreamRTOS : public BasicModemStreamRTOS<ModemStream> {
 public:
  using BasicModemStreamRTOS::BasicModemStreamRTOS;
};

/**
 * @brief MFSKStream (multi-tone FSK) which receives in a separate task. See
 * BasicModemStreamRTOS.
 * @copyright GPLv3
 */
class MFSKStreamRTOS : public BasicModemStreamRTOS<MFSKStream> {
 public:
  using BasicModemStreamRTOS::BasicModemStreamRTOS;
};

/**
 * @brief AX25Stream (APRS) which receives in a separate task. See
 * BasicModemStreamRTOS.
 * @copyright GPLv3
 */
class AX25StreamRTOS : public BasicModemStreamRTOS<AX25Stream> {
 public:
  using BasicModemStreamRTOS::BasicModemStreamRTOS;
};

/**
 * @brief PSKStream which receives in a separate task. See
 * BasicModemStreamRTOS.
 * @copyright GPLv3
 */
class PSKStreamRTOS : public BasicModemStreamRTOS<PSKStream> {
 public:
  using BasicModemStreamRTOS::BasicModemStreamRTOS;
};

/**
 * @brief OFDMStream which receives in a separate task. See
 * BasicModemStreamRTOS.
 * @copyright GPLv3
 */
class OFDMStreamRTOS : public BasicModemStreamRTOS<OFDMStream> {
 public:
  using BasicModemStreamRTOS::BasicModemStreamRTOS;
};

/**
 * @brief GGWaveStream which receives in a separate task. See
 * BasicModemStreamRTOS.
 * @copyright GPLv3
 */
class GGWaveStreamRTOS : public BasicModemStreamRTOS<GGWaveStream> {
 public:
  using BasicModemStreamRTOS::BasicModemStreamRTOS;
};

}  // namespace tinymodem
