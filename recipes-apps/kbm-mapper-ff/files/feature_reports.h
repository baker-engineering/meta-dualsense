#ifndef FEATURE_REPORTS_H
#define FEATURE_REPORTS_H
/* Plausible stub data for DualSense HID feature reports. DS4Windows and other
 * Sony-aware tools query these via GET_REPORT(Feature, <id>) on the USB
 * control endpoint. We return realistic-shaped responses so the controller is
 * recognised as a real DualSense.
 *
 * Each report's first byte is the report ID; sizes match what the descriptor
 * advertises (the report counts in dualsense_report_descriptor[]).
 */
#include <stddef.h>
#include <stdint.h>

struct feature_report {
    uint8_t id;
    size_t  size;       /* bytes including the report ID prefix */
    const uint8_t *data;
};

extern const struct feature_report feature_reports[];
extern const size_t feature_reports_count;

const struct feature_report *find_feature_report(uint8_t id);

#endif
