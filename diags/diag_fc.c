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

#include <ctype.h>
#include <dirent.h>
#include <getopt.h>
#include <regex.h>
#include <servicelog-1/servicelog.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/utsname.h>
#include "diag_fc.h"
#include "platform.h"
#include "utils.h"

#define ITEM_DATA_LENGTH	255

struct section {
	struct header_section {
		uint8_t tag;
		uint8_t reserved;
		uint16_t length;
	} header;
	unsigned char *data;
} __attribute__((packed));

struct item {
	struct header_item {
		char keyword[2];
		uint8_t reserved;
		uint8_t length;
	} header;
	char data[ITEM_DATA_LENGTH];
} __attribute__((packed));

/* Forward declarations */
static int diagnose_fc(char *host_name, struct fc_notify *notify);
static void print_usage(char *command);
static int read_sysfs_string(const char *path, char *buf, size_t size);
static int read_sysfs_uint64(const char *path, uint64_t *value);

/**
 * read_sysfs_string - Read a string value from sysfs
 * @path: Full path to sysfs file
 * @buf: Buffer to store the string
 * @size: Size of buffer
 *
 * Returns 0 on success, -1 on error
 */
static int read_sysfs_string(const char *path, char *buf, size_t size)
{
	FILE *fp;
	char *newline;

	if (!path || !buf || size == 0) {
		fprintf(stderr, "Invalid parameters to %s\n", __func__);
		return -1;
	}

	fp = fopen(path, "r");
	if (!fp) {
		fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
		return -1;
	}

	if (!fgets(buf, size, fp)) {
		fclose(fp);
		return -1;
	}

	fclose(fp);

	/* Remove trailing newline */
	newline = strchr(buf, '\n');
	if (newline)
		*newline = '\0';

	return 0;
}

/**
 * read_sysfs_uint64 - Read a uint64_t value from sysfs
 * @path: Full path to sysfs file
 * @value: Pointer to store the value
 *
 * Returns 0 on success, -1 on error
 */
static int read_sysfs_uint64(const char *path, uint64_t *value)
{
	FILE *fp;
	int rc;

	if (!path || !value) {
		fprintf(stderr, "Invalid parameters to %s\n", __func__);
		return -1;
	}

	fp = fopen(path, "r");
	if (!fp) {
		fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
		return -1;
	}

	rc = fscanf(fp, "%lu", value);
	fclose(fp);

	if (rc != 1) {
		fprintf(stderr, "Failed to read value from %s\n", path);
		return -1;
	}

	return 0;
}

/**
 * read_fc_statistics - Read FC statistics from sysfs
 * @host_name: FC host name (e.g., "host0")
 * @stats: Pointer to fc_statistics structure to fill
 *
 * Returns 0 on success, -1 on error
 */
