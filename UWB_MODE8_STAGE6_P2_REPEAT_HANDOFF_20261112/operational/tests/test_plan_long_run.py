"""Adversarial route plan generator: never claim segmented work is gap-free."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"host"))
from ops_plan_long_run import make_plan,save_new
from ops_route import validate_plan

class RoutePlanTests(unittest.TestCase):
    def test_1000_observations_are_32_plus_short_last_point(self):
        p=make_plan(937,1000)
        self.assertEqual(len(p["points"]),32)
        self.assertEqual(sum(x["count"] for x in p["points"]),1000)
        self.assertEqual(p["points"][-1]["count"],8)
        self.assertEqual(validate_plan(p),p)
        self.assertEqual(len({x["point_id"] for x in p["points"]}),32)
    def test_one_point_and_maximum_count(self):
        self.assertEqual(make_plan(1,1)["points"][0]["count"],1)
        self.assertEqual(len(make_plan(1,320000)["points"]),10000)
    def test_invalid_count_or_prefix_fails_before_rf(self):
        for kw in ({"route_id":0,"total":20},
                   {"route_id":2**32,"total":20},
                   {"route_id":5,"total":0},
                   {"route_id":5,"total":320001},
                   {"route_id":5,"total":20,"per_point":33},
                   {"route_id":5,"total":20,"prefix":"../bad"},
                   {"route_id":5,"total":20,"prefix":"A"*30}):
            with self.subTest(kw=kw),self.assertRaises(ValueError):
                make_plan(**kw)
    def test_original_plan_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"plan.json"
            p=make_plan(13,35)
            save_new(path,p)
            prior=path.read_bytes()
            with self.assertRaises(FileExistsError):
                save_new(path,make_plan(14,35))
            self.assertEqual(prior,path.read_bytes())
            self.assertEqual(json.loads(prior),p)

if __name__=="__main__":
    unittest.main()
