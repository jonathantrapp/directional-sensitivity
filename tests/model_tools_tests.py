"""Offline regression checks for fitting boundaries and miss-aware comparison."""
import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
class ModelToolsTests(unittest.TestCase):
    def test_fast_miss_cannot_win_by_click_time(self):
        with tempfile.TemporaryDirectory(prefix='directional-model-',dir=ROOT/'build') as folder:
            root=Path(folder)
            self.assertTrue(root.resolve().is_relative_to((ROOT/'build').resolve()))
            (root/'session.json').write_text(json.dumps(dict(mode='randomized_profile_comparison',
                timeout_us=5000000,session_complete=False)))
            with (root/'trials.csv').open('w',newline='') as f:
                w=csv.writer(f); w.writerow(['trial','outcome','condition','duration_us','direction','distance','rep','first_paint_us'])
                w.writerow(['trial_1','hit','baseline',900000,'NW',0,0,1000])
                w.writerow(['trial_2','miss','candidate',10000,'NW',0,1,0])
                w.writerow(['trial_3','focus_lost','baseline',20000,'SE',0,0,0])
            (root/'timing.csv').write_text('trial,identified,onset_us,end_us,epochs\ntrial_1,1,100000,800000,2\ntrial_2,1,1000,5000,1\n')
            output=root/'summary.json'
            result=subprocess.run([sys.executable,str(ROOT/'tools/compare_summary.py'),str(root),'--output',str(output)],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            summary=json.loads(output.read_text())
            self.assertEqual(summary['conditions']['candidate']['mean_deadline_penalized_ms'],5000)
            self.assertEqual(summary['paired_mean_penalized_improvement_ms'],-4100)
            self.assertEqual(summary['excluded_outcomes'],{'focus_lost':1})
            self.assertFalse(summary['human_improvement_established'])
            self.assertEqual(summary['raw_movement_timing']['baseline']['mean_primary_candidate_ms'],700)
            self.assertEqual(summary['raw_movement_timing']['candidate']['n'],1)
            self.assertIsNone(summary['paired_by_direction']['NW']['descriptive_bootstrap_95pct_interval_ms'])

    def test_adjacent_angles_are_not_cross_matched(self):
        with tempfile.TemporaryDirectory(prefix='directional-model-',dir=ROOT/'build') as folder:
            root=Path(folder); self.assertTrue(root.resolve().is_relative_to((ROOT/'build').resolve()))
            (root/'session.json').write_text(json.dumps(dict(mode='randomized_profile_comparison',timeout_us=5000000,session_complete=False)))
            with (root/'trials.csv').open('w',newline='') as f:
                w=csv.writer(f); w.writerow(['outcome','condition','duration_us','direction','distance','rep','angle_index','angle_degrees','target_x','target_y'])
                # Two different angles share the nearest E compass sector.
                for angle in (0,1):
                    w.writerow(['hit','baseline',100000,'E',0,0,angle,angle*15,100,angle*25])
                    w.writerow(['hit','candidate',110000,'E',0,1,angle,angle*15,100,angle*25])
            output=root/'summary.json'
            result=subprocess.run([sys.executable,str(ROOT/'tools/compare_summary.py'),str(root),'--output',str(output)],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            report=json.loads(output.read_text())
            self.assertEqual(report['matched_target_pairs'],2)
            self.assertEqual(set(report['by_target_angle_degrees']),{'0','15'})
            self.assertEqual(set(report['by_target_quadrant']),{'cardinal_targets','SE'})

    def test_fitting_never_reuses_test_as_training(self):
        with tempfile.TemporaryDirectory(prefix='directional-model-',dir=ROOT/'build') as folder:
            self.assertTrue(Path(folder).resolve().is_relative_to((ROOT/'build').resolve()))
            result=subprocess.run([sys.executable,str(ROOT/'tools/fit_personal.py'),
                '--train',folder,'--test',folder,'--output',str(Path(folder)/'out')],capture_output=True)
            self.assertEqual(result.returncode,2)
            self.assertFalse((Path(folder)/'out').exists())

if __name__=='__main__': unittest.main()
