"""Inspect displayed trajectories and recorded target-entry times, read-only."""
import argparse,csv,io,json,math,statistics,subprocess
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
ROOT=Path(__file__).resolve().parents[1]
def rows(path):
    with path.open(newline='') as f:return list(csv.DictReader(f))
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session',type=Path);parser.add_argument('--profile',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path);a=parser.parse_args()
    if a.output.exists():parser.error('Output must be a new directory')
    meta=json.loads((a.session/'session.json').read_text());trials=rows(a.session/'trials.csv')
    timing={r['trial']:r for r in rows(a.session/'timing.csv')};packets={t['trial']:[] for t in trials}
    for r in rows(a.session/'packets.csv'):packets[r['trial']].append(r)
    records=[];trajectories={};a.output.mkdir(parents=True)
    for t in trials:
        if t['outcome'] not in ('hit','miss'):continue
        p=packets[t['trial']]
        cmd=[str(ROOT/'build/Release/replay.exe'),'--csv-diag']
        if t['condition']=='candidate':cmd+=['--profile',str(a.profile),'--dpi',str(meta['dpi']),'--sensitivity',str(meta['reference_sensitivity'])]
        result=subprocess.run(cmd,input=''.join(f"{s['dx']},{s['dy']}\n" for s in p),capture_output=True,text=True)
        if result.returncode:raise RuntimeError(result.stderr)
        outputs=list(csv.DictReader(io.StringIO(result.stdout)))
        if len(outputs)!=len(p):raise ValueError('Replay sample count mismatch')
        tx,ty=int(t['target_x']),int(t['target_y']);scale=meta['pixels_per_count'];x=y=0
        points=[(0,0)];entries=[];inside=False;first_inside=None;last_entry=None
        for s,r in zip(p,outputs):
            x+=int(r['out_x']);y+=int(r['out_y']);points.append((x*scale,y*scale))
            now_inside=math.hypot(x-tx,y-ty)*scale<=meta['target_radius_px']
            if now_inside and not inside:
                last_entry=int(s['time_us']);entries.append(last_entry)
                if first_inside is None:first_inside=last_entry
            inside=now_inside
        if (x,y)!=(int(t['cursor_endpoint_x']),int(t['cursor_endpoint_y'])):raise ValueError('Displayed trajectory does not reconcile')
        duration=int(t['duration_us'])
        records.append(dict(trial=t['trial'],condition=t['condition'],angle=float(t['angle_degrees']),distance=int(t['distance']),
            outcome=t['outcome'],duration_ms=duration/1000,entries=len(entries),
            first_entry_to_click_ms=(duration-first_inside)/1000 if first_inside is not None else None,
            final_entry_to_click_ms=(duration-last_entry)/1000 if inside and last_entry is not None else None))
        trajectories[t['trial']]=points
    font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',14)
    for page in range(math.ceil(len(records)/24)):
        image=Image.new('RGB',(1800,1280),'white');draw=ImageDraw.Draw(image)
        draw.text((15,10),'Displayed paths | gray full, blue timing candidate, orange target, black click | coordinates normalized to target distance',fill='black',font=font)
        for j,r in enumerate(records[page*24:(page+1)*24]):
            t=next(t for t in trials if t['trial']==r['trial']);points=trajectories[r['trial']]
            tx=int(t['target_x'])*meta['pixels_per_count'];ty=int(t['target_y'])*meta['pixels_per_count'];k=95/math.hypot(tx,ty)
            ox=j%6*300;oy=55+j//6*300;cx=ox+150;cy=oy+165
            pts=[(cx+x*k,cy+y*k) for x,y in points]
            draw.text((ox+8,oy),f"{r['trial']} {r['angle']:g}° {r['condition']}",fill='black',font=font)
            lag=f"{r['final_entry_to_click_ms']:.0f}ms inside before click" if r['final_entry_to_click_ms'] is not None else 'click outside target'
            draw.text((ox+8,oy+21),f"{r['outcome']} / {lag}",fill='black',font=font)
            draw.line((cx-130,cy,cx+130,cy),fill='#eeeeee');draw.line((cx,cy-130,cx,cy+130),fill='#eeeeee')
            draw.line(pts,fill='#aaaaaa',width=2)
            phase=timing[r['trial']]
            if phase['identified']=='1':
                segment=pts[int(phase['begin']):int(phase['end'])+1]
                if len(segment)>1:draw.line(segment,fill='#1768ac',width=2)
            for x,y,color,size in [(cx+tx*k,cy+ty*k,'#ed9b40',5),(*pts[-1],'black',3)]:draw.ellipse((x-size,y-size,x+size,y+size),fill=color)
        image.save(a.output/f'paths-{page+1}.png')
    (a.output/'target-entry.json').write_text(json.dumps(records,indent=2))
    for c in ('baseline','candidate'):
        sub=[r for r in records if r['condition']==c and r['outcome']=='hit']
        print(c,'mean final-entry-to-click ms',round(statistics.mean(r['final_entry_to_click_ms'] for r in sub),1),
            'median',round(statistics.median(r['final_entry_to_click_ms'] for r in sub),1),
            'multiple entries',sum(r['entries']>1 for r in sub),'/',len(sub))
if __name__=='__main__':main()
