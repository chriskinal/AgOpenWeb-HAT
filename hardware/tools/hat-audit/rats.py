import sys,math,collections
sys.path.insert(0,'.')
from pcb import load_pcb
def mst(pts):
    if len(pts)<2: return 0,[]
    inT=[0]; rest=list(range(1,len(pts))); L=0; E=[]
    while rest:
        d,i,j=min((math.dist(pts[a],pts[b]),a,b) for a in inT for b in rest)
        L+=d; E.append((pts[i],pts[j])); inT.append(j); rest.remove(j)
    return L,E
def netpts(parts):
    N=collections.defaultdict(list)
    for r,p in parts.items():
        for q in p['pads']:
            if q['net'] and not q['net'].startswith(r+'_'): N[q['net']].append((q['x'],q['y'],r+'.'+q['num']))
    return N
def cross(a,b,c,d):
    def o(p,q,r): return (q[0]-p[0])*(r[1]-p[1])-(q[1]-p[1])*(r[0]-p[0])
    if a in(c,d) or b in(c,d): return False
    return o(a,b,c)*o(a,b,d)<0 and o(c,d,a)*o(c,d,b)<0
def score(parts,skip=('GND','+3V3','5V_MAIN')):
    N=netpts(parts); tot=0; edges=[]; per={}
    for n,pts in N.items():
        L,E=mst([p[:2] for p in pts]); per[n]=L
        if n in skip: continue
        tot+=L; edges+=[(n,a,b) for a,b in E]
    x=0
    for i in range(len(edges)):
        for j in range(i+1,len(edges)):
            if edges[i][0]!=edges[j][0] and cross(edges[i][1],edges[i][2],edges[j][1],edges[j][2]): x+=1
    return tot,x,per,edges
if __name__=='__main__':
    d,parts,tracks,vias,holes=load_pcb()
    tot,x,per,edges=score(parts)
    print('signal MST total %.0f mm, crossings %d'%(tot,x))
    for n,L in sorted(per.items(),key=lambda x:-x[1])[:45]: print(f'{n:14} {L:6.1f}')
    # J1 row crossings: edges whose endpoints lie on opposite sides of y=-19.55 band (excluding J1 pads)
    cr=collections.Counter()
    for n,a,b in edges:
        if (a[1]>-17 and b[1]<-22) or (b[1]>-17 and a[1]<-22): cr[n]+=1
    print('nets crossing J1 row:',dict(cr))
    for r in sorted(parts,key=lambda r:(-parts[r]['c'][1]//8,parts[r]['c'][0])):
        p=parts[r]; b=p['box']
        print(f"{r:5} c=({p['c'][0]:5.1f},{p['c'][1]:6.1f}) rot={p['rot']:4} box x{b[0]:5.1f}..{b[1]:5.1f} y{b[2]:6.1f}..{b[3]:6.1f} L{p['pads'][0]['layer'] if p['pads'] else ''}")
