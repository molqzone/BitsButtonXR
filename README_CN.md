# BitsButtonXR

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/language-C++17-orange.svg)](https://en.cppreference.com/)
[![Framework](https://img.shields.io/badge/Framework-LibXR-green)](https://github.com/Jiu-xiao/libxr)
[![GitHub stars](https://img.shields.io/github/stars/molqzone/BitsButtonXR?style=social)](https://github.com/molqzone/BitsButtonXR/stargazers)
[![GitHub forks](https://img.shields.io/github/forks/molqzone/BitsButtonXR?style=social)](https://github.com/molqzone/BitsButtonXR/network/members)
[![GitHub issues](https://img.shields.io/github/issues/molqzone/BitsButtonXR)](https://github.com/molqzone/BitsButtonXR/issues)

**[English](README.md)** | [中文](README_CN.md)

**BitsButtonXR** 是一个专为嵌入式系统设计的高性能按键检测模块。

## 模块简介

它继承了 [BitsButton](https://github.com/530china/BitsButton) 框架经典的**二进制序列状态机技术**，并基于 [XRobot](https://xrobot-org.github.io/docs/concept) 的设计规范进行了适配。通过集成 [LibXR](https://github.com/Jiu-xiao/libxr) 中间件，BitsButtonXR 实现了**自动化的运行管理**与**标准化的事件分发**，无需用户手动维护轮询时序，极大地简化了系统集成逻辑。

详细的使用教程和 API 文档请查阅 [项目 Wiki](https://github.com/YourUsername/BitsButtonXR/wiki)。

## 特性总结

- 基于 `LibXR::GPIO` 与 `LibXR::Timer` 接口实现硬件抽象，解耦底层平台差异并统一时序调度。

- 遵循 `Application` 框架规范，支持通过 MANIFEST 实现依赖注入与生命周期的自动化管理。

- 采用 `LibXR::Event` 信号与 `LibXR::LockFreeQueue` 数据分离架构，构建线程安全的单生产者多消费者模型。

- 应用**中断唤醒与定时器轮询相结合**的混合调度机制，实现低功耗响应与机械抖动的物理隔离。

## 硬件需求

- 需提供与 `single_buttons` 配置中 `key_alias` 相匹配的 GPIO 设备节点

## 构造参数

- `single_buttons`
  - 单按键配置列表 (`SingleButtonConfig`)。包含 GPIO 别名、有效电平（高/低）及时间约束参数（短按/长按阈值等）。

- `combined_buttons`
  - 组合按键配置列表 (`CombinedButtonConfig`)。包含组合键别名、是否抑制单键事件及构成该组合键的按键索引数组。

### API 说明

```cpp
/** 时间参数约束 */
struct ButtonConstraints {
    uint16_t short_press_time_ms;         ///< 短按时间阈值 (毫秒)
    uint16_t long_press_start_time_ms;    ///< 长按开始时间 (毫秒)
    uint16_t long_press_period_triger_ms; ///< 长按保持事件触发周期 (毫秒)
    uint16_t time_window_time_ms;         ///< 双击检测窗口时间 (毫秒)
};

/** 组合按键配置 */
struct CombinedButtonConfig {
    const char *combined_alias; ///< 组合键的名称标识符
    bool suppress_single_keys; ///< 是否抑制单个按键事件
    std::initializer_list<const char *> constituent_aliases; ///< 组合键中包含的按键别名列表
    ButtonConstraints constraints; ///< 该组合键的时间约束
};

/** 单按键配置 */
struct SingleButtonConfig {
    const char *key_alias;         ///< 按键的GPIO名称标识符
    bool active_level;             ///< 表示按键按下的GPIO电平
    ButtonConstraints constraints; ///< 该按键的时间约束
};
```

## 依赖

- 无依赖（除 LibXR 基础框架外）。
