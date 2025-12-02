#pragma once

// clang-format off
/* === MODULE MANIFEST V2 ===
module_description: BitsButtonXR module for button management
constructor_args:
  single_buttons:
    - key_alias: "btn1"
      active_level: false
      constraints:
        short_press_time_ms: 50
        long_press_start_time_ms: 1000
        long_press_period_triger_ms: 500
        time_window_time_ms: 300
    - key_alias: "btn2"
      active_level: false
      constraints:
        short_press_time_ms: 50
        long_press_start_time_ms: 1000
        long_press_period_triger_ms: 500
        time_window_time_ms: 300
  combined_buttons: []
template_args: []
required_hardware: []
depends: []
=== END MANIFEST === */
// clang-format on

#include "app_framework.hpp"
#include "gpio.hpp"
#include "libxr_def.hpp"
#include "timer.hpp"
#include <cstdint>

#define BITS_BTN_MAX_SINGLES 32
#define BITS_BTN_MAX_COMBINED 16
#define BITS_BTN_INVALID_INDEX 0xFF

class BitsButtonXR : public LibXR::Application {
public:
  enum class ButtonEvent : uint8_t {
    PRESSED = 0,          ///< Button initially pressed
    LONG_PRESS_START = 1, ///< Long press detected (after threshold)
    LONG_PRESS_HOLD = 2,  ///< Periodic long press hold
    RELEASED = 3,         ///< Button released
    CLICK_FINISH = 4,     ///< Click followed by long press
  };

  using ButtonStateBits = uint32_t;
  using ButtonMaskType = uint32_t;

  struct ButtonConstraints {
    uint16_t short_press_time_ms;         ///< Time threshold for short press
    uint16_t long_press_start_time_ms;    ///< Time when long press starts
    uint16_t long_press_period_triger_ms; ///< Period for long press hold events
    uint16_t time_window_time_ms;         ///< Window for double click detection
  };

  struct CombinedButtonConfig {
    const char *combined_alias; ///< Name identifier for the combination
    bool suppress_single_keys; ///< Whether to suppress individual button events
    uint8_t key_count;         ///< Number of buttons in this combination
    const uint8_t *button_indices; ///< Array of button indices
  };

  struct SingleButtonConfig {
    const char *key_alias;         ///< GPIO name identifier for the button
    bool active_level;             ///< GPIO level that indicates button press
    ButtonConstraints constraints; ///< Timing constraints for this button
  };

  struct ButtonEventResult {
    const char *key_alias;      ///< Button name that triggered event
    ButtonEvent event_type;     ///< Type of event that occurred
    ButtonStateBits state_bits; ///< Current state bits of all buttons
    uint16_t long_press_count;  ///< Count of long press periods triggered
    uint32_t system_tick;       ///< System tick when event was generated
  };

  /**
   * @brief Construct a new BitsButtonXR object
   * @param hw Hardware container for GPIO access
   * @param app Application manager reference
   * @param single_buttons List of individual button configurations
   * @param combined_buttons List of combined button configurations
   */
  BitsButtonXR(LibXR::HardwareContainer &hw, LibXR::ApplicationManager &app,
               std::initializer_list<SingleButtonConfig> single_buttons,
               std::initializer_list<CombinedButtonConfig> combined_buttons)
      : LibXR::Application(), result_queue_(16),
        state_timer_(LibXR::Timer::CreateTask(StateTimerOnTick, this,
                                              TIMER_INTERVAL_MS)) {
    UNUSED(app);

    /* Ensure state_timer_ is sleeping */
    LibXR::Timer::Stop(state_timer_);

    /* Initialize Buttons Configuration */
    InitializeSingleButtons(hw, single_buttons);
    InitializeCombinedButtons(combined_buttons);
  }

  /**
   * @brief Get the event handle for button events
   * @return Event handle for button notifications
   */
  LibXR::Event GetEventHandle() { return button_events_; }

