#!/usr/bin/env python3
"""ROS2 Humble robot odom/IMU recorder for Mode8 offline RF-time alignment.

This node DOES NOT fabricate DW40<->ROS clock synchronization pairs, derive
an antenna delay, command /cmd_vel, or decide whether an RF measurement
exists. A separate observed physical sync event must be supplied to
ops_pose_align.py for each independent anchor/tagA/tagB domain.
"""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import time

class RosCaptureHold(ValueError):pass

def stamp_ns(stamp)->int:
    sec=int(stamp.sec);nsec=int(stamp.nanosec)
    if sec<0 or not 0<=nsec<1_000_000_000 or sec==0 and nsec==0:
        raise RosCaptureHold("invalid/unset ROS sensor header stamp")
    return sec*1_000_000_000+nsec

def yaw_from_quaternion(q):
    x,y,z,w=(float(q.x),float(q.y),float(q.z),float(q.w))
    if not all(math.isfinite(v) for v in (x,y,z,w)):
        raise RosCaptureHold("nonfinite quaternion")
    norm=x*x+y*y+z*z+w*w
    if abs(norm-1)>0.05:raise RosCaptureHold("un-normalized robot orientation quaternion")
    scale=1/math.sqrt(norm)
    x*=scale;y*=scale;z*=scale;w*=scale
    return math.atan2(2*(w*z+x*y),1-2*(y*y+z*z))

def _sha_file(path):
    h=hashlib.sha256()
    with path.open("rb") as f:
        for buf in iter(lambda:f.read(65536),b""):h.update(buf)
    return h.hexdigest()

class Recorder:
    def __init__(self,out:Path):
        self.out=Path(out)
        if self.out.exists():raise FileExistsError(self.out)
        self.out.mkdir(parents=True,exist_ok=False)
        self.file={}
        self.writer={}
        self.last={"odom":0,"imu":0}
        self.count={"odom":0,"imu":0}
        self.errors=[]
        self.closed=False
        layouts={
            "odom":["ros_ns","x_m","y_m","yaw_rad","frame_id","child_frame_id","arrival_mono_ns"],
            "imu":["ros_ns","yaw_rad","gyro_z_rad_s","acc_x_m_s2","acc_y_m_s2","acc_z_m_s2","frame_id","arrival_mono_ns"]
        }
        for key,fields in layouts.items():
            fp=(self.out/f"{key}.csv.partial").open("x",newline="",encoding="utf-8")
            self.file[key]=fp
            writer=csv.DictWriter(fp,fieldnames=fields)
            writer.writeheader()
            self.writer[key]=writer

    def _row(self,domain,stamp,row):
        if self.closed:raise RosCaptureHold("recorder closed")
        if stamp<=self.last[domain]:
            self.errors.append({"domain":domain,"reason":"nonmonotonic ROS header stamp",
                                "stamp":stamp,"prior":self.last[domain]})
            raise RosCaptureHold("nonmonotonic ROS header stamp; stop and inspect raw")
        self.last[domain]=stamp
        self.writer[domain].writerow(row)
        self.count[domain]+=1

    def odom(self,msg):
        try:
            stamp=stamp_ns(msg.header.stamp)
            pose=msg.pose.pose
            yaw=yaw_from_quaternion(pose.orientation)
            x=float(pose.position.x);y=float(pose.position.y)
            if not math.isfinite(x) or not math.isfinite(y):
                raise RosCaptureHold("nonfinite odom XY")
            self._row("odom",stamp,{"ros_ns":stamp,"x_m":x,"y_m":y,
                                   "yaw_rad":yaw,"frame_id":msg.header.frame_id,
                                   "child_frame_id":msg.child_frame_id,
                                   "arrival_mono_ns":time.monotonic_ns()})
        except (RosCaptureHold,ValueError,TypeError,AttributeError) as e:
            self.errors.append({"domain":"odom","error":str(e)})
            raise

    def imu(self,msg):
        try:
            stamp=stamp_ns(msg.header.stamp)
            data={"ros_ns":stamp,"yaw_rad":yaw_from_quaternion(msg.orientation),
                  "gyro_z_rad_s":float(msg.angular_velocity.z),
                  "acc_x_m_s2":float(msg.linear_acceleration.x),
                  "acc_y_m_s2":float(msg.linear_acceleration.y),
                  "acc_z_m_s2":float(msg.linear_acceleration.z),
                  "frame_id":msg.header.frame_id,"arrival_mono_ns":time.monotonic_ns()}
            if any(not math.isfinite(data[k]) for k in ("gyro_z_rad_s","acc_x_m_s2","acc_y_m_s2","acc_z_m_s2")):
                raise RosCaptureHold("nonfinite IMU values")
            self._row("imu",stamp,data)
        except (RosCaptureHold,ValueError,TypeError,AttributeError) as e:
            self.errors.append({"domain":"imu","error":str(e)})
            raise

    def close(self):
        if self.closed:return
        self.closed=True
        for name,fp in self.file.items():
            fp.flush();os.fsync(fp.fileno());fp.close()
        files={}
        for name in ("odom","imu"):
            temporary=self.out/f"{name}.csv.partial"
            if self.errors or not self.count[name]:
                # Preserve .partial when capture is not a complete valid source.
                files[name]={"partial":str(temporary.name),"rows":self.count[name]}
                continue
            final=self.out/f"{name}.csv"
            os.replace(temporary,final)
            files[name]={"sha256":_sha_file(final),"bytes":final.stat().st_size,
                         "rows":self.count[name]}
        receipt={"schema":"mode8.ros2-capture.v1",
                 "status":"CAPTURED_UNSYNCHRONIZED" if not self.errors and all(self.count.values()) else "INCOMPLETE",
                 "counts":self.count,"files":files,"errors":self.errors,
                 "rf_clock_synchronized":False,
                 "requires_external_sync_events":True}
        path=self.out/"ros_capture_receipt.json"
        with path.open("x",encoding="utf-8") as fp:
            json.dump(receipt,fp,indent=2);fp.flush();os.fsync(fp.fileno())
        return receipt

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out",type=Path,required=True)
    p.add_argument("--odom-topic",default="/odom")
    p.add_argument("--imu-topic",default="/imu/data")
    p.add_argument("--duration",type=float,default=0,help="seconds; 0 means until Ctrl+C")
    args=p.parse_args()
    if args.duration<0: p.error("duration must be >=0")
    try:
        import rclpy
        from rclpy.node import Node
        from nav_msgs.msg import Odometry
        from sensor_msgs.msg import Imu
    except ImportError as e:
        p.error(f"ROS2 rclpy/nav_msgs/sensor_msgs unavailable; source must run on Pi ROS2 Humble: {e}")

    rec=Recorder(args.out)
    rclpy.init()
    class Mode8RecorderNode(Node):
        def __init__(self):
            super().__init__("mode8_ros_capture")
            self.create_subscription(Odometry,args.odom_topic,rec.odom,20)
            self.create_subscription(Imu,args.imu_topic,rec.imu,50)
            if args.duration:
                self.create_timer(args.duration,lambda:rclpy.shutdown())
    node=Mode8RecorderNode()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:pass
    finally:
        node.destroy_node()
        if rclpy.ok():rclpy.shutdown()
        verdict=rec.close()
        print(json.dumps(verdict,indent=2))
    return 0 if verdict["status"]=="CAPTURED_UNSYNCHRONIZED" else 2

if __name__=="__main__":
    raise SystemExit(main())
