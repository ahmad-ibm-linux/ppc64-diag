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
 * test_diag_fc.c - Unit tests for diag_fc (new passive-collector design).
 *
 * Tests the data structures, constants, and public API declared in diag_fc.h.
 * Does not require FC hardware or a running system — all tests are purely
 * structural or use the public API with known-invalid inputs.
 *
 * Build (on LPAR, from ppc64-diag root after configure):
 *   gcc -o diags/test/test_diag_fc \
 *       diags/test/test_diag_fc.c \
 *       diags/diag_fc.c \
 *       common/platform.c \
 *       common/utils.c \
 *       -I common -I diags \
 *       -Wall -Wextra -g \
 *       && echo "Build OK"
 *
 * Run:
 *   ./diags/test/test_diag_fc
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../diag_fc.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static int g_pass;
static int g_fail;

#define TEST_PASS(msg) do { printf("  PASS  %s\n", (msg)); g_pass++; } while (0)
#define TEST_FAIL(msg) do { printf("  FAIL  %s\n", (msg)); g_fail++; } while (0)
#define CHECK(cond, msg) do { if (cond) TEST_PASS(msg); else TEST_FAIL(msg); } while (0)

/* ------------------------------------------------------------------ */
/* 1. Structure layout and constants                                    */
/* ------------------------------------------------------------------ */

static void test_constants(void)
{
	printf("\n[1] Constants\n");

	CHECK(MAX_FC_PORTS >= 2,       "MAX_FC_PORTS >= 2 (adapter has 2 ports)");
	CHECK(PCI_ADDR_LEN >= 16,      "PCI_ADDR_LEN fits 'DDDD:BB:DD.F'");
	CHECK(FC_HOST_LEN  >= 8,       "FC_HOST_LEN fits 'hostNNN'");
	CHECK(WWN_LEN      >= 20,      "WWN_LEN fits '0x100000109bec9b93'");
	CHECK(LOCATION_LENGTH >= 30,   "LOCATION_LENGTH fits IBM loc-code");
	CHECK(FIELD_LEN    >= 32,      "FIELD_LEN fits generic short fields");
	CHECK(DESCR_LENGTH >= 256,     "DESCR_LENGTH fits reason strings");

	CHECK(strcmp(CSTATUS_OK,           "collected")    == 0, "CSTATUS_OK value");
	CHECK(strcmp(CSTATUS_UNAVAILABLE,  "unavailable")  == 0, "CSTATUS_UNAVAILABLE value");
	CHECK(strcmp(CSTATUS_PARSE_ERROR,  "parse_error")  == 0, "CSTATUS_PARSE_ERROR value");
	CHECK(strcmp(CSTATUS_NOT_ATTEMPTED,"not_attempted") == 0, "CSTATUS_NOT_ATTEMPTED value");
}

static void test_struct_sizes(void)
{
	printf("\n[2] Structure sizes\n");

	/* Sizes should be non-trivially large (sanity, not exact) */
	CHECK(sizeof(struct fc_port)        > 100, "fc_port has reasonable size");
	CHECK(sizeof(struct fc_adapter_vpd) > 50,  "fc_adapter_vpd has reasonable size");
	CHECK(sizeof(struct fc_incident)    > 500, "fc_incident has reasonable size");

	/* fc_port field sizes match the constants */
	struct fc_port p;
	CHECK(sizeof(p.pci_addr)         == PCI_ADDR_LEN,   "fc_port.pci_addr matches PCI_ADDR_LEN");
	CHECK(sizeof(p.fc_host)          == FC_HOST_LEN,    "fc_port.fc_host matches FC_HOST_LEN");
	CHECK(sizeof(p.wwpn)             == WWN_LEN,        "fc_port.wwpn matches WWN_LEN");
	CHECK(sizeof(p.wwnn)             == WWN_LEN,        "fc_port.wwnn matches WWN_LEN");
	CHECK(sizeof(p.port_state)       == FIELD_LEN,      "fc_port.port_state matches FIELD_LEN");
	CHECK(sizeof(p.port_type)        == FIELD_LEN,      "fc_port.port_type matches FIELD_LEN");
	CHECK(sizeof(p.speed)            == FIELD_LEN,      "fc_port.speed matches FIELD_LEN");
	CHECK(sizeof(p.supported_speeds) == FIELD_LEN,      "fc_port.supported_speeds matches FIELD_LEN");
	CHECK(sizeof(p.fabric_name)      == FIELD_LEN,      "fc_port.fabric_name matches FIELD_LEN");

	/* fc_incident holds MAX_FC_PORTS ports */
	struct fc_incident inc;
	CHECK(sizeof(inc.ports) == MAX_FC_PORTS * sizeof(struct fc_port),
	      "fc_incident.ports array matches MAX_FC_PORTS");
	CHECK(sizeof(inc.location_code) == LOCATION_LENGTH,
	      "fc_incident.location_code matches LOCATION_LENGTH");
}