  /**
   * @brief Generate event ID for Event::Register
   * Format: [Reserved 8bit] [Index 8bit] [Type 8bit]
   * @param index Button index (0 ~ N-1). Combined button indices follow single
   * button indices
   * @param type Event type from ButtonEvent enum
   * @return Generated event ID
   */
  static constexpr uint32_t MakeEventId(uint8_t index, ButtonEvent type) {
    return (static_cast<uint32_t>(index) << 8) | static_cast<uint32_t>(type);
  }

  /**
   * @brief Get event result and remove from queue (consume)
   * @param out_result Reference to store the event result
   * @return True if event was successfully retrieved and removed, false
   * otherwise
   */
  bool GetEventResult(ButtonEventResult &out_result) {
    return result_queue_.Pop(out_result) == LibXR::ErrorCode::OK;
  }

  /**
   * @brief Peek event result without removing from queue
   * @param out_result Reference to store the event result
   * @return True if event was successfully retrieved, false otherwise
   * @note Event remains in queue for other callbacks to see. Typically,
   * event-driven models use "consume" pattern, so GetEventResult is more
   * common
   */
  bool PeekEventResult(ButtonEventResult &out_result) {
    return result_queue_.Peek(out_result) == LibXR::ErrorCode::OK;
  }

  /**
   * @brief Monitor function called by application framework
   */
  void OnMonitor() override {}

private:
  constexpr static uint16_t TIMER_INTERVAL_MS = 10;
  constexpr static uint32_t IDLE_SLEEP_THRESHOLD = 10;
  constexpr static uint32_t DEBOUNCE_TIME_MS = 20;

  enum class InternalState : uint8_t {
    IDLE = 0,
    PRESSED = 1,
    LONG_PRESS = 2,
    RELEASE = 3,
    RELEASE_WINDOW = 4,
    FINISH = 5
  };

  struct SingleButton {
    const char *key_alias;         ///< Alias name for the button
    LibXR::GPIO *gpio_handle;      ///< Hardware handle
    bool active_level;             ///< Active level for button press
    ButtonConstraints constraints; ///< Timing parameters
    uint8_t logic_index;           ///< Logical index

    InternalState current_state; ///< Current state machine state
    ButtonStateBits state_bits;  ///< Click history (0b10, 0b1010...)

    uint32_t state_entry_tick; ///< Global tick value entering current state
    uint16_t long_press_cnt;   ///< Long press event triggered count
  };

  struct CombinedButton {
    SingleButton combined_btn;    ///< Embedded button state machine
    ButtonMaskType combined_mask; ///< Bit mask of buttons in this combo
    uint8_t key_count;            ///< Number of buttons in this combo
    bool suppress_single_keys;    ///< Suppress individual button events
    bool is_active;               ///< Whether this combo is currently active
  };

  LibXR::Event button_events_; ///< Event system for button notifications
  LibXR::LockFreeQueue<ButtonEventResult>
      result_queue_; ///< Queue for event results

  LibXR::Timer::TimerHandle
      state_timer_; ///< Global timer handle for state timing management

  volatile bool is_polling_active_ = false; ///< Flag for active polling mode

  uint32_t mask_update_tick_ = 0; ///< Timestamp when button mask last changed

  uint32_t idle_hysteresis_ =
      0; ///< Counter to delay sleep after button release

  uint32_t active_logic_count_ = 0; ///< Count of buttons in non-IDLE state

  uint32_t valid_single_count_ = 0;   ///< Count of valid single buttons
  uint32_t valid_combined_count_ = 0; ///< Count of valid combined buttons

  inline void RecordHistory(SingleButton &btn, bool pressed) {
    btn.state_bits = (btn.state_bits << 1) | (pressed ? 1 : 0);
  }

  [[maybe_unused]] ButtonMaskType current_mask_ =
      0; ///< Current button state mask
  [[maybe_unused]] ButtonMaskType last_mask_ =
      0; ///< Previous button state mask

