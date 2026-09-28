import sys,math,collections,re,json
sys.path.insert(0,'.')
from pcb import load_pcb
d,parts,tracks,vias,holes=load_pcb()
U=0.254; ox,oy=float(d['head']['x']),float(d['head']['y'])
def paths(txt):
    # EasyEDA path text -> list of polygons in mm
    out=[]
    for sub in txt.split('M')[1:]:
        v=list(map(float,re.findall(r'-?[\d.]+',sub)))
        out.append([((v[i]-ox)*U,-(v[i+1]-oy)*U) for i in range(0,len(v)-1,2)])
    return out
def inpoly(p,poly):
    x,y=p;c=False
    for (x1,y1),(x2,y2) in zip(poly,poly[1:]+poly[:1]):
        if (y1>y)!=(y2>y) and x<(x2-x1)*(y-y1)/(y2-y1)+x1: c=not c
    return c
def area(poly): return abs(sum(a[0]*b[1]-b[0]*a[1] for a,b in zip(poly,poly[1:]+poly[:1])))/2
# copper fills: (layer, net, polygons); a point is on copper if an odd number of polygons contain it,
# and its island is the smallest polygon containing it
fills=[]
for sh in d['shape']:
    q=sh.split('~')
    if q[0]=='COPPERAREA' and q[10] not in('','[[]]'):
        fills.append((q[1],q[3],[p for f in json.loads(q[10]) for t in f for p in paths(t)]))
    elif q[0]=='PLANEZONE':
        fills.append((q[1],q[2],[p for t in sh.split('#@$')[1:] for p in paths(t.split('~',1)[1])]))
def island(fi,p):
    polys=fills[fi][2]; hit=[i for i,pl in enumerate(polys) if inpoly(p,pl)]
    return min(hit,key=lambda i:area(polys[i])) if len(hit)%2 else None
def segdist(p,a,b):
    ax,ay=a;bx,by=b;px,py=p
    dx,dy=bx-ax,by-ay
    L=dx*dx+dy*dy
    t=0 if L==0 else max(0,min(1,((px-ax)*dx+(py-ay)*dy)/L))
    return math.hypot(px-ax-t*dx,py-ay-t*dy)
nets=collections.defaultdict(lambda:dict(pads=[],segs=[],vias=[]))
for r,p in parts.items():
    for pad in p['pads']:
        if pad['net']: nets[pad['net']]['pads'].append((r,pad))
for t in tracks:
    if t['layer'] in('1','2','21','22'):
        for a,b in zip(t['pts'],t['pts'][1:]): nets[t['net']]['segs'].append((t['layer'],a,b,t['w']))
for v in vias: nets[v['net']]['vias'].append(v)
def padhit(pad,layer,pt,w):
    if pad['layer']!='11' and pad['layer']!=layer: return False
    hw=max(pad['w'],pad['h'])/2 if pad['rot'] not in('0','180','90','270','') else None
    # axis-aligned approx w/ rotation
    rot=float(pad['rot'] or 0)%180
    W,H=(pad['w'],pad['h']) if abs(rot)<1 else ((pad['h'],pad['w']) if abs(rot-90)<1 else (max(pad['w'],pad['h']),)*2)
    return abs(pt[0]-pad['x'])<=W/2+w/2 and abs(pt[1]-pad['y'])<=H/2+w/2
res={}
for n,N in nets.items():
    items=[('pad',i) for i in range(len(N['pads']))]+[('seg',i) for i in range(len(N['segs']))]+[('via',i) for i in range(len(N['vias']))]
    par={k:k for k in items}
    def f(x):
        while par[x]!=x: par[x]=par[par[x]]; x=par[x]
        return x
    def u(a,b): par[f(a)]=f(b)
    S=N['segs']
    for i,(l,a,b,w) in enumerate(S):
        for j in range(i+1,len(S)):
            l2,a2,b2,w2=S[j]
            if l!=l2: continue
            tol=(w+w2)/2
            if min(segdist(a,a2,b2),segdist(b,a2,b2),segdist(a2,a,b),segdist(b2,a,b))<=tol*0.6: u(('seg',i),('seg',j))
        for k,(r,pad) in enumerate(N['pads']):
            if padhit(pad,l,a,0) or padhit(pad,l,b,0): u(('seg',i),('pad',k))
        for k,v in enumerate(N['vias']):
            if segdist((v['x'],v['y']),a,b)<=v['d']/2: u(('seg',i),('via',k))
    for k,v in enumerate(N['vias']):
        for m,(r,pad) in enumerate(N['pads']):
            if padhit(pad,'1',(v['x'],v['y']),v['d']) or padhit(pad,'2',(v['x'],v['y']),v['d']): u(('via',k),('pad',m))
    # pours and planes: pads, vias and track ends on the same fill island are connected
    for fi,(fl,fn,_) in enumerate(fills):
        if fn!=n: continue
        pts=[(('pad',m),(pad['x'],pad['y'])) for m,(r,pad) in enumerate(N['pads']) if pad['layer'] in(fl,'11')]
        pts+=[(('via',k),(v['x'],v['y'])) for k,v in enumerate(N['vias'])]
        pts+=[(('seg',i),e) for i,(l,a,b,w) in enumerate(S) if l==fl for e in(a,b)]
        first={}
        for key,p in pts:
            isl=island(fi,p)
            if isl is None: continue
            if isl in first: u(key,first[isl])
            else: first[isl]=key
    groups=collections.defaultdict(list)
    for m,(r,pad) in enumerate(N['pads']): groups[f(('pad',m))].append(f"{r}.{pad['num']}")
    res[n]=list(groups.values())
if __name__=='__main__':
    real={n:g for n,g in res.items() if len(nets[n]['pads'])>1}
    done=[n for n,g in real.items() if len(g)==1]
    print('nets with >=2 pads:',len(real),'fully routed:',len(done))
    inc={n:g for n,g in real.items() if len(g)>1}
    tot=0
    for n,g in sorted(inc.items(),key=lambda x:-len(x[1])):
        tot+=len(g)-1
        if n=='GND': print('GND islands',len(g),'of',len(nets[n]['pads']),'pads'); continue
        print(f"{n}: {len(g)} islands, missing {len(g)-1}: "+' | '.join(','.join(x) for x in g))
    print('total missing connections',tot)
