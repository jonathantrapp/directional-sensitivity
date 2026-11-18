"""Read-only packet audit and contact sheets. Outputs are separate from recordings."""
import csv
import hashlib
import json
import math
from pathlib import Path
from collections import Counter, defaultdict
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'sessions/baseline-002'
REVIEW = ROOT / 'sessions/baseline-002-review-final'
OUT = ROOT / 'build/baseline-002-audit'
OUT.mkdir(parents=True, exist_ok=True)
def rows(path):
    with path.open(newline='') as f:
        return list(csv.DictReader(f))
trials = rows(SOURCE / 'trials.csv')
reviews = {r['trial']: r for r in rows(REVIEW / 'review.csv')}
packets = defaultdict(list)
for r in rows(SOURCE / 'packets.csv'):
    packets[r['trial']].append({k: int(v) for k,v in r.items() if k != 'trial'})
manifest = json.loads((SOURCE / 'session.json').read_text())
scale = manifest['pixels_per_count']
checks, details, traces = [], [], {}
for t in trials:
    name = t['trial']; p = packets[name]; r = reviews[name]
    tx,ty = int(t['target_x']),int(t['target_y']); length = math.hypot(tx,ty)
    xs,ys,ts,along,cross = [0],[0],[0.0],[0.0],[0.0]
    x=y=0
    gaps=[]; last_motion=None; first_motion=None; flags=Counter(); buttons=Counter()
    for i,s in enumerate(p):
        flags[s['flags']]+=1; buttons[s['buttons']]+=1
        if s['dx'] or s['dy']:
            first_motion = s['time_us'] if first_motion is None else first_motion
            if last_motion is not None: gaps.append(s['time_us']-last_motion)
            last_motion=s['time_us']
        if s['flags'] & 5:
            checks.append(f'{name}: unsupported packet {i}')
        else:
            x+=s['dx']; y+=s['dy']
        xs.append(x*scale); ys.append(y*scale); ts.append(s['time_us']/1000)
        along.append((x*tx+y*ty)/length)
        cross.append((x*ty-y*tx)/length)
    checks.extend(f'{name}: {label}' for label,ok in [
        ('packet count',len(p)==int(t['packet_count'])),
        ('sample order',all(s['sample']==i for i,s in enumerate(p))),
        ('timestamp order',all(b['time_us']>=a['time_us'] for a,b in zip(p,p[1:]))),
        ('endpoint',x==int(t['endpoint_x']) and y==int(t['endpoint_y'])),
        ('first paint',int(t['target_painted'])==1),
        ('no prepaint movement',all(s['time_us']>=int(t['first_paint_us']) for s in p if s['dx'] or s['dy'])),
        ('end after last sample',not p or p[-1]['time_us']<=int(t['duration_us'])),
        ('click termination',bool(p) and bool(p[-1]['buttons']&1)),
    ] if not ok)
    end=int(r['end']); begin=int(r['begin'])
    if int(r['eligible']):
        prefix=p[begin:end]
        if sum(s['dx'] for s in prefix)!=int(r['x']) or sum(s['dy'] for s in prefix)!=int(r['y']):
            checks.append(f'{name}: review endpoint mismatch')
    error=math.hypot(x-tx,y-ty)*scale
    if (error<=manifest['target_radius_px']) != (t['outcome']=='hit'):
        checks.append(f'{name}: hit classification')
    onset=next((i for i,a in enumerate(along) if a>=4),len(along)-1)
    early_min=min(along[:onset+1])
    # Descriptive landmark only: not used to replace labels or recommend gains.
    tenth=next((i for i,a in enumerate(along) if a>=length*.1),len(along)-1)
    detail=dict(trial=name,direction=t['direction'],split=t['split'],outcome=t['outcome'],
        eligible=bool(int(r['eligible'])),reason=r['reason'],boundary=r['boundary'],
        endpoint_error_px=error,final_progress=along[-1]/length,
        duration_ms=int(t['duration_us'])/1000,first_motion_ms=(first_motion or 0)/1000,
        early_backward_px=-min(0,early_min)*scale,
        ten_percent_time_ms=ts[tenth],max_cross_track_px=max(map(abs,cross))*scale,
        path_to_target_ratio=float(t['path_counts'])/length,
        max_movement_gap_ms=max(gaps,default=0)/1000,
        segment_progress=along[end]/length if int(r['eligible']) else None,
        segment_to_click_px=math.hypot(xs[-1]-xs[end],ys[-1]-ys[end]) if int(r['eligible']) else None,
        flags=dict(flags),buttons=dict(buttons))
    details.append(detail); traces[name]=(xs,ys,ts,along,tx*scale,ty*scale)

# Contact sheets use original packet paths, never resampled or smoothed.
font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',18)
small=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',14)
def line(draw, points, color, width=2):
    if len(points)>1: draw.line(points,fill=color,width=width)