  [[maybe_unused]] std::array<SingleButton, BITS_BTN_MAX_SINGLES>
      single_buttons_{}; ///< Array of single button states
  [[maybe_unused]] std::array<CombinedButton, BITS_BTN_MAX_COMBINED>
      combined_buttons_{}; ///< Array of combined button states

  /**
   * @brief Initialize single button configurations
   * @param hw Hardware container for GPIO access
   * @param configs List of button configurations
   * @return Error code indicating success or failure
   */
  LibXR::ErrorCode
  InitializeSingleButtons(LibXR::HardwareContainer &hw,
                          std::initializer_list<SingleButtonConfig> configs) {
    if (configs.size() > BITS_BTN_MAX_SINGLES) {
      return LibXR::ErrorCode::SIZE_ERR;
    }

    valid_single_count_ = 0;
    for (size_t i = 0; i < configs.size(); ++i) {
      const auto &cfg = *(configs.begin() + i);
      LibXR::GPIO *gpio_handle = hw.template Find<LibXR::GPIO>(cfg.key_alias);
      if (!gpio_handle) {
        return LibXR::ErrorCode::NOT_FOUND;
      }

      single_buttons_.at(i) = {/* Static configuration */
                               .key_alias = cfg.key_alias,
                               .gpio_handle = gpio_handle,
                               .active_level = cfg.active_level,
                               .constraints = cfg.constraints,
                               .logic_index = static_cast<uint8_t>(i),

                               .current_state = InternalState::IDLE,
                               .state_bits = 0,

                               .state_entry_tick = 0,
                               .long_press_cnt = 0};

      valid_single_count_++;

      if (cfg.active_level) {
        gpio_handle->SetConfig({LibXR::GPIO::Direction::FALL_RISING_INTERRUPT,
                                LibXR::GPIO::Pull::UP});
      } else {
        gpio_handle->SetConfig({LibXR::GPIO::Direction::FALL_RISING_INTERRUPT,
                                LibXR::GPIO::Pull::DOWN});
      }

      auto button_on_click_cb = [](bool, BitsButtonXR *instance) {
        instance->WakeUpFromIsr();
      };

      auto gpio_callback =
          LibXR::GPIO::Callback::Create(button_on_click_cb, this);
      gpio_handle->RegisterCallback(gpio_callback);

      gpio_handle->EnableInterrupt();
    }

    return LibXR::ErrorCode::OK;
  }

  /**
   * @brief Calculate combined button mask from button indices
   * @param button_indices Array of button indices
   * @param key_count Number of keys in the combination
   * @param mask Calculated bit mask for the combination
   * @return Error code indicating success or failure
   */
  LibXR::ErrorCode CalculateCombinedMask(const uint8_t *button_indices,
                                         uint8_t key_count,
                                         ButtonMaskType &mask) {
    if (!button_indices || key_count == 0) {
      return LibXR::ErrorCode::ARG_ERR;
    }

    mask = 0;

    for (uint8_t j = 0; j < key_count; ++j) {
      uint8_t button_index = button_indices[j];

      // Bounds checking
      if (button_index >= BITS_BTN_MAX_SINGLES) {
        return LibXR::ErrorCode::OUT_OF_RANGE;
      }

      mask |= static_cast<ButtonMaskType>(1UL) << button_index;
    }

    return LibXR::ErrorCode::OK;
  }

  /**
   * @brief Sort combined buttons by key count in descending order
   * @param count Number of valid combined buttons to sort
   */
  void SortCombinedButtonsDescending(size_t count) {
    if (count <= 1) {
      return;
    }

    if (count > combined_buttons_.size()) {
      count = combined_buttons_.size();
    }

    for (size_t i = 1; i < count; ++i) {
      CombinedButton curr_btn = combined_buttons_.at(i);
      uint8_t curr_keys = curr_btn.key_count;
      int16_t pos = static_cast<int16_t>(i - 1);

      while (pos >= 0 && combined_buttons_.at(pos).key_count < curr_keys) {
        combined_buttons_.at(pos + 1) = combined_buttons_.at(pos);
        pos--;
      }

      combined_buttons_.at(pos + 1) = curr_btn;
    }
  }

