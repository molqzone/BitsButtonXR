# BitsButtonXR

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/language-C++17-orange.svg)](https://en.cppreference.com/)
[![Framework](https://img.shields.io/badge/Framework-LibXR-green)](https://github.com/Jiu-xiao/libxr)
[![GitHub stars](https://img.shields.io/github/stars/molqzone/BitsButtonXR?style=social)](https://github.com/molqzone/BitsButtonXR/stargazers)
[![GitHub forks](https://img.shields.io/github/forks/molqzone/BitsButtonXR?style=social)](https://github.com/molqzone/BitsButtonXR/network/members)
[![GitHub issues](https://img.shields.io/github/issues/molqzone/BitsButtonXR)](https://github.com/molqzone/BitsButtonXR/issues)

**[English](README.md)** | [中文](README_CN.md)

**BitsButtonXR** is a high-performance button detection module designed specifically for embedded systems.

## Module Introduction

It inherits the classic **binary sequence state machine technology** from [BitsButton](https://github.com/530china/BitsButton) framework and has been adapted according to the design concept of [XRobot](https://xrobot-org.github.io/docs/concept). Through integration with [LibXR](https://github.com/Jiu-xiao/libxr) middleware, BitsButtonXR achieves **automated runtime management** and **standardized event distribution**, eliminating the need for manual polling sequence maintenance and greatly simplifying system integration logic.

For detailed usage tutorials and API documentation, please refer to the [Project Wiki](https://github.com/YourUsername/BitsButtonXR/wiki).

## Features

- Based on `LibXR::GPIO` and `LibXR::Timer` interfaces for hardware abstraction, decoupling underlying platform differences and unifying timing scheduling.

- Follows `Application` framework specifications, supporting dependency injection and automated lifecycle management through MANIFEST.

- Uses `LibXR::Event` signals and `LibXR::LockFreeQueue` data separation architecture to build a thread-safe single-producer multi-consumer model.

- Applies a hybrid scheduling mechanism **combining interrupt wake-up and timer polling** to achieve low-power response and physical isolation from mechanical bounce.

## Hardware Requirements

- GPIO device nodes matching the `key_alias` in the `single_buttons` configuration must be provided.

## Constructor Parameters

- `single_buttons`
  - Single button configuration list (`SingleButtonConfig`). Contains GPIO alias, active level (high/low), and timing constraint parameters (short press/long press thresholds, etc.).

- `combined_buttons`
  - Combined button configuration list (`CombinedButtonConfig`). Contains combination alias, whether to suppress single button events, and array of button indices that form the combination.

### API Reference

```cpp
/** Timing parameter constraints */
struct ButtonConstraints {
    uint16_t short_press_time_ms;         ///< Time threshold for short press
    uint16_t long_press_start_time_ms;    ///< Time when long press starts
    uint16_t long_press_period_triger_ms; ///< Period for long press hold events
    uint16_t time_window_time_ms;         ///< Window for double click detection
};

/** Combined button configuration */
struct CombinedButtonConfig {
    const char *combined_alias; ///< Name identifier for the combination
    bool suppress_single_keys; ///< Whether to suppress individual button events
    std::initializer_list<const char *> constituent_aliases; ///< List of button aliases in combination
    ButtonConstraints constraints; ///< Timing constraints for this combination
};

/** Single button configuration */
struct SingleButtonConfig {
    const char *key_alias;         ///< GPIO name identifier for the button
    bool active_level;             ///< GPIO level that indicates button press
    ButtonConstraints constraints; ///< Timing constraints for this button
};
```

## Dependencies

- No dependencies (except for the LibXR basic framework).
