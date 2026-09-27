import json,collections,os
PCB=os.environ.get('HAT_PCB',os.path.expanduser('~/Downloads/PCB_PCB_AOW-HAT_2026-09-26.json'))
U=0.254
def load_pcb():
    d=json.load(open(PCB))
    ox,oy=float(d['head']['x']),float(d['head']['y'])
    X=lambda x:(float(x)-ox)*U; Y=lambda y:-(float(y)-oy)*U
    parts={}
    for s in d['shape']:
        if not s.startswith('LIB'): continue
        P=s.split('#@$'); h=P[0].split('~')
        attrs=h[3].split('`'); a=dict(zip(attrs[0::2],attrs[1::2]))
        ref=[p.split('~')[10] for p in P if p.startswith('TEXT~P~')]
        ref=ref[0] if ref else 'NOREF_'+h[6]
        pads=[]
        for p in P:
            if p.startswith('PAD'):
                q=p.split('~')
                pads.append(dict(shape=q[1],x=X(q[2]),y=Y(q[3]),w=float(q[4])*U,h=float(q[5])*U,layer=q[6],net=q[7],num=q[8],hole=float(q[9] or 0)*2*U,rot=q[11]))
        sx=[];sy=[]
        for p in P:
            q=p.split('~')
            if q[0]=='TRACK' and q[2] in('3','4'):
                v=list(map(float,q[4].split())); sx+=[X(t) for t in v[0::2]]; sy+=[Y(t) for t in v[1::2]]
        xs=[p['x'] for p in pads]+sx or [X(h[1])]; ys=[p['y'] for p in pads]+sy or [Y(h[2])]
        parts[ref]=dict(c=(X(h[1]),Y(h[2])),rot=h[4],pads=pads,pkg=a.get('package'),layer=h[7] if len(h)>7 else '',box=(min(xs),max(xs),min(ys),max(ys)),attrs=a,raw=h)
    tracks=[];vias=[];holes=[]
    for s in d['shape']:
        q=s.split('~')
        if q[0]=='TRACK':
            v=list(map(float,q[4].split())); pts=[(X(v[i]),Y(v[i+1])) for i in range(0,len(v),2)]
            tracks.append(dict(w=float(q[1])*U,layer=q[2],net=q[3],pts=pts))
        elif q[0]=='VIA': vias.append(dict(x=X(q[1]),y=Y(q[2]),d=float(q[3])*U,net=q[4],hole=float(q[5])*2*U))
        elif q[0]=='HOLE': holes.append(dict(x=X(q[1]),y=Y(q[2]),d=float(q[3])*2*U))
    return d,parts,tracks,vias,holes
if __name__=='__main__':
    import sys
    sys.path.insert(0,'.')
    from sch import load_sch
    d,parts,tracks,vias,holes=load_pcb()
    sp,spins,_=load_sch()
    print('pcb parts',len(parts),'sch parts',len(sp))
    print('only pcb',sorted(set(parts)-set(sp)),'only sch',sorted(set(sp)-set(parts)))
    diff=[]
    for r,p in parts.items():
        for pad in p['pads']:
            k=(r,pad['num']); sn=spins.get(k)
            if sn is None:
                if pad['net']: diff.append((k,'pad not in sch',pad['net']))
                continue
            n=next(iter(sn['nets'])) if sn['nets'] else ''
            if n!=pad['net']: diff.append((k,'sch',n,'pcb',pad['net']))
    print('DIFFS',diff)
    for k in spins:
        if k[0] in parts and not any(p['num']==k[1] for p in parts[k[0]]['pads']): print('sch pin w/o pad',k)
    for r in sorted(parts): 
        if sp.get(r) and sp[r]['pkg']!=parts[r]['pkg']: print('pkg diff',r,sp[r]['pkg'],parts[r]['pkg'])
    lay=collections.Counter((t['layer'],round(t['w'],3)) for t in tracks); print(lay)
    print(collections.Counter(p['pads'][0]['layer'] if p['pads'] else None for p in parts.values()))