for page in range(4):
    image=Image.new('RGB',(1800,1280),'white'); draw=ImageDraw.Draw(image)
    draw.text((20,10),f'Baseline 002: trials {page*24+1}-{(page+1)*24} | gray full path, blue selected prefix, orange target, black click',fill='black',font=font)
    for j,(t,d) in enumerate(zip(trials[page*24:(page+1)*24],details[page*24:(page+1)*24])):
        ox=(j%6)*300; oy=55+(j//6)*300
        xs,ys,ts,a,tx,ty=traces[t['trial']]; radius=math.hypot(tx,ty)
        cx,cy=ox+150,oy+160; factor=95/radius
        pts=[(cx+x*factor,cy+y*factor) for x,y in zip(xs,ys)]
        draw.rectangle((ox+5,oy+30,ox+295,oy+290),outline='#eeeeee')
        draw.line((cx-125,cy,cx+125,cy),fill='#eeeeee'); draw.line((cx,cy-125,cx,cy+125),fill='#eeeeee')
        line(draw,pts,'#999999')
        if d['eligible']: line(draw,pts[:int(reviews[t['trial']]['end'])+1],'#1768ac')
        for x,y,color,size in [(cx,cy,'#444444',2),(cx+tx*factor,cy+ty*factor,'#ed9b40',6),(*pts[-1],'black',3)]:
            draw.ellipse((x-size,y-size,x+size,y+size),fill=color)
        label='candidate' if d['eligible'] else ('cardinal' if int(t['target_x'])*int(t['target_y'])==0 else 'flagged start')
        draw.text((ox+10,oy+2),f"{t['trial'][6:]} {d['direction']} {d['outcome']} | {label}",fill='black',font=small)
    image.save(OUT/f'paths-{page+1}.png')
flagged=[d for d in details if d['reason']=='wrong_direction_start']
image=Image.new('RGB',(1800,1500),'white'); draw=ImageDraw.Draw(image)
draw.text((20,10),'All 12 flagged starts: complete progress toward target vs time (blue); target orange; -2px rejection red',fill='black',font=font)
for j,d in enumerate(flagged):
    xs,ys,ts,a,tx,ty=traces[d['trial']]; radius=math.hypot(tx,ty)
    ox=(j%3)*600; oy=55+(j//3)*355
    left,right,top,bottom=ox+55,ox+580,oy+45,oy+290
    low=min(-8,min(a)*scale-4); high=max(radius,max(a)*scale)+10
    def pos(t,v): return (left+t/max(ts[-1],1)*(right-left),bottom-(v-low)/(high-low)*(bottom-top))
    for value,color in [(0,'#cccccc'),(radius,'#ed9b40'),(-4*scale,'#c33')]:
        line(draw,[pos(0,value),pos(ts[-1],value)],color,1)
    line(draw,[pos(t,v*scale) for t,v in zip(ts,a)],'#1768ac')
    draw.text((ox+15,oy),f"{d['trial']} {d['direction']} | early backward {d['early_backward_px']:.1f}px | {d['outcome']}",fill='black',font=small)
    draw.text((left,bottom+8),f"0 to {ts[-1]:.0f} ms; vertical {low:.0f} to {high:.0f} client px",fill='black',font=small)
image.save(OUT/'flagged-starts.png')
summary=dict(trials=len(trials),packets=sum(map(len,packets.values())),integrity_issues=checks,
    outcomes=dict(Counter(d['outcome'] for d in details)),
    boundaries=dict(Counter(d['boundary'] for d in details if d['eligible'])),
    source_sha256={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in SOURCE.glob('*') if p.is_file()},
    details=details)
(OUT/'audit.json').write_text(json.dumps(summary,indent=2))
report = '''# Baseline 002: inspection of every trial

Reviewed 2026-10-23. All 96 recorded paths were inspected visually, together with packet consistency checks and the 12 flagged progress-over-time plots. This is an inspection of the existing run, not a new calibration or proof of aiming improvement. Original session and review files were not modified.

## Findings

- All 27,880 packet rows reconcile with the 96 trial records. Packet indices and timestamps are ordered; reconstructed endpoints and hit classifications match. All trials have a target-paint marker, no recorded movement before that marker, and a terminating left-click report. No absolute/attribute-change flags were present. This verifies the saved stream, not sensor delivery or real display timing.
- All 96 trials are useful as performance observations: 87 hits, 9 misses. The 48 horizontal/vertical targets are baseline diagnostics, not discarded recordings. Their paths can contain diagonal packets, so target direction alone does not establish zero gain exposure.
- Of 48 diagonal targets, 36 currently qualify as candidate prefixes. The 12 flagged trials ALL ended in hits and all contain a clear later forward movement. They are not worthless or corrupted. Their early backward projection ranges from 2.12 to 45.96 client pixels; these are genuinely different paths, not uniformly one-count jitter.
- Three flagged trials (52, 74, 76) fall close to the current 2-pixel rejection threshold. Two (34, 73) retreat about 6–7 pixels. Seven retreat more than 10 pixels. The binary rule is sensitive near its threshold and hides this distinction.
- The retained prefixes reach 83.1–117.1% of target distance along its direction. The old tiny-fragment problem is not present in this run under v2. However, neither distance nor reaching a hit certifies an isolated ballistic flick.
- Sixteen candidates end at click, 19 at a movement gap, and one at cumulative reversal. Nine selected endpoints differ from the final click by more than 10 pixels. This can be an appropriate separation of initial movement and correction; it is not automatically a bug. Curved paths and gradual corrections mean the existing rule still cannot reliably label a human's intended first flick in every trial.
- The exclusions are directionally uneven: NE loses six trials, NW five, SW one, SE none. Any profile fitted only to retained traces could inherit this selection bias. The current minimum-coverage gate correctly prevents a complete profile.

The recording does not reveal WHY the backward movement happened. Recentring, hand preparation, target response, or another cause cannot be distinguished with certainty from these counts. Do not relabel a later forward stroke as if it started at the original center: its starting offset matters.

## What should change next

Keep all trials in performance summaries and distinguish “recorded trial,” “identified movement segment,” and “eligible fitting sample” in the UI. Replace the single usable/unusable presentation with explicit start/end/offset and confidence/reason annotations. Review low-motion preparation separately from substantial backward excursions, with fixed rules tested independently of which rule produces nicer gains. A stationary-ready cue before arm would help distinguish preparation from target response in future sessions. Keep the 12 existing paths available for a separate exploratory analysis, without silently treating them as clean flicks or weakening validation to force a profile.

No gains were recommended/applied and no driver changes were made during this inspection. You do not need to repeat the 96-trial run to retain or inspect this information.

## Visual evidence

- [Trials 1–24](../build/baseline-002-audit/paths-1.png)
- [Trials 25–48](../build/baseline-002-audit/paths-2.png)
- [Trials 49–72](../build/baseline-002-audit/paths-3.png)
- [Trials 73–96](../build/baseline-002-audit/paths-4.png)
- [All 12 flagged starts over time](../build/baseline-002-audit/flagged-starts.png)

Gray is the full path, blue the selected prefix, orange the target, black the click. Path sheets use target-distance-normalized coordinates, positive Y down. Progress plots use client pixels projected along the target and application-dispatch time from arm; they do not show sensor or display latency.

## Each flagged trial

Backward distance is the greatest negative projection before forward onset, not total path length. Every row below ended in a hit.

| Trial | Direction | Split | Early backward projection (px) |
| --- | --- | --- | ---: |
'''
for d in flagged:
    report+=f"| {d['trial']} | {d['direction']} | {d['split']} | {d['early_backward_px']:.2f} |\n"
report+='''
## Every trial: outcome and current segment decision

Progress is selected endpoint projection divided by target distance. A dash means no selected candidate, not missing recorded data. These are the existing v2 decisions; visual inspection does not turn them into ground-truth onset labels.

| Trial | Direction | Outcome | Decision / boundary | Selected progress | Final error (px) |
| --- | --- | --- | --- | ---: | ---: |
'''
for d in details:
    decision=d['boundary'] if d['eligible'] else ('cardinal diagnostic' if d['reason']=='cardinal_target_diagnostic_only' else 'flagged backward start')
    progress=f"{100*d['segment_progress']:.1f}%" if d['eligible'] else '—'
    report+=f"| {d['trial']} | {d['direction']} | {d['outcome']} | {decision} | {progress} | {d['endpoint_error_px']:.2f} |\n"
report+='''
Sources: original `sessions/baseline-002/session.json`, `trials.csv`, `packets.csv`, and derived `sessions/baseline-002-review-final/review.csv`. Machine-readable per-trial measurements and SHA-256 source hashes are in [audit.json](../build/baseline-002-audit/audit.json). The audit script is read-only with respect to recordings.
'''
(ROOT/'docs/baseline-002-inspection.md').write_text(report,encoding='utf-8')
# Confirm the plotting/report pass did not mutate any source file.
for name,digest in summary['source_sha256'].items():
    assert hashlib.sha256((SOURCE/name).read_bytes()).hexdigest()==digest
print(json.dumps({k:v for k,v in summary.items() if k!='details'},indent=2))
print('FLAGGED',json.dumps(flagged,indent=2))
print('ACCEPTED CUTOFFS',json.dumps([d for d in details if d['eligible'] and (d['segment_to_click_px']>10 or not .8<=d['segment_progress']<=1.2)],indent=2))
