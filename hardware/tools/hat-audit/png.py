import sys,math,pickle
sys.path.insert(0,'.')
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle,Circle,FancyArrowPatch,FancyBboxPatch
from plan import *
from final import build
P,M=build()
COL={'A':'#d62728','B':'#1f77b4','C':'#2ca02c','D':'#9467bd','E':'#ff7f0e','F':'#8c564b'}
moved={r:m['k'] for m in M for r in m['refs']}
fig=plt.figure(figsize=(17,13.2),facecolor='white')
ax=fig.add_axes([0.03,0.04,0.60,0.88]); tx=fig.add_axes([0.645,0.04,0.34,0.88]); tx.axis('off')
ax.add_patch(FancyBboxPatch((0,-72.5),65,72.5,boxstyle='round,pad=0,rounding_size=3',fc='#f4f7f2',ec='black',lw=2))
# reserved corridor
ax.add_patch(Rectangle((33.4,-39.6),4.2,16.2,fc='#ffe9a8',ec='#c79a00',ls='--',lw=1.2,alpha=0.7,zorder=1))
ax.text(35.5,-31.5,'keep clear:\nSPI / CS route\nJ1 to J6 gaps',fontsize=7,rotation=90,ha='center',va='center',color='#7a5c00',zorder=2)
FIX=('J1','J2','J3','J6','J7')
def draw(p,fc,ec,lw=0.5,alpha=1,z=3,ls='-',hatch=None):
    e=ext(p); ax.add_patch(Rectangle((e[0],e[2]),e[1]-e[0],e[3]-e[2],fc=fc,ec=ec,lw=lw,alpha=alpha,zorder=z,ls=ls,hatch=hatch)); return e
for r,p in P0.items():
    if r in moved: continue
    bot=p['pads'] and all(q['layer']=='2' for q in p['pads'])
    e=draw(p,'#c9c9c9' if r in FIX else ('#e6d9c3' if bot else '#dfe5ea'),'#777',0.5)
    w,h=e[1]-e[0],e[3]-e[2]
    big=r in FIX or w*h>12
    ax.text((e[0]+e[1])/2,(e[2]+e[3])/2,r,fontsize=9 if r in FIX else (6.5 if big else 4.6),ha='center',va='center',color='#333',zorder=4,weight='bold' if r in FIX else 'normal',rotation=90 if (h>2.2*w and h>3) else 0)
ax.add_patch(Circle((7.1,-7.2),6.25,fc='none',ec='#777',lw=0.8,zorder=3))
for h in holes:
    ko=(5.5 if h['d']<3 else 6.5)/2
    ax.add_patch(Circle((h['x'],h['y']),ko,fc='none',ec='#888',ls=':',lw=0.8,zorder=3))
    ax.add_patch(Circle((h['x'],h['y']),h['d']/2,fc='white',ec='black',lw=1.2,zorder=4))
for m in M:
    c=COL[m['k']]
    b0,b1=m['b0'],m['b1']
    ax.add_patch(Rectangle((b0[0],b0[2]),b0[1]-b0[0],b0[3]-b0[2],fc='none',ec=c,lw=1.6,ls='--',hatch='////',alpha=0.55,zorder=5))
    for r in m['refs']:
        e=draw(P[r],c,'black',0.6,alpha=0.85,z=7)
        ax.text((e[0]+e[1])/2,(e[2]+e[3])/2,r,fontsize=6.5 if (e[1]-e[0])*(e[3]-e[2])>6 else 4.8,ha='center',va='center',color='white',weight='bold',zorder=8)
    ax.add_patch(Rectangle((b1[0]-0.35,b1[2]-0.35),b1[1]-b1[0]+0.7,b1[3]-b1[2]+0.7,fc='none',ec=c,lw=2.2,zorder=8))
    a=((b0[0]+b0[1])/2,(b0[2]+b0[3])/2); b=((b1[0]+b1[1])/2,(b1[2]+b1[3])/2)
    # shorten arrow to box edges
    def edge(bx,frm,to):
        cx,cy=(bx[0]+bx[1])/2,(bx[2]+bx[3])/2; dx,dy=to[0]-frm[0],to[1]-frm[1]
        hw,hh=(bx[1]-bx[0])/2+0.4,(bx[3]-bx[2])/2+0.4
        t=min(hw/abs(dx) if dx else 9,hh/abs(dy) if dy else 9)
        L=math.hypot(dx,dy); return (cx+dx*t,cy+dy*t)
    s=edge(b0,a,b); e2=edge(b1,b,a)
    rad={'A':-0.25,'B':0.3,'C':-0.18,'D':0.25,'E':-0.2,'F':0.3}[m['k']]
    ax.add_patch(FancyArrowPatch(s,e2,arrowstyle='-|>',mutation_scale=22,lw=2.6,color=c,connectionstyle=f'arc3,rad={rad}',zorder=9))
    lx,ly={'A':(44.2,-26.6),'B':(55.0,-52.0),'C':(45.6,-66.2) ,'D':(56.5,-28.6),'E':(49.8,-2.4),'F':(23.3,-29.6)}[m['k']]
    ax.text(lx,ly,m['k'],fontsize=13,weight='bold',color='white',ha='center',va='center',zorder=10,bbox=dict(boxstyle='circle,pad=0.25',fc=c,ec='black',lw=1))
