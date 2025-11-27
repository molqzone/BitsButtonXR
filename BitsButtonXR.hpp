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
    const char **single_key_aliases;
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
    // Hardware initialization example:
    // auto dev = hw.template Find<LibXR::GPIO>("led");
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

  std::array<SingleButton, BITS_BTN_MAX_SINGLES> single_buttons_{};
  std::array<CombinedButton, BITS_BTN_MAX_COMBINED> combined_buttons_{};

  // Index array for sorted access to combined buttons
  std::array<uint16_t, BITS_BTN_MAX_COMBINED> combo_sorted_indices_{};

  // Find GPIO with hw.template FindOrExit<LibXR::GPIO> for
  // std::initializer_list<SingleButtonConfig> single_buttons
  // and initialize SingleButton structures.
  // and bind GPIO callbacks to single buttons.
  LibXR::ErrorCode BitsButtonXR::InitializeSingleButtons(
      LibXR::HardwareContainer &hw,
      std::initializer_list<SingleButtonConfig> configs) {
    if (configs.size() > BITS_BTN_MAX_SINGLES) {
      return LibXR::ErrorCode::SIZE_ERR;
    }

    auto it = single_buttons_.begin();
    for (const auto &cfg : configs) {
      // 查找硬件句柄
      LibXR::GPIO *gpio_handle = hw.template Find<LibXR::GPIO>(cfg.key_alias);
      if (!gpio_handle) {
        return LibXR::ErrorCode::NOT_FOUND;
      }

      *it = {.key_alias = cfg.key_alias,
             .current_state = ButtonState::IDLE,
             .last_state = ButtonState::IDLE,
             .state_bits = 0,
             .constraints = cfg.constraints,
             .state_entry_time = 0,
             .gpio_handle = gpio_handle,
             .active_level = cfg.active_level};

      // TODO: Register GPIO interrupt/callback to read button state
      // gpio_handle->RegisterCallback(...);

      ++it;
    }
    return LibXR::ErrorCode::OK;
  }

  // Helper function to find single button index by alias using C-style strings
  uint8_t GetButtonIndexByAlias(const char *alias) {
    if (!alias) {
      return BITS_BTN_INVALID_INDEX;
    }

    for (size_t i = 0; i < single_buttons_.size(); ++i) {
      const auto &btn = single_buttons_.at(i);
      if (btn.key_alias && strcmp(btn.key_alias, alias) == 0) {
        return static_cast<uint8_t>(i);
      }
    }
    return BITS_BTN_INVALID_INDEX;
  }

  /**
   * @brief  Calculate combined button mask from single button aliases
   * @param  single_key_aliases: Array of single button alias strings
   * @param  key_count: Number of keys in the combination
   * @param [out] mask: Calculated bit mask for the combination
   * @retval LibXR::ErrorCode::OK on success, error code on failure
   * @note   Handles validation, bounds checking, and bit mask calculation
   */
  LibXR::ErrorCode CalculateCombinedMask(const char **single_key_aliases,
                                         uint8_t key_count,
                                         ButtonMaskType &mask) {
    if (!single_key_aliases || key_count == 0) {
      return LibXR::ErrorCode::ARG_ERR;
    }

    mask = 0;

    // Process each button in the combination
    for (uint8_t j = 0; j < key_count; ++j) {
      const char *single_alias = single_key_aliases[j];
      if (!single_alias) {
        return LibXR::ErrorCode::ARG_ERR;
      }

      // Find button index by alias
      uint8_t single_index = GetButtonIndexByAlias(single_alias);
      if (single_index == BITS_BTN_INVALID_INDEX) {
        return LibXR::ErrorCode::ARG_ERR;
      }

      // Bounds checking for bit mask calculation
      if (single_index >= sizeof(ButtonMaskType) * 8) {
        return LibXR::ErrorCode::OUT_OF_RANGE;
      }

      // Set bit for this button in the mask
      mask |= static_cast<ButtonMaskType>(1UL) << single_index;
    }

    return LibXR::ErrorCode::OK;
  }

  /**
   * @brief  Sort combined buttons by key count in descending order using index
   * array
   * @param  count: Number of valid combined buttons to sort
   * @retval None
   * @note   Uses insertion sort algorithm with index array approach:
   *         - Maintains original array order while providing sorted access via
   * indices
   *         - Efficient for small arrays (O(n²) worst case, O(n) best case)
   *         - Stable sort (maintains relative order of equal elements)
   *         - Ensures combos with more keys are matched first during detection
   *         - Uses std::array::at() for bounds safety and LibXR::String for
   * comparisons
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
      combo_sorted_indices_.at(i) = static_cast<uint16_t>(i);
    }

    // Insertion sort on index array in descending order by key_count
    // This ensures that button combinations with more keys are checked first,
    // preventing false positives where smaller combinations match larger ones
    for (size_t i = 1; i < count; ++i) {
      uint16_t temp_idx = combo_sorted_indices_.at(i);
      uint8_t temp_key_count = combined_buttons_.at(temp_idx).key_count;
      int16_t j = static_cast<int16_t>(i) - 1;

      // Find insertion position: higher key count has higher priority
      // Shift indices of buttons with lower key count to the right
      while (j >= 0 &&
             combined_buttons_.at(combo_sorted_indices_.at(j)).key_count <
                 temp_key_count) {
        combo_sorted_indices_.at(j + 1) = combo_sorted_indices_.at(j);
        j--;
      }

      // Insert current button index at correct position
      combo_sorted_indices_.at(j + 1) = temp_idx;
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
      if (!cfg.combined_alias || cfg.key_count == 0 ||
          !cfg.single_key_aliases) {
        return LibXR::ErrorCode::ARG_ERR;
      }
    }

    size_t combined_index = 0;

    // First pass: Calculate combined masks and collect metadata
    for (const auto &cfg : configs) {
      ButtonMaskType mask = 0;

      // Calculate combined mask using the helper function
      auto result =
          CalculateCombinedMask(cfg.single_key_aliases, cfg.key_count, mask);
      if (result != LibXR::ErrorCode::OK) {
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

    // Sort combined buttons by key count in descending order
    // Using insertion sort for efficiency with small arrays and maintaining
    // stability
    SortCombinedButtonsDescending(combined_index);

    return LibXR::ErrorCode::OK;
  }
};
