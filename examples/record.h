#ifndef BISHFPM_EXAMPLE_RECORD_H
#define BISHFPM_EXAMPLE_RECORD_H
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Read and drain exactly one physical record. Reject NUL and overlong records.
 * 1=record, -1=malformed, 0=EOF. Caller checks ferror after the loop. */
static int read_record(FILE *fp, char *out, size_t capacity) {
  if (ferror(fp) || feof(fp))
    return 0;
  size_t n = 0;
  int c, bad = 0, seen = 0;
  while ((c = fgetc(fp)) != EOF) {
    seen = 1;
    if (c == '\n')
      break;
    if (c == 0 || n + 1 >= capacity)
      bad = 1;
    else
      out[n++] = (char)c;
  }
  if (ferror(fp))
    return 0;
  if (!seen)
    return 0;
  if (n && out[n - 1] == '\r')
    n--;
  out[n] = 0;
  return bad ? -1 : 1;
}
/* Empty columns, including a final empty column, are retained. */
static bool split_tabs(char *record, char **fields, size_t capacity,
                       size_t *count) {
  *count = 0;
  for (;;) {
    if (*count == capacity)
      return false;
    fields[(*count)++] = record;
    char *tab = strchr(record, '\t');
    if (!tab)
      return true;
    *tab = 0;
    record = tab + 1;
  }
}

/* Preserve the adapters' 1e14-second input ceiling, safely below
 * UINT64_MAX/1000. */
static bool parse_seconds(const char *text, double *seconds,
                          uint64_t *milliseconds) {
  char *end;
  double value = strtod(text, &end);
  if (end == text || *end || !isfinite(value) || value < 0 || value > 1e14)
    return false;
  *seconds = value;
  *milliseconds = (uint64_t)(value * 1000.0);
  return true;
}
#endif
