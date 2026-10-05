/* Created by IBM Bob | 2026-08-31 20:43 UTC | Ahmad Hussain */
/*
 * Copyright (C) 2026 IBM Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,
 * USA.
 */

#ifndef _DIAG_FC_H
#define _DIAG_FC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Limits                                                               */
/* ------------------------------------------------------------------ */

#define MAX_FC_PORTS		8

#define PCI_ADDR_LEN		16
#define FC_HOST_LEN		32
#define WWN_LEN			32
#define HOSTNAME_LEN		256
#define VERSION_LEN		128
#define SYMBOLIC_NAME_LEN	256
#define FIELD_LEN		64	/* generic short field */
#define LOCATION_LENGTH		80
#define DESCR_LENGTH		1024
#define FC_HOST_STATS_ATTRS	2	/* error_frames, dumped_frames */
#define MAX_REMOTE_PORTS	32
#define MAX_MPATH_DEVICES	16
#define MPATH_DEV_NAME_LEN	32
#define ROLES_LEN		64

/* ------------------------------------------------------------------ */
/* Collection status                                                    */
/* ------------------------------------------------------------------ */

/*
 * Values used in vpd_status and location_status to distinguish between
 * hardware unavailability, parse failures, and successful collection.
 */
#define CSTATUS_OK		"collected"
#define CSTATUS_UNAVAILABLE	"unavailable"
#define CSTATUS_UNSUPPORTED	"unsupported"
#define CSTATUS_PARSE_ERROR	"parse_error"
#define CSTATUS_NOT_ATTEMPTED	"not_attempted"

/* ------------------------------------------------------------------ */
/* Per-port information                                                 */
/* ------------------------------------------------------------------ */

/*
 * struct fc_statistics - Error counters from /sys/class/fc_host/hostX/statistics/.
 *
 * Fields are stored as strings to match the existing fc_port convention.
 * "unknown" is used when the sysfs attribute cannot be read.
 */
struct fc_statistics {
	char error_frames[FIELD_LEN];
	char dumped_frames[FIELD_LEN];
};

/*
 * struct fc_port - All sysfs-readable information for one FC host/port.
 *
 * pci_addr: PCI domain:bus:dev.fn string, e.g. "0155:90:00.0".
 * fc_host:  Linux kernel fc_host name, e.g. "host1".
 * Fields that cannot be read are stored as "unknown".
 */
struct fc_port {
	char pci_addr[PCI_ADDR_LEN];
	char fc_host[FC_HOST_LEN];
	char wwpn[WWN_LEN];
	char wwnn[WWN_LEN];
	char port_state[FIELD_LEN];
	char port_type[FIELD_LEN];
	char port_id[FIELD_LEN];
	char speed[FIELD_LEN];
	char supported_speeds[FIELD_LEN];
	char fabric_name[FIELD_LEN];
	bool pci_function_present;
	bool fc_host_present;
	struct fc_statistics stats;
};

/*
 * struct fc_remote_port - Minimal identity for one remote FC port.
 *
 * Sourced from /sys/class/fc_remote_ports/rport-X:Y-Z/.
 * Kept minimal: enough to identify the port if it goes down.
 */
struct fc_remote_port {
	char port_id[FIELD_LEN];
	char port_name[WWN_LEN];	/* WWPN */
	char port_state[FIELD_LEN];
	char roles[ROLES_LEN];
};

/*
 * struct multipath_device - Minimal record of one dm-multipath device.
 *
 * dm_name: kernel dm device name, e.g. "dm-3".
 * dev_name: one FC-backed block device slave, e.g. "sdb".
 *           There may be multiple slaves; we record the first one found.
 */
struct multipath_device {
	char dm_name[MPATH_DEV_NAME_LEN];
	char dev_name[MPATH_DEV_NAME_LEN];
};

/* ------------------------------------------------------------------ */
/* Adapter-level VPD                                                    */
/* ------------------------------------------------------------------ */

/*
 * struct fc_adapter_vpd - Identity fields from PCIe VPD.
 *
 * Keyword mappings below are candidates to be verified against the
 * actual binary VPD of the target adapter before treating as definitive.
 * Fields not found in VPD remain as empty strings.
 */
