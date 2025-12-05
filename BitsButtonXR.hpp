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
#include <atomic>
#include <cstdint>

#define BITS_BTN_MAX_SINGLES 32
#define BITS_BTN_MAX_COMBINED 16
#define BITS_BTN_MAX_TOTAL (BITS_BTN_MAX_SINGLES + BITS_BTN_MAX_COMBINED)
#define BITS_BTN_INVALID_INDEX 0xFF

class BitsButtonXR : public LibXR::Application {
public:
  // Event ID bit field layout and constants
  constexpr static uint8_t EVENT_ID_TYPE_BITS = 8;
  constexpr static uint8_t EVENT_ID_INDEX_BITS = 8;
  constexpr static uint8_t EVENT_ID_TYPE_SHIFT = 0;
  constexpr static uint8_t EVENT_ID_INDEX_SHIFT = 8;
  constexpr static uint32_t EVENT_ID_TYPE_MASK = 0xFFu;
  constexpr static uint32_t EVENT_ID_INDEX_MASK = 0xFFu;

  enum class ButtonEvent : uint8_t {
    PRESSED = 0,          ///< Button initially pressed
    LONG_PRESS_START = 1, ///< Long press detected (after threshold)
    LONG_PRESS_HOLD = 2,  ///< Periodic long press hold
    RELEASED = 3,         ///< Button released
    CLICK_FINISH = 4,     ///< Click followed by long press
  };

  using ButtonStateBits = uint32_t;
  using ButtonMaskType = uint32_t;
  using ButtonIndexType = uint8_t;

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
    ButtonConstraints constraints; ///< Timing constraints for this combination
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
   * @brief Construct a new BitsButtonXR object with unified data-oriented
   * design
   * @param hw Hardware container for GPIO access
   * @param app Application manager reference
   * @param single_configs List of individual button configurations
   * @param combined_configs List of combined button configurations
   */
  BitsButtonXR(LibXR::HardwareContainer &hw, LibXR::ApplicationManager &app,
               std::initializer_list<SingleButtonConfig> single_configs,
               std::initializer_list<CombinedButtonConfig> combined_configs)
      : LibXR::Application(), result_queue_(16),
        state_timer_(LibXR::Timer::CreateTask(StateTimerOnTick, this,
                                              TIMER_INTERVAL_MS)) {
    UNUSED(app);
    LibXR::Timer::Add(state_timer_);
    LibXR::Timer::Stop(state_timer_);

    // 1. Capacity Check
    if (single_configs.size() + combined_configs.size() > BITS_BTN_MAX_TOTAL) {
      return; // Consider adding an ASSERT here
    }

    total_count_ = 0;
    physical_count_ = 0;

    // 2. Initialize Physical Buttons
    for (const auto &cfg : single_configs) {
      if (InitPhysicalButton(hw, cfg) != LibXR::ErrorCode::OK) {
        // Handle partial failure or just continue/return
        // In embedded, usually we trap error or ignore the broken button
      }
    }

    // 3. Initialize Combined Buttons
    for (const auto &cfg : combined_configs) {
      InitCombinedButton(cfg);
    }

    // 4. Sort Priorities
    SortCombinedButtons();
  }

  /**
   * @brief Get the event handle for button events
   * @return Event handle for button notifications
   */
  LibXR::Event GetEventHandle() { return button_events_; }