  /**
   * @brief Initialize combined button configurations
   * @param configs List of combined button configurations
   * @return Error code indicating success or failure
   */
  LibXR::ErrorCode InitializeCombinedButtons(
      std::initializer_list<CombinedButtonConfig> configs) {
    if (configs.size() > BITS_BTN_MAX_COMBINED) {
      return LibXR::ErrorCode::SIZE_ERR;
    }

    for (const auto &cfg : configs) {
      if (!cfg.combined_alias || cfg.key_count == 0 || !cfg.button_indices) {
        return LibXR::ErrorCode::ARG_ERR;
      }
    }

    valid_combined_count_ = 0;
    size_t insert_pos = 0;

    for (const auto &cfg : configs) {
      ButtonMaskType mask = 0;

      auto ec = CalculateCombinedMask(cfg.button_indices, cfg.key_count, mask);
      if (ec != LibXR::ErrorCode::OK) {
        return LibXR::ErrorCode::ARG_ERR;
      }

      auto &combined_btn = combined_buttons_.at(insert_pos);
      combined_btn.combined_mask = mask;
      combined_btn.key_count = cfg.key_count;
      combined_btn.suppress_single_keys = cfg.suppress_single_keys;
      combined_btn.is_active = true;

      combined_btn.combined_btn.key_alias = cfg.combined_alias;
      combined_btn.combined_btn.current_state = InternalState::IDLE;
      combined_btn.combined_btn.gpio_handle = nullptr;

      valid_combined_count_++;
    }

    SortCombinedButtonsDescending(valid_combined_count_);

    return LibXR::ErrorCode::OK;
  }

  /**
   * @brief Emit button event to queue and notify listeners
   * @param btn Reference to the button that triggered the event
   * @param type Type of event that occurred
   */
  void EmitEvent(const SingleButton &btn, ButtonEvent type) {
    ButtonEventResult res = {btn.key_alias, type, btn.state_bits,
                             btn.long_press_cnt, LibXR::Thread::GetTime()};

    result_queue_.Push(res);

    button_events_.Active(MakeEventId(btn.logic_index, type));
  }

  inline void WakeUpFromIsr() {
    if (is_polling_active_) {
      return;
    }

    LibXR::Timer::Start(state_timer_);
    is_polling_active_ = true;
    idle_hysteresis_ = 0;

    for (auto &btn : single_buttons_) {
      if (btn.gpio_handle) {
        btn.gpio_handle->DisableInterrupt();
      }
    }
  }

  inline void CheckAndEnterSleep() {
    if (current_mask_ != 0 || active_logic_count_ > 0) {
      idle_hysteresis_ = 0;
      return;
    }

    idle_hysteresis_++;
    if (idle_hysteresis_ > IDLE_SLEEP_THRESHOLD) {
      EnterSleepMode();
    }
  }

  inline void EnterSleepMode() {
    LibXR::Timer::Stop(state_timer_);
    is_polling_active_ = false;

    for (auto &btn : single_buttons_) {
      if (btn.gpio_handle) {
        btn.gpio_handle->EnableInterrupt();
      }
    }
  }