/* ------------------------------------------------------------------ */
/* 2. Struct initialization                                             */
/* ------------------------------------------------------------------ */

static void test_struct_init(void)
{
	printf("\n[3] Struct zero-initialization\n");

	struct fc_port port;
	memset(&port, 0, sizeof(port));
	CHECK(port.pci_function_present == false, "fc_port.pci_function_present initializes false");
	CHECK(port.fc_host_present      == false, "fc_port.fc_host_present initializes false");
	CHECK(port.pci_addr[0]          == '\0',  "fc_port.pci_addr initializes empty");

	struct fc_adapter_vpd vpd;
	memset(&vpd, 0, sizeof(vpd));
	CHECK(vpd.part_number[0]    == '\0', "fc_adapter_vpd.part_number initializes empty");
	CHECK(vpd.serial_number[0]  == '\0', "fc_adapter_vpd.serial_number initializes empty");
	CHECK(vpd.ccin[0]           == '\0', "fc_adapter_vpd.ccin initializes empty");

	struct fc_incident inc;
	memset(&inc, 0, sizeof(inc));
	CHECK(inc.num_ports               == 0,    "fc_incident.num_ports initializes 0");
	CHECK(inc.trigger_type[0]         == '\0', "fc_incident.trigger_type initializes empty");
	CHECK(inc.adapter_symbolic_name[0]== '\0', "fc_incident.adapter_symbolic_name initializes empty");
}

/* ------------------------------------------------------------------ */
/* 3. collect_fc_incident — NULL argument rejection                    */
/* ------------------------------------------------------------------ */

static void test_collect_null_args(void)
{
	printf("\n[4] collect_fc_incident — NULL argument rejection\n");

	struct fc_incident inc;

	/* NULL pci_addr must return -1 */
	int rc = collect_fc_incident(NULL, "TEST", "manual", "", NULL, NULL, &inc);
	CHECK(rc == -1, "NULL pci_addr returns -1");

	/* empty pci_addr must return -1 */
	rc = collect_fc_incident("", "TEST", "manual", "", NULL, NULL, &inc);
	CHECK(rc == -1, "empty pci_addr returns -1");

	/* NULL out struct must return -1 */
	rc = collect_fc_incident("0155:90:00.0", "TEST", "manual", "", NULL, NULL, NULL);
	CHECK(rc == -1, "NULL out struct returns -1");
}

/* ------------------------------------------------------------------ */
/* 4. write_fc_report — NULL argument rejection                        */
/* ------------------------------------------------------------------ */

static void test_write_null_args(void)
{
	printf("\n[5] write_fc_report — NULL argument rejection\n");

	int rc = write_fc_report(NULL, "-", NULL);
	CHECK(rc == -1, "NULL inc returns -1");
}

/* ------------------------------------------------------------------ */
/* 5. write_fc_report — stdout output on minimal zeroed struct         */
/* ------------------------------------------------------------------ */