int read_fc_statistics(const char *host_name, struct fc_statistics *stats)
{
	char path[PATH_MAX];
	int errors = 0;

	if (!host_name || !stats) {
		fprintf(stderr, "Invalid parameters to %s\n", __func__);
		return -1;
	}

	memset(stats, 0, sizeof(*stats));

	/* Read each statistic from sysfs */
	snprintf(path, sizeof(path), "%s/%s/statistics/tx_frames", FC_HOST_PATH, host_name);
	if (read_sysfs_uint64(path, &stats->tx_frames) < 0)
		errors++;

	snprintf(path, sizeof(path), "%s/%s/statistics/rx_frames", FC_HOST_PATH, host_name);
	if (read_sysfs_uint64(path, &stats->rx_frames) < 0)
		errors++;

	snprintf(path, sizeof(path), "%s/%s/statistics/tx_words", FC_HOST_PATH, host_name);
	if (read_sysfs_uint64(path, &stats->tx_words) < 0)
		errors++;

	snprintf(path, sizeof(path), "%s/%s/statistics/rx_words", FC_HOST_PATH, host_name);
	if (read_sysfs_uint64(path, &stats->rx_words) < 0)
		errors++;

	snprintf(path, sizeof(path), "%s/%s/statistics/lip_count", FC_HOST_PATH, host_name);
	if (read_sysfs_uint64(path, &stats->lip_count) < 0)
		errors++;

	snprintf(path, sizeof(path), "%s/%s/statistics/nos_count", FC_HOST_PATH, host_name);
	if (read_sysfs_uint64(path, &stats->nos_count) < 0)
		errors++;

	snprintf(path, sizeof(path), "%s/%s/statistics/error_frames", FC_HOST_PATH, host_name);
	if (read_sysfs_uint64(path, &stats->error_frames) < 0)
		errors++;

	snprintf(path, sizeof(path), "%s/%s/statistics/dumped_frames", FC_HOST_PATH, host_name);
	if (read_sysfs_uint64(path, &stats->dumped_frames) < 0)
		errors++;

	snprintf(path, sizeof(path), "%s/%s/statistics/link_failure_count", FC_HOST_PATH, host_name);
	if (read_sysfs_uint64(path, &stats->link_failure_count) < 0)
		errors++;

	snprintf(path, sizeof(path), "%s/%s/statistics/loss_of_sync_count", FC_HOST_PATH, host_name);
	if (read_sysfs_uint64(path, &stats->loss_of_sync_count) < 0)
		errors++;

	snprintf(path, sizeof(path), "%s/%s/statistics/loss_of_signal_count", FC_HOST_PATH, host_name);
	if (read_sysfs_uint64(path, &stats->loss_of_signal_count) < 0)
		errors++;

	snprintf(path, sizeof(path), "%s/%s/statistics/invalid_tx_word_count", FC_HOST_PATH, host_name);
	if (read_sysfs_uint64(path, &stats->invalid_tx_word_count) < 0)
		errors++;

	snprintf(path, sizeof(path), "%s/%s/statistics/invalid_crc_count", FC_HOST_PATH, host_name);
	if (read_sysfs_uint64(path, &stats->invalid_crc_count) < 0)
		errors++;

	/* Some statistics may not be available on all HBAs */
	snprintf(path, sizeof(path), "%s/%s/statistics/fcp_input_requests", FC_HOST_PATH, host_name);
	read_sysfs_uint64(path, &stats->fcp_input_requests);

	snprintf(path, sizeof(path), "%s/%s/statistics/fcp_output_requests", FC_HOST_PATH, host_name);
	read_sysfs_uint64(path, &stats->fcp_output_requests);

	snprintf(path, sizeof(path), "%s/%s/statistics/fcp_control_requests", FC_HOST_PATH, host_name);
	read_sysfs_uint64(path, &stats->fcp_control_requests);

	snprintf(path, sizeof(path), "%s/%s/statistics/fcp_input_megabytes", FC_HOST_PATH, host_name);
	read_sysfs_uint64(path, &stats->fcp_input_megabytes);

	snprintf(path, sizeof(path), "%s/%s/statistics/fcp_output_megabytes", FC_HOST_PATH, host_name);
	read_sysfs_uint64(path, &stats->fcp_output_megabytes);

	/* Allow some errors for optional statistics */
	if (errors > 5) {
		fprintf(stderr, "Too many errors reading statistics for %s\n", host_name);
		return -1;
	}

	return 0;
}

/**
 * read_fc_host_info - Read FC host information from sysfs
 * @host_name: FC host name (e.g., "host0")
 * @host: Pointer to fc_host structure to fill
 *
 * Returns 0 on success, -1 on error
 */
