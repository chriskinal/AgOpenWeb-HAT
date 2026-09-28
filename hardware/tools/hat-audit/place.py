import json,sys,math,re
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
d=json.load(open(sys.argv[1])); out=sys.argv[2]
U=0.254; ox,oy=float(d['head']['x']),float(d['head']['y'])
X=lambda x:(float(x)-ox)*U; Y=lambda y:-(float(y)-oy)*U
parts={}
for s in d['shape']:
    if not s.startswith('LIB'): continue
    P=s.split('#@$'); h=P[0].split('~')
    ref=[p.split('~')[10] for p in P if p.startswith('TEXT~P~')]
    if not ref: continue
    pl=[p.split('~')[6] for p in P if p.startswith('PAD')]
    layer='bottom' if pl and all(x=='2' for x in pl) else 'top'
    pads=[(X(q[2]),Y(q[3]),q[7],q[8]) for q in (p.split('~') for p in P if p.startswith('PAD'))]
    sx=[];sy=[]
    for p in P:
        q=p.split('~')
        if q[0]=='TRACK' and q[2] in('3','4'):
            v=list(map(float,q[4].split())); sx+= [X(a) for a in v[0::2]]; sy+= [Y(b) for b in v[1::2]]
    parts[ref[0]]={'pads':pads,'c':(X(h[1]),Y(h[2])),'layer':layer,'silk':(min(sx),max(sx),min(sy),max(sy)) if sx else None}
T1=set('C3 C4 C9 C11 C15 C16 C17 C18 C19 C20 C21 C22 C23 C24 C25 C26 C27 C28 C29 C30 C31 C34 C35 C36 C37 C38 C39 C43 C44 C45 C46 C47 C48 C49'.split())
own={}
def o(owner,*rs):
    for r in rs: own[r]=owner
o('U1','C3','C4'); o('U3','C11','R12','R13','C12','R14'); o('U4','C15','C16','C17','C18','C19'); o('Q2','R7','R8','C7')
o('Q3','R5','R6','C6','Q6','Q7','Q1'); o('U2','C9','C8','R10','D4','Q4','Q8','Q5'); o('D4','R9'); o('Q5','C10','R11'); o('Q7','R15','C13'); o('Q8','Q9')
o('J3','TV2','TV1','D6','R23','D7','R24','D8','R25','D13','D14','D9','D10','D11','D12','D15','D16','D17','R34','R35','R36')
o('R3','R4','C5','D2'); o('U8','R16','R3'); o('J1','R18'); o('R16','R17','C14','D5'); o('R23','SJ1'); o('R24','SJ2'); o('R25','SJ3')
o('X1','C29','R20','R21','R22'); o('U5','C20','C21','C22'); o('U6','C23','C24','C25'); o('U7','C26','C27','C28')
o('U8','C30','C31','R26','C32','R27','C33','R19'); o('U9','C34','C35','C36','C37','C38','C39')
o('D15','R28'); o('D16','R29'); o('D17','R30'); o('R28','R31','C40'); o('R29','R32','C41','D18'); o('R30','R33','C42')
o('U10','R37','C43','C44','R38'); o('R38','SW1'); o('U11','C45','R39'); o('D19','C46','C50'); o('D20','C47'); o('D21','C48'); o('D22','C49')
o('Q10','R40','R41'); o('BZ1','R42','Q10')
GNDN={'GND',''}
onb=lambda r:-1<parts[r]['c'][0]<66 and -73.5<parts[r]['c'][1]<1
res=[]
for r,ow in own.items():
    if r not in parts or ow not in parts: res.append((r,ow,None,'missing')); continue
    if not(onb(r) and onb(ow)): res.append((r,ow,None,'unplaced')); continue
    A=parts[r]['pads']; B=parts[ow]['pads']
    shared=[(a,b) for a in A for b in B if a[2]==b[2] and a[2] not in GNDN]
    cand=shared or [(a,b) for a in A for b in B]
    a,b=min(cand,key=lambda ab:math.dist(ab[0][:2],ab[1][:2]))
    dist=math.dist(a[:2],b[:2])
    lim=3.0 if r in T1 else (12.0 if ow=='J3' else 8.0)
    res.append((r,ow,dist,'OK' if dist<=lim else 'FAR',a,b,lim))