static void test_write_zeroed_struct(void)
{
	printf("\n[6] write_fc_report — zeroed struct produces valid JSON skeleton\n");

	struct fc_incident inc;
	memset(&inc, 0, sizeof(inc));
	inc.num_ports = 0;

	/*
	 * Redirect stdout to /dev/null just to check it doesn't crash or
	 * return an error.  Real JSON content is checked in smoke_test_fc.sh.
	 */
	FILE *orig = stdout;
	FILE *devnull = fopen("/dev/null", "w");
	if (!devnull) {
		printf("  SKIP  cannot open /dev/null\n");
		return;
	}

	/* write_fc_report writes to the FILE* via fopen internally; test via
	 * a temp file to avoid redirecting process stdout */
	char tmppath[] = "/tmp/test_diag_fc_XXXXXX";
	int tmpfd = mkstemp(tmppath);
	if (tmpfd < 0) {
		fclose(devnull);
		printf("  SKIP  cannot create temp file\n");
		return;
	}
	close(tmpfd);

	int rc = write_fc_report(&inc, tmppath, NULL);
	CHECK(rc == 0, "write_fc_report succeeds on zeroed struct");

	/* Verify the file starts with '{' and ends with '}' */
	FILE *f = fopen(tmppath, "r");
	if (f) {
		char buf[4096];
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = '\0';

		CHECK(n > 10,          "JSON output is non-trivially long");
		CHECK(buf[0] == '{',   "JSON output starts with '{'");
		/* find last non-whitespace */
		size_t last = n;
		while (last > 0 && (buf[last-1] == '\n' || buf[last-1] == '\r'
				    || buf[last-1] == ' '))
			last--;
		CHECK(last > 0 && buf[last-1] == '}', "JSON output ends with '}'");

		/* Must contain required top-level keys */
		CHECK(strstr(buf, "\"schema_version\"") != NULL, "JSON has schema_version");
		CHECK(strstr(buf, "\"trigger\"")         != NULL, "JSON has trigger section");
		CHECK(strstr(buf, "\"machine\"")         != NULL, "JSON has machine section");
		CHECK(strstr(buf, "\"adapter\"")         != NULL, "JSON has adapter section");
		CHECK(strstr(buf, "\"ports\"")           != NULL, "JSON has ports section");
		CHECK(strstr(buf, "\"collection\"")      != NULL, "JSON has collection section");
	} else {
		TEST_FAIL("cannot reopen temp file for validation");
	}

	remove(tmppath);
	fclose(devnull);
	(void)orig;
}

/* ------------------------------------------------------------------ */
/* 6. CSTATUS_* string lengths fit in vpd_status / location_status     */
/* ------------------------------------------------------------------ */

static void test_cstatus_fit(void)
{
	printf("\n[7] CSTATUS_* strings fit in fc_incident status fields\n");

	struct fc_incident inc;
	CHECK(strlen(CSTATUS_OK)           < sizeof(inc.vpd_status), "CSTATUS_OK fits vpd_status");
	CHECK(strlen(CSTATUS_UNAVAILABLE)  < sizeof(inc.vpd_status), "CSTATUS_UNAVAILABLE fits");
	CHECK(strlen(CSTATUS_PARSE_ERROR)  < sizeof(inc.vpd_status), "CSTATUS_PARSE_ERROR fits");
	CHECK(strlen(CSTATUS_NOT_ATTEMPTED)< sizeof(inc.vpd_status), "CSTATUS_NOT_ATTEMPTED fits");
}

/* ------------------------------------------------------------------ */
/* 8. New struct sizes and zero-initialization (fc_statistics,         */
/*    fc_remote_port, multipath_device)                                */
/* ------------------------------------------------------------------ */

