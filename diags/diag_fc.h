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

#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CONFIG_FILE		"/etc/ppc64-diag/diag_fc.config"
#define DESCR_LENGTH		1024
#define KEY_LENGTH		128
#define LOCATION_LENGTH		80
#define MAX_DICT_ELEMENTS	64
#define FC_HOST_PATH		"/sys/class/fc_host"
#define FC_RPORT_PATH		"/sys/class/fc_remote_ports"
#define WWPN_LENGTH		20
#define WWNN_LENGTH		20
#define PCI_ADDR_LENGTH		16

/* Struct to parse key=value settings in a file */
struct dictionary {
	char key[KEY_LENGTH];
	long double value;
};

/* Fibre Channel statistics from sysfs */
struct fc_statistics {
	uint64_t tx_frames;
	uint64_t rx_frames;
	uint64_t tx_words;
	uint64_t rx_words;
	uint64_t lip_count;
	uint64_t nos_count;
	uint64_t error_frames;
	uint64_t dumped_frames;
	uint64_t link_failure_count;
	uint64_t loss_of_sync_count;
	uint64_t loss_of_signal_count;
	uint64_t invalid_tx_word_count;
	uint64_t invalid_crc_count;
	uint64_t fcp_input_requests;
	uint64_t fcp_output_requests;
	uint64_t fcp_control_requests;
	uint64_t fcp_input_megabytes;
	uint64_t fcp_output_megabytes;
};

/* Fibre Channel host information */
struct fc_host {
	char name[NAME_MAX];			/* e.g., "host0" */
	char wwpn[WWPN_LENGTH];			/* World Wide Port Name */
	char wwnn[WWNN_LENGTH];			/* World Wide Node Name */
	char port_state[16];			/* Online/Offline/Linkdown */
	char speed[16];				/* Current link speed */
	char supported_speeds[64];		/* Supported speeds */
	char fabric_name[WWNN_LENGTH];		/* Connected fabric WWNN */
	char port_type[16];			/* NPort/FPort/etc */
	char symbolic_name[256];		/* Human-readable name */
	uint64_t port_id;			/* FC address */
	char pci_address[PCI_ADDR_LENGTH];	/* PCI address for EEH correlation */
	struct fc_statistics stats;		/* Current statistics */
	bool eeh_event_detected;		/* EEH event flag (PowerPC) */
};

/* Fibre Channel remote port information */
struct fc_remote_port {
	char name[NAME_MAX];			/* e.g., "rport-0:0-0" */
	char wwpn[WWPN_LENGTH];			/* World Wide Port Name */
	char wwnn[WWNN_LENGTH];			/* World Wide Node Name */
	char port_state[16];			/* Online/Blocked/etc */
	uint64_t port_id;			/* FC address */
	char roles[32];				/* Target/Initiator */
};

/* Multipath device information */
struct multipath_device {
	char dm_name[NAME_MAX];			/* e.g., "dm-0" */
	char mpath_name[NAME_MAX];		/* e.g., "mpatha" */
	char wwid[256];				/* World Wide ID */
	int total_paths;
	int active_paths;
	char **slave_devices;			/* Array of underlying devices */
};

/* EEH event information (PowerPC-specific) */
struct eeh_event {
	char pci_address[PCI_ADDR_LENGTH];	/* Affected PCI device */
	char event_type[64];			/* "Frozen PE", "Permanent failure", etc. */
	time_t timestamp;			/* When event occurred */
	char description[256];			/* Full event description */
};

/* Fibre Channel VPD data from lspci */
struct fc_vpd_data {
	char manufacturer[64];
	char part_number[64];
	char serial_number[64];
	char firmware_version[32];
	char hardware_revision[32];
	char pci_address[PCI_ADDR_LENGTH];	/* e.g., "0000:01:00.0" */
	char device_name[256];			/* Product name */
};

/* Notification flags for monitoring */
struct fc_notify {
	bool port_state_change;
	bool link_errors;
	bool multipath_degradation;
	bool performance_drop;
	bool eeh_events;
};

/* Function declarations */
extern int read_fc_statistics(const char *host_name, struct fc_statistics *stats);
extern int read_fc_host_info(const char *host_name, struct fc_host *host);
extern int read_fc_vpd_data(const char *host_name, struct fc_vpd_data *vpd);
extern int discover_fc_hosts(struct fc_host **hosts, int *count);
extern int discover_fc_rports(struct fc_remote_port **rports, int *count);
extern int discover_multipath_devices(struct multipath_device **mpaths, int *count);
extern int check_eeh_events(struct fc_host *host, struct eeh_event **events, int *count);
extern int read_file_dict(char *file_name, struct dictionary *dict, int max_params);
extern int location_code_fc(char *location, char *host_name);

#endif /* _DIAG_FC_H */

// Made with Bob