ax.set_xlim(-2.5,67.5); ax.set_ylim(-75,2.5); ax.set_aspect('equal')
ax.set_xticks(range(0,66,5)); ax.set_yticks(range(-70,1,5)); ax.tick_params(labelsize=8); ax.grid(lw=0.25,color='#bbb')
ax.set_xlabel('EasyEDA editor X (mm)',fontsize=9); ax.set_ylabel('EasyEDA editor Y (mm)',fontsize=9)
fig.text(0.03,0.955,'AOW HAT: component moves to relieve routing congestion',fontsize=18,weight='bold')
fig.text(0.03,0.932,'Top view. Hatched dashed box = where the block is now. Solid colour = where it goes. Grey parts stay put.',fontsize=10.5,color='#333')
# table
y=0.985
tx.text(0,y,'What to move',fontsize=14,weight='bold',va='top'); y-=0.04
for m in M:
    c=COL[m['k']]
    tx.text(0.0,y,m['k'],fontsize=12,weight='bold',color='white',va='top',bbox=dict(boxstyle='circle,pad=0.25',fc=c,ec='black',lw=0.8))
    rot=f", rotate block {m['rot']}°" if m['rot'] else ''
    tx.text(0.075,y+0.004,f"{m['name']}{rot}",fontsize=11,weight='bold',va='top',color=c)
    y-=0.022
    tx.text(0.075,y,m['why'],fontsize=9,va='top',color='#333'); y-=0.021
    for r in m['refs']:
        tx.text(0.075,y,f"{r:<4}",fontsize=8.5,va='top',family='monospace',weight='bold')
        tx.text(0.16,y,f"({m['c0'][r][0]:5.1f}, {m['c0'][r][1]:6.1f})  →  ({m['c1'][r][0]:5.1f}, {m['c1'][r][1]:6.1f})",fontsize=8.5,va='top',family='monospace')
        y-=0.0158
    y-=0.009
tx.text(0.075,y+0.006,'Coordinates are footprint origins, X, Y in mm.',fontsize=8,va='top',color='#555',style='italic'); y-=0.03
tx.text(0,y,'Before you re-route',fontsize=14,weight='bold',va='top'); y-=0.032
notes=['Pour GND on the bottom layer (and top). GND is 121 pads:\nrouted as tracks it eats the channels every other net needs.',
 'Set rules to 0.2 mm track / 0.2 mm gap, power 0.5 mm.\nThe file uses 0.125 / 0.10 mm, below JLCPCB 2-layer limits.',
 'Hand-route first: 5V_MAIN, +3V3, SPI bus, CAN pairs, clock.\nThen autoroute the rest.',
 'Keep the yellow corridor free of parts.',
 'Drop J1 pin 17 from CM4_3V3 (pin 1 is the same rail).']
for i,n in enumerate(notes,1):
    tx.text(0.0,y,f'{i}.',fontsize=9.5,va='top',weight='bold'); tx.text(0.05,y,n,fontsize=9.5,va='top',linespacing=1.35)
    y-=0.0175*(n.count('\n')+1)+0.009
tx.set_xlim(0,1); tx.set_ylim(0,1)
import os
out=os.environ.get('HAT_OUT',os.path.expanduser('~/Downloads/HAT_layout_moves.png'))
fig.savefig(out,dpi=150,facecolor='white'); print(out)
