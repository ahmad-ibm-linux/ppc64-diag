#!/bin/bash
#
# eeh_scenario_test.sh - EEH-scenario validation for diag_fc
#
# Tests three scenarios diag_fc encounters in production:
#
#   Scenario 1 (HEALTHY)    — adapter fully online, both ports up.
#                             Validates the nominal EEH-callsite output.
#
#   Scenario 2 (UNBOUND)    — driver unbound from one PCI function.
#                             Simulates EEH freeze: PCI dir still in sysfs
#                             but fc_host is gone.  diag_fc must still
#                             collect VPD and sibling port data and report
#                             fc_host_present: false for the unbound function.
#
#   Scenario 3 (FAKE GONE)  — diag_fc called with a non-existent PCI addr.
#                             Simulates an adapter removed from the bus after
#                             EEH permanent failure.  Must produce a partial
#                             report with pci_function_present: false and exit 0.
#
# Usage:
#   bash eeh_scenario_test.sh
#   DIAG_FC=/path/to/diag_fc bash eeh_scenario_test.sh
#
# Requirements:
#   - Run as root (driver unbind/rebind requires root)
#   - Adapter 0155:90:00.0 / 0155:90:00.1 present and using lpfc driver
#   - python3 available for JSON validation
#
# Safety:
#   - Unbind is only held for the duration of the test (~2 seconds)
#   - A trap ensures rebind on any exit, including Ctrl-C
#   - No EEH injection; no hardware risk

set -uo pipefail

DIAG_FC="${DIAG_FC:-/home/ahmad/ppc64-diag/diags/diag_fc}"
PCI_0="${PCI_0:-0155:90:00.0}"
PCI_1="${PCI_1:-0155:90:00.1}"
DRIVER="lpfc"
DRIVER_BIND_PATH="/sys/bus/pci/drivers/${DRIVER}"
OUTDIR="/tmp/diag_fc_eeh_$$"
mkdir -p "$OUTDIR"

PASS=0
FAIL=0
SKIP=0
UNBOUND=""   # track whether we need to rebind on exit

# ------------------------------------------------------------------ #
# Helpers                                                              #
# ------------------------------------------------------------------ #
pass()  { echo "  PASS  $1"; ((PASS++)); true; }
fail()  { echo "  FAIL  $1"; ((FAIL++)); }
skip()  { echo "  SKIP  $1"; ((SKIP++)); }
info()  { echo "  INFO  $1"; }

check_contains() {
    local label="$1" key="$2" file="$3"
    grep -q "$key" "$file" && pass "$label" || fail "$label (key '$key' not found)"
}
check_value_is() {
    local label="$1" key="$2" expected="$3" file="$4"
    local actual
    actual=$(python3 -c "import json,sys; d=json.load(open('$file')); print(d$key)" 2>/dev/null || echo "PARSE_ERROR")
    [[ "$actual" == "$expected" ]] && pass "$label (got: $actual)" || fail "$label (expected '$expected', got '$actual')"
}

rebind_if_needed() {
    if [[ -n "$UNBOUND" ]]; then
        echo "  INFO  rebinding $UNBOUND to $DRIVER ..."
        echo "$UNBOUND" > "${DRIVER_BIND_PATH}/bind" 2>/dev/null || true
        sleep 1
        UNBOUND=""
    fi
}
trap 'rebind_if_needed; echo ""; echo "=== Cleanup done ==="' EXIT

valid_json() {
    local file="$1"
    python3 -c "import json,sys; json.load(open('$file'))" 2>/dev/null \
        && pass "valid JSON" || fail "invalid JSON"
}

# ------------------------------------------------------------------ #
# Pre-flight                                                           #
# ------------------------------------------------------------------ #
echo "=== diag_fc EEH scenario test ==="
echo "Binary:  $DIAG_FC"
echo "Adapter: $PCI_0 + $PCI_1 (driver: $DRIVER)"
echo ""

[[ -x "$DIAG_FC" ]] || { echo "FATAL: $DIAG_FC not executable"; exit 1; }
[[ $(id -u) -eq 0 ]]  || { echo "FATAL: must run as root (driver unbind requires root)"; exit 1; }

# Verify both PCI functions are currently bound
if [[ ! -d "${DRIVER_BIND_PATH}/${PCI_0}" ]]; then
    echo "FATAL: $PCI_0 not bound to $DRIVER — cannot run unbound scenario"
    exit 1
fi

# ================================================================== #
# SCENARIO 1: HEALTHY — nominal EEH callsite output                  #
# ================================================================== #
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "SCENARIO 1: HEALTHY ADAPTER (nominal EEH callsite)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
OUT1="$OUTDIR/scenario1_healthy.json"

"$DIAG_FC" \
    -p "$PCI_0" \
    -t "EEH_PERMANENT_FAILURE" \
    -s "platform" \
    -r "EEH declared permanent failure on FC slot" \
    -o "$OUT1"
RC=$?