int read_fc_host_info(const char *host_name, struct fc_host *host)
{
	char path[PATH_MAX];

	if (!host_name || !host) {
		fprintf(stderr, "Invalid parameters to %s\n", __func__);
		return -1;
	}

	memset(host, 0, sizeof(*host));
	strncpy(host->name, host_name, sizeof(host->name) - 1);

	/* Read WWPN */
	snprintf(path, sizeof(path), "%s/%s/port_name", FC_HOST_PATH, host_name);
	if (read_sysfs_string(path, host->wwpn, sizeof(host->wwpn)) < 0) {
		fprintf(stderr, "Failed to read WWPN for %s\n", host_name);
		return -1;
	}

	/* Read WWNN */
	snprintf(path, sizeof(path), "%s/%s/node_name", FC_HOST_PATH, host_name);
	if (read_sysfs_string(path, host->wwnn, sizeof(host->wwnn)) < 0) {
		fprintf(stderr, "Failed to read WWNN for %s\n", host_name);
		return -1;
	}

	/* Read port state */
	snprintf(path, sizeof(path), "%s/%s/port_state", FC_HOST_PATH, host_name);
	if (read_sysfs_string(path, host->port_state, sizeof(host->port_state)) < 0) {
		fprintf(stderr, "Failed to read port state for %s\n", host_name);
		return -1;
	}

	/* Read speed */
	snprintf(path, sizeof(path), "%s/%s/speed", FC_HOST_PATH, host_name);
	if (read_sysfs_string(path, host->speed, sizeof(host->speed)) < 0) {
		/* Speed may not be available if port is offline */
		strcpy(host->speed, "unknown");
	}

	/* Read supported speeds */
	snprintf(path, sizeof(path), "%s/%s/supported_speeds", FC_HOST_PATH, host_name);
	if (read_sysfs_string(path, host->supported_speeds, sizeof(host->supported_speeds)) < 0) {
		strcpy(host->supported_speeds, "unknown");
	}

	/* Read fabric name */
	snprintf(path, sizeof(path), "%s/%s/fabric_name", FC_HOST_PATH, host_name);
	if (read_sysfs_string(path, host->fabric_name, sizeof(host->fabric_name)) < 0) {
		strcpy(host->fabric_name, "unknown");
	}

	/* Read port type */
	snprintf(path, sizeof(path), "%s/%s/port_type", FC_HOST_PATH, host_name);
	if (read_sysfs_string(path, host->port_type, sizeof(host->port_type)) < 0) {
		strcpy(host->port_type, "unknown");
	}

	/* Read symbolic name */
	snprintf(path, sizeof(path), "%s/%s/symbolic_name", FC_HOST_PATH, host_name);
	if (read_sysfs_string(path, host->symbolic_name, sizeof(host->symbolic_name)) < 0) {
		strcpy(host->symbolic_name, "");
	}

	/* Read port ID */
	snprintf(path, sizeof(path), "%s/%s/port_id", FC_HOST_PATH, host_name);
	if (read_sysfs_uint64(path, &host->port_id) < 0) {
		host->port_id = 0;
	}

	/* Read statistics */
	if (read_fc_statistics(host_name, &host->stats) < 0) {
		fprintf(stderr, "Warning: Failed to read statistics for %s\n", host_name);
	}

	return 0;
}

/**
 * discover_fc_hosts - Discover all FC hosts in the system
 * @hosts: Pointer to array of fc_host structures (allocated by function)
 * @count: Pointer to store the number of hosts found
 *
 * Returns 0 on success, -1 on error
 * Caller is responsible for freeing the hosts array
 */
int discover_fc_hosts(struct fc_host **hosts, int *count)
{
	DIR *dir;
	struct dirent *entry;
	struct fc_host *host_array = NULL;
	int host_count = 0;
	int capacity = 10;

	if (!hosts || !count) {
		fprintf(stderr, "Invalid parameters to %s\n", __func__);
		return -1;
	}

	*hosts = NULL;
	*count = 0;

	dir = opendir(FC_HOST_PATH);
	if (!dir) {
		fprintf(stderr, "Failed to open %s: %s\n", FC_HOST_PATH, strerror(errno));
		return -1;
	}

	/* Allocate initial array */
	host_array = calloc(capacity, sizeof(struct fc_host));
	if (!host_array) {
		fprintf(stderr, "Failed to allocate memory for FC hosts\n");
		closedir(dir);
		return -1;
	}

	while ((entry = readdir(dir)) != NULL) {
		/* Skip . and .. */
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		/* Only process host* entries */
		if (strncmp(entry->d_name, "host", 4) != 0)
			continue;

		/* Expand array if needed */
		if (host_count >= capacity) {
			capacity *= 2;
			struct fc_host *new_array = realloc(host_array, capacity * sizeof(struct fc_host));
			if (!new_array) {
				fprintf(stderr, "Failed to expand FC host array\n");
				free(host_array);
				closedir(dir);
				return -1;
			}
			host_array = new_array;
		}

		/* Read host information */
		if (read_fc_host_info(entry->d_name, &host_array[host_count]) == 0) {
			host_count++;
		}
	}

	closedir(dir);

	*hosts = host_array;
	*count = host_count;

	return 0;
}

