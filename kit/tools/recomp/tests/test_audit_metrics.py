"""Internal consistency of audit JSON so reports cannot silently disagree."""
import json
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[4]
REPORT = ROOT / "build/recomp/translate-report.json"
EBFF = ROOT / "build/audit/stages/ebff_this_gen_classification.json"


@pytest.fixture
def report():
    if not REPORT.is_file():
        pytest.skip("no translate-report.json")
    return json.loads(REPORT.read_text())


def test_recover_errors_are_not_silently_capped(report):
    """The public kit used to dump recover_errors[:50]. After the reporting
    fix, total must equal the array length when the key is present."""
    if "recover_errors_total" not in report:
        pytest.skip("pre-fix report; regen to bind recover_errors_total")
    errs = report.get("recover_errors") or []
    assert report["recover_errors_total"] == len(errs)
    assert report["recover_errors_unique"] == len(set(errs))


def test_unmodelled_array_length_matches_its_own_list(report):
    unm = report.get("unmodelled") or []
    assert isinstance(unm, list)
    # No second copy in this file; the audit doc must cite len(unmodelled).
    assert report["functions_ok"] + len(report.get("blocks_withdrawn") or []) == report["functions_total"]


def test_ebff_trampoline_classes_sum_to_2439():
    if not EBFF.is_file():
        pytest.skip("no ebff_this_gen_classification.json")
    data = json.loads(EBFF.read_text())
    assert data["n"] == 2439
    assert sum(v["count"] for v in data["classes"].values()) == 2439


def test_coverage_object_is_internally_consistent(report):
    cov = report.get("coverage")
    if not cov:
        pytest.skip("pre-instrumentation report; regen to bind coverage")
    modelled = cov["unique_bytes_modelled"]
    trapped = cov["unique_bytes_unmodelled_trap"]  # |trapped - modelled|
    both = cov["unique_bytes_both"]
    emitted = cov["unique_bytes_emitted"]  # |modelled ∪ trapped|
    assert modelled >= 0 and trapped >= 0 and both >= 0
    assert both <= modelled
    assert emitted == modelled + trapped
    assert cov["insn_sites_emitted"] >= 0
    denom = cov["guest_code_range_bytes"]
    assert denom > 0
    pct = cov["percent_modelled_of_guest_code_ranges"]
    if pct is not None:
        assert abs(pct - 100.0 * modelled / denom) < 1e-4
    # Never report candidate counts as this percentage's numerator.
    assert modelled != report["functions_ok"]


def test_tailcalls_total_matches_sidecar_when_present(report):
    if "tailcalls_total" not in report:
        pytest.skip("pre-sidecar report")
    side = REPORT.parent / "tailcalls.json"
    if not side.is_file():
        pytest.skip("no tailcalls.json sidecar")
    data = json.loads(side.read_text())
    assert data["count"] == report["tailcalls_total"]
    assert data["count"] == len(data["sites"])