static void test_new_structs(void)
{
	printf("\n[8] New struct sizes and zero-initialization\n");

	/* fc_statistics */
	struct fc_statistics stats;
	memset(&stats, 0, sizeof(stats));
	CHECK(sizeof(stats.error_frames)  == FIELD_LEN, "fc_statistics.error_frames matches FIELD_LEN");
	CHECK(sizeof(stats.dumped_frames) == FIELD_LEN, "fc_statistics.dumped_frames matches FIELD_LEN");
	CHECK(stats.error_frames[0]  == '\0', "fc_statistics.error_frames initializes empty");
	CHECK(stats.dumped_frames[0] == '\0', "fc_statistics.dumped_frames initializes empty");

	/* fc_port embeds fc_statistics */
	struct fc_port port;
	memset(&port, 0, sizeof(port));
	CHECK(sizeof(port.stats) == sizeof(struct fc_statistics),
	      "fc_port.stats has correct size");

	/* fc_remote_port */
	struct fc_remote_port rp;
	memset(&rp, 0, sizeof(rp));
	CHECK(sizeof(rp.port_id)    == FIELD_LEN, "fc_remote_port.port_id matches FIELD_LEN");
	CHECK(sizeof(rp.port_name)  == WWN_LEN,   "fc_remote_port.port_name matches WWN_LEN");
	CHECK(sizeof(rp.port_state) == FIELD_LEN, "fc_remote_port.port_state matches FIELD_LEN");
	CHECK(sizeof(rp.roles)      == ROLES_LEN, "fc_remote_port.roles matches ROLES_LEN");
	CHECK(rp.port_id[0] == '\0',    "fc_remote_port.port_id initializes empty");
	CHECK(rp.port_name[0] == '\0',  "fc_remote_port.port_name initializes empty");
	CHECK(rp.port_state[0] == '\0', "fc_remote_port.port_state initializes empty");
	CHECK(rp.roles[0]     == '\0',  "fc_remote_port.roles initializes empty");

	/* multipath_device */
	struct multipath_device md;
	memset(&md, 0, sizeof(md));
	CHECK(sizeof(md.dm_name)  == MPATH_DEV_NAME_LEN, "multipath_device.dm_name matches MPATH_DEV_NAME_LEN");
	CHECK(sizeof(md.dev_name) == MPATH_DEV_NAME_LEN, "multipath_device.dev_name matches MPATH_DEV_NAME_LEN");
	CHECK(md.dm_name[0]  == '\0', "multipath_device.dm_name initializes empty");
	CHECK(md.dev_name[0] == '\0', "multipath_device.dev_name initializes empty");

	/* fc_incident now holds remote_ports and mpath_devices arrays */
	struct fc_incident inc;
	memset(&inc, 0, sizeof(inc));
	CHECK(inc.num_remote_ports   == 0, "fc_incident.num_remote_ports initializes 0");
	CHECK(inc.num_mpath_devices  == 0, "fc_incident.num_mpath_devices initializes 0");
	CHECK(sizeof(inc.remote_ports)   == MAX_REMOTE_PORTS  * sizeof(struct fc_remote_port),
	      "fc_incident.remote_ports array matches MAX_REMOTE_PORTS");
	CHECK(sizeof(inc.mpath_devices) == MAX_MPATH_DEVICES * sizeof(struct multipath_device),
	      "fc_incident.mpath_devices array matches MAX_MPATH_DEVICES");

	/* MAX_REMOTE_PORTS and MAX_MPATH_DEVICES sanity */
	CHECK(MAX_REMOTE_PORTS  >= 8,  "MAX_REMOTE_PORTS >= 8");
	CHECK(MAX_MPATH_DEVICES >= 4,  "MAX_MPATH_DEVICES >= 4");
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
	printf("=== diag_fc unit tests (passive-collector design) ===\n");

	test_constants();
	test_struct_sizes();
	test_struct_init();
	test_collect_null_args();
	test_write_null_args();
	test_write_zeroed_struct();
	test_cstatus_fit();
	test_new_structs();

	printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