/**
 * diagnose_fc - Run diagnostics on a single FC host
 * @host_name: FC host name (e.g., "host0")
 * @notify: Notification flags
 *
 * Returns 0 on success, -1 on error
 */
static int diagnose_fc(char *host_name, struct fc_notify *notify)
{
	struct fc_host host;
	int rc;

	if (!host_name) {
		fprintf(stderr, "Invalid host name\n");
		return -1;
	}

	rc = read_fc_host_info(host_name, &host);
	if (rc < 0) {
		fprintf(stderr, "Failed to read information for %s\n", host_name);
		return -1;
	}

	/* Print basic information */
	fprintf(stdout, "Running diagnostics for %s (WWPN: %s)\n", host.name, host.wwpn);
	fprintf(stdout, "  Port State: %s\n", host.port_state);
	fprintf(stdout, "  Speed: %s\n", host.speed);
	fprintf(stdout, "  Link Failures: %lu\n", host.stats.link_failure_count);
	fprintf(stdout, "  Invalid CRC: %lu\n", host.stats.invalid_crc_count);
	fprintf(stdout, "  Loss of Sync: %lu\n", host.stats.loss_of_sync_count);
	fprintf(stdout, "  Loss of Signal: %lu\n", host.stats.loss_of_signal_count);

	/* Basic health check */
	if (strcmp(host.port_state, "Online") != 0) {
		fprintf(stdout, "  WARNING: Port is not online\n");
	} else {
		fprintf(stdout, "  Status: Healthy\n");
	}

	return 0;
}

/**
 * print_usage - Print usage information
 * @command: Command name
 */
static void print_usage(char *command)
{
	fprintf(stdout, "Usage: %s [OPTIONS] [host...]\n", command);
	fprintf(stdout, "\n");
	fprintf(stdout, "Fibre Channel diagnostics tool\n");
	fprintf(stdout, "\n");
	fprintf(stdout, "Options:\n");
	fprintf(stdout, "  -h, --help     Display this help message\n");
	fprintf(stdout, "\n");
	fprintf(stdout, "If no hosts are specified, all FC hosts will be checked\n");
}

int main(int argc, char *argv[])
{
	int opt, rc = 0;
	struct fc_notify notify = {
		.port_state_change = true,
		.link_errors = true,
		.multipath_degradation = false,
		.performance_drop = false,
		.eeh_events = true
	};

	if (get_platform() != PLATFORM_PSERIES_LPAR) {
		fprintf(stdout, "%s is only supported in PowerVM LPARs\n", argv[0]);
		return 0;
	}

	static struct option long_options[] = {
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "h", long_options, NULL)) != -1) {
		switch (opt) {
		case 'h':
			print_usage(argv[0]);
			return 0;
		case '?':
			print_usage(argv[0]);
			return -1;
		default:
			print_usage(argv[0]);
			return -1;
		}
	}

	/* No hosts specified, check all */
	if (optind == argc) {
		struct fc_host *hosts = NULL;
		int count = 0, i;

		if (discover_fc_hosts(&hosts, &count) < 0) {
			fprintf(stderr, "Failed to discover FC hosts\n");
			return -1;
		}

		if (count == 0) {
			fprintf(stdout, "No FC hosts detected in system\n");
			return 0;
		}

		fprintf(stdout, "Found %d FC host(s)\n\n", count);

		for (i = 0; i < count; i++) {
			rc += diagnose_fc(hosts[i].name, &notify);
			fprintf(stdout, "\n");
		}

		free(hosts);
	} else {
		/* Check specified hosts */
		while (optind < argc) {
			rc += diagnose_fc(argv[optind], &notify);
			fprintf(stdout, "\n");
			optind++;
		}
	}

	if (rc == 0)
		fprintf(stdout, "FC diag command completed successfully\n");
	else
		fprintf(stderr, "FC diag command failed with rc %d\n", rc);

	return rc;
}

// Made with Bob
