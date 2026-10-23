// Types used in colorclock.ino function signatures. They live in a header so
// Arduino's auto-generated prototypes (inserted above the sketch's own code)
// can see them.
#pragma once
#include <stdint.h>

// Which backlight schedule applies right now (off wins over dim).
enum Schedule { SCHED_NORMAL, SCHED_DIM, SCHED_OFF };

// One row of the on-screen settings.
enum RowKind { ROW_TOGGLE, ROW_PERCENT, ROW_TIME, ROW_ACTION };
struct ConfigRow {
  const char* label;
  RowKind     kind;
  void*       value;              // bool* / uint8_t* / uint16_t*; none for ROW_ACTION
  uint8_t     min, max;           // ROW_PERCENT range
};