echo ""
echo "[1.1] Exit code and output"
[[ $RC -eq 0 ]] && pass "exit 0" || fail "exit should be 0 (got $RC)"
[[ -f "$OUT1" ]] && pass "report file created" || { fail "report not created"; exit 1; }
valid_json "$OUT1"

echo ""
echo "[1.2] Trigger captured correctly"
check_contains "trigger type = EEH_PERMANENT_FAILURE" '"type": "EEH_PERMANENT_FAILURE"' "$OUT1"
check_contains "trigger source = platform"            '"source": "platform"'            "$OUT1"
check_contains "trigger pci_function = $PCI_0"        "\"$PCI_0\""                      "$OUT1"

echo ""
echo "[1.3] Both PCI functions mapped from trigger on $PCI_0"
check_contains "port $PCI_0 in report" "\"$PCI_0\"" "$OUT1"
check_contains "port $PCI_1 in report" "\"$PCI_1\"" "$OUT1"

echo ""
echo "[1.4] Both ports fc_host_present and Online"
FC_PRESENT=$(grep -c '"fc_host_present": true' "$OUT1" || true)
[[ "$FC_PRESENT" -eq 2 ]] && pass "both ports fc_host_present: true (got $FC_PRESENT)" \
                           || fail "expected 2 fc_host_present:true (got $FC_PRESENT)"
ONLINE=$(grep -c '"port_state": "Online"' "$OUT1" || true)
[[ "$ONLINE" -eq 2 ]] && pass "both ports Online" \
                       || fail "expected 2 Online ports (got $ONLINE)"

echo ""
echo "[1.5] VPD collected (SN, CCIN, part number)"
check_contains "vpd_status = collected" '"vpd_status": "collected"' "$OUT1"
check_contains "serial_number present"  '"serial_number"'           "$OUT1"
check_contains "ccin present"           '"ccin"'                    "$OUT1"
grep -q '"serial_number": ""' "$OUT1" \
    && fail "serial_number is empty" || pass "serial_number non-empty"
grep -q '"ccin": ""' "$OUT1" \
    && fail "ccin is empty" || pass "ccin non-empty"

echo ""
info "Scenario 1 report: $OUT1"


# ================================================================== #
# SCENARIO 2: UNBOUND — driver detached, PCI dir still present       #
# Simulates EEH frozen slot: hardware alive but driver torn down      #
# ================================================================== #
# ================================================================== #
# SCENARIO 2 safety guard                                             #
# Driver unbind/rebind is disruptive — do NOT run on production       #
# machines without explicit opt-in.                                   #
# Set EEH_TEST_ALLOW=1 to enable this scenario.                       #
#                                                                     #
# For true EEH error injection, use the errinjct tool instead:        #
# TODO: document errinjct usage once reviewer provides details.       #
# ================================================================== #
OUT2=""
if [[ "${EEH_TEST_ALLOW:-0}" != "1" ]]; then
    skip "Scenario 2 (driver unbind) — set EEH_TEST_ALLOW=1 to enable"
    echo "  INFO  Skipping Scenario 2: EEH_TEST_ALLOW not set."
    echo "  INFO  To run: EEH_TEST_ALLOW=1 bash $0"
else
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "SCENARIO 2: DRIVER UNBOUND (EEH frozen / driver torn down)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
OUT2="$OUTDIR/scenario2_unbound.json"

echo "  INFO  unbinding $PCI_0 from $DRIVER ..."
echo "$PCI_0" > "${DRIVER_BIND_PATH}/unbind"
UNBOUND="$PCI_0"
sleep 1   # let sysfs settle

# Verify the unbind actually happened
if [[ -d "${DRIVER_BIND_PATH}/${PCI_0}" ]]; then
    fail "unbind did not take effect — skipping scenario 2"
    skip "scenario 2 (unbind failed)"