  /**
   * @brief Update the state machine for a single button
   * @param btn Reference to the button state structure
   * @param is_pressed Current button state (true if pressed)
   * @param current_tick Current system tick time
   */
  void UpdateSingleButtonState(SingleButton &btn, bool is_pressed,
                               uint32_t current_tick) {
    auto old_state = btn.current_state;

    uint32_t elapsed_ms = current_tick - btn.state_entry_tick;

    switch (btn.current_state) {
    case InternalState::IDLE:
      if (is_pressed) {
        btn.current_state = InternalState::PRESSED;
        btn.state_entry_tick = current_tick;
        RecordHistory(btn, true);
        EmitEvent(btn, ButtonEvent::PRESSED);
      }
      break;

    case InternalState::PRESSED:
      if (!is_pressed) {
        btn.current_state = InternalState::RELEASE;
        btn.state_entry_tick = current_tick;
      } else if (elapsed_ms > btn.constraints.long_press_start_time_ms) {
        btn.current_state = InternalState::LONG_PRESS;
        btn.state_entry_tick = current_tick;
        btn.long_press_cnt = 0;
        RecordHistory(btn, true);
        EmitEvent(btn, ButtonEvent::LONG_PRESS_START);
      }
      break;

    case InternalState::LONG_PRESS:
      if (!is_pressed) {
        btn.current_state = InternalState::RELEASE;
        btn.state_entry_tick = current_tick;
      } else if (elapsed_ms > btn.constraints.long_press_period_triger_ms) {
        btn.state_entry_tick = current_tick;
        btn.long_press_cnt++;
        EmitEvent(btn, ButtonEvent::LONG_PRESS_HOLD);
      }
      break;

    case InternalState::RELEASE:
      RecordHistory(btn, false);
      EmitEvent(btn, ButtonEvent::RELEASED);

      btn.current_state = InternalState::RELEASE_WINDOW;
      btn.state_entry_tick = current_tick;
      break;

    case InternalState::RELEASE_WINDOW:
      if (is_pressed) {
        btn.current_state = InternalState::IDLE;
      } else if (elapsed_ms > btn.constraints.time_window_time_ms) {
        btn.current_state = InternalState::FINISH;
      }
      break;

    case InternalState::FINISH:
      EmitEvent(btn, ButtonEvent::CLICK_FINISH);
      btn.state_bits = 0;
      btn.current_state = InternalState::IDLE;
      break;
    }

    bool was_active = (old_state != InternalState::IDLE);
    bool is_active = (btn.current_state != InternalState::IDLE);

    if (!was_active && is_active) {
      active_logic_count_++;
    } else if (was_active && !is_active) {
      active_logic_count_--;
    }
  }

  /**
   * @brief Timer callback function for button state management
   * @param instance Pointer to the BitsButtonXR instance
   */
  static void StateTimerOnTick(BitsButtonXR *instance) {
    uint32_t now = LibXR::Thread::GetTime();

    ButtonMaskType new_mask = 0;
    for (size_t i = 0; i < instance->valid_single_count_; ++i) {
      auto &btn = instance->single_buttons_[i];
      new_mask |= (btn.gpio_handle->Read() == btn.active_level)
                      ? (1UL << btn.logic_index)
                      : 0;
    }

    if (new_mask != instance->last_mask_) {
      instance->last_mask_ = new_mask;
      instance->mask_update_tick_ = now;
      return;
    }

    if (now - instance->mask_update_tick_ < DEBOUNCE_TIME_MS) {
      return;
    }

    instance->current_mask_ = new_mask;

    ButtonMaskType suppression = 0;
    for (size_t i = 0; i < instance->valid_combined_count_; ++i) {
      auto &combo = instance->combined_buttons_[i];

      bool match = (instance->current_mask_ & combo.combined_mask) ==
                   combo.combined_mask;

      instance->UpdateSingleButtonState(combo.combined_btn, match, now);

      if (match || combo.combined_btn.current_state != InternalState::IDLE) {
        if (combo.suppress_single_keys) {
          suppression |= combo.combined_mask;
        }
      }
    }

    for (size_t i = 0; i < instance->valid_single_count_; ++i) {
      auto &btn = instance->single_buttons_[i];
      if (suppression & (1UL << btn.logic_index)) {
        continue;
      }

      bool pressed = (instance->current_mask_ & (1UL << btn.logic_index));
      instance->UpdateSingleButtonState(btn, pressed, now);
    }

    if (instance->current_mask_ == 0 && instance->active_logic_count_ == 0) {
      instance->idle_hysteresis_++;
      if (instance->idle_hysteresis_ > IDLE_SLEEP_THRESHOLD) {
        instance->EnterSleepMode();
      }
    } else {
      instance->idle_hysteresis_ = 0;
    }
  }
};