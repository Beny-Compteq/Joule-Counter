/*
 * Calibration metadata stored in the on-board EEPROM (U7).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef METADATA_H_
#define METADATA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ppk2.h"

/*
 * Byte offsets in the EEPROM, as the stock firmware lays them out. Confirmed
 * against a real unit by diffing its primary image against its stock backup
 * copy, whose VDD, mode and IA the stock firmware reported differently (see
 * calibration/README.md). HW is not stored: stock derives it from the SoC's
 * device ID. Bytes not named here belong to stock and are never written.
 */
#define EEPROM_OFF_R		0	/* float[5] */
#define EEPROM_OFF_GS		20	/* float[5] */
#define EEPROM_OFF_O		40	/* float[5] */
#define EEPROM_OFF_S		60	/* float[5] */
#define EEPROM_OFF_I		80	/* float[5] */
#define EEPROM_OFF_GI		100	/* float[5] */
#define EEPROM_OFF_VDD		120	/* uint16, mV */
#define EEPROM_OFF_UG		128	/* float[5] */
#define EEPROM_OFF_MODE		251	/* uint8 */
#define EEPROM_OFF_CALIBRATED	252	/* uint8; stock's flag, read but never written */
#define EEPROM_OFF_IA		255	/* uint16, unaligned */
#define EEPROM_STOCK_IMAGE_SIZE	257

/* In-RAM view. Not a byte image of the EEPROM; see the offsets above. */
struct ppk2_metadata {
	float r[PPK2_RANGE_COUNT];	/* shunt resistance per range, ohm */
	float gs[PPK2_RANGE_COUNT];	/* gain, quadratic term */
	float o[PPK2_RANGE_COUNT];	/* zero-current ADC reading */
	float s[PPK2_RANGE_COUNT];	/* supply-voltage dependent offset */
	float i[PPK2_RANGE_COUNT];	/* constant offset */
	float gi[PPK2_RANGE_COUNT];	/* gain, linear term */
	float ug[PPK2_RANGE_COUNT];	/* user gains */
	uint16_t vdd;			/* source-mode setpoint, mV */
	uint16_t hw;			/* low 16 bits of the device ID, as stock reports */
	uint8_t mode;			/* enum ppk2_mode */
	uint8_t calibrated;		/* stock's stored flag, informational */
	uint16_t ia;			/* instrumentation amplifier offset wiper */
};

#define METADATA_TEXT_SIZE 2048

int metadata_init(void);

struct ppk2_metadata *metadata_get(void);

/* The stored shunt values are usable, whatever stock's flag byte says. */
bool metadata_is_calibrated(void);

/* True when the IA wiper value had to be invented rather than read. */
bool metadata_ia_unknown(void);

/* Writes the fields this firmware owns (vdd, ug, mode, ia), only if changed. */
int metadata_save(void);
void metadata_save_deferred(void);
void metadata_load_defaults(void);

int metadata_backup_write(void);
int metadata_backup_restore(void);
int metadata_backup_valid(void);

/* Formats the reply to GetMetadata, terminated by "END\n". */
size_t metadata_format(char *buf, size_t size);

/* Sets one field from its text name ("r0", "gs3", "vdd", "mode", ...). */
int metadata_set(const char *key, const char *value);

int metadata_raw_read(size_t offset, uint8_t *buf, size_t len);

#endif /* METADATA_H_ */
