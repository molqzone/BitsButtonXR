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

  /** Timing constraints for button behavior detection */
  struct ButtonConstraints {
    uint16_t short_press_time_ms;         ///< Time threshold for short press
    uint16_t long_press_start_time_ms;    ///< Time when long press starts
    uint16_t long_press_period_triger_ms; ///< Period for long press hold events
    uint16_t time_window_time_ms;         ///< Window for double click detection
  };

  /** Configuration for combined button patterns */
  struct CombinedButtonConfig {
    const char *combined_alias; ///< Name identifier for the combination
    bool suppress_single_keys; ///< Whether to suppress individual button events
    uint8_t key_count;         ///< Number of buttons in this combination
    const uint8_t *button_indices; ///< Array of button indices
  };

  /** Configuration for individual single buttons */
  struct SingleButtonConfig {
    const char *key_alias; ///< GPIO name identifier for the button
    bool active_level; ///< GPIO level that indicates button press (false=low,
                       ///< true=high)
    ButtonConstraints constraints; ///< Timing constraints for this button
  };

  /** Result structure for button events */
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

  /** @brief Get the event handle for button events */
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
    // Pop both gets data and removes it from queue (consumes the event)
    return result_queue_.Pop(out_result) == LibXR::ErrorCode::OK;
  }

  /**
   * @brief Peek event result without removing from queue
   * @param out_result Reference to store the event result
   * @return True if event was successfully retrieved, false otherwise
   * @note Event remains in queue for other callbacks to see. Typically,
   * event-driven models use "consume" pattern, so GetEventResult is more common
   */
  bool PeekEventResult(ButtonEventResult &out_result) {
    return result_queue_.Peek(out_result) == LibXR::ErrorCode::OK;
  }

  /** @brief Monitor function called by application framework */
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

  /** Internal state for a single button */
  struct SingleButton {
    /* Static configuration */
    const char *key_alias;         ///< Alias name for the button
    LibXR::GPIO *gpio_handle;      ///< Hardware handle
    bool active_level;             ///< Active level for button press
    ButtonConstraints constraints; ///< Timing parameters
    uint8_t logic_index;           ///< Logical index

    /* Dynamic state */
    InternalState current_state; ///< Current state machine state
    ButtonStateBits state_bits;  ///< Click history (0b10, 0b1010...)

    /* Timing and counting */
    uint32_t state_entry_tick; ///< Global tick value entering current state
    uint16_t long_press_cnt;   ///< Long press event triggered count
  };

  /** Internal state for a combined button pattern */
  struct CombinedButton {
    SingleButton combined_btn;    ///< Embedded button state machine
    ButtonMaskType combined_mask; ///< Bit mask of buttons in this combo
    uint8_t key_count;            ///< Number of buttons in this combo
    bool suppress_single_keys;    ///< Suppress individual button events
    bool is_active;               ///< Whether this combo is currently active
  };

  /** Event system for button notifications */
  LibXR::Event button_events_;
  LibXR::LockFreeQueue<ButtonEventResult> result_queue_;

  /** Global timer handle for state timing management */
  LibXR::Timer::TimerHandle state_timer_;

  /** Flag to indicate if the system is in active polling mode */
  volatile bool is_polling_active_ = false;

  /** Timestamp when the button mask last changed (for global debounce) */
  uint32_t mask_update_tick_ = 0;

  /** Counter to delay sleep (Hysteresis) after buttons are released */
  uint32_t idle_counter_ = 0;

  /** Current and previous button states for change detection */
  [[maybe_unused]] ButtonMaskType current_mask_ =
      0; ///< Current button state mask
  [[maybe_unused]] ButtonMaskType last_mask_ =
      0; ///< Previous button state mask

  /** Storage for button configurations and states */
  [[maybe_unused]] std::array<SingleButton, BITS_BTN_MAX_SINGLES>
      single_buttons_{}; ///< Array of single button states
  [[maybe_unused]] std::array<CombinedButton, BITS_BTN_MAX_COMBINED>
      combined_buttons_{}; ///< Array of combined button states

  /** Index array for sorted access to combined buttons */
  std::array<uint16_t, BITS_BTN_MAX_COMBINED> sorted_indices_{};

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

                               /* Dynamic state */
                               .current_state = InternalState::IDLE,
                               .state_bits = 0,

                               /* Timing and counting */
                               .state_entry_tick = 0,
                               .long_press_cnt = 0};

      // TODO: Register GPIO interrupt/callback to read button state
      // gpio_handle->RegisterCallback(...);

      /* GPIO Initializing */
      if (cfg.active_level) {
        gpio_handle->SetConfig({LibXR::GPIO::Direction::FALL_RISING_INTERRUPT,
                                LibXR::GPIO::Pull::UP});
      } else {
        gpio_handle->SetConfig({LibXR::GPIO::Direction::FALL_RISING_INTERRUPT,
                                LibXR::GPIO::Pull::DOWN});
      }

      auto button_on_click_cb = [](bool, BitsButtonXR &instance) {
        instance.WakeUpFromIsr();
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

      // Set bit for this button in the mask
      mask |= static_cast<ButtonMaskType>(1UL) << button_index;
    }

    return LibXR::ErrorCode::OK;
  }

  /**
   * @brief Sort combined buttons by key count in descending order
   * @param count Number of valid combined buttons to sort
   */
  void SortCombinedButtonsDescending(size_t count) {
    // Early exit for trivial cases
    if (count <= 1) {
      return;
    }

    // Validate count against array bounds
    if (count > combined_buttons_.size()) {
      count = combined_buttons_.size();
    }

    // Initialize index array (0,1,2,...)
    for (size_t i = 0; i < count; ++i) {
      sorted_indices_.at(i) = static_cast<uint16_t>(i);
    }

    // Insertion sort on index array in descending order by key_count
    // This ensures that button combinations with more keys are checked first,
    // preventing false positives where smaller combinations match larger ones
    for (size_t i = 1; i < count; ++i) {
      uint16_t curr_idx = sorted_indices_.at(i);
      uint8_t curr_keys = combined_buttons_.at(curr_idx).key_count;
      int16_t pos = static_cast<int16_t>(i - 1);

      // Find insertion position: higher key count has higher priority
      // Shift indices of buttons with lower key count to the right
      while (pos >= 0 &&
             combined_buttons_.at(sorted_indices_.at(pos)).key_count <
                 curr_keys) {
        sorted_indices_.at(pos + 1) = sorted_indices_.at(pos);
        pos--;
      }

      // Insert current button index at correct position
      sorted_indices_.at(pos + 1) = curr_idx;
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

    // Validate all configs before processing to fail fast
    for (const auto &cfg : configs) {
      if (!cfg.combined_alias || cfg.key_count == 0 || !cfg.button_indices) {
        return LibXR::ErrorCode::ARG_ERR;
      }
    }

    size_t combined_index = 0;

    // First pass: Calculate combined masks and collect metadata
    for (const auto &cfg : configs) {
      ButtonMaskType mask = 0;

      // Calculate combined mask using the helper function
      auto ec = CalculateCombinedMask(cfg.button_indices, cfg.key_count, mask);
      if (ec != LibXR::ErrorCode::OK) {
        return LibXR::ErrorCode::ARG_ERR;
      }

      // Fill runtime structure using std::array::at() for bounds safety
      auto &combined_btn = combined_buttons_.at(combined_index);
      combined_btn.combined_mask = mask;
      combined_btn.key_count = cfg.key_count;
      combined_btn.suppress_single_keys = cfg.suppress_single_keys;
      combined_btn.is_active = true;

      // Initialize embedded SingleButton state machine
      combined_btn.combined_btn.key_alias = cfg.combined_alias;
      combined_btn.combined_btn.current_state = InternalState::IDLE;
      // Combined buttons don't need GPIO handle and active_level
      combined_btn.combined_btn.gpio_handle = nullptr;

      ++combined_index;
    }

    SortCombinedButtonsDescending(combined_index);

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

    // Push to queue
    // If queue is full (ErrorCode != OK), you can choose to ignore or log error
    // However, a depth of 16 will never be full for human button presses,
    // unless callbacks are stuck
    result_queue_.Push(res);

    // Send notification signal
    button_events_.Active(MakeEventId(btn.logic_index, type));
  }

  void WakeUpFromIsr() {
    // Avoid redundant wake-ups
    if (is_polling_active_) {
      return;
    }

    LibXR::Timer::Start(state_timer_);
    is_polling_active_ = true;
    idle_counter_ = 0;

    for (auto &btn : single_buttons_) {
      if (btn.gpio_handle) {
        btn.gpio_handle->DisableInterrupt();
      }
    }
  }

  void CheckAndEnterSleep() {
    // Check whether all the buttons are idle for a certain period
    if (current_mask_ != 0) {
      idle_counter_ = 0;
      return;
    }

    // All the states are IDLE
    for (auto &btn : single_buttons_) {
      if (btn.current_state != InternalState::IDLE) {
        idle_counter_ = 0;
        return;
      }
    }

    // Hysteresis counter to avoid rapid sleep/wake cycles
    idle_counter_++;
    if (idle_counter_ > IDLE_SLEEP_THRESHOLD) {
      EnterSleepMode();
    }
  }

  void EnterSleepMode() {
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
    // TODO: Implement state machine logic
  }

  /**
   * @brief Timer callback function for button state management
   * @param instance Pointer to the BitsButtonXR instance
   */
  static void StateTimerOnTick(BitsButtonXR *instance) {
    if (!instance) {
      return;
    }

    uint32_t current_tick = LibXR::Thread::GetTime();

    /* Snapshot current button mask */
    ButtonMaskType new_mask = 0;
    for (size_t i = 0; i < BITS_BTN_MAX_SINGLES; ++i) {
      auto &btn = instance->single_buttons_.at(i);
      if (btn.gpio_handle && btn.gpio_handle->Read() == btn.active_level) {
        new_mask |= static_cast<ButtonMaskType>(1UL) << i;
      }
    }

    /* Global debounce */
    if (new_mask != instance->current_mask_) {
      instance->last_mask_ = new_mask;
      instance->mask_update_tick_ = current_tick;
    }

    // Check whether debounce time has passed
    if ((current_tick - instance->mask_update_tick_) < DEBOUNCE_TIME_MS) {
      return; // Still within debounce period, skip processing
    }

    // Update current mask after debounce
    instance->current_mask_ = new_mask;

    /* Dispatching */
    ButtonMaskType suppression_mask = 0;

    // Combined Buttons Processing
    for (size_t i = 0; i < instance->combined_buttons_.size(); ++i) {
      // Ensuring processing in sorted order
      uint16_t idx = instance->sorted_indices_[i];
      if (idx >= instance->combined_buttons_.size()) {
        continue;
      }

      auto &combined = instance->combined_buttons_[idx];
      if (combined.key_count == 0) {
        continue;
      }

      // Check if combination matches
      bool match = (instance->current_mask_ & combined.combined_mask) ==
                   combined.combined_mask;

      instance->UpdateSingleButtonState(combined.combined_btn, match,
                                        current_tick);

      // If matched or still active, consider suppressing single keys
      if (match || combined.combined_btn.current_state != InternalState::IDLE) {
        if (combined.suppress_single_keys) {
          suppression_mask |= combined.combined_mask;
        }
      }
    }

    // Single Buttons Processing
    for (auto &btn : instance->single_buttons_) {
      if (!btn.gpio_handle) {
        continue;
      }

      // Skip buttons suppressed by combined buttons
      if (suppression_mask & (1UL << btn.logic_index)) {
        continue;
      }

      // Check if button is pressed
      bool is_pressed = (instance->current_mask_ & (1UL << btn.logic_index));

      instance->UpdateSingleButtonState(btn, is_pressed, current_tick);
    }

    /* Check for sleep condition */
    instance->CheckAndEnterSleep();
  }
};