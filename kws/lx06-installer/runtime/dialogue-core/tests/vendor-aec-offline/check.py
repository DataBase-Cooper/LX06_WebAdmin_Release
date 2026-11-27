import csv, sys
from pathlib import Path
p=Path(sys.argv[1])
assert p.exists(), 'matrix metrics not generated'
rows=list(csv.DictReader(p.open()))
expected={'aligned_far','offset_minus80','offset_plus80','drift_plus200ppm','drift_minus200ppm','gain_step','clip','near_only','doubletalk','path_step'}
assert {r['case'] for r in rows} == expected
assert all(int(r['frames']) == 300 and int(r['samples']) == 38400 for r in rows)
assert all(int(r['guard_frames']) == 240 for r in rows)
print('matrix contract passed:', len(rows), 'cases')