else
    echo "  INFO  $PCI_0 unbound — running diag_fc now"

    "$DIAG_FC" \
        -p "$PCI_0" \
        -t "EEH_PERMANENT_FAILURE" \
        -s "platform" \
        -r "EEH frozen slot — driver torn down" \
        -o "$OUT2"
    RC=$?

    echo ""
    echo "[2.1] Exit code and output"
    [[ $RC -eq 0 ]] && pass "exit 0 even with unbound function" \
                    || fail "exit should be 0 (got $RC)"
    [[ -f "$OUT2" ]] && pass "report file created" || { fail "report not created"; }
    [[ -f "$OUT2" ]] && valid_json "$OUT2"

    if [[ -f "$OUT2" ]]; then
        echo ""
        echo "[2.2] Unbound function: pci_function_present true, fc_host_present false"
        # PCI dir still exists even when driver is unbound
        check_contains "pci_function_present: true for $PCI_0" '"pci_function_present": true' "$OUT2"
        # fc_host gone because driver is unbound
        check_contains "fc_host_present: false for $PCI_0" '"fc_host_present": false' "$OUT2"

        echo ""
        echo "[2.3] Sibling $PCI_1 still bound — should show fc_host_present: true"
        if [[ -d "${DRIVER_BIND_PATH}/${PCI_1}" ]]; then
            # Both pci_function_present:true and fc_host_present:true should appear
            # (once for the sibling — the unbound one will be false)
            check_contains "at least one fc_host_present: true (sibling)" \
                '"fc_host_present": true' "$OUT2"
        else
            skip "sibling $PCI_1 also unbound — cannot verify"
        fi

        echo ""
        echo "[2.4] VPD still readable when driver unbound (kernel keeps VPD sysfs)"
        VPD_STATUS=$(python3 -c "import json; d=json.load(open('$OUT2')); print(d['adapter']['vpd_status'])" 2>/dev/null)
        info "vpd_status when unbound = $VPD_STATUS"
        [[ "$VPD_STATUS" == "collected" ]] \
            && pass "VPD still collected when driver unbound" \
            || fail "VPD not collected when driver unbound (got: $VPD_STATUS) — kernel may have removed vpd sysfs"

        echo ""
        echo "[2.5] Both PCI functions still listed in ports array"
        check_contains "port $PCI_0 present in ports" "\"$PCI_0\"" "$OUT2"
        check_contains "port $PCI_1 present in ports" "\"$PCI_1\"" "$OUT2"
    fi

    echo ""
    echo "  INFO  rebinding $PCI_0 to $DRIVER ..."
    echo "$PCI_0" > "${DRIVER_BIND_PATH}/bind"
    UNBOUND=""
    sleep 2   # wait for fc_host to come back up
    echo "  INFO  rebind complete"
fi

fi  # EEH_TEST_ALLOW

echo ""
[[ -n "$OUT2" ]] && info "Scenario 2 report: $OUT2"


# ================================================================== #
# SCENARIO 3: FAKE GONE — non-existent PCI address                   #
# Simulates adapter fully removed from bus after EEH permanent failure#
# ================================================================== #
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "SCENARIO 3: ADAPTER GONE (non-existent PCI address)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
OUT3="$OUTDIR/scenario3_gone.json"
FAKE_PCI="dead:00:00.0"

echo "  INFO  invoking diag_fc with fake PCI addr '$FAKE_PCI'"

"$DIAG_FC" \
    -p "$FAKE_PCI" \
    -t "EEH_PERMANENT_FAILURE" \
    -s "platform" \
    -r "adapter fully removed from bus" \
    -o "$OUT3" 2>/dev/null
RC=$?

echo ""
echo "[3.1] Must exit 0 — partial report is always better than nothing"
[[ $RC -eq 0 ]] && pass "exit 0 for gone adapter" \
                || fail "exit should be 0 for gone adapter (got $RC)"
[[ -f "$OUT3" ]] && pass "report file created" || { fail "report not created"; }
[[ -f "$OUT3" ]] && valid_json "$OUT3"

if [[ -f "$OUT3" ]]; then
    echo ""
    echo "[3.2] Trigger pci_function recorded even though adapter is gone"
    check_contains "trigger pci_function = $FAKE_PCI" "\"$FAKE_PCI\"" "$OUT3"

    echo ""
    echo "[3.3] pci_function_present: false for gone adapter"
    check_contains "pci_function_present: false" '"pci_function_present": false' "$OUT3"

    echo ""
    echo "[3.4] fc_host_present: false for gone adapter"
    check_contains "fc_host_present: false" '"fc_host_present": false' "$OUT3"

    echo ""
    echo "[3.5] VPD and location unavailable (graceful degradation)"
    VPD_S=$(python3 -c "import json; d=json.load(open('$OUT3')); print(d['adapter']['vpd_status'])" 2>/dev/null)
    LOC_S=$(python3 -c "import json; d=json.load(open('$OUT3')); print(d['adapter']['location_status'])" 2>/dev/null)
    info "vpd_status   = $VPD_S"
    info "location_status = $LOC_S"
    [[ "$VPD_S"  != "collected"  ]] && pass "vpd_status not 'collected' (correct for gone adapter)" \
                                     || fail "vpd_status is 'collected' for a non-existent device"
    [[ "$LOC_S" != "collected" ]] && pass "location_status not 'collected' (correct for gone adapter)" \
                                   || fail "location_status is 'collected' for a non-existent device"

    echo ""
    echo "[3.6] Machine context still collected (hostname, kernel)"
    check_contains "hostname present"       '"hostname"'       "$OUT3"
    check_contains "kernel_release present" '"kernel_release"' "$OUT3"
fi

echo ""
info "Scenario 3 report: $OUT3"


# ================================================================== #
# Summary                                                             #
# ================================================================== #
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
echo "Reports in: $OUTDIR/"
echo ""

if [[ $FAIL -eq 0 ]]; then
    echo "ALL EEH SCENARIO CHECKS PASSED"
    exit 0
else
    echo "SOME CHECKS FAILED — review output above"
    echo ""
    echo "Quick inspect:"
    [[ -n "$OUT2" ]] && echo "  python3 -c \"import json,pprint; pprint.pprint(json.load(open('$OUT2')))\"  # unbound"
    echo "  python3 -c \"import json,pprint; pprint.pprint(json.load(open('$OUT3')))\"  # gone"
    exit 1
fi
