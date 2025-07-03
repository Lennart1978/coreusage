# coreusage

A small CLI tool for displaying CPU usage.

## Author

(2025) Lennart Martens

## Description

CLI program to display current CPU usage. It is intended for Linux users and designed as a lightweight command-line tool.

## Build Instructions

Requirements:

- GCC or compatible C compiler
- Make
- libsensors development files (usually provided by the package `lm_sensors` or `libsensors-dev`)

To compile, run in the project directory:

```bash
make
```

This will build the binary: "coreusage"

## Installation

```bash
sudo make install
```

## License

This project is released under MIT license

## Screenshot

![screenshot](screenshot.jpg)
