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

/*
 * diag_fc - FC adapter failure evidence collector for PowerVM LPARs.
 *
 * Invoked after an external component (EEH subsystem, platform firmware,
 * or a test harness) identifies a permanent FC adapter failure.
 * Attempts to collect adapter identity, port state, driver, firmware, and
 * machine context into a structured JSON report.
 *
 * diag_fc does NOT detect failures, monitor FC health, or run continuously.
 * All sysfs access is strictly read-only.
 *
 * Usage:
 *   diag_fc -p <pci_addr> [OPTIONS]
 *
 * Example:
 *   diag_fc -p 0155:90:00.0 \
 *           -t EEH_PERMANENT_FAILURE \
 *           -s platform \
 *           -r "I/O adapter permanently unavailable" \
 *           -o /tmp/diag_fc.json
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "diag_fc.h"
#include "platform.h"
#include "utils.h"

/* ------------------------------------------------------------------ */
/* Config                                                               */
/* ------------------------------------------------------------------ */

#define FC_CONFIG_FILE		"/etc/ppc64-diag/diag_fc.config"
#define FC_CONFIG_KEY_LEN	128

/*
 * struct fc_config - Parsed settings from FC_CONFIG_FILE.
 */
struct fc_config {
	long          max_report_size_kb;	/* 0 = no limit */
	int           mpath_enabled;		/* 1 = collect, 0 = skip */
};

/*
 * read_fc_config - Parse FC_CONFIG_FILE into cfg.
 *
 * Uses the same key=value format as diag_nvme.config.
 * Silently uses defaults if the file is absent or a key is unrecognised.
 * Logs a warning to stderr on parse errors (bad lines) but continues.
 */
static void read_fc_config(struct fc_config *cfg)
{
	FILE *fp;
	int line_no;
	char *line = NULL;
	size_t line_sz = 0;
	char key[FC_CONFIG_KEY_LEN];
	long val;

	/* Safe defaults */
	cfg->max_report_size_kb = 4096;
	cfg->mpath_enabled      = 1;

	fp = fopen(FC_CONFIG_FILE, "r");
	if (!fp)
		return;		/* absent config is fine */

	for (line_no = 1; getline(&line, &line_sz, fp) != -1; line_no++) {
		/* Skip blank lines and comment lines */
		{
			const char *p = line;

			while (*p == ' ' || *p == '\t')
				p++;
			if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
				continue;
		}

		/* Width matches FC_CONFIG_KEY_LEN - 1 */
		if (sscanf(line, " %127[^= ] = %ld", key, &val) < 2) {
			fprintf(stderr,
				"diag_fc: %s line %d: parse error: %s\n",
				FC_CONFIG_FILE, line_no, line);
			continue;
		}

		if (!strcmp(key, "MAX_REPORT_SIZE_KB"))
			cfg->max_report_size_kb = val;
		else if (!strcmp(key, "MPATH_ENABLED"))
			cfg->mpath_enabled = (val != 0) ? 1 : 0;
	}

	free(line);
	fclose(fp);
}

/* ------------------------------------------------------------------ */
/* Private path constants                                               */
/* ------------------------------------------------------------------ */

#define FC_SYS_PATH		"/sys/class/fc_host"
#define PCI_SYS_PATH		"/sys/bus/pci/devices"
#define DEVICE_TREE_PATH	"/sys/firmware/devicetree/base"

/* ------------------------------------------------------------------ */
/* Internal sysfs helpers                                               */
/* ------------------------------------------------------------------ */

/*
 * read_sysfs_str - Read a sysfs attribute into buf.
 *
 * Validates arguments, reads up to bufsz-1 bytes, NUL-terminates, and
 * strips trailing whitespace (space, tab, newline, carriage return).
 * Retries on EINTR.
 *
 * Returns 0 on success, -1 on error.
 */