  /**
   * @brief Generate event ID for Event::Register
   * Format: [Reserved 16bit] [Index 8bit] [Type 8bit]
   * @param index Button index (0 ~ N-1). Combined button indices follow single
   * button indices
   * @param type Event type from ButtonEvent enum
   * @return Generated event ID
   */
  static uint32_t MakeEventId(ButtonIndexType index, ButtonEvent type) {
    ASSERT(index <= BITS_BTN_MAX_SINGLES + BITS_BTN_MAX_COMBINED);
    ASSERT(static_cast<uint32_t>(type) <= EVENT_ID_TYPE_MASK);

    // Force masking even in release to prevent bit corruption
    return ((static_cast<uint32_t>(index) & EVENT_ID_INDEX_MASK)
            << EVENT_ID_INDEX_SHIFT) |
           ((static_cast<uint32_t>(type) & EVENT_ID_TYPE_MASK)
            << EVENT_ID_TYPE_SHIFT);
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
  static_assert(BITS_BTN_MAX_SINGLES <= sizeof(ButtonMaskType) * 8,
                "ButtonMaskType unable to hold all physical buttons");

  constexpr static uint16_t TIMER_INTERVAL_MS = 10;
  constexpr static uint32_t IDLE_SLEEP_THRESHOLD = 10;
  constexpr static uint8_t DEBOUNCE_THRESHOLD =
      2; // Need 2 consecutive readings for stability

  enum class InternalState : uint8_t {
    IDLE = 0,
    PRESSED = 1,
    LONG_PRESS = 2,
    RELEASE = 3,
    RELEASE_WINDOW = 4,
    FINISH = 5
  };

  /**
   * @brief Unified button structure using tagged union for data-oriented design
   */
  struct GenericButton {
    // === Common Header (Base) ===
    const char *key_alias;       ///< Button name identifier
    InternalState current_state; ///< Current state machine state
    ButtonStateBits state_bits;  ///< Click history (0b10, 0b1010...)
    uint32_t state_entry_tick;   ///< Global tick value entering current state
    uint16_t long_press_cnt;     ///< Long press event triggered count
    uint8_t debounce_counter; ///< Counter for stable readings (used by physical
                              ///< buttons)

    // === Type Tag ===
    enum Type : uint8_t { PHYSICAL, COMBINED } type;
    uint8_t logic_index; ///< Global index (0 ~ Total-1)

    // === Differentiated Configuration (Union) ===
    union Config {
      // Physical button specific configuration
      struct {
        LibXR::GPIO *gpio;    ///< Hardware handle
        bool active_level;    ///< Active level for button press
        bool last_raw_state;  ///< Last raw GPIO reading
        bool debounced_state; ///< Current debounced stable state
      } phys;

      // Combined button specific configuration
      struct {
        ButtonMaskType mask;  ///< Bit mask of buttons in this combo
        bool suppress_single; ///< Suppress individual button events
        uint8_t key_count;    ///< Number of buttons in this combo
      } comb;
    } cfg;

    // Common constraints (shared by both types)
    ButtonConstraints constraints;
  };

  LibXR::Event button_events_; ///< Event system for button notifications
  LibXR::LockFreeQueue<ButtonEventResult>
      result_queue_; ///< Queue for event results

  LibXR::Timer::TimerHandle
      state_timer_; ///< Global timer handle for state timing management

  std::atomic<bool> is_polling_active_ =
      false; ///< Flag for active polling mode

  std::atomic<bool> interrupts_need_disable_ =
      false; ///< Flag to disable interrupts in first timer callback

  uint32_t idle_hysteresis_ =
      0; ///< Counter to delay sleep after button release

  uint8_t total_count_ =
      0; ///< Total count of all buttons (physical + combined)
  uint8_t physical_count_ = 0; ///< Count of physical buttons (for optimization)

  ButtonMaskType current_mask_ = 0; ///< Current button state mask
  std::array<GenericButton, BITS_BTN_MAX_TOTAL>
      all_buttons_{}; ///< Unified array of all button states

  void RecordHistory(GenericButton &btn, bool pressed) {
    btn.state_bits = (btn.state_bits << 1) | (pressed ? 1 : 0);
  }

  /**
   * @brief Reset button state to initial values
   * @param btn Reference to button to reset
   */
  void ResetState(GenericButton &btn) {
    btn.current_state = InternalState::IDLE;
    btn.state_bits = 0;
    btn.state_entry_tick = 0;
    btn.long_press_cnt = 0;
    btn.debounce_counter = 0;
  }

  // === Private Helper Functions ===

  /**
   * @brief Initialize a single physical button
   * @param hw Hardware container for GPIO access
   * @param cfg Single button configuration
   * @return Error code indicating success or failure
   */
  LibXR::ErrorCode InitPhysicalButton(LibXR::HardwareContainer &hw,
                                      const SingleButtonConfig &cfg) {
    auto &btn =
        all_buttons_[total_count_]; // Direct access since we are inside class

    // Initialize common parts
    btn.type = GenericButton::PHYSICAL;
    btn.key_alias = cfg.key_alias;
    btn.logic_index = total_count_;
    btn.constraints = cfg.constraints;
    ResetState(btn);

    // Hardware Lookup
    LibXR::GPIO *gpio_handle = hw.template Find<LibXR::GPIO>(cfg.key_alias);
    if (!gpio_handle) {
      return LibXR::ErrorCode::NOT_FOUND;
    }

    // Union setup
    btn.cfg.phys.gpio = gpio_handle;
    btn.cfg.phys.active_level = cfg.active_level;
    btn.cfg.phys.last_raw_state = false;
    btn.cfg.phys.debounced_state = false;

    // Hardware Config
    auto dir = LibXR::GPIO::Direction::FALL_RISING_INTERRUPT;
    auto pull =
        cfg.active_level ? LibXR::GPIO::Pull::DOWN : LibXR::GPIO::Pull::UP;
    gpio_handle->SetConfig({dir, pull});

    // Callback Registration
    auto gpio_callback = LibXR::GPIO::Callback::Create(
        [](bool, BitsButtonXR *instance) { instance->WakeUpFromIsr(); }, this);
    gpio_handle->RegisterCallback(gpio_callback);
    gpio_handle->EnableInterrupt();

    physical_count_++;
    total_count_++;
    return LibXR::ErrorCode::OK;
  }

  /**
   * @brief Initialize a combined button
   * @param cfg Combined button configuration
   */
  void InitCombinedButton(const CombinedButtonConfig &cfg) {
    // Validate indices first
    for (uint8_t j = 0; j < cfg.key_count; ++j) {
      if (cfg.button_indices[j] >= physical_count_) {
        return;
      }
    }

    auto &btn = all_buttons_[total_count_];

    // Common Init
    btn.type = GenericButton::COMBINED;
    btn.key_alias = cfg.combined_alias;
    btn.logic_index = total_count_;
    btn.constraints = cfg.constraints;
    ResetState(btn);

    // Union setup - inline mask calculation for simplicity
    ButtonMaskType mask = 0;
    for (uint8_t j = 0; j < cfg.key_count; ++j) {
      mask |= static_cast<ButtonMaskType>(1UL) << cfg.button_indices[j];
    }

    btn.cfg.comb.mask = mask;
    btn.cfg.comb.suppress_single = cfg.suppress_single_keys;
    btn.cfg.comb.key_count = cfg.key_count;

    total_count_++;
  }

  /**
   * @brief Sort combined buttons by key_count descending (Insertion Sort)
   */
  void SortCombinedButtons() {
    if (total_count_ <= physical_count_ + 1) {
      return;
    }

    // Insertion Sort: Range [physical_count_ + 1, total_count_)
    for (size_t i = physical_count_ + 1; i < total_count_; ++i) {
      GenericButton temp = all_buttons_[i];
      uint8_t temp_count = temp.cfg.comb.key_count;

      size_t j = i;
      // Compare with previous combined button
      while (j > physical_count_ &&
             all_buttons_[j - 1].cfg.comb.key_count < temp_count) {
        all_buttons_[j] = all_buttons_[j - 1];
        j--;
      }
      all_buttons_[j] = temp;
    }
  }

  /**
   * @brief Emit button event to queue and notify listeners
   * @param btn Reference to the button that triggered the event
   * @param type Type of event that occurred
   */
  void EmitEvent(const GenericButton &btn, ButtonEvent type) {
    ButtonEventResult res = {btn.key_alias, type, btn.state_bits,
                             btn.long_press_cnt, LibXR::Thread::GetTime()};

    result_queue_.Push(res);

    button_events_.Active(MakeEventId(btn.logic_index, type));
  }

  void WakeUpFromIsr() {
    if (is_polling_active_) {
      return;
    }

    LibXR::Timer::Start(state_timer_);
    is_polling_active_ = true;
    idle_hysteresis_ = 0;
    interrupts_need_disable_ = true; // Mark that interrupts need disabling
  }

  void EnterSleepMode() {
    LibXR::Timer::Stop(state_timer_);
    is_polling_active_ = false;

    // Enable interrupts for all physical buttons
    for (size_t i = 0; i < physical_count_; ++i) {
      auto &btn = all_buttons_[i];
      if (btn.type == GenericButton::PHYSICAL && btn.cfg.phys.gpio) {
        btn.cfg.phys.gpio->EnableInterrupt();
      }
    }
  }

  /**
   * @brief Update the state machine for a generic button (unified for physical
   * and combined)
   * @param btn Reference to the button state structure
   * @param is_active Current button state (true if active/pressed)
   * @param current_tick Current system tick time
   */
  void UpdateGenericState(GenericButton &btn, bool is_active,
                          uint32_t current_tick) {
    uint32_t elapsed_ms = current_tick - btn.state_entry_tick;

    switch (btn.current_state) {
    case InternalState::IDLE:
      if (is_active) {
        btn.current_state = InternalState::PRESSED;
        btn.state_entry_tick = current_tick;
        RecordHistory(btn, true);
        EmitEvent(btn, ButtonEvent::PRESSED);
      }
      break;

    case InternalState::PRESSED:
      if (!is_active) {
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
      if (!is_active) {
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
      if (is_active) {
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
  }

  /**
   * @brief Update debounced state for a physical button using integration
   * method
   * @param btn Reference to the button structure
   * @param raw_state Current raw GPIO reading
   */
  void UpdateButtonDebounce(GenericButton &btn, bool raw_state) {
    // Only applicable to physical buttons
    if (btn.type != GenericButton::PHYSICAL) {
      return;
    }

    if (raw_state != btn.cfg.phys.last_raw_state) {
      // State changed, reset counter
      btn.debounce_counter = 1;
      btn.cfg.phys.last_raw_state = raw_state;
    } else if (btn.debounce_counter < DEBOUNCE_THRESHOLD) {
      // Same state, increment counter
      btn.debounce_counter++;
    }

    // Update debounced state only when we have enough stable readings
    if (btn.debounce_counter >= DEBOUNCE_THRESHOLD) {
      btn.cfg.phys.debounced_state = btn.cfg.phys.last_raw_state;
    }
  }

  /**
   * @brief Timer callback function for button state management using unified
   * data-oriented design
   * @param instance Pointer to the BitsButtonXR instance
   */
  static void StateTimerOnTick(BitsButtonXR *instance) {
    uint32_t now = LibXR::Thread::GetTime();

    // Step 0: Disable interrupts if needed (moved from ISR for faster ISR
    // response)
    if (instance->interrupts_need_disable_) {
      for (size_t i = 0; i < instance->physical_count_; ++i) {
        auto &btn = instance->all_buttons_[i];
        if (btn.type == GenericButton::PHYSICAL && btn.cfg.phys.gpio) {
          btn.cfg.phys.gpio->DisableInterrupt();
        }
      }
      instance->interrupts_need_disable_ = false;
    }

    // Step 1: Update debounced state for physical buttons + build current mask
    instance->current_mask_ = 0;
    for (size_t i = 0; i < instance->physical_count_; ++i) {
      auto &btn = instance->all_buttons_[i];
      bool raw_state = btn.cfg.phys.gpio->Read() == btn.cfg.phys.active_level;
      instance->UpdateButtonDebounce(btn, raw_state);

      if (btn.cfg.phys.debounced_state) {
        instance->current_mask_ |= (1UL << btn.logic_index);
      }
    }

    uint32_t active_count = 0;
    ButtonMaskType suppression_mask = 0;
    ButtonMaskType consumed_mask = 0; // 记录已经被大组合键"吃掉"的物理按键

    // Lambda helper: core business logic for button state updates and counting
    // Compiler will inline this for zero overhead
    auto process_button = [&](GenericButton &btn, bool input_active) {
      instance->UpdateGenericState(btn, input_active, now);
      if (btn.current_state != InternalState::IDLE) {
        active_count++;
      }
    };

    // Step 2: Process combined buttons first with greedy matching (longest
    // match wins)
    for (size_t i = instance->physical_count_; i < instance->total_count_;
         ++i) {
      auto &btn = instance->all_buttons_[i];

      // 检查 mask 是否匹配，并且其中的按键没有被更大的组合键吃掉
      bool match =
          (instance->current_mask_ & btn.cfg.comb.mask) == btn.cfg.comb.mask;
      bool consumed =
          (consumed_mask & btn.cfg.comb.mask) != 0; // 如果有重叠，就被视为冲突

      // 只有未被消费的才能触发（强组合压制弱组合）
      bool effective_active = match && !consumed;

      // Use common logic for state update and counting
      process_button(btn, effective_active);

      // 如果这个组合键匹配了，就"吃掉"这些物理键，防止更小的组合键触发
      // 注意：只要 match 就吃掉，不管是否激活状态（贪婪匹配策略）
      if (match) {
        consumed_mask |= btn.cfg.comb.mask;

        // Combined button specific suppression logic
        if (btn.cfg.comb.suppress_single) {
          suppression_mask |= btn.cfg.comb.mask;
        }
      }
    }

    // Step 3: Process physical buttons with suppression applied
    for (size_t i = 0; i < instance->physical_count_; ++i) {
      auto &btn = instance->all_buttons_[i];

      // Physical button specific suppression logic
      if (suppression_mask & (1UL << btn.logic_index)) {
        // Force reset suppressed buttons to IDLE
        if (btn.current_state != InternalState::IDLE) {
          btn.current_state = InternalState::IDLE;
          btn.state_bits = 0;
          btn.long_press_cnt = 0;
        }
        continue;
      }

      bool pressed = btn.cfg.phys.debounced_state;

      // Use common logic for state update and counting
      process_button(btn, pressed);
    }

    // Step 4: Sleep check
    if (instance->current_mask_ == 0 && active_count == 0) {
      instance->idle_hysteresis_++;
      if (instance->idle_hysteresis_ > IDLE_SLEEP_THRESHOLD) {
        instance->EnterSleepMode();
      }
    } else {
      instance->idle_hysteresis_ = 0;
    }
  }
};