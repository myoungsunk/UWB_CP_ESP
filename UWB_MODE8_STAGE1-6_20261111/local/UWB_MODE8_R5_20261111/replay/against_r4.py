import locale, sys, unittest
from pathlib import Path
old = Path(sys.argv[1])
current = Path.cwd()
sys.path[:0] = [str(old / 'tools'), str(old / 'tests')]
print('R4 tools path:', old / 'tools', flush=True)
print('Locale:', locale.getencoding(), 'UTF-8 mode:', sys.flags.utf8_mode, flush=True)
import analyze, capture_audit, offline_preflight, source_contract, upload, test_r4_regression
assert Path(analyze.__file__).resolve().parent == (old / 'tools').resolve()
assert Path(test_r4_regression.__file__).resolve().parent == (old / 'tests').resolve()
sys.path.insert(0, str(current / 'tests'))
import test_r5_regression
suite = unittest.TestSuite((
    unittest.defaultTestLoader.loadTestsFromModule(test_r5_regression),
    test_r4_regression.ContractRegressions('test_vendor_fcs_contract_and_fixed_buffer'),
))
result = unittest.TextTestRunner(verbosity=2).run(suite)
assert result.testsRun == 6
methods = {str(test).split(' ', 1)[0] for test, _ in result.failures + result.errors}
expected = {
    'test_missing_poll_retains_other_radios_p1', 'test_conflicting_poll_cannot_be_disguised_as_missed',
    'test_anchor_tx_slot_mutations_block_p2_with_or_without_static_reuse',
    'test_sketch_overrides_invalidate_existing_images',
    'test_generated_exports_are_excluded_and_legacy_receipts_are_refused',
    'test_vendor_fcs_contract_and_fixed_buffer',
}
assert methods == expected, methods
assert len(result.errors) == 1 and 'UnicodeDecodeError' in result.errors[0][1]
print('Expected failures verified against frozen R4.')

