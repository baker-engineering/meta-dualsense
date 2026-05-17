#include "feature_reports.h"

/* === Feature 0x05 — stick / IMU calibration === */
/* DS4Windows reads this to set deadzones and IMU drift. All-zero values mean
 * "factory defaults / no calibration applied", which is acceptable. */
static const uint8_t fr_05_calibration[41] = {
    0x05,
    /* 40 bytes of calibration; zeros are accepted as default. */
};

/* === Feature 0x09 — Pair Info / MAC address === */
/* Format: [ID, MAC0..MAC5, padding...]. DS4Windows uses bytes 1..6 as the
 * unique controller identifier; any plausible MAC works. */
static const uint8_t fr_09_pairinfo[20] = {
    0x09,
    0xde, 0xad, 0xbe, 0xef, 0x00, 0x01,   /* device MAC */
    0x08, 0x25, 0x00,                     /* link key length / reserved */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* host MAC (zeros = unpaired) */
    0x00, 0x00, 0x00, 0x00,
};

/* === Feature 0x20 — Hardware / firmware info === */
static const uint8_t fr_20_hwfw[64] = {
    0x20,
    /* 63 bytes; zeros are inert. */
};

/* === Feature 0x21 — small status === */
static const uint8_t fr_21_status[5] = {
    0x21,
    0x00, 0x00, 0x00, 0x00,
};

/* === Feature 0x22 — other === */
static const uint8_t fr_22_other[64] = { 0x22, };

/* === Feature 0x08 — protocol options === */
static const uint8_t fr_08_options[48] = { 0x08, };

/* === Feature 0x0A — unknown / aux === */
static const uint8_t fr_0a_aux[27] = { 0x0a, };

/* === Feature 0xA0 — single-byte capability === */
static const uint8_t fr_a0_cap[2] = { 0xa0, 0x00 };

/* === Feature 0x80..0x85, 0xE0, 0xF0..0xF5 — firmware/factory test === */
static const uint8_t fr_80[64] = { 0x80, };
static const uint8_t fr_81[64] = { 0x81, };
static const uint8_t fr_82[10] = { 0x82, };
static const uint8_t fr_83[64] = { 0x83, };
static const uint8_t fr_84[64] = { 0x84, };
static const uint8_t fr_85[3]  = { 0x85, 0x00, 0x00 };
static const uint8_t fr_e0[64] = { 0xe0, };
static const uint8_t fr_f0[64] = { 0xf0, };
static const uint8_t fr_f1[64] = { 0xf1, };
static const uint8_t fr_f2[64] = { 0xf2, };
static const uint8_t fr_f4[64] = { 0xf4, };
static const uint8_t fr_f5[4]  = { 0xf5, 0x00, 0x00, 0x00 };

const struct feature_report feature_reports[] = {
    { 0x05, sizeof(fr_05_calibration), fr_05_calibration },
    { 0x08, sizeof(fr_08_options),     fr_08_options },
    { 0x09, sizeof(fr_09_pairinfo),    fr_09_pairinfo },
    { 0x0a, sizeof(fr_0a_aux),         fr_0a_aux },
    { 0x20, sizeof(fr_20_hwfw),        fr_20_hwfw },
    { 0x21, sizeof(fr_21_status),      fr_21_status },
    { 0x22, sizeof(fr_22_other),       fr_22_other },
    { 0x80, sizeof(fr_80),             fr_80 },
    { 0x81, sizeof(fr_81),             fr_81 },
    { 0x82, sizeof(fr_82),             fr_82 },
    { 0x83, sizeof(fr_83),             fr_83 },
    { 0x84, sizeof(fr_84),             fr_84 },
    { 0x85, sizeof(fr_85),             fr_85 },
    { 0xa0, sizeof(fr_a0_cap),         fr_a0_cap },
    { 0xe0, sizeof(fr_e0),             fr_e0 },
    { 0xf0, sizeof(fr_f0),             fr_f0 },
    { 0xf1, sizeof(fr_f1),             fr_f1 },
    { 0xf2, sizeof(fr_f2),             fr_f2 },
    { 0xf4, sizeof(fr_f4),             fr_f4 },
    { 0xf5, sizeof(fr_f5),             fr_f5 },
};

const size_t feature_reports_count =
    sizeof(feature_reports) / sizeof(feature_reports[0]);

const struct feature_report *find_feature_report(uint8_t id) {
    for (size_t i = 0; i < feature_reports_count; i++)
        if (feature_reports[i].id == id) return &feature_reports[i];
    return NULL;
}
