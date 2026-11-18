"""Reconcile comparison packets with the exact displayed fixed-point endpoints."""
import argparse
import csv
import hashlib
import io
import json
import math
import subprocess
import sys
from collections import Counter,defaultdict
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
def rows(path):
    with path.open(newline='') as f:return list(csv.DictReader(f))
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('sessions',nargs='+',type=Path);p.add_argument('--output',required=True,type=Path)
    args=p.parse_args()
    if args.output.exists():p.error('Output directory must be new')
    args.output.mkdir(parents=True);reports=[]
    for source in args.sessions:
        meta=json.loads((source/'session.json').read_text())
        if meta['mode']!='randomized_profile_comparison':raise ValueError('Not a comparison recording')
        original_hashes={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in source.iterdir() if f.is_file()}
        trials=rows(source/'trials.csv'); packets=defaultdict(list)
        for r in rows(source/'packets.csv'):packets[r['trial']].append(r)
        prefix=args.output/source.name;prefix.mkdir()
        profile=prefix/'recorded-profile.dsp'
        text=f"directional-profile=1\ndpi={meta['dpi']}\nreference_sensitivity={meta['reference_sensitivity']}\n"
        for q,values in zip(('ne','se','sw','nw'),meta['candidate_gains_q16']):
            if len(values)!=2 or any(type(v)!=int for v in values):raise ValueError('Invalid recorded gains')
            text+=q+'='+','.join(str(v/65536) for v in values)+'\n'
        profile.write_text(text)
        replay_path=prefix/'completed-traces.csv'; issues=[];outcomes=Counter(); displayed_changes=Counter()
        with replay_path.open('w',newline='') as f:
            w=csv.writer(f);w.writerow(('trial','split','target_x','target_y','dx','dy'))
            for t in trials:
                ps=packets[t['trial']];x=y=0;last=-1;outcomes[t['outcome']]+=1
                for i,s in enumerate(ps):
                    stamp=int(s['time_us']);dx,dy=int(s['dx']),int(s['dy'])
                    if int(s['sample'])!=i or stamp<last:issues.append(t['trial']+': packet/time order')
                    if dx or dy:
                        if stamp<int(t['first_paint_us']):issues.append(t['trial']+': pre-paint movement')
                    last=stamp
                    if not int(s['flags'])&5:x+=dx;y+=dy
                    if t['outcome'] in ('hit','miss'):w.writerow((t['trial'],'validation',t['target_x'],t['target_y'],dx,dy))
                if len(ps)!=int(t['packet_count']) or (x,y)!=(int(t['endpoint_x']),int(t['endpoint_y'])):
                    issues.append(t['trial']+': count/endpoint mismatch')
                if t['outcome'] not in ('hit','miss'):continue
                if not ps or not int(ps[-1]['buttons'])&1:issues.append(t['trial']+': missing final click')
                cx,cy=int(t['cursor_endpoint_x']),int(t['cursor_endpoint_y'])
                error=math.hypot(cx-int(t['target_x']),cy-int(t['target_y']))*meta['pixels_per_count']
                if (error<=meta['target_radius_px'])!=(t['outcome']=='hit'):issues.append(t['trial']+': displayed hit mismatch')
                if (cx,cy)!=(x,y):displayed_changes[t['condition']+'/'+t['direction']]+=1
        score=subprocess.run([str(ROOT/'build/Release/score_profile.exe'),str(profile),str(replay_path),
            str(meta['dpi']),str(meta['reference_sensitivity'])],capture_output=True,text=True)
        if score.returncode:raise RuntimeError(score.stderr)
        by_id={t['trial']:t for t in trials}
        for r in csv.DictReader(io.StringIO(score.stdout)):
            t=by_id[r['trial']];kind='candidate' if t['condition']=='candidate' else 'baseline'
            if (int(r[kind+'_x']),int(r[kind+'_y']))!=(int(t['cursor_endpoint_x']),int(t['cursor_endpoint_y'])):
                issues.append(t['trial']+': exact processor replay mismatch')
            if int(r['overflow_packets']):issues.append(t['trial']+': replay overflow')
        (prefix/'endpoint-replay.csv').write_text(score.stdout)
        result=subprocess.run([sys.executable,str(ROOT/'tools/compare_summary.py'),str(source),
            '--output',str(prefix/'summary.json')],capture_output=True,text=True)
        if result.returncode:raise RuntimeError(result.stderr)
        summary=json.loads((prefix/'summary.json').read_text())
        for name,digest in original_hashes.items():
            if hashlib.sha256((source/name).read_bytes()).hexdigest()!=digest:raise ValueError('Source file changed')
        report=dict(session=str(source),packets=sum(map(len,packets.values())),outcomes=dict(outcomes),
            integrity_issues=issues,changed_endpoint_counts=dict(displayed_changes),source_sha256=original_hashes,summary=summary)
        reports.append(report)
        print(source.name,'packets',report['packets'],'issues',issues,'changed endpoints',dict(displayed_changes))
        print('NW paired:',summary.get('paired_by_direction',{}).get('NW'))
    (args.output/'audit.json').write_text(json.dumps(reports,indent=2))
if __name__=='__main__':main()
