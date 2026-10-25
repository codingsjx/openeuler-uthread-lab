"""Fixture-only checks for data validation and safe report serialization."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from benchmark import distribution, read_trace
from make_report import embed_json, collect

class ReportingTests(unittest.TestCase):
    def test_known_quartiles(self):
        result = distribution([10,20,30,40,50])
        self.assertEqual((result['median'],result['q1'],result['q3']), (30,20,40))
    def test_script_terminator_is_not_executable(self):
        original = {'label': '</script><script>alert(1)</script>&'}
        encoded = embed_json(original)
        self.assertNotIn('<',encoded)
        self.assertEqual(json.loads(encoded),original)
    def test_bad_trace_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'bad.csv'
            path.write_text('time_us,tid,event,detail\n10,0,ready,spawn\n5,0,running,dispatch\n',encoding='utf-8')
            with self.assertRaises(ValueError): read_trace(path)
            path.write_text('time_us,tid,event,detail\n10,-1,dropped,9\n',encoding='utf-8')
            with self.assertRaises(ValueError): read_trace(path)
    def test_failed_measurements_are_not_reported_as_passed(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'benchmark.json'
            path.write_text('{"status":"failed"}',encoding='utf-8')
            with self.assertRaises(ValueError): collect(Path(directory),path)

if __name__ == '__main__':
    unittest.main()
