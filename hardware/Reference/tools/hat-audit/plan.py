import sys,math,copy,collections
sys.path.insert(0,'.')
from pcb import load_pcb
from rats import score,netpts,mst
d,P0,tracks,vias,holes=load_pcb()
def ext(p):
    xs=[];ys=[]
    for q in p['pads']:
        rot=float(q['rot'] or 0)%180
        W,H=(q['w'],q['h']) if rot<1 else (q['h'],q['w'])
        xs+=[q['x']-W/2,q['x']+W/2]; ys+=[q['y']-H/2,q['y']+H/2]
    b=p['box']
    return (min(xs+[b[0]]),max(xs+[b[1]]),min(ys+[b[2]]),max(ys+[b[3]]))
def move(parts,refs,dx=0,dy=0,rot=0,pivot=None):
    """rigid move of a group: rotate by rot (deg CCW, multiples of 90) about pivot (default group centre), then translate"""
    E=[ext(parts[r]) for r in refs]
    cx=(min(e[0] for e in E)+max(e[1] for e in E))/2; cy=(min(e[2] for e in E)+max(e[3] for e in E))/2
    if pivot: cx,cy=pivot
    c,s=round(math.cos(math.radians(rot))),round(math.sin(math.radians(rot)))
    def T(x,y): 
        x-=cx;y-=cy
        return cx+c*x-s*y+dx, cy+s*x+c*y+dy
    for r in refs:
        p=parts[r]
        for q in p['pads']:
            q['x'],q['y']=T(q['x'],q['y'])
            if rot%180: q['rot']=str((float(q['rot'] or 0)+90)%360)
        b=p['box']; pts=[T(b[0],b[2]),T(b[1],b[3])]
        p['box']=(min(a[0] for a in pts),max(a[0] for a in pts),min(a[1] for a in pts),max(a[1] for a in pts))
        p['c']=T(*p['c'])
def check(parts,margin=0.15):
    bad=[]
    R=list(parts)
    E={r:ext(parts[r]) for r in R}
    lay={r:('B' if parts[r]['pads'] and all(q['layer']=='2' for q in parts[r]['pads']) else 'T') for r in R}
    for i,a in enumerate(R):
        for b in R[i+1:]:
            if lay[a]!=lay[b]: continue
            A,B=E[a],E[b]
            if A[0]<B[1]+margin and B[0]<A[1]+margin and A[2]<B[3]+margin and B[2]<A[3]+margin:
                ov=min(A[1],B[1])-max(A[0],B[0]),min(A[3],B[3])-max(A[2],B[2])
                bad.append((a,b,round(min(ov),2)))
    for h in holes:
        ko=(5.5 if h['d']<3 else 6.5)/2
        for r in R:
            if lay[r]=='B': continue
            x0,x1,y0,y1=E[r]; cx=min(max(h['x'],x0),x1); cy=min(max(h['y'],y0),y1)
            if math.dist((h['x'],h['y']),(cx,cy))<ko: bad.append((r,'HOLE@%.0f,%.0f'%(h['x'],h['y']),round(ko-math.dist((h['x'],h['y']),(cx,cy)),2)))
    for r in R:
        x0,x1,y0,y1=E[r]
        if x0<0.3 or x1>64.7 or y1>-0.3 or y0<-72.2: bad.append((r,'EDGE',(round(x0,1),round(x1,1),round(y0,1),round(y1,1))))
    return bad
def j1cross(parts):
    N=netpts(parts); out={}
    for n,pts in N.items():
        up=[p for p in pts if p[1]>-17]; dn=[p for p in pts if p[1]<-22]
        j=[p for p in pts if -22<=p[1]<=-17]
        if up and dn: out[n]='band-to-band'
        elif j and up and any(abs(p[1]+20.82)<0.1 for p in j): out[n]='lower-row pin -> top band'
    return out
