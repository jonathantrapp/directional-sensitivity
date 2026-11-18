"""Summarize a randomized app-local comparison, keeping misses in the score."""
import argparse
import csv
import json
import random
import statistics
from collections import Counter, defaultdict
from pathlib import Path

def condition_stats(values):
    hits=[time for hit,time,score in values if hit]
    return {'n':len(values),'hit_rate':sum(hit for hit,time,score in values)/len(values),
        'mean_hit_duration_ms':statistics.mean(hits)/1000 if hits else None,
        'mean_deadline_penalized_ms':statistics.mean(score for hit,time,score in values)/1000}

def paired_stats(values):
    if not values: return {'n':0}
    if len(values)<2:
        return {'n':len(values),'mean_penalized_improvement_ms':statistics.mean(values),
            'descriptive_bootstrap_95pct_interval_ms':None,'limitation':'One matched pair cannot estimate variation.'}
    rng=random.Random(20261023)
    samples=sorted(statistics.mean(rng.choices(values,k=len(values))) for _ in range(10000))
    return {'n':len(values),'mean_penalized_improvement_ms':statistics.mean(values),
        'descriptive_bootstrap_95pct_interval_ms':[samples[249],samples[9749]]}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('session',type=Path); p.add_argument('--output',required=True,type=Path)
    args=p.parse_args()
    if args.output.exists(): p.error('Output must be a new file')
    meta=json.loads((args.session/'session.json').read_text())
    if meta['mode']!='randomized_profile_comparison': p.error('Not a comparison session')
    with (args.session/'trials.csv').open(newline='') as f: trials=list(csv.DictReader(f))
    groups=defaultdict(list); exclusions=Counter(); cells=defaultdict(dict)
    directions=defaultdict(lambda:defaultdict(list))
    angles=defaultdict(lambda:defaultdict(list)); cell_directions={}
    target_quadrants=defaultdict(lambda:defaultdict(list)); cell_quadrants={}
    for t in trials:
        if t['outcome'] not in ('hit','miss','timeout'):
            exclusions[t['outcome']]+=1; continue
        condition=t['condition']
        if condition not in ('baseline','candidate'): raise ValueError('Invalid condition')
        # Misses cannot win by clicking quickly. The penalty is explicit and
        # complements hit rate, not an estimate of unobserved correction time.
        penalty=int(t['duration_us']) if t['outcome']=='hit' else meta['timeout_us']
        groups[condition].append((t['outcome']=='hit',int(t['duration_us']),penalty))
        directions[t['direction']][condition].append(groups[condition][-1])
        if t.get('angle_degrees') is not None:
            angles[t['angle_degrees']][condition].append(groups[condition][-1])
        # Compass labels become sectors in wide-angle mode. Pair by the exact
        # target angle so adjacent angles cannot collide or get cross-matched.
        key=(t.get('angle_index',t['direction']),t['distance'],int(t['rep'])//2)
        cell_directions[key]=t['direction']
        if 'target_x' in t and 'target_y' in t:
            x,y=int(t['target_x']),int(t['target_y'])
            q='cardinal_targets' if not x or not y else ('NE' if x>0 else 'NW') if y<0 else ('SE' if x>0 else 'SW')
            target_quadrants[q][condition].append(groups[condition][-1]); cell_quadrants[key]=q
        if condition in cells[key]: raise ValueError('Duplicate condition in matched target pair')
        cells[key][condition]=penalty
    report={'session_complete':meta['session_complete'],'excluded_outcomes':dict(exclusions),
        'direction_grouping':'nearest_compass_sector' if meta.get('direction_count',8)>8 else 'exact_compass_target_direction',
        'penalty_definition':'hit duration, otherwise trial deadline; not actual time to a successful hit',
        'conditions':{},'human_improvement_established':False}
    for name,values in groups.items():
        report['conditions'][name]=condition_stats(values)
    report['by_direction']={d:{c:condition_stats(v) for c,v in conditions.items()} for d,conditions in directions.items()}
    if angles:
        report['by_target_angle_degrees']={d:{c:condition_stats(v) for c,v in conditions.items()} for d,conditions in angles.items()}
    if target_quadrants:
        report['by_target_quadrant']={q:{c:condition_stats(v) for c,v in conditions.items()} for q,conditions in target_quadrants.items()}
    timing_path=args.session/'timing.csv'
    if timing_path.exists():
        with timing_path.open(newline='') as f: timing={r['trial']:r for r in csv.DictReader(f)}
        timing_groups=defaultdict(list)
        for t in trials:
            a=timing.get(t['trial'])
            if t['outcome'] not in ('hit','miss') or not a or a['identified']!='1': continue
            onset,end=int(a['onset_us']),int(a['end_us'])
            timing_groups[t['condition']].append(((end-onset)/1000,
                max(0,onset-int(t['first_paint_us']))/1000,
                max(0,int(t['duration_us'])-end)/1000,int(a['epochs'])>1))
        report['raw_movement_timing']={name:{'n':len(v),
            'mean_primary_candidate_ms':statistics.mean(a for a,b,c,d in v),
            'mean_pre_candidate_ms':statistics.mean(b for a,b,c,d in v),
            'mean_post_candidate_ms':statistics.mean(c for a,b,c,d in v),
            'multiple_epoch_fraction':statistics.mean(d for a,b,c,d in v)} for name,v in timing_groups.items()}
        report['timing_limitation']='Raw-count dispatch-time heuristic; pre/post intervals include preparation and click waiting, not known reaction/correction latency.'
    differences=[(c['baseline']-c['candidate'])/1000 for c in cells.values() if len(c)==2]
    by_direction_differences=defaultdict(list)
    for key,c in cells.items():
        if len(c)==2: by_direction_differences[cell_directions[key]].append((c['baseline']-c['candidate'])/1000)
    report['paired_by_direction']={d:paired_stats(values) for d,values in by_direction_differences.items()}
    quadrant_differences=defaultdict(list)
    for key,c in cells.items():
        if len(c)==2 and key in cell_quadrants:
            quadrant_differences[cell_quadrants[key]].append((c['baseline']-c['candidate'])/1000)
    if quadrant_differences:
        report['paired_by_target_quadrant']={q:paired_stats(v) for q,v in quadrant_differences.items()}
        report['quadrant_limitation']='Target quadrant groups all its tested angles; actual packet exposure may cross quadrant boundaries.'
    report['matched_target_pairs']=len(differences)
    if differences:
        rng=random.Random(20261023)
        samples=sorted(statistics.mean(rng.choices(differences,k=len(differences))) for _ in range(10000))
        report['paired_mean_penalized_improvement_ms']=statistics.mean(differences)
        report['descriptive_bootstrap_95pct_interval_ms']=[samples[249],samples[9749]]
    report['decision']='Exploratory same-session evidence only; inspect direction-specific results and repeat on another day before adopting.'
    args.output.write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
if __name__=='__main__': main()