struct fc_adapter_vpd {
	char part_number[FIELD_LEN];		/* PN keyword */
	char fru_part_number[FIELD_LEN];	/* FN keyword */
	char serial_number[FIELD_LEN];		/* SN keyword */
	char ccin[FIELD_LEN];			/* VH keyword (IBM vendor-specific) */
	char feature_code[FIELD_LEN];		/* FC keyword */
	char firmware_level[FIELD_LEN];		/* V0 keyword (IBM vendor-specific) */
	char ec_level[FIELD_LEN];		/* EC keyword */
};

/* ------------------------------------------------------------------ */
/* Full incident record                                                 */
/* ------------------------------------------------------------------ */

/*
 * struct fc_incident - Everything collected for one permanent-failure event.
 *
 * One physical adapter may expose multiple PCI functions and FC hosts.
 * All of them are correlated into a single incident.
 */
struct fc_incident {
	/* --- trigger information (supplied by caller) --- */
	char trigger_type[FIELD_LEN];		/* e.g. "EEH_PERMANENT_FAILURE" */
	char trigger_source[FIELD_LEN];		/* e.g. "platform", "manual" */
	char trigger_pci_addr[PCI_ADDR_LEN];
	char trigger_reason[DESCR_LENGTH];
	char trigger_reference[DESCR_LENGTH];	/* platform/EEH event ID */
	char trigger_timestamp[FIELD_LEN];	/* ISO 8601 UTC */

	/* --- machine context --- */
	char hostname[HOSTNAME_LEN];
	char os_name[FIELD_LEN];
	char kernel_release[VERSION_LEN];	/* uname -r */
	char kernel_build[VERSION_LEN];		/* uname -v */
	char architecture[FIELD_LEN];		/* uname -m */

	/* --- adapter identity --- */
	char adapter_symbolic_name[SYMBOLIC_NAME_LEN]; /* raw fc_host/symbolic_name */
	char adapter_model[FIELD_LEN];		/* "unknown" until derived */
	char pci_vendor_id[8];			/* e.g. "0x10df" */
	char pci_device_id[8];			/* e.g. "0xf500" */
	char pci_subsystem_vendor_id[8];
	char pci_subsystem_device_id[8];
	char driver_name[FIELD_LEN];
	char driver_version[VERSION_LEN];
	struct fc_adapter_vpd vpd;

	/* --- per-port data --- */
	struct fc_port ports[MAX_FC_PORTS];
	int num_ports;

	/* --- remote ports visible at failure time --- */
	struct fc_remote_port remote_ports[MAX_REMOTE_PORTS];
	int num_remote_ports;

	/* --- dm-multipath devices backed by this adapter --- */
	struct multipath_device mpath_devices[MAX_MPATH_DEVICES];
	int num_mpath_devices;

	/* --- IBM location code --- */
	char location_code[LOCATION_LENGTH];

	/* --- collection metadata --- */
	char collect_timestamp[FIELD_LEN];	/* ISO 8601 UTC */
	char vpd_status[FIELD_LEN];		/* CSTATUS_* value */
	char location_status[FIELD_LEN];	/* CSTATUS_* value */
};

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

struct fc_config;	/* defined in diag_fc.c; opaque to callers */

/*
 * collect_fc_incident - Collect FC adapter failure evidence.
 *
 * @pci_addr:          PCI domain:bus:dev.fn of the affected function
 * @trigger_type:      short string, e.g. "EEH_PERMANENT_FAILURE"
 * @trigger_source:    what generated the trigger, e.g. "platform"
 * @trigger_reason:    human-readable failure reason
 * @trigger_timestamp: ISO 8601 event time; NULL uses collection time
 * @cfg:               optional configuration, or NULL for defaults
 * @out:               caller-supplied incident struct to fill
 *
 * Performs only read-only sysfs and device-tree access.
 * Returns 0 on success or partial collection.
 * Returns -1 only if pci_addr or out is NULL.
 * A gone adapter produces a partial report, not a fatal error.
 */
int collect_fc_incident(const char *pci_addr,
			const char *trigger_type,
			const char *trigger_source,
			const char *trigger_reason,
			const char *trigger_timestamp,
			const struct fc_config *cfg,
			struct fc_incident *out);

/*
 * write_fc_report - Write incident as a JSON file.
 *
 * @inc:  completed incident struct
 * @path: output file path; NULL or "-" writes to stdout
 * @cfg:  optional configuration, or NULL for defaults
 *
 * Returns 0 on success, -1 on error.
 */
int write_fc_report(const struct fc_incident *inc, const char *path,
		    const struct fc_config *cfg);

#endif /* _DIAG_FC_H */
