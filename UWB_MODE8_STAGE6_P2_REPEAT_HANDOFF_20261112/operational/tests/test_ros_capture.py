"""ROS2 recorder pure ingestion tests: no ROS2 libraries needed for host CI."""
from pathlib import Path
import math
import sys
import tempfile
import unittest
from types import SimpleNamespace as Obj
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"host"))
from ops_ros_capture import Recorder,stamp_ns,yaw_from_quaternion,RosCaptureHold

def header(ns):
    return Obj(stamp=Obj(sec=ns//1000000000,nanosec=ns%1000000000),frame_id="odom")
def quat(rad):
    return Obj(x=0.,y=0.,z=math.sin(rad/2),w=math.cos(rad/2))
def odom(ns,yaw=0.):
    return Obj(header=header(ns),child_frame_id="base_link",
               pose=Obj(pose=Obj(position=Obj(x=1.,y=2.),orientation=quat(yaw))))
def imu(ns):
    return Obj(header=header(ns),orientation=quat(0.),
               angular_velocity=Obj(z=.1),
               linear_acceleration=Obj(x=0.,y=0.,z=9.81))

class RosCaptureTests(unittest.TestCase):
    def test_ros2_odom_imu_csv_and_unsynced_contract(self):
        with tempfile.TemporaryDirectory() as folder:
            dest=Path(folder)/"run"
            rec=Recorder(dest)
            rec.odom(odom(1_500_000_000,math.pi/2))
            rec.odom(odom(1_600_000_000))
            rec.imu(imu(1_500_000_000))
            verdict=rec.close()
            self.assertEqual(verdict["status"],"CAPTURED_UNSYNCHRONIZED")
            self.assertFalse(verdict["rf_clock_synchronized"])
            self.assertTrue((dest/"odom.csv").is_file())
            self.assertTrue((dest/"imu.csv").is_file())
            self.assertFalse((dest/"odom.csv.partial").exists())
            self.assertEqual(rec.close(),None)
    def test_duplicate_stamp_fail_closed_preserves_partials(self):
        with tempfile.TemporaryDirectory() as folder:
            dest=Path(folder)/"run"
            rec=Recorder(dest);rec.odom(odom(1_500_000_000))
            with self.assertRaises(RosCaptureHold):
                rec.odom(odom(1_500_000_000))
            rec.imu(imu(1_500_000_000))
            receipt=rec.close()
            self.assertEqual(receipt["status"],"INCOMPLETE")
            self.assertTrue((dest/"odom.csv.partial").exists())
            self.assertFalse((dest/"odom.csv").exists())
    def test_bad_ros_stamp_and_quaternion_rejected(self):
        with self.assertRaises(RosCaptureHold):stamp_ns(Obj(sec=0,nanosec=0))
        with self.assertRaises(RosCaptureHold):stamp_ns(Obj(sec=1,nanosec=1_000_000_000))
        with self.assertRaises(RosCaptureHold):yaw_from_quaternion(Obj(x=0,y=0,z=0,w=0))
    def test_preexisting_output_cannot_be_overwritten(self):
        with tempfile.TemporaryDirectory() as d:
            target=Path(d)/"run";target.mkdir()
            with self.assertRaises(FileExistsError):Recorder(target)

if __name__=="__main__":
    unittest.main()