fig,ax=plt.subplots(figsize=(11,13))
ax.plot([0,65,65,0,0],[0,0,-72.5,-72.5,0],'k-',lw=1.5)
for r,p in parts.items():
    if not onb(r): continue
    xs=[q[0] for q in p['pads']] or [p['c'][0]]; ys=[q[1] for q in p['pads']] or [p['c'][1]]
    x0,x1,y0,y1=min(xs)-0.6,max(xs)+0.6,min(ys)-0.6,max(ys)+0.6
    if p['silk']:
        a0,a1,b0,b1=p['silk']; x0,x1,y0,y1=min(x0,a0),max(x1,a1),min(y0,b0),max(y1,b1)
    p['box']=(x0,x1,y0,y1)
    col='#9ecae1' if p['layer']=='top' else '#fdd0a2'
    ax.add_patch(plt.Rectangle((x0,y0),x1-x0,y1-y0,fc=col,ec='#555',lw=0.4,alpha=0.8))
    ax.text((x0+x1)/2,(y0+y1)/2,r,fontsize=5.5,ha='center',va='center')
for x in res:
    if x[2] is None: continue
    r,ow,dist,st,a,b,lim=x
    ax.plot([a[0],b[0]],[a[1],b[1]],'-',color='green' if st=='OK' else 'red',lw=1.4 if st=='FAR' else 0.6)

# board holes + standoff keep-outs (M2.5 HAT holes ~5.5 mm, M3 GNSS holes ~6.5 mm)
for sh in d['shape']:
    f=sh.split('~')
    if f[0]!='HOLE': continue
    hx,hy,dia=X(f[1]),Y(f[2]),2*float(f[3])*U
    ko=5.5 if dia<3.0 else 6.5

    ax.add_patch(plt.Circle((hx,hy),dia/2,fc='white',ec='black',lw=1.2,zorder=5))
    ax.add_patch(plt.Circle((hx,hy),ko/2,fill=False,ec='purple',ls='--',lw=0.8,zorder=5))
    ax.text(hx,hy-ko/2-0.9,f'Ø{dia:.1f}',fontsize=5,ha='center',color='purple',zorder=6)
ax.set_aspect('equal'); ax.set_xlim(-3,68); ax.set_ylim(-75,3); ax.grid(lw=0.2)
ax.set_title('Placement check: green = near owner, red = too far | blue = top, orange = bottom | purple dashed = standoff keep-out')
plt.savefig(out,dpi=130,bbox_inches='tight')
intr=[]
for sh in d['shape']:
    f=sh.split('~')
    if f[0]!='HOLE': continue
    hx,hy,dia=X(f[1]),Y(f[2]),2*float(f[3])*U; ko=(5.5 if dia<3.0 else 6.5)/2

    for r,p in parts.items():
        if 'box' not in p or p['layer']=='bottom': continue
        x0,x1,y0,y1=p['box']; cx=min(max(hx,x0),x1); cy=min(max(hy,y0),y1)
        dd=math.dist((hx,hy),(cx,cy))
        if dd<ko: intr.append((r,round(hx,1),round(hy,1),round(ko-dd,2)))
print('KEEP-OUT intrusions (part, hole x,y, overlap mm):',intr)
ob=[]
for r,p in parts.items():
    if 'box' not in p: continue
    x0,x1,y0,y1=p['box']
    if x0<0 or x1>65 or y1>0 or y0<-72.5: ob.append((r,round(x0,1),round(x1,1),round(y0,1),round(y1,1)))
print('past board edge:',ob)
far=sorted([x for x in res if x[3]=='FAR'],key=lambda x:-x[2]/x[6])
miss=[x for x in res if x[3]=='missing']
onboard=[r for r,p in parts.items() if not(-1<p['c'][0]<66 and -73.5<p['c'][1]<1)]
print('parts',len(parts),'| off-board:',sorted(onboard))
print('missing refs:',[x[0] for x in miss])
print('FAR (%d):'%len(far))
for r,ow,dist,st,a,b,lim in far: print(f'  {r:5} -> {ow:4} {dist:5.1f} mm (limit {lim})')
