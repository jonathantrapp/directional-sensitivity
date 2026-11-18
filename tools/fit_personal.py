"""Fit an explainable model on earlier sessions; assess a later session once.

Uses the compiled fixed-point processor for scoring. Never installs a driver,
changes an input device, rewrites recordings, or silently applies the profile.
"""
import argparse
import csv
import hashlib
import io
import json
import math
import random
import statistics
import subprocess
from collections import Counter, defaultdict
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
def read_csv(path):
    with path.open(newline='') as f: return list(csv.DictReader(f))
def run(tool,*args):
    return subprocess.run([str(ROOT/'build/Release'/f'{tool}.exe'),*map(str,args)],capture_output=True,text=True,check=False)
def quadrant(x,y): return ('NE' if x>0 else 'NW') if y<0 else ('SE' if x>0 else 'SW')
def bootstrap(values):
    rng=random.Random(20261023)
    samples=sorted(statistics.mean(rng.choices(values,k=len(values))) for _ in range(10000))
    return [samples[249],samples[9749]]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--train',nargs='+',required=True,type=Path)
    parser.add_argument('--test',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    sources=[p.resolve() for p in args.train]+[args.test.resolve()]
    if len(set(sources))!=len(sources): parser.error('Training and test sessions must be distinct')
    if args.output.exists(): parser.error('Output directory must be new')
    manifests=[json.loads((p/'session.json').read_text()) for p in sources]
    setup_keys=('dpi','reference_sensitivity','pixels_per_count','device_path','baseline_mapping')
    for manifest in manifests:
        if manifest['synthetic'] or manifest['mode']!='identity_only': parser.error('Only live identity recordings supported')
        if any(manifest[k]!=manifests[0][k] for k in setup_keys): parser.error('Recording setups do not match')
    args.output.mkdir(parents=True)
    hashes={str(p):{f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in p.iterdir() if f.is_file()} for p in sources}
    dpi=manifests[0]['dpi']; sensitivity=manifests[0]['reference_sensitivity']
    input_path=args.output/'model-trials.csv'
    counts=defaultdict(Counter); outcomes={}; all_test=[]
    with input_path.open('w',newline='') as f:
        writer=csv.writer(f); writer.writerow(('trial','split','target_x','target_y','dx','dy'))
        for n,p in enumerate(sources):
            review=args.output/f'source-{n+1}-review'
            result=run('review_capture',p,review)
            if result.returncode: raise RuntimeError(result.stderr)
            rows=read_csv(review/'candidate-trials.csv')
            split='validation' if n==len(sources)-1 else 'train'
            seen=set()
            for row in rows:
                # IDs must remain unique even when captures share trial numbering.
                writer.writerow((f's{n+1}_{row["trial"]}',split,row['target_x'],row['target_y'],row['dx'],row['dy']))
                if row['trial'] not in seen:
                    seen.add(row['trial']); counts[split][quadrant(int(row['target_x']),int(row['target_y']))]+=1
            trials={t['trial']:t for t in read_csv(p/'trials.csv')}
            outcomes[str(p)] = dict(Counter(t['outcome'] for t in trials.values()))
            if split=='validation':
                for packet in read_csv(p/'packets.csv'):
                    t=trials[packet['trial']]
                    if t['outcome'] in ('hit','miss'):
                        all_test.append((packet['trial'],'validation',t['target_x'],t['target_y'],packet['dx'],packet['dy']))
    fit=run('calibrate',input_path,dpi,sensitivity)
    (args.output/'fit-log.txt').write_text(fit.stderr)
    report=dict(model='robust_median_four_linked_diagonal_gains',
        training_sessions=[str(p) for p in sources[:-1]],test_session=str(sources[-1]),
        counts={k:dict(v) for k,v in counts.items()},outcomes=outcomes,
        setup={k:manifests[0][k] for k in setup_keys if k!='device_path'},source_sha256=hashes,
        validation_role='later session used for model gating; no longer an untouched future test',
        human_benefit_proven=False,fit_exit_code=fit.returncode,fit_log=fit.stderr)
    if fit.returncode==0:
        report['decision']='Experimental candidate for app-local randomized comparison only; replay is not proof of improved human aim.'
        profile=args.output/'candidate.dsp'; profile.write_text(fit.stdout)
        report['profile']={k:v for k,v in (line.split('=',1) for line in fit.stdout.splitlines())}
        full_path=args.output/'full-test-trials.csv'
        with full_path.open('w',newline='') as f:
            w=csv.writer(f); w.writerow(('trial','split','target_x','target_y','dx','dy')); w.writerows(all_test)
        for label,path in [('selected_prefixes',input_path),('full_test_traces',full_path)]:
            score=run('score_profile',profile,path,dpi,sensitivity)
            if score.returncode: raise RuntimeError(score.stderr)
            (args.output/f'{label}-scores.csv').write_text(score.stdout)
            measurements=defaultdict(list)
            for row in csv.DictReader(io.StringIO(score.stdout)):
                if label=='selected_prefixes' and not row['trial'].startswith(f's{len(sources)}_'): continue
                tx,ty=int(row['target_x']),int(row['target_y']); norm=tx*tx+ty*ty
                group=quadrant(tx,ty) if tx and ty else 'cardinal_targets'
                b=((int(row['baseline_x'])-tx)**2+(int(row['baseline_y'])-ty)**2)/norm
                c=((int(row['candidate_x'])-tx)**2+(int(row['candidate_y'])-ty)**2)/norm
                if int(row['overflow_packets']): raise RuntimeError('Overflow in scoring')
                measurements[group].append((b,c))
            report[label]={q:dict(n=len(vals),baseline_relative_rmse=math.sqrt(statistics.mean(b for b,c in vals)),
                candidate_relative_rmse=math.sqrt(statistics.mean(c for b,c in vals)),
                mean_paired_squared_error_improvement=statistics.mean(b-c for b,c in vals),
                bootstrap_95pct_mean_improvement=bootstrap([b-c for b,c in vals])) for q,vals in measurements.items()}
    else:
        report['decision']='No validated candidate exported; do not weaken coverage or tune on test outcomes.'
    for p in sources:
        for name,digest in hashes[str(p)].items():
            if hashlib.sha256((p/name).read_bytes()).hexdigest()!=digest: raise RuntimeError('Source changed during analysis')
    (args.output/'model-report.json').write_text(json.dumps(report,indent=2))
    print(fit.stderr)
    print(json.dumps({k:v for k,v in report.items() if k in ('counts','selected_prefixes','full_test_traces','decision')},indent=2))

if __name__=='__main__': main()
