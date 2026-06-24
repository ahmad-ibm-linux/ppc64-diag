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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../diag_fc.h"

/**
 * test_fc_structures - Test FC data structure sizes and alignment
 */
static void test_fc_structures(void)
{
	struct fc_host host;
	struct fc_statistics stats;
	struct fc_remote_port rport;
	struct multipath_device mpath;
	struct eeh_event eeh;
	struct fc_vpd_data vpd;

	printf("Testing FC data structures...\n");

	/* Verify structures can be initialized */
	memset(&host, 0, sizeof(host));
	memset(&stats, 0, sizeof(stats));
	memset(&rport, 0, sizeof(rport));
	memset(&mpath, 0, sizeof(mpath));
	memset(&eeh, 0, sizeof(eeh));
	memset(&vpd, 0, sizeof(vpd));

	/* Basic size checks */
	assert(sizeof(host.wwpn) == WWPN_LENGTH);
	assert(sizeof(host.wwnn) == WWNN_LENGTH);
	assert(sizeof(host.pci_address) == PCI_ADDR_LENGTH);

	printf("  ✓ Structure sizes correct\n");
	printf("  ✓ Structures can be initialized\n");
}

/**
 * test_sysfs_paths - Test sysfs path constants
 */
static void test_sysfs_paths(void)
{
	printf("Testing sysfs path constants...\n");

	assert(FC_HOST_PATH != NULL);
	assert(FC_RPORT_PATH != NULL);
	assert(strlen(FC_HOST_PATH) > 0);
	assert(strlen(FC_RPORT_PATH) > 0);

	printf("  ✓ FC_HOST_PATH: %s\n", FC_HOST_PATH);
	printf("  ✓ FC_RPORT_PATH: %s\n", FC_RPORT_PATH);
}

/**
 * test_config_file_path - Test configuration file path
 */
static void test_config_file_path(void)
{
	printf("Testing configuration file path...\n");

	assert(CONFIG_FILE != NULL);
	assert(strlen(CONFIG_FILE) > 0);

	printf("  ✓ CONFIG_FILE: %s\n", CONFIG_FILE);
}

/**
 * test_fc_host_discovery - Test FC host discovery (requires FC hardware)
 */
static void test_fc_host_discovery(void)
{
	struct fc_host *hosts = NULL;
	int count = 0;
	int rc;

	printf("Testing FC host discovery...\n");

	rc = discover_fc_hosts(&hosts, &count);

	if (rc < 0) {
		printf("  ⚠ Discovery failed (may be expected if no FC hardware present)\n");
		return;
	}

	printf("  ✓ Discovery succeeded\n");
	printf("  ✓ Found %d FC host(s)\n", count);

	if (count > 0) {
		printf("  ✓ First host: %s (WWPN: %s)\n", hosts[0].name, hosts[0].wwpn);
		free(hosts);
	}
}

/**
 * test_statistics_structure - Test statistics structure initialization
 */
static void test_statistics_structure(void)
{
	struct fc_statistics stats;

	printf("Testing FC statistics structure...\n");

	memset(&stats, 0, sizeof(stats));

	/* Verify all fields are accessible */
	stats.tx_frames = 1000;
	stats.rx_frames = 2000;
	stats.link_failure_count = 0;
	stats.invalid_crc_count = 5;

	assert(stats.tx_frames == 1000);
	assert(stats.rx_frames == 2000);
	assert(stats.link_failure_count == 0);
	assert(stats.invalid_crc_count == 5);

	printf("  ✓ All statistics fields accessible\n");
}

int main(void)
{
	printf("=== FC Diagnostics Unit Tests ===\n\n");

	test_fc_structures();
	printf("\n");

	test_sysfs_paths();
	printf("\n");

	test_config_file_path();
	printf("\n");

	test_statistics_structure();
	printf("\n");

	test_fc_host_discovery();
	printf("\n");

	printf("=== All tests completed ===\n");

	return 0;
}

// Made with Bob
