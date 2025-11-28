#pragma once

// clang-format off
/* === MODULE MANIFEST V2 ===
module_description: BitsButtonXR module for button management
constructor_args: []
template_args: []
required_hardware: []
depends: []
=== END MANIFEST === */
// clang-format on

#include "app_framework.hpp"
#include "gpio.hpp"
#include <cstdint>

#define BITS_BTN_MAX_SINGLES 32
#define BITS_BTN_MAX_COMBINED 16
#define BITS_BTN_INVALID_INDEX 0xFF

class BitsButtonXR : public LibXR::Application {
public:
  using ButtonStateBits = uint32_t;
  using ButtonMaskType = uint32_t;

  enum class ButtonEvent : uint8_t {
    PRESSED = 0,         ///< Button initially pressed
    LONG_PRESS = 1,      ///< Long press detected (after threshold)
    LONG_PRESS_HOLD = 2, ///< Periodic long press hold
    RELEASED = 3,        ///< Button released
    DOUBLE_CLICK = 4,    ///< Double click detected
    CLICK_HOLD = 5       ///< Click followed by long press
  };

  enum class ButtonState : uint8_t {
    IDLE = 0,         ///< No button activity
    DEBOUNCE = 1,     ///< Debounce window after GPIO edge
    PRESSED = 2,      ///< Button confirmed pressed
    LONG_PRESS = 3,   ///< Long press state (active hold)
    RELEASE_WAIT = 4, ///< Waiting for potential second click
    RELEASED = 5      ///< Button confirmed released
  };

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
    const uint8_t
        *button_indices; ///< Array of button indices that form this combo
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
    ButtonStateBits state_bits; ///< Current state of all buttons
    uint16_t long_press_count;  ///< Count of long press periods triggered
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
               std::initializer_list<CombinedButtonConfig> combined_buttons) {
    InitializeSingleButtons(hw, single_buttons);
    InitializeCombinedButtons(combined_buttons);
  }

  /** @brief Get the event handle for button events */
  LibXR::Event GetEventHandle() { return button_events_; }

  /** @brief Monitor function called by application framework */
  void OnMonitor() override {}

private:
  /** Internal state for a single button */
  struct SingleButton {
    const char *key_alias;         ///< GPIO name identifier
    ButtonState current_state;     ///< Current state machine state
    ButtonState last_state;        ///< Previous state machine state
    ButtonStateBits state_bits;    ///< Bit mask for this button
    ButtonConstraints constraints; ///< Timing constraints
    LibXR::Timer::TimerHandle
        state_timer;          ///< Timer handle for state timing management
    LibXR::GPIO *gpio_handle; ///< GPIO handle for reading button state
    bool active_level;  ///< GPIO level for button press (false=low, true=high)
    uint8_t button_bit; ///< Bit position in mask
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

      single_buttons_.at(i) = {.key_alias = cfg.key_alias,
                               .current_state = ButtonState::IDLE,
                               .last_state = ButtonState::IDLE,
                               .state_bits = 0,
                               .constraints = cfg.constraints,
                               .state_timer = LibXR::Timer::CreateTask<void *>(
                                   BitsButtonXR::ButtonStateTimerCallback,
                                   static_cast<void *>(&single_buttons_.at(i)),
                                   cfg.constraints.short_press_time_ms),
                               .gpio_handle = gpio_handle,
                               .active_level = cfg.active_level,
                               .button_bit = static_cast<uint8_t>(i)};

      // TODO: Register GPIO interrupt/callback to read button state
      // gpio_handle->RegisterCallback(...);

      /* GPIO Initializing */
      if (cfg.active_level) {
        gpio_handle->SetConfig(
            {LibXR::GPIO::Direction::FALL_INTERRUPT, LibXR::GPIO::Pull::UP});
      } else {
        gpio_handle->SetConfig({LibXR::GPIO::Direction::RISING_INTERRUPT,
                                LibXR::GPIO::Pull::DOWN});
      }

      auto gpio_callback_function = [](bool, SingleButton *single_button) {
        // GPIO callback function to handle button state changes
        // This function should read the GPIO level and update the button state
        // machine accordingly
      };

      auto gpio_callback = LibXR::GPIO::Callback::Create(
          gpio_callback_function(gpio_callback_function,
                                 SingleButton * single_button),
          &single_buttons_);
      gpio_handle->RegisterCallback(gpio_callback);

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
     * @brief Timer callback function for button state management
     * @param button Pointer to the button instance
     */
    static void ButtonStateTimerCallback(
        void *arg); // TODO: This function should be a lambda

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
        uint8_t curr_keys = 0;
        curr_keys = combined_buttons_.at(curr_idx).key_count;
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
        auto ec =
            CalculateCombinedMask(cfg.button_indices, cfg.key_count, mask);
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
        combined_btn.combined_btn.current_state = ButtonState::IDLE;
        // Combined buttons don't need GPIO handle and active_level
        combined_btn.combined_btn.gpio_handle = nullptr;

        ++combined_index;
      }

      SortCombinedButtonsDescending(combined_index);

      return LibXR::ErrorCode::OK;
    }
  };

  void BitsButtonXR::ButtonStateTimerCallback(void *arg) {
    // Timer callback for button state management
    // This function is used to debounce and count for long presses
    // The Timer will be stopped when the button is not pressed
    SingleButton *button = static_cast<SingleButton *>(arg);
    if (button && button->gpio_handle) {
      // Read the current GPIO state
      bool current_level = button->gpio_handle->Read();
      UNUSED(current_level);
      bool is_pressed = (current_level == button->active_level);
      UNUSED(is_pressed);

      // Update state machine based on current input and constraints
      // This is a placeholder - actual state machine logic would be implemented
      // here
      // TODO: Implement full button state machine logic
    }
  }
