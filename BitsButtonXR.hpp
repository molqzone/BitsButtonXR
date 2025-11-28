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

#include "libxr.hpp"
#include <cstdint>
#include <cstring>

#define BITS_BTN_MAX_SINGLES 32
#define BITS_BTN_MAX_COMBINED 16
#define BITS_BTN_INVALID_INDEX 0xFF

class BitsButtonXR : public LibXR::Application {
public:
  using TimeStampMS = uint32_t;
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

  struct ButtonConstraints {
    uint16_t short_press_time_ms;
    uint16_t long_press_start_time_ms;
    uint16_t long_press_period_triger_ms;
    uint16_t time_window_time_ms;
  };

  struct CombinedButtonConfig {
    const char *combined_alias;
    bool suppress_single_keys;
    uint8_t key_count;
    const uint8_t *button_indices;
  };

  struct SingleButtonConfig {
    const char *key_alias;
    uint8_t active_level;
    ButtonConstraints constraints;
  };

  struct ButtonEventResult {
    const char *key_alias;
    ButtonEvent event_type;
    ButtonStateBits state_bits;
    uint16_t long_press_period_trigger_cnt;
  };

  BitsButtonXR(LibXR::HardwareContainer &hw, LibXR::ApplicationManager &app,
               std::initializer_list<SingleButtonConfig> single_buttons,
               std::initializer_list<CombinedButtonConfig> combined_buttons)
      : LibXR::Application(hw, app) {
    InitializeSingleButtons(hw, single_buttons);
    InitializeCombinedButtons(combined_buttons);
  }

  LibXR::Event GetEventHandle() { return button_events_; }

  void OnMonitor() override {}

private:
  struct SingleButton {
    const char *key_alias;
    ButtonState current_state;
    ButtonState last_state;
    ButtonStateBits state_bits;
    ButtonConstraints constraints;
    TimeStampMS state_entry_time;
    LibXR::GPIO *gpio_handle;
    uint8_t active_level;
    uint8_t button_bit;
  };

  struct CombinedButton {
    SingleButton combined_btn;
    ButtonMaskType combined_mask;
    uint8_t key_count;
    bool suppress_single_keys;
    bool is_active;
  };

  LibXR::Event button_events_;

  ButtonMaskType current_mask_ = 0;
  ButtonMaskType last_mask_ = 0;

  [[maybe_unused]] std::array<SingleButton, BITS_BTN_MAX_SINGLES>
      single_buttons_{};
  [[maybe_unused]] std::array<CombinedButton, BITS_BTN_MAX_COMBINED>
      combined_buttons_{};

  // Index array for sorted access to combined buttons
  std::array<uint16_t, BITS_BTN_MAX_COMBINED> sorted_indices_{};

  // Find GPIO with hw.template FindOrExit<LibXR::GPIO> for
  // std::initializer_list<SingleButtonConfig> single_buttons
  // and initialize SingleButton structures.
  // and bind GPIO callbacks to single buttons.
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
                               .state_entry_time = 0,
                               .gpio_handle = gpio_handle,
                               .active_level = cfg.active_level,
                               .button_bit = static_cast<uint8_t>(i)};

      // TODO: Register GPIO interrupt/callback to read button state
      // gpio_handle->RegisterCallback(...);
    }

    return LibXR::ErrorCode::OK;
  }

  /**
   * @brief  Calculate combined button mask from button indices
   * @param  button_indices: Array of button indices
   * @param  key_count: Number of keys in the combination
   * @param [out] mask: Calculated bit mask for the combination
   * @retval LibXR::ErrorCode::OK on success, error code on failure
   * @note   Handles validation, bounds checking, and bit mask calculation
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
   * @brief  Sort combined buttons by key count in descending order using index
   * array
   * @param  count: Number of valid combined buttons to sort
   * @retval None
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
      uint8_t curr_keys = 0; // Initialize to avoid clang-tidy warning
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

  // Initialize combined buttons to CombinedButton, sort by key count
  // descending. Enhanced with std::array features for type safety and
  // efficiency.
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
      combined_btn.combined_btn.current_state = ButtonState::IDLE;
      // Combined buttons don't need GPIO handle and active_level
      combined_btn.combined_btn.gpio_handle = nullptr;

      ++combined_index;
    }

    SortCombinedButtonsDescending(combined_index);

    return LibXR::ErrorCode::OK;
  }
};