static int read_sysfs_str(const char *path, char *buf, size_t bufsz)
{
	int fd;
	ssize_t n;
	size_t len;

	if (!path || !buf || bufsz == 0) {
		errno = EINVAL;
		return -1;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	do {
		n = read(fd, buf, bufsz - 1);
	} while (n < 0 && errno == EINTR);

	close(fd);

	if (n <= 0)
		return -1;

	buf[n] = '\0';

	/* Strip trailing whitespace using index-based loop */
	len = (size_t)n;
	while (len > 0) {
		unsigned char c = (unsigned char)buf[len - 1];

		if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
			break;
		buf[--len] = '\0';
	}

	return 0;
}

/*
 * safe_read - Read a sysfs attribute, storing "unknown" on any failure.
 */
static void safe_read(const char *path, char *buf, size_t bufsz)
{
	if (read_sysfs_str(path, buf, bufsz) != 0)
		snprintf(buf, bufsz, "unknown");
}

/* ------------------------------------------------------------------ */
/* PCI helpers                                                          */
/* ------------------------------------------------------------------ */

/*
 * pci_function_exists - Return true if the PCI sysfs directory exists.
 */
static bool pci_function_exists(const char *pci_addr)
{
	char path[PATH_MAX];
	struct stat st;
	int rc;

	if (!pci_addr || !*pci_addr)
		return false;

	rc = snprintf(path, sizeof(path), "%s/%s", PCI_SYS_PATH, pci_addr);
	if (rc < 0 || (size_t)rc >= sizeof(path))
		return false;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/*
 * fc_host_for_pci - Find the fc_host name for a given PCI address.
 *
 * Scans /sys/class/fc_host/. For each entry hostX, resolves
 * /sys/class/fc_host/hostX/device via realpath() and checks whether the
 * last path component matches pci_addr.
 *
 * Returns 0 and fills host_name on success, -1 if not found.
 */
static int fc_host_for_pci(const char *pci_addr, char *host_name,
			   size_t namesz)
{
	DIR *dir;
	struct dirent *de;
	char link_path[PATH_MAX];
	char resolved[PATH_MAX];
	const char *last;
	int rc;

	if (!pci_addr || !*pci_addr || !host_name || namesz == 0)
		return -1;

	host_name[0] = '\0';

	dir = opendir(FC_SYS_PATH);
	if (!dir)
		return -1;

	while ((de = readdir(dir)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		rc = snprintf(link_path, sizeof(link_path),
			      "%s/%s/device", FC_SYS_PATH, de->d_name);
		if (rc < 0 || (size_t)rc >= sizeof(link_path))
			continue;

		if (realpath(link_path, resolved) == NULL)
			continue;

		/*
		 * resolved is the canonical path of the PCI function directory,
		 * e.g. /sys/devices/.../0155:90:00.0
		 * Find the last path separator and compare what follows it.
		 */
		/*
		 * resolved may be a child of the PCI function dir
		 * (e.g. .../0155:90:00.0/host1) on some kernels.
		 * Walk every path component looking for pci_addr.
		 */
		last = resolved;
		while (*last) {
			const char *slash = strchr(last, '/');

			if (!slash) {
				if (strcmp(last, pci_addr) == 0) {
					snprintf(host_name, namesz, "%s",
						 de->d_name);
					closedir(dir);
					return 0;
				}
				break;
			}
			if ((size_t)(slash - last) == strlen(pci_addr) &&
			    strncmp(last, pci_addr,
				    (size_t)(slash - last)) == 0) {
				snprintf(host_name, namesz, "%s", de->d_name);
				closedir(dir);
				return 0;
			}
			last = slash + 1;
		}
	}
	closedir(dir);
	return -1;
}

/*
 * is_fc_pci_function - Return true if a PCI sysfs entry is an FC controller.
 *
 * Checks PCI class 0x0c04xx (Fibre Channel) first via the class sysfs file.
 * Falls back to checking whether the function maps to an fc_host entry.
 */
static bool is_fc_pci_function(const char *pci_addr)
{
	char path[PATH_MAX];
	char class_str[16];
	unsigned long pci_class;
	char *end;
	int rc;

	if (!pci_addr || !*pci_addr)
		return false;

	if (!pci_function_exists(pci_addr))
		return false;

	/* Check PCI class: 0x0c04xx = Fibre Channel controller */
	rc = snprintf(path, sizeof(path), "%s/%s/class", PCI_SYS_PATH, pci_addr);
	if (rc >= 0 && (size_t)rc < sizeof(path) &&
	    read_sysfs_str(path, class_str, sizeof(class_str)) == 0) {
		errno = 0;
		end = NULL;
		pci_class = strtoul(class_str, &end, 0);
		if (errno == 0 && end != class_str && *end == '\0') {
			if ((pci_class >> 8) == 0x0c04)
				return true;
		}
	}

	/* Fallback: check if any fc_host maps to this address */
	{
		char host_name[FC_HOST_LEN];

		if (fc_host_for_pci(pci_addr, host_name, sizeof(host_name)) == 0)
			return true;
	}

	return false;
}

/*
 * sibling_pci_functions - Find all FC PCI functions on the same physical slot.
 *
 * For pci_addr "0155:90:00.0", the slot prefix is "0155:90:00".
 * A valid sibling has the form "0155:90:00.<F>" where the character at
 * prefix_len is exactly '.', and the function is confirmed as an FC device.
 *
 * The trigger pci_addr is always placed first in addrs[].
 * Returns the number of entries filled (>= 1), or 0 on bad input.
 */
static int sibling_pci_functions(const char *pci_addr, char addrs[][PCI_ADDR_LEN],
				 int max_addrs)
{
	char prefix[PCI_ADDR_LEN];
	size_t prefix_len;
	char *dot;
	DIR *dir;
	struct dirent *de;
	int count = 0;
	int rc;

	if (!pci_addr || !*pci_addr || !addrs || max_addrs <= 0)
		return 0;

	rc = snprintf(prefix, sizeof(prefix), "%s", pci_addr);
	if (rc < 0 || (size_t)rc >= sizeof(prefix))
		return 0;

	dot = strrchr(prefix, '.');
	if (!dot)
		return 0;

	*dot = '\0';			/* prefix = "DDDD:BB:DD" */
	prefix_len = strlen(prefix);

	/* Trigger function is always entry 0 */
	snprintf(addrs[count++], PCI_ADDR_LEN, "%s", pci_addr);

	dir = opendir(PCI_SYS_PATH);
	if (!dir)
		return count;

	while ((de = readdir(dir)) != NULL && count < max_addrs) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		/* Must begin with "DDDD:BB:DD" */
		if (strncmp(de->d_name, prefix, prefix_len) != 0)
			continue;

		/* The very next character must be '.' */
		if (de->d_name[prefix_len] != '.')
			continue;

		/* Do not re-add the trigger function */
		if (strcmp(de->d_name, pci_addr) == 0)
			continue;

		/* Only include FC PCI functions */
		if (!is_fc_pci_function(de->d_name))
			continue;

		snprintf(addrs[count++], PCI_ADDR_LEN, "%s", de->d_name);
	}
	closedir(dir);
	return count;
}

/* ------------------------------------------------------------------ */
/* Per-port collection                                                  */
/* ------------------------------------------------------------------ */

/*
 * collect_port - Fill an fc_port struct for one PCI function.
 *
 * Sets pci_function_present and fc_host_present flags.
 * If the fc_host is not found, all string fields are set to "unknown".
 * All sysfs reads are read-only.
 */
static void collect_port(const char *pci_addr, struct fc_port *port)
{
	char path[PATH_MAX];
	int rc;

	if (!pci_addr || !port)
		return;

	memset(port, 0, sizeof(*port));
	snprintf(port->pci_addr, sizeof(port->pci_addr), "%s", pci_addr);
	port->pci_function_present = pci_function_exists(pci_addr);

	if (fc_host_for_pci(pci_addr, port->fc_host,
			    sizeof(port->fc_host)) != 0) {
		snprintf(port->fc_host,          sizeof(port->fc_host),          "unknown");
		snprintf(port->wwpn,             sizeof(port->wwpn),             "unknown");
		snprintf(port->wwnn,             sizeof(port->wwnn),             "unknown");
		snprintf(port->port_state,       sizeof(port->port_state),       "unknown");
		snprintf(port->port_type,        sizeof(port->port_type),        "unknown");
		snprintf(port->port_id,          sizeof(port->port_id),          "unknown");
		snprintf(port->speed,            sizeof(port->speed),            "unknown");
		snprintf(port->supported_speeds, sizeof(port->supported_speeds), "unknown");
		snprintf(port->fabric_name,      sizeof(port->fabric_name),      "unknown");
		snprintf(port->stats.error_frames,  sizeof(port->stats.error_frames),  "unknown");
		snprintf(port->stats.dumped_frames, sizeof(port->stats.dumped_frames), "unknown");
		port->fc_host_present = false;
		return;
	}
	port->fc_host_present = true;

/* Helper: build FC sysfs path and read it safely into port->field */
#define RD(attr, field) \
	do { \
		rc = snprintf(path, sizeof(path), "%s/%s/%s", \
			      FC_SYS_PATH, port->fc_host, (attr)); \
		if (rc > 0 && (size_t)rc < sizeof(path)) \
			safe_read(path, port->field, sizeof(port->field)); \
		else \
			snprintf(port->field, sizeof(port->field), "unknown"); \
	} while (0)

	RD("port_name",        wwpn);
	RD("node_name",        wwnn);
	RD("port_state",       port_state);
	RD("port_type",        port_type);
	RD("port_id",          port_id);
	RD("speed",            speed);
	RD("supported_speeds", supported_speeds);
	RD("fabric_name",      fabric_name);

#undef RD

/* Helper: build FC statistics sysfs path and read into port->stats.field */
#define RDST(attr, field) \
	do { \
		rc = snprintf(path, sizeof(path), "%s/%s/statistics/%s", \
			      FC_SYS_PATH, port->fc_host, (attr)); \
		if (rc > 0 && (size_t)rc < sizeof(path)) \
			safe_read(path, port->stats.field, \
				  sizeof(port->stats.field)); \
		else \
			snprintf(port->stats.field, \
				 sizeof(port->stats.field), "unknown"); \
	} while (0)

	RDST("error_frames",  error_frames);
	RDST("dumped_frames", dumped_frames);

#undef RDST
}

/* ------------------------------------------------------------------ */
/* VPD collection                                                       */
/* ------------------------------------------------------------------ */

/*
 * collect_vpd - Attempt to read IBM VPD from PCIe VPD sysfs.
 *
 * Parses the standard PCIe VPD resource format (large/small resource
 * data types).  Extracts known keywords into vpd struct fields.
 * Fields not present in VPD remain as empty strings.
 *
 * Keyword mappings confirmed against Emulex LPe38102-P VPD
 * (hexdump of /sys/bus/pci/devices/0155:90:00.0/vpd):
 *   PN → part_number       (e.g. "03HD016")
 *   SN → serial_number     (e.g. "Y050HY1CR00A")
 *   EC → ec_level          (e.g. "P45971")
 *   FC → feature_code      (e.g. "EN1N")
 *   VH → ccin              (IBM vendor-specific, e.g. "2CFD")
 *   V0 → firmware_level    (IBM vendor-specific, e.g. "FP15162532")
 *   V5 → adapter model string (e.g. "LPe38102-P") — not yet mapped
 *
 * Returns 0 on success, -1 if VPD file cannot be opened or is unreadable.
 * Sets vpd_status in the caller to CSTATUS_* accordingly.
 */
static int collect_vpd(const char *pci_addr, struct fc_adapter_vpd *vpd,
		       char *vpd_status, size_t status_sz)
{
	char vpd_path[PATH_MAX];
	int fd, rc;
	uint8_t tag;
	uint8_t lenbuf[2];
	uint16_t section_len, parsed;
	uint8_t kw_len;
	uint8_t data[256];
	char kw[3];

	if (!pci_addr || !vpd || !vpd_status)
		return -1;

	memset(vpd, 0, sizeof(*vpd));

	rc = snprintf(vpd_path, sizeof(vpd_path),
		      "%s/%s/vpd", PCI_SYS_PATH, pci_addr);
	if (rc < 0 || (size_t)rc >= sizeof(vpd_path)) {
		snprintf(vpd_status, status_sz, CSTATUS_NOT_ATTEMPTED);
		return -1;
	}

	fd = open(vpd_path, O_RDONLY);
	if (fd < 0) {
		snprintf(vpd_status, status_sz, CSTATUS_UNAVAILABLE);
		return -1;
	}

	do {
		if (read(fd, &tag, 1) < 1)
			goto read_err;

		if (tag & 0x80) {
			/* Large resource: 2-byte length follows */
			if (read(fd, lenbuf, 2) < 2)
				goto read_err;
			section_len = (uint16_t)(lenbuf[0] +
						 (lenbuf[1] << 8));
		} else {
			section_len = tag & 0x07;
		}

		switch (tag) {
		case 0x82: /* Identifier String — read and discard */
			{
				uint8_t discard[256];
				uint16_t remaining = section_len;

				while (remaining > 0) {
					uint16_t chunk = remaining < sizeof(discard)
						? remaining
						: (uint16_t)sizeof(discard);
					ssize_t got = read(fd, discard, chunk);

					if (got <= 0)
						goto read_err;
					remaining -= (uint16_t)got;
				}
			}
			break;

		case 0x90: /* VPD-R */
		case 0x91: /* VPD-W */
			parsed = 0;
			while (parsed < section_len) {
				if (read(fd, kw, 2) < 2)
					goto read_err;
				if (read(fd, &kw_len, 1) < 1)
					goto read_err;
				if (kw_len > sizeof(data) - 1)
					kw_len = sizeof(data) - 1;
				if (read(fd, data, kw_len) < kw_len)
					goto read_err;
				data[kw_len] = '\0';
				kw[2] = '\0';

				if      (!strcmp(kw, "PN"))
					snprintf(vpd->part_number,
						 sizeof(vpd->part_number),
						 "%s", (char *)data);
				else if (!strcmp(kw, "FN"))
					snprintf(vpd->fru_part_number,
						 sizeof(vpd->fru_part_number),
						 "%s", (char *)data);
				else if (!strcmp(kw, "SN"))
					snprintf(vpd->serial_number,
						 sizeof(vpd->serial_number),
						 "%s", (char *)data);
				else if (!strcmp(kw, "VH"))
					snprintf(vpd->ccin,
						 sizeof(vpd->ccin),
						 "%s", (char *)data);
				else if (!strcmp(kw, "FC"))
					snprintf(vpd->feature_code,
						 sizeof(vpd->feature_code),
						 "%s", (char *)data);
				else if (!strcmp(kw, "V0"))
					snprintf(vpd->firmware_level,
						 sizeof(vpd->firmware_level),
						 "%s", (char *)data);
				else if (!strcmp(kw, "EC"))
					snprintf(vpd->ec_level,
						 sizeof(vpd->ec_level),
						 "%s", (char *)data);

				parsed += (uint16_t)(kw_len + 3);
			}
			break;

		case 0x78: /* End Tag */
			break;

		default:
			fprintf(stderr,
				"diag_fc: unknown VPD tag 0x%02x for %s — "
				"stopping VPD parse\n", tag, pci_addr);
			close(fd);
			snprintf(vpd_status, status_sz, CSTATUS_PARSE_ERROR);
			return -1;
		}
	} while (tag != 0x78);

	close(fd);
	snprintf(vpd_status, status_sz, CSTATUS_OK);
	return 0;

read_err:
	fprintf(stderr,
		"diag_fc: VPD read error for %s — partial VPD\n", pci_addr);
	close(fd);
	snprintf(vpd_status, status_sz, CSTATUS_PARSE_ERROR);
	return -1;
}

/* ------------------------------------------------------------------ */
/* Location code                                                        */
/* ------------------------------------------------------------------ */

/*
 * collect_location - Read IBM location code via the device tree.
 *
 * Follows PCI sysfs devspec to find the device-tree node, then reads
 * ibm,loc-code.  Sets location_status to a CSTATUS_* value.
 *
 * Returns 0 on success, -1 on error.
 */
static int collect_location(const char *pci_addr, char *location,
			    size_t locsz, char *loc_status, size_t status_sz)
{
	char devspec_path[PATH_MAX];
	char dt_loc_path[PATH_MAX];
	char devspec[2 * NAME_MAX];
	char *nl;
	int fd, rc;
	ssize_t n;

	if (!pci_addr || !location || locsz == 0 || !loc_status) {
		if (loc_status)
			snprintf(loc_status, status_sz, CSTATUS_NOT_ATTEMPTED);
		return -1;
	}

	rc = snprintf(devspec_path, sizeof(devspec_path),
		      "%s/%s/devspec", PCI_SYS_PATH, pci_addr);
	if (rc < 0 || (size_t)rc >= sizeof(devspec_path)) {
		snprintf(loc_status, status_sz, CSTATUS_NOT_ATTEMPTED);
		return -1;
	}

	fd = open(devspec_path, O_RDONLY);
	if (fd < 0) {
		snprintf(loc_status, status_sz, CSTATUS_UNAVAILABLE);
		return -1;
	}

	memset(devspec, 0, sizeof(devspec));
	do {
		n = read(fd, devspec, sizeof(devspec) - 1);
	} while (n < 0 && errno == EINTR);
	close(fd);

	if (n <= 0) {
		snprintf(loc_status, status_sz, CSTATUS_UNAVAILABLE);
		return -1;
	}

	/* Strip trailing newline (kernel commit 14c19b2a40b6) */
	nl = strchr(devspec, '\n');
	if (nl)
		*nl = '\0';

	/* devspec must begin with '/' to form a valid device-tree path */
	if (devspec[0] != '/') {
		fprintf(stderr,
			"diag_fc: devspec for %s does not begin with '/': %s\n",
			pci_addr, devspec);
		snprintf(loc_status, status_sz, CSTATUS_PARSE_ERROR);
		return -1;
	}

	rc = snprintf(dt_loc_path, sizeof(dt_loc_path),
		      "%s%s/ibm,loc-code", DEVICE_TREE_PATH, devspec);
	if (rc < 0 || (size_t)rc >= sizeof(dt_loc_path)) {
		snprintf(loc_status, status_sz, CSTATUS_NOT_ATTEMPTED);
		return -1;
	}

	fd = open(dt_loc_path, O_RDONLY);
	if (fd < 0) {
		snprintf(loc_status, status_sz, CSTATUS_UNAVAILABLE);
		return -1;
	}

	memset(location, 0, locsz);
	do {
		n = read(fd, location, locsz - 1);
	} while (n < 0 && errno == EINTR);
	close(fd);

	if (n <= 0) {
		snprintf(loc_status, status_sz, CSTATUS_UNAVAILABLE);
		return -1;
	}

	trim_trail_space(location);
	snprintf(loc_status, status_sz, CSTATUS_OK);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Adapter identity                                                     */
/* ------------------------------------------------------------------ */

/*
 * collect_adapter_identity - Read PCI IDs, driver name/version, symbolic_name.
 *
 * PCI vendor/device IDs are stored raw (e.g. "0x10df") in separate fields
 * rather than combined, to preserve the original sysfs values exactly.
 */
static void collect_adapter_identity(const char *pci_addr,
				     struct fc_incident *inc)
{
	char path[PATH_MAX];
	char drv_link[PATH_MAX];
	char drv_resolved[PATH_MAX];
	char sym_path[PATH_MAX];
	char host_name[FC_HOST_LEN];
	const char *slash;
	int rc;

	if (!pci_addr || !inc)
		return;

#define RDID(attr, field) \
	do { \
		rc = snprintf(path, sizeof(path), "%s/%s/%s", \
			      PCI_SYS_PATH, pci_addr, (attr)); \
		if (rc > 0 && (size_t)rc < sizeof(path)) \
			safe_read(path, inc->field, sizeof(inc->field)); \
		else \
			snprintf(inc->field, sizeof(inc->field), "unknown"); \
	} while (0)

	RDID("vendor",           pci_vendor_id);
	RDID("device",           pci_device_id);
	RDID("subsystem_vendor", pci_subsystem_vendor_id);
	RDID("subsystem_device", pci_subsystem_device_id);

#undef RDID

	/* Driver name — last component of the 'driver' symlink */
	rc = snprintf(drv_link, sizeof(drv_link),
		      "%s/%s/driver", PCI_SYS_PATH, pci_addr);
	if (rc > 0 && (size_t)rc < sizeof(drv_link) &&
	    realpath(drv_link, drv_resolved) != NULL) {
		slash = strrchr(drv_resolved, '/');
		snprintf(inc->driver_name, sizeof(inc->driver_name),
			 "%s", slash ? slash + 1 : drv_resolved);
	} else {
		snprintf(inc->driver_name, sizeof(inc->driver_name), "unknown");
	}

	/* Driver version from /sys/module/<driver>/version */
	rc = snprintf(path, sizeof(path),
		      "/sys/module/%s/version", inc->driver_name);
	if (rc > 0 && (size_t)rc < sizeof(path))
		safe_read(path, inc->driver_version, sizeof(inc->driver_version));
	else
		snprintf(inc->driver_version, sizeof(inc->driver_version),
			 "unknown");

	/*
	 * symbolic_name — stored verbatim.
	 * On lpfc contains: "Emulex LPe38102-P FV14.2.624.10 DV14.4.0.5 ..."
	 * adapter_model is left "unknown" until a stable derivation rule
	 * is confirmed.
	 */
	if (fc_host_for_pci(pci_addr, host_name, sizeof(host_name)) == 0) {
		rc = snprintf(sym_path, sizeof(sym_path),
			      "%s/%s/symbolic_name", FC_SYS_PATH, host_name);
		if (rc > 0 && (size_t)rc < sizeof(sym_path))
			safe_read(sym_path, inc->adapter_symbolic_name,
				  sizeof(inc->adapter_symbolic_name));
	}

	if (inc->adapter_symbolic_name[0] == '\0')
		snprintf(inc->adapter_symbolic_name,
			 sizeof(inc->adapter_symbolic_name), "unknown");

	snprintf(inc->adapter_model, sizeof(inc->adapter_model), "unknown");
}

/* ------------------------------------------------------------------ */
/* Machine context                                                      */
/* ------------------------------------------------------------------ */

static void collect_machine_context(struct fc_incident *inc)
{
	struct utsname u;

	if (!inc)
		return;

	if (gethostname(inc->hostname, sizeof(inc->hostname)) != 0)
		snprintf(inc->hostname, sizeof(inc->hostname), "unknown");
	inc->hostname[sizeof(inc->hostname) - 1] = '\0';

	if (uname(&u) == 0) {
		snprintf(inc->os_name,        sizeof(inc->os_name),
			 "%s", u.sysname);
		snprintf(inc->kernel_release, sizeof(inc->kernel_release),
			 "%s", u.release);
		snprintf(inc->kernel_build,   sizeof(inc->kernel_build),
			 "%s", u.version);
		snprintf(inc->architecture,   sizeof(inc->architecture),
			 "%s", u.machine);
	} else {
		snprintf(inc->os_name,        sizeof(inc->os_name),        "unknown");
		snprintf(inc->kernel_release, sizeof(inc->kernel_release), "unknown");
		snprintf(inc->kernel_build,   sizeof(inc->kernel_build),   "unknown");
		snprintf(inc->architecture,   sizeof(inc->architecture),   "unknown");
	}
}

/* ------------------------------------------------------------------ */
/* Public: collect_fc_incident                                          */
/* ------------------------------------------------------------------ */

int collect_fc_incident(const char *pci_addr,
			const char *trigger_type,
			const char *trigger_source,
			const char *trigger_reason,
			const char *trigger_timestamp,
			const struct fc_config *cfg,
			struct fc_incident *inc)
{
	char siblings[MAX_FC_PORTS][PCI_ADDR_LEN];
	int nsiblings, i;
	time_t now;
	struct tm tm_utc;
	char timebuf[FIELD_LEN];

	(void)cfg;	/* consumed by collect_mpath_devices() in a later task */

	if (!pci_addr || !*pci_addr || !inc)
		return -1;

	memset(inc, 0, sizeof(*inc));

	/* Initialise status fields to "not_attempted" */
	snprintf(inc->vpd_status,      sizeof(inc->vpd_status),
		 CSTATUS_NOT_ATTEMPTED);
	snprintf(inc->location_status, sizeof(inc->location_status),
		 CSTATUS_NOT_ATTEMPTED);

	/* Trigger metadata */
	snprintf(inc->trigger_type,     sizeof(inc->trigger_type),
		 "%s", trigger_type   ? trigger_type   : "UNKNOWN");
	snprintf(inc->trigger_source,   sizeof(inc->trigger_source),
		 "%s", trigger_source ? trigger_source : "unknown");
	snprintf(inc->trigger_pci_addr, sizeof(inc->trigger_pci_addr),
		 "%s", pci_addr);
	snprintf(inc->trigger_reason,   sizeof(inc->trigger_reason),
		 "%s", trigger_reason ? trigger_reason : "");
	/* trigger_reference left empty — filled by caller if available */

	/* Collection timestamp (UTC, thread-safe) */
	time(&now);
	if (gmtime_r(&now, &tm_utc) != NULL)
		strftime(timebuf, sizeof(timebuf),
			 "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
	else
		snprintf(timebuf, sizeof(timebuf), "unknown");

	snprintf(inc->collect_timestamp, sizeof(inc->collect_timestamp),
		 "%s", timebuf);

	/* Trigger timestamp: caller-supplied or fall back to collection time */
	if (trigger_timestamp && trigger_timestamp[0] != '\0')
		snprintf(inc->trigger_timestamp, sizeof(inc->trigger_timestamp),
			 "%s", trigger_timestamp);
	else
		snprintf(inc->trigger_timestamp, sizeof(inc->trigger_timestamp),
			 "%s", timebuf);

	/* Warn if adapter is already gone — collection continues */
	if (!pci_function_exists(pci_addr))
		fprintf(stderr,
			"diag_fc: warn: %s not in sysfs — "
			"adapter may be gone; collecting partial data\n",
			pci_addr);

	/* Machine context */
	collect_machine_context(inc);

	/* Adapter identity */
	collect_adapter_identity(pci_addr, inc);

	/* Location code */
	collect_location(pci_addr,
			 inc->location_code, sizeof(inc->location_code),
			 inc->location_status, sizeof(inc->location_status));
	if (strcmp(inc->location_status, CSTATUS_OK) != 0)
		snprintf(inc->location_code, sizeof(inc->location_code),
			 "unknown");

	/* VPD */
	collect_vpd(pci_addr, &inc->vpd,
		    inc->vpd_status, sizeof(inc->vpd_status));

	/*
	 * Enumerate all FC PCI functions on this physical slot and
	 * collect per-port data for each.
	 */
	nsiblings = sibling_pci_functions(pci_addr, siblings, MAX_FC_PORTS);
	if (nsiblings <= 0) {
		collect_port(pci_addr, &inc->ports[0]);
		inc->num_ports = 1;
	} else {
		inc->num_ports = 0;
		for (i = 0; i < nsiblings && inc->num_ports < MAX_FC_PORTS;
		     i++) {
			collect_port(siblings[i],
				     &inc->ports[inc->num_ports]);
			inc->num_ports++;
		}
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* JSON report writer                                                   */
/* ------------------------------------------------------------------ */

/*
 * json_str - Write a JSON string value with full RFC 8259 escaping.
 * Control bytes < 0x20 are written as \uXXXX.
 * Handles a NULL pointer gracefully by writing an empty string.
 */
static void json_str(FILE *f, const char *s)
{
	unsigned char c;

	if (!s)
		s = "";

	fputc('"', f);
	for (; *s; s++) {
		c = (unsigned char)*s;
		switch (c) {
		case '"':  fputs("\\\"", f); break;
		case '\\': fputs("\\\\", f); break;
		case '\n': fputs("\\n",  f); break;
		case '\r': fputs("\\r",  f); break;
		case '\t': fputs("\\t",  f); break;
		case '\b': fputs("\\b",  f); break;
		case '\f': fputs("\\f",  f); break;
		default:
			if (c < 0x20)
				fprintf(f, "\\u%04x", (unsigned int)c);
			else
				fputc(c, f);
			break;
		}
	}
	fputc('"', f);
}

int write_fc_report(const struct fc_incident *inc, const char *path,
		    const struct fc_config *cfg)
{
	FILE *f;
	int i;
	bool to_stdout;

	(void)cfg;	/* consumed by size-advisory check in a later task */

	if (!inc)
		return -1;

	to_stdout = (!path || !strcmp(path, "-"));

	if (to_stdout) {
		f = stdout;
	} else {
		f = fopen(path, "w");
		if (!f) {
			fprintf(stderr,
				"diag_fc: cannot open report %s: %s\n",
				path, strerror(errno));
			return -1;
		}
	}

	fprintf(f, "{\n");
	fprintf(f, "  \"schema_version\": \"1.0\",\n");

	/* --- Trigger --- */
	fprintf(f, "  \"trigger\": {\n");
	fprintf(f, "    \"type\": ");       json_str(f, inc->trigger_type);      fprintf(f, ",\n");
	fprintf(f, "    \"source\": ");     json_str(f, inc->trigger_source);    fprintf(f, ",\n");
	fprintf(f, "    \"pci_function\": "); json_str(f, inc->trigger_pci_addr); fprintf(f, ",\n");
	fprintf(f, "    \"reason\": ");     json_str(f, inc->trigger_reason);    fprintf(f, ",\n");
	fprintf(f, "    \"reference\": ");  json_str(f, inc->trigger_reference); fprintf(f, ",\n");
	fprintf(f, "    \"timestamp\": ");  json_str(f, inc->trigger_timestamp); fprintf(f, "\n");
	fprintf(f, "  },\n");

	/* --- Machine --- */
	fprintf(f, "  \"machine\": {\n");
	fprintf(f, "    \"hostname\": ");       json_str(f, inc->hostname);       fprintf(f, ",\n");
	fprintf(f, "    \"os\": ");             json_str(f, inc->os_name);        fprintf(f, ",\n");
	fprintf(f, "    \"kernel_release\": "); json_str(f, inc->kernel_release); fprintf(f, ",\n");
	fprintf(f, "    \"kernel_build\": ");   json_str(f, inc->kernel_build);   fprintf(f, ",\n");
	fprintf(f, "    \"architecture\": ");   json_str(f, inc->architecture);   fprintf(f, "\n");
	fprintf(f, "  },\n");

	/* --- Adapter --- */
	fprintf(f, "  \"adapter\": {\n");
	fprintf(f, "    \"symbolic_name\": ");       json_str(f, inc->adapter_symbolic_name);  fprintf(f, ",\n");
	fprintf(f, "    \"model\": ");               json_str(f, inc->adapter_model);           fprintf(f, ",\n");
	fprintf(f, "    \"pci_vendor_id\": ");        json_str(f, inc->pci_vendor_id);          fprintf(f, ",\n");
	fprintf(f, "    \"pci_device_id\": ");        json_str(f, inc->pci_device_id);          fprintf(f, ",\n");
	fprintf(f, "    \"pci_subsystem_vendor\": "); json_str(f, inc->pci_subsystem_vendor_id); fprintf(f, ",\n");
	fprintf(f, "    \"pci_subsystem_device\": "); json_str(f, inc->pci_subsystem_device_id); fprintf(f, ",\n");
	fprintf(f, "    \"driver\": ");              json_str(f, inc->driver_name);             fprintf(f, ",\n");
	fprintf(f, "    \"driver_version\": ");      json_str(f, inc->driver_version);          fprintf(f, ",\n");
	fprintf(f, "    \"location_code\": ");        json_str(f, inc->location_code);          fprintf(f, ",\n");
	fprintf(f, "    \"location_status\": ");      json_str(f, inc->location_status);        fprintf(f, ",\n");
	fprintf(f, "    \"vpd_status\": ");           json_str(f, inc->vpd_status);             fprintf(f, ",\n");
	fprintf(f, "    \"vpd\": {\n");
	fprintf(f, "      \"part_number\": ");      json_str(f, inc->vpd.part_number);     fprintf(f, ",\n");
	fprintf(f, "      \"fru_part_number\": ");  json_str(f, inc->vpd.fru_part_number); fprintf(f, ",\n");
	fprintf(f, "      \"serial_number\": ");    json_str(f, inc->vpd.serial_number);   fprintf(f, ",\n");
	fprintf(f, "      \"ccin\": ");             json_str(f, inc->vpd.ccin);            fprintf(f, ",\n");
	fprintf(f, "      \"feature_code\": ");     json_str(f, inc->vpd.feature_code);    fprintf(f, ",\n");
	fprintf(f, "      \"firmware_level\": ");   json_str(f, inc->vpd.firmware_level);  fprintf(f, ",\n");
	fprintf(f, "      \"ec_level\": ");         json_str(f, inc->vpd.ec_level);        fprintf(f, "\n");
	fprintf(f, "    }\n");
	fprintf(f, "  },\n");

	/* --- Ports --- */
	fprintf(f, "  \"ports\": [\n");
	for (i = 0; i < inc->num_ports; i++) {
		const struct fc_port *p = &inc->ports[i];

		fprintf(f, "    {\n");
		fprintf(f, "      \"pci_function\": ");        json_str(f, p->pci_addr);         fprintf(f, ",\n");
		fprintf(f, "      \"fc_host\": ");             json_str(f, p->fc_host);          fprintf(f, ",\n");
		fprintf(f, "      \"pci_function_present\": %s,\n",
			p->pci_function_present ? "true" : "false");
		fprintf(f, "      \"fc_host_present\": %s,\n",
			p->fc_host_present ? "true" : "false");
		fprintf(f, "      \"wwpn\": ");                json_str(f, p->wwpn);             fprintf(f, ",\n");
		fprintf(f, "      \"wwnn\": ");                json_str(f, p->wwnn);             fprintf(f, ",\n");
		fprintf(f, "      \"port_state\": ");          json_str(f, p->port_state);       fprintf(f, ",\n");
		fprintf(f, "      \"port_type\": ");           json_str(f, p->port_type);        fprintf(f, ",\n");
		fprintf(f, "      \"port_id\": ");             json_str(f, p->port_id);          fprintf(f, ",\n");
		fprintf(f, "      \"speed\": ");               json_str(f, p->speed);            fprintf(f, ",\n");
		fprintf(f, "      \"supported_speeds\": ");    json_str(f, p->supported_speeds); fprintf(f, ",\n");
		fprintf(f, "      \"fabric_name\": ");         json_str(f, p->fabric_name);      fprintf(f, "\n");
		fprintf(f, "    }%s\n", (i < inc->num_ports - 1) ? "," : "");
	}
	fprintf(f, "  ],\n");

	/* --- Collection metadata --- */
	fprintf(f, "  \"collection\": {\n");
	fprintf(f, "    \"timestamp\": "); json_str(f, inc->collect_timestamp); fprintf(f, "\n");
	fprintf(f, "  }\n");

	fprintf(f, "}\n");

	if (ferror(f)) {
		fprintf(stderr, "diag_fc: write error on report\n");
		if (!to_stdout)
			fclose(f);
		return -1;
	}

	if (!to_stdout)
		fclose(f);

	return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

static void print_usage(const char *cmd)
{
	fprintf(stdout,
		"Usage: %s -p <pci_addr> [OPTIONS]\n"
		"\n"
		"FC adapter failure evidence collector for PowerVM LPARs.\n"
		"Reads adapter identity, port state, and machine context into\n"
		"a structured JSON report.  Strictly read-only.\n"
		"\n"
		"Required:\n"
		"  -p <pci_addr>    PCI domain:bus:dev.fn, e.g. 0155:90:00.0\n"
		"\n"
		"Options:\n"
		"  -t <type>        Trigger type (default: EEH_PERMANENT_FAILURE)\n"
		"  -s <source>      Trigger source (default: manual)\n"
		"  -r <reason>      Human-readable failure reason\n"
		"  -T <timestamp>   Trigger time in ISO 8601 format\n"
		"  -o <path>        Output JSON file; - for stdout (default)\n"
		"  -h               Print this help\n"
		"\n"
		"Exit codes:\n"
		"  0  Report written successfully\n"
		"  1  Invalid command-line usage\n"
		"  2  Fatal collection error\n"
		"  3  Report-writing error\n",
		cmd);
}

int main(int argc, char *argv[])
{
	int opt;
	const char *pci_addr     = NULL;
	const char *trigger_type = "EEH_PERMANENT_FAILURE";
	const char *source       = "manual";
	const char *reason       = "";
	const char *trig_ts      = NULL;
	const char *outfile      = "-";
	struct fc_incident inc;
	struct fc_config cfg;

	read_fc_config(&cfg);

	static struct option long_options[] = {
		{"pci",       required_argument, NULL, 'p'},
		{"type",      required_argument, NULL, 't'},
		{"source",    required_argument, NULL, 's'},
		{"reason",    required_argument, NULL, 'r'},
		{"timestamp", required_argument, NULL, 'T'},
		{"output",    required_argument, NULL, 'o'},
		{"help",      no_argument,       NULL, 'h'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "p:t:s:r:T:o:h",
				  long_options, NULL)) != -1) {
		switch (opt) {
		case 'p': pci_addr     = optarg; break;
		case 't': trigger_type = optarg; break;
		case 's': source       = optarg; break;
		case 'r': reason       = optarg; break;
		case 'T': trig_ts      = optarg; break;
		case 'o': outfile      = optarg; break;
		case 'h': print_usage(argv[0]); return 0;
		default:  print_usage(argv[0]); return 1;
		}
	}

	if (!pci_addr) {
		fprintf(stderr, "Error: -p <pci_addr> is required\n\n");
		print_usage(argv[0]);
		return 1;
	}

	if (get_platform() != PLATFORM_PSERIES_LPAR) {
		fprintf(stderr,
			"%s is only supported in PowerVM LPARs\n", argv[0]);
		return 1;
	}

	if (collect_fc_incident(pci_addr, trigger_type, source,
				reason, trig_ts, &cfg, &inc) != 0) {
		fprintf(stderr, "diag_fc: fatal collection error\n");
		return 2;
	}

	if (write_fc_report(&inc, outfile, &cfg) != 0) {
		fprintf(stderr, "diag_fc: report writing failed\n");
		return 3;
	}

	return 0;
}
