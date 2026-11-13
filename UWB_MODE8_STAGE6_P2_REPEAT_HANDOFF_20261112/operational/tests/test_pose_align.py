"""Offline RF clock/ROS odom alignment, no inferred synchronization."""
import math
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"host"))
from ops_pose_align import align,fit_domain,rf_to_ros,unwrap,interpolate_pose,AlignmentHold,MOD

BASE=1_800_000_000_000_000_000
DT=int(63897600000)
def sync():
    return [{"domain":"tagA","radio_ticks":str((900_000_000_000+i*DT)&(MOD-1)),
             "ros_ns":str(BASE+i*1_000_000_000)} for i in range(5)]
def odom():
    return [{"ros_ns":str(BASE+i*1_000_000_000),
             "x_m":str(.2*i),"y_m":"0","yaw_rad":"0"} for i in range(5)]
def obs():
    return [{"domain":"tagA","session":"6","txn":"2","probe":"P1",
             "radio_ticks":str((900_000_000_000+2*DT)&(MOD-1))}]

class AlignTests(unittest.TestCase):
    def test_wrap_aware_sync_and_pose(self):
        rows,fit=align(sync(),odom(),obs())
        self.assertEqual(len(rows),1)
        self.assertAlmostEqual(rows[0]["x_m"],.4,places=5)
        self.assertLess(abs(fit["tagA"]["ppm"]),100)

    def test_bad_sync_and_unknown_domain_rejected(self):
        damaged=sync();damaged[2]=dict(damaged[2],ros_ns=str(BASE+2_100_000_000))
        with self.assertRaisesRegex(AlignmentHold,"RMS"):align(damaged,odom(),obs())
        wrong=obs();wrong[0]["domain"]="tagB"
        with self.assertRaisesRegex(AlignmentHold,"unknown"):align(sync(),odom(),wrong)

    def test_backward_ambiguous_ticks_and_no_extrapolation(self):
        with self.assertRaises(AlignmentHold):unwrap([300,300])
        with self.assertRaises(AlignmentHold):unwrap([300,200])
        with self.assertRaises(AlignmentHold):interpolate_pose(
            [(100,0,0,0),(200,1,0,0)],99)

    def test_multiple_wrap_choices_require_coarse_time(self):
        model={"raw0":0,"span":2*MOD+10000,"mean_x":MOD,
               "mean_ns":BASE,"slope_ns_per_tick":1e9/63897600000.0,
               "ppm":0,"rms_ns":0,"samples":5}
        with self.assertRaisesRegex(AlignmentHold,"wrap ambiguous"):
            rf_to_ros(1000,model)
        expected=round(BASE+(1000+MOD-MOD)*model["slope_ns_per_tick"])
        self.assertEqual(rf_to_ros(1000,model,approx_ns=expected),expected)
        with self.assertRaisesRegex(AlignmentHold,"coarse ROS time"):
            rf_to_ros(1000,model,approx_ns=BASE+10_000_000_000)

    def test_heading_wrap_shortest_arc(self):
        p=[(10,0,0,math.radians(179)),(20,1,0,math.radians(-179))]
        x,y,yaw=interpolate_pose(p,15)
        self.assertAlmostEqual(x,.5)
        self.assertAlmostEqual(abs(math.degrees(yaw)),180,places=7)
